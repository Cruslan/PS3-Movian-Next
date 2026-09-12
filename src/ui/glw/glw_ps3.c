/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <assert.h>
#include <sys/time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>

#include "settings.h"

#include "glw.h"
#include "glw_rsx.h"
#include "glw_settings.h"
#include "glw_video_common.h"

#include "main.h"
#include "settings.h"
#include "misc/extents.h"
#include "misc/str.h"
#include "navigator.h"
#include "arch/arch.h"

/**
 * PPU LV2 Kernel Services & RSX Graphics Subsystem Headers (PSL1GHT v2):
 * Supplies low-level LV2 syscalls, RSX command fifo primitives, hardware registers,
 * and memory management for the PlayStation 3 Cell Broadband Engine & RSX NV47 GPU.
 */
#include <ppu-lv2.h>
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/commands.h>
#include <rsx/nv40.h>

#include <lv2/memory.h>
#include <sys/memory.h>

#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>

#include <io/pad.h>
#include <io/kb.h>

#include <sysmodule/sysmodule.h>

/**
 * Type aliases and compatibility bindings:
 * Bridges historical PSL1GHT v1 structure types and LV2 uppercase syscall macros
 * to modern PSL1GHT v2 canonical standards.
 */
typedef padData PadData;
typedef padInfo2 PadInfo2;
typedef videoResolution VideoResolution;
typedef videoState VideoState;
typedef videoConfiguration VideoConfiguration;
typedef sys_mem_container_t mem_container_t;

#ifndef Lv2Syscall0
#define Lv2Syscall0 lv2syscall0
#define Lv2Syscall1 lv2syscall1
#define Lv2Syscall2 lv2syscall2
#define Lv2Syscall3 lv2syscall3
#define Lv2Syscall4 lv2syscall4
#define Lv2Syscall5 lv2syscall5
#define Lv2Syscall6 lv2syscall6
#define Lv2Syscall7 lv2syscall7
#define Lv2Syscall8 lv2syscall8
#endif


#include "glw_rec.h"

typedef struct glw_ps3 {

  glw_root_t gr;

  float gp_browser_alpha;

  int gp_stop;
  int gp_seekmode;

  VideoResolution res;

  u32 framebuffer[2];
  int framebuffer_pitch;

  u32 depthbuffer;
  int depthbuffer_pitch;

  char kb_present[MAX_KEYBOARDS];

  KbConfig kb_config[MAX_KEYBOARDS];

  float scale;
  int button_assign;

  struct glw *osk_widget;
  mem_container_t osk_container;

} glw_ps3_t;

glw_ps3_t *glwps3;
char *rsx_address;
static struct extent_pool *rsx_mempool;
static hts_mutex_t rsx_mempool_lock;
static void clear_btns(void);

static u32 *rsx_initial_cmd_buffer = NULL;
static u32 rsx_cmd_buffer_words = 0;
static int first_fb = 1;

#define GCM_LABEL_INDEX 255
static u32 sLabelVal = 1;

extern s32 ioPadGetDataExtra(u32 port, u32* type, PadData* data);

/**
 * @brief Wait for all queued RSX backend commands to complete execution.
 *
 * Emits a backend label write command into the RSX command ring buffer, flushes
 * the FIFO via rsxFlushBuffer, and actively polls the hardware label address until
 * the RSX rasterization backend acknowledges completion of all prior rendering tasks.
 *
 * @param context Pointer to active GCM context descriptor.
 *
 * Algorithmic & Synchronization Invariants:
 * - Emits hardware label token at GCM_LABEL_INDEX (255) holding sLabelVal.
 * - Yields host CPU via usleep(30) during active polling to avoid thread contention.
 *
 * Complexity:
 * - Time Complexity: O(T) where T is RSX hardware pipeline drain duration.
 * - Space Complexity: O(1) stack allocation.
 */
static void
waitFinish(gcmContextData *context)
{
  /* Emit backend label write command into the active RSX command buffer */
  rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);

  /* Flush the FIFO to guarantee the RSX hardware immediately fetches and processes commands */
  rsxFlushBuffer(context);

  /* Poll the volatile memory-mapped label register until the RSX signals completion */
  while(*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX) != sLabelVal)
    usleep(30);

  /* Increment monotonic label sequence value for subsequent synchronizations */
  ++sLabelVal;
}

/**
 * @brief Synchronize the RSX command processor to complete idle state.
 *
 * Flushes all pending hardware commands in the FIFO and ensures GET and PUT pointers
 * are completely aligned and drained. This establishes architectural synchronization
 * before registering framebuffers and initializing rendering context, avoiding FIFO desync.
 *
 * @param context Pointer to active GCM context descriptor.
 *
 * Algorithmic Invariants:
 * - Emits paired write-backend and wait-backend label commands to enforce pipeline fence.
 * - Calls waitFinish to block PPU execution until the RSX processor arrives at full idle.
 *
 * Complexity:
 * - Time Complexity: O(T) where T is RSX hardware pipeline drain duration.
 * - Space Complexity: O(1) stack allocation.
 */
static void
waitRSXIdle(gcmContextData *context)
{
  /* Enqueue backend label write and synchronization wait barriers in command FIFO */
  rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, sLabelVal);
  rsxSetWaitLabel(context, GCM_LABEL_INDEX, sLabelVal);

  /* Increment label counter before dispatching finish barrier */
  ++sLabelVal;

  /* Flush and block until RSX has completely drained all commands and entered idle state */
  waitFinish(context);
}

/**
 * @brief Waits for hardware VSYNC display scanout swap to complete.
 *
 * Polls the RSX display flip status register until the pending buffer flip has
 * transitioned to the scanout rasterizer. Sleeps in 200 microsecond increments
 * to yield CPU time to concurrent worker and media decoding threads.
 *
 * Algorithmic Complexity:
 * - Time Complexity: O(1) bounded by VSYNC duration (max 16.6 ms at 60 Hz).
 * - Space Complexity: O(1) stack allocation.
 */
static void
waitFlip()
{
  int i = 0;
  while(gcmGetFlipStatus() != 0) {
    i++;
    usleep(200);
    if(i == 10000) {
      TRACE(TRACE_ERROR, "GLW", "Flip never happend, system reboot");
      Lv2Syscall3(379, 0x1200, 0, 0 );
      gcmResetFlipStatus();
    }
  }
  gcmResetFlipStatus();
}

/**
 * @brief Reset the RSX command buffer to the initial post-setup address every frame.
 *
 * Implements the canonical command buffer ring-buffer reset documented by RPCS3 lead RSX
 * developer kd-11 in ps3gl (source/rsxutil.c):
 * 1. Emits rsxFinish(ctx, 1) command marker into the active stream.
 * 2. Emits an NV40 hardware JUMP opcode pointing directly to rsx_initial_cmd_buffer offset.
 * 3. Enforces a PowerPC architecture memory barrier (__asm__ volatile("sync" ::: "memory"))
 *    guaranteeing that the JUMP opcode is committed to physical RAM before modifying control registers.
 * 4. Updates the hardware PUT control register (ctrl->put = startoffs). Because the RSX GET pointer
 *    is currently at the tail of the buffer (GET != PUT), the RSX FIFO fetches and executes through
 *    the JUMP instruction, branching GET to startoffs. At startoffs, GET == PUT, causing the RSX
 *    command processor to cleanly halt in an idle state without fetching stale data.
 * 5. PPU spins on ctrl->get with usleep yielding until the RSX processor arrives at startoffs.
 * 6. Resets GCM context cursors (current, begin, end) back to rsx_initial_cmd_buffer.
 *
 * @param gp Pointer to PS3 UI state context.
 *
 * Complexity:
 * - Time Complexity: O(1) bounded hardware register polling.
 * - Space Complexity: O(1) stack allocation.
 */
static void
resetCommandBuffer(glw_ps3_t *gp)
{
  gcmContextData *ctx = gp->gr.gr_be.be_ctx;
  u32 startoffs = 0;

  rsxFinish(ctx, 1);

  if(unlikely(rsxAddressToOffset(rsx_initial_cmd_buffer, &startoffs) != 0))
    return;

  rsxSetJumpCommand(ctx, startoffs);
  __asm__ volatile("sync" ::: "memory");

  gcmControlRegister volatile *ctrl = gcmGetControlRegister();
  ctrl->put = startoffs;

  uint32_t spins = 0;
  while(ctrl->get != startoffs && spins < 100000) {
    usleep(30);
    spins++;
  }

  ctx->current = rsx_initial_cmd_buffer;
  ctx->begin   = rsx_initial_cmd_buffer;
  ctx->end     = rsx_initial_cmd_buffer + rsx_cmd_buffer_words;
}

/**
 * @brief Queue hardware display buffer flip, enforce VSYNC synchronization,
 * and execute seamless command buffer ring-buffer reset at each frame boundary.
 *
 * Implements canonical frame-boundary display synchronization and command buffer resetting:
 * 1. Drains pending RSX commands for the current frame via waitFinish(ctx).
 * 2. Synchronizes with hardware VSYNC scanout swap via waitFlip().
 * 3. Enqueues display flip for the current backbuffer via gcmSetFlip(ctx, buffer) and flushes FIFO.
 * 4. Writes gcmSetWaitFlip(ctx) so RSX rasterizer waits for upcoming display scanout before rendering.
 * 5. Calls resetCommandBuffer(gp) to return the 4MB buffer back to rsx_initial_cmd_buffer every single frame.
 *
 * @param gp Pointer to PS3 UI state context.
 * @param buffer Double-buffering index (0 or 1) to be flipped to the display scanout.
 *
 * Time Complexity: O(1) command emission and bounded register synchronization.
 * Space Complexity: O(1) stack allocation.
 */
static void
flip(glw_ps3_t *gp, s32 buffer) 
{
  gcmContextData *ctx = gp->gr.gr_be.be_ctx;

  /* 1. Flush active rendering commands and ensure RSX finishes execution for this frame */
  waitFinish(ctx);

  /* 2. Synchronize with display VSYNC scanout */
  if(!first_fb)
    waitFlip();
  else {
    gcmResetFlipStatus();
    first_fb = 0;
  }

  /* Log initial frames and periodic milestones for runtime verification */
  static int flip_count = 0;
  if(flip_count++ < 5 || (flip_count % 300) == 0) {
    TRACE(TRACE_DEBUG, "GLW", "flip #%d on buffer %d", flip_count, buffer);
  }

  /* 3. Dispatch hardware flip for the backbuffer we just finished rendering */
  gcmSetFlip(ctx, buffer);
  rsxFlushBuffer(ctx);

  /* 4. Enqueue VSYNC wait-flip marker for the next frame */
  gcmSetWaitFlip(ctx);

  /* 5. Reset command buffer back to rsx_initial_cmd_buffer every single frame */
  resetCommandBuffer(gp);
}


/**
 *
 */
int
rsx_alloc(int size, int alignment)
{
  int pos;

  hts_mutex_lock(&rsx_mempool_lock);
  pos = extent_alloc_aligned(rsx_mempool, (size + 15) >> 4, alignment >> 4);
  if(0)TRACE(TRACE_DEBUG, "RSXMEM", "Alloc %d bytes (%d align) -> 0x%x",
	size, alignment, pos << 4);

  if(pos == -1) {
    int total, avail, fragments;
    extent_stats(rsx_mempool, &total, &avail, &fragments);
    TRACE(TRACE_ERROR, "RSX",
          "Low memory condition. Available %d of %d in %d fragments",
          avail * 16, total * 16, fragments);
  }

  hts_mutex_unlock(&rsx_mempool_lock);
  return pos == -1 ? -1 : pos << 4;
}


/**
 *
 */
void
rsx_free(int pos, int size)
{
  int r;

  hts_mutex_lock(&rsx_mempool_lock);
  r = extent_free(rsx_mempool, pos >> 4, (size + 15) >> 4);

  if(0)TRACE(TRACE_DEBUG, "RSXMEM", "Free %d + %d = %d", pos, size, r);

  if(r != 0)
    panic("RSX memory corrupted, error %d", r);

  hts_mutex_unlock(&rsx_mempool_lock);
}


/**
 *
 */
static pixmap_t *
rsx_read_pixels(glw_root_t *gr)
{
  glw_ps3_t *gp = (glw_ps3_t *)gr;

  pixmap_t *pm = pixmap_create(gr->gr_width, gr->gr_height, PIXMAP_RGBA, 0);

  memcpy(pm->pm_data, rsx_to_ppu(gp->framebuffer[0]),
         pm->pm_linesize * pm->pm_height);
  return pm;
}


/**
 * @brief Defensive hardware GCM Context callback executed on command buffer boundary exhaustion.
 *
 * Invoked by librsx when immediate-mode drawing commands exceed context->end.
 * In PSL1GHT v2 rsx_function_macros.h:
 *   #define RSX_CONTEXT_CURRENT_BEGIN(count) do { \
 *     if((context->current + (count)) > context->end) { \
 *       if(rsxContextCallback(context,(count))!=0) return; \
 *     } \
 *   } while(0)
 *
 * If this callback returns 0, execution does NOT abort and writing continues directly past
 * context->end, causing memory corruption of adjacent font glyph and texture pools.
 * Returning -1 forces rsxContextCallback to return -1, causing the calling routine
 * (e.g. rsxDrawVertex4f or glw_rsx_set_vp_constant_4f) to immediately abort and return,
 * establishing a hard hardware barrier against memory corruption.
 *
 * @param context Pointer to active GCM context descriptor.
 * @param count Number of 32-bit command words requested by the caller.
 * @return s32 -1 to abort command emission and prevent writing past context->end.
 *
 * Complexity:
 * - Time Complexity: O(1) error logging.
 * - Space Complexity: O(1) stack allocation.
 */
static s32
movian_rsx_cb(gcmContextData *context, u32 count)
{
  TRACE(TRACE_ERROR, "RSX", "Command buffer overflow intercepted and prevented (count=%u current=%p end=%p)\n",
        (unsigned int)count, context->current, context->end);
  return -1;
}

/**
 *
 */
static void
init_screen(glw_ps3_t *gp)
{

  /**
   * Allocate an 8MB shared host I/O memory window aligned to a 1MB boundary.
   * Granularity 0x400 represents 1MB pages in LV2 sys_memory_allocate (syscall 348).
   * Providing an 8MB memory window with a 4MB hardware command buffer provides hundreds
   * of frames of continuous rendering between ring-buffer wraps while conserving over 56MB
   * of GameOS physical RAM required for the Cell hardware video decoder (cellVdec).
   */
  u32 taddr;
  Lv2Syscall3(348, 8 * 1024 * 1024, 0x400, (u64)&taddr);
  void *host_addr = (void *)(uint64_t)taddr;
  assert(host_addr != NULL);

  /**
   * Initialize RSX Command Buffer & Hardware Context (librsx):
   * Allocates a 4MB command buffer (0x400000) inside the 8MB shared host I/O memory
   * window accessible by both PPE and RSX DMA engine.
   */
  s32 r_rsx = rsxInit(&gp->gr.gr_be.be_ctx, 4 * 1024 * 1024, 8 * 1024 * 1024, host_addr); 
  assert(r_rsx == 0);
  assert(gp->gr.gr_be.be_ctx != NULL);
  
  /* Synchronize RSX hardware FIFO so all initial configuration commands from rsxInit are flushed and processed */
  waitRSXIdle(gp->gr.gr_be.be_ctx);

  /* Register robust hardware ring-buffer wrap callback */
  rsxSetUserCallback(movian_rsx_cb);
  gp->gr.gr_be.be_ctx->callback = movian_rsx_cb;

  /**
   * Save initial post-initialization command buffer pointer.
   * rsxInit executes initial hardware register configuration between offset 0x1000 and 0x13a0.
   * Subsequent rendering frames must reset back to this initial cursor rather than 0x1000,
   * avoiding accidental corruption of hardware device objects or flip queues.
   */
  rsx_initial_cmd_buffer = gp->gr.gr_be.be_ctx->current;
  u32 initial_offset = 0;
  rsxAddressToOffset(rsx_initial_cmd_buffer, &initial_offset);

  u32 total_cb_bytes = 4 * 1024 * 1024;
  u32 used_init_bytes = (initial_offset >= 4096) ? (initial_offset - 4096) : 0;
  u32 available_bytes = (total_cb_bytes > used_init_bytes + 4096) ? (total_cb_bytes - used_init_bytes - 4096) : (total_cb_bytes / 2);
  rsx_cmd_buffer_words = available_bytes / sizeof(u32);

  gp->gr.gr_be.be_ctx->begin = rsx_initial_cmd_buffer;
  gp->gr.gr_be.be_ctx->current = rsx_initial_cmd_buffer;
  gp->gr.gr_be.be_ctx->end = rsx_initial_cmd_buffer + rsx_cmd_buffer_words;

  TRACE(TRACE_DEBUG, "RSX", "Command buffer bounds: begin=%p current=%p end=%p span=%ld bytes (init_offset=0x%x)\n",
        gp->gr.gr_be.be_ctx->begin, gp->gr.gr_be.be_ctx->current, gp->gr.gr_be.be_ctx->end,
        (long)((char*)gp->gr.gr_be.be_ctx->end - (char*)gp->gr.gr_be.be_ctx->begin), (unsigned int)initial_offset);
  
  gcmConfiguration config;
  gcmGetConfiguration(&config);

  TRACE(TRACE_DEBUG, "RSX", "memory @ %p size = %u\n",
	config.localAddress, (unsigned int)config.localSize);

  hts_mutex_init(&rsx_mempool_lock);
  rsx_mempool = extent_create(0, config.localSize >> 4);
  rsx_address = config.localAddress;



  VideoState state;
  videoGetState(0, 0, &state);
  
  // Get the current resolution
  videoGetResolution(state.displayMode.resolution, &gp->res);
  
  int num = gp->res.width;
  int den = gp->res.height;
  
  switch(state.displayMode.aspect) {
  case VIDEO_ASPECT_4_3:
    num = 4; den = 3;
    break;
  case VIDEO_ASPECT_16_9:
    num = 16; den = 9;
    break;
  }

  gp->scale = (float)(num * gp->res.height) / (float)(den * gp->res.width);

  TRACE(TRACE_DEBUG, "RSX",
	"Video resolution %d x %d  aspect=%d, pixel wscale=%f",
	gp->res.width, gp->res.height, state.displayMode.aspect, gp->scale);


  gp->framebuffer_pitch = 4 * gp->res.width; // each pixel is 4 bytes
  gp->depthbuffer_pitch = 4 * gp->res.width; // 4 bytes per pixel for Z24S8 (24-bit depth + 8-bit stencil)
  
  // Configure the buffer format to xRGB
  VideoConfiguration vconfig;
  memset(&vconfig, 0, sizeof(VideoConfiguration));
  vconfig.resolution = state.displayMode.resolution;
  vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
  vconfig.pitch = gp->framebuffer_pitch;
  vconfig.aspect = state.displayMode.aspect;

  videoConfigure(0, &vconfig, NULL, 0);
  videoGetState(0, 0, &state);
  
  const s32 buffer_size = gp->framebuffer_pitch * gp->res.height; 
  const s32 depth_buffer_size = gp->depthbuffer_pitch * gp->res.height;
  TRACE(TRACE_DEBUG, "RSX", "Buffer will be %d bytes", buffer_size);
  
  gcmSetFlipMode(GCM_FLIP_VSYNC); // Wait for VSYNC to flip
  
  // Allocate two buffers for the RSX to draw to the screen (double buffering)
  // Mandate strict 64-byte alignment required by NV40/G70 scanout & render surfaces
  gp->framebuffer[0] = rsx_alloc(buffer_size, 64);
  gp->framebuffer[1] = rsx_alloc(buffer_size, 64);

  TRACE(TRACE_DEBUG, "RSX", "Buffers at 0x%x 0x%x\n",
	gp->framebuffer[0], gp->framebuffer[1]);

  /* Allocate depth buffer with 4 bytes per pixel for Z24S8 */
  gp->depthbuffer = rsx_alloc(depth_buffer_size, 64);
  
  // Setup the display buffers
  gcmSetDisplayBuffer(0, gp->framebuffer[0],
		      gp->framebuffer_pitch, gp->res.width, gp->res.height);
  gcmSetDisplayBuffer(1, gp->framebuffer[1],
		      gp->framebuffer_pitch, gp->res.width, gp->res.height);

  gp->gr.gr_br_read_pixels = rsx_read_pixels;
}




/**
 * @brief Initialize PS3 GLW display, pad controllers, and system preferences.
 *
 * @param gp Pointer to PS3 UI state context.
 * @return 0 on success.
 *
 * Time Complexity: O(1) system hardware initialization.
 * Space Complexity: O(1) memory overhead.
 */
static int
glw_ps3_init(glw_ps3_t *gp)
{
  init_screen(gp);
  glw_rsx_init_context(&gp->gr);

  ioPadInit(7);
  ioKbInit(MAX_KB_PORT_NUM);

  int i;
  for(i = 0; i < 7; i++)
    ioPadSetPortSetting(i, 0x2);

  /* Query PS3 system parameter for cross vs circle button assignment convention */
  if(sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, &gp->button_assign))
    gp->button_assign = 1;

  return 0;
}


/**
 *
 */
static void
osk_returned(glw_ps3_t *gp)
{
  oskCallbackReturnParam param = {0};
  uint8_t ret[512];
  uint8_t buf[512];
  
  if(param.res != 0)
    return;

  assert(gp->osk_widget != NULL);

  param.len = sizeof(ret)/2;
  param.str = (u16 *)ret;
  oskUnloadAsync(&param);

  ucs2_to_utf8(buf, sizeof(buf), ret, sizeof(ret), 0);

  glw_lock(&gp->gr);

  glw_t *w = gp->osk_widget;
  if(!(w->glw_flags & GLW_DESTROYING) && w->glw_class->gc_update_text) {
    w->glw_class->gc_update_text(w, (const char *)buf);
  }
  glw_osk_close(&gp->gr);
  glw_unlock(&gp->gr);
}


/**
 *
 */
static void
osk_destroyed(glw_ps3_t *gp)
{
  glw_t *w = gp->osk_widget;
  assert(w != NULL);

  if(!(w->glw_flags & GLW_DESTROYING)) {
    event_t *e = event_create_action(ACTION_SUBMIT);
    glw_event_to_widget(w, e);
    event_release(e);
  }
  glw_unref(w);
  gp->osk_widget = NULL;

  if(gp->osk_container != 0xFFFFFFFFU)
    sysMemContainerDestroy(gp->osk_container);
}

/**
 *
 */
static void
osk_open(glw_root_t *gr, const char *title, const char *input, glw_t *w,
	 int password)
{
  oskParam param = {0};
  oskInputFieldInfo ifi = {0};
  glw_ps3_t *gp = (glw_ps3_t *)gr;
  
  if(gp->osk_widget)
    return;

  if(title == NULL)
    title = "";

  if(input == NULL)
    input = "";

  void *title16;
  void *input16;

  size_t s;
  s = utf8_to_ucs2(NULL, title, 0);
  title16 = malloc(s);
  utf8_to_ucs2(title16, title, 0);

  s = utf8_to_ucs2(NULL, input, 0);
  input16 = malloc(s);
  utf8_to_ucs2(input16, input, 0);

  param.firstViewPanel = password ? OSK_PANEL_TYPE_PASSWORD :
    OSK_PANEL_TYPE_DEFAULT;
  param.allowedPanels = param.firstViewPanel;
  param.prohibitFlags = OSK_PROHIBIT_RETURN;

  ifi.message = (u16 *)title16;
  ifi.startText = (u16 *)input16;
  ifi.maxLength = 256;

  /* Allocate a dedicated 2MB memory container for the OSK utility */
  if(sysMemContainerCreate(&gp->osk_container, 2 * 1024 * 1024))
    gp->osk_container = 0xFFFFFFFFU;

  oskSetKeyLayoutOption(3);

  int ret = oskLoadAsync(gp->osk_container, &param, &ifi);

  if(!ret) {
    gp->osk_widget = w;
    glw_ref(w);
  }

  free(title16);
  free(input16);
}


/**
 * @brief Dispatch system utility and XMB event notifications.
 *
 * @param status Event identifier representing OSK, XMB, or application termination state.
 * @param param Event-specific auxiliary payload data.
 * @param userdata Pointer to user state context (glw_ps3_t).
 *
 * Time Complexity: O(1) state branch evaluation.
 * Space Complexity: O(1).
 */
static void 
eventHandle(u64 status, u64 param, void *userdata) 
{
  glw_ps3_t *gp = userdata;
  switch(status) {
  case 0x11:
    TRACE(TRACE_INFO, "XMB", "Got close request from XMB");
    break;
  case SYSUTIL_EXIT_GAME:
    gp->gp_stop = 1;
    break;
  case SYSUTIL_MENU_OPEN:
    TRACE(TRACE_INFO, "XMB", "Opened");
    media_global_hold(1, MP_HOLD_OS);
    break;
  case SYSUTIL_MENU_CLOSE:
    TRACE(TRACE_INFO, "XMB", "Closed");
    media_global_hold(0, MP_HOLD_OS);
    break;
  case SYSUTIL_DRAW_BEGIN:
    break;
  case SYSUTIL_DRAW_END:
    break;
  case SYSUTIL_OSK_LOADED: 
    TRACE(TRACE_DEBUG, "OSK", "Loaded");
    break;
  case SYSUTIL_OSK_DONE: 
    TRACE(TRACE_DEBUG, "OSK", "Finished");
    osk_returned(gp);
    break;
  case SYSUTIL_OSK_UNLOADED:
    TRACE(TRACE_DEBUG, "OSK", "Unloaded");
    osk_destroyed(gp);
    break;
  case SYSUTIL_OSK_INPUT_CANCELED:
    TRACE(TRACE_DEBUG, "OSK", "Input canceled");
    oskAbort();
    break;

  default:
    TRACE(TRACE_DEBUG, "LV2", "Unhandled event 0x%lx", status);
    break;
  }
}


/**
 * @brief Bind hardware color buffer and depth-stencil surface for rendering.
 *
 * @param gp Pointer to PS3 UI state context.
 * @param currentBuffer Framebuffer index (0 or 1) targeted for rasterization.
 *
 * Invariant Rationale:
 * RSX hardware rasterizer expects a populated gcmSurface descriptor with linear
 * pitch, 32-bit X8R8G8B8 color target, and 16-bit ZETA depth configuration.
 *
 * Time Complexity: O(1) fixed-size hardware command generation.
 * Space Complexity: O(1) stack allocation for gcmSurface.
 */
static void
setupRenderTarget(glw_ps3_t *gp, u32 currentBuffer) 
{
  gcmSurface sf;
  memset(&sf, 0, sizeof(gcmSurface));

  /* Configure surface target geometry and color formats */
  sf.type = GCM_SURFACE_TYPE_LINEAR;
  sf.antiAlias = GCM_SURFACE_CENTER_1;
  sf.colorFormat = GCM_SURFACE_X8R8G8B8;
  sf.colorTarget = GCM_SURFACE_TARGET_0;

  /* Color target 0: RSX local GDDR3 VRAM offset and line stride */
  sf.colorLocation[0] = GCM_LOCATION_RSX;
  sf.colorOffset[0] = gp->framebuffer[currentBuffer];
  sf.colorPitch[0] = gp->framebuffer_pitch;

  /* Multiple Render Targets (MRT 1-3): Set unused dummy slots to RSX memory */
  sf.colorLocation[1] = GCM_LOCATION_RSX;
  sf.colorLocation[2] = GCM_LOCATION_RSX;
  sf.colorLocation[3] = GCM_LOCATION_RSX;
  sf.colorPitch[1] = 64;
  sf.colorPitch[2] = 64;
  sf.colorPitch[3] = 64;

  /* Depth buffer: 24-bit depth / 8-bit stencil format (Z24S8 matching 4 bytes/pixel depthbuffer_pitch) */
  sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
  sf.depthLocation = GCM_LOCATION_RSX;
  sf.depthOffset = gp->depthbuffer;
  sf.depthPitch = gp->depthbuffer_pitch;

  /* Dimensions and viewport origin */
  sf.width = gp->res.width;
  sf.height = gp->res.height;
  sf.x = 0;
  sf.y = 0;

  /* Enqueue render target swap and disable hardware depth test for 2D UI */
  rsxSetSurface(gp->gr.gr_be.be_ctx, &sf);
  rsxSetDepthTestEnable(gp->gr.gr_be.be_ctx, 0);
  rsxSetDepthWriteEnable(gp->gr.gr_be.be_ctx, 0);
}

/**
 * @brief Render single frame into offscreen backbuffer with viewport setup & clearing.
 *
 * @param gp Pointer to PS3 UI state context.
 * @param buffer Target display backbuffer index.
 * @param with_universe Boolean flag indicating whether widget tree layout & draw executes.
 *
 * Mathematical Viewport Transform:
 * Viewport scale is derived such that NDC coordinates [-1, 1] map to pixel space [0, W] and [0, H]:
 *   scale = (width * 0.5, -height * 0.5, 1.0, 0.0) [inverted Y for screen-space downward coords]
 *   offset = (width * 0.5, height * 0.5, 0.0, 0.0)
 *
 * Time Complexity: O(N) where N is the number of active layout widgets in the universe.
 * Space Complexity: O(1) rendering state overhead.
 */
static void 
drawFrame(glw_ps3_t *gp, int buffer, int with_universe)
{
  extern int browser_visible;

  gcmContextData *ctx = gp->gr.gr_be.be_ctx;

  /* First bind the render target surface to establish surface dimensions and pixel formats */
  setupRenderTarget(gp, buffer);

  /* Diagnostic trace verifying continuous active rasterization */
  static int frame_count = 0;
  if(frame_count++ < 5 || (frame_count % 300) == 0) {
    TRACE(TRACE_DEBUG, "GLW", "drawFrame #%d on buffer %d (with_universe=%d)",
          frame_count, buffer, with_universe);
  }

  /* Setup viewport scale and translation vectors */
  f32 scale[4] = { gp->res.width * 0.5f, -gp->res.height * 0.5f, 1.0f, 0.0f };
  f32 offset[4] = { gp->res.width * 0.5f, gp->res.height * 0.5f, 0.0f, 0.0f };
  rsxSetViewport(ctx, 0, 0, gp->res.width, gp->res.height, 0.0f, 1.0f, scale, offset);
  rsxSetViewportClip(ctx, 0, gp->res.width, gp->res.height);

  /* Disable near/far clipping plane rejection and enable Z clamping.
   * Movian's 2D perspective matrix positions layout elements at negative Z depths.
   * Without disabling near/far culling (cullNearFar=0, zClampEnable=1, cullIgnoreW=1),
   * the RSX hardware rasterizer discards all primitives before fragment generation. */
  rsxSetZMinMaxControl(ctx, 0, 1, 1);

  /* Configure hardware alpha blending equation: (SrcRGB * SrcAlpha) + (DstRGB * (1 - SrcAlpha)) */
  rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ZERO);
  rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
  rsxSetBlendEnable(ctx, 1);

  /* Set clear color to solid black and clear depth-stencil buffer (24-bit depth 0xffffff, 8-bit stencil 0x00) */
  rsxSetClearColor(ctx, 0x00000000);
  rsxSetClearDepthStencil(ctx, 0xffffff00);

  /* Issue clear command for RGBA, Z depth, and stencil buffers */
  rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);

  /* Reset cached shader pointers in case XMB overlay altered GPU registers */
  gp->gr.gr_be.be_vp_current = NULL;
  gp->gr.gr_be.be_fp_current = NULL;

  /* Disable backface culling for 2D UI elements to prevent quad winding drops */
  rsxSetCullFaceEnable(ctx, 0);

  if(!with_universe)
    return;

  glw_lock(&gp->gr);
  glw_prepare_frame(&gp->gr, 0);

  gp->gr.gr_width = gp->res.width;
  gp->gr.gr_height = gp->res.height;

  glw_rctx_t rc;
  int zmax = 0;
  glw_rctx_init(&rc, gp->gr.gr_width * gp->scale, gp->gr.gr_height, 1, &zmax);

  glw_lp(&gp->gp_browser_alpha, &gp->gr, browser_visible, 0.1);

  rc.rc_alpha = 1 - gp->gp_stop * 0.1 - gp->gp_browser_alpha * 0.8;

  glw_layout0(gp->gr.gr_universe, &rc);
  glw_render0(gp->gr.gr_universe, &rc);
  glw_unlock(&gp->gr);
  glw_post_scene(&gp->gr);
}


/**
 *
 */
typedef enum {
  BTN_LEFT = 1,
  BTN_UP,
  BTN_RIGHT,
  BTN_DOWN,
  BTN_CROSS,
  BTN_CIRCLE,
  BTN_TRIANGLE,
  BTN_SQUARE,
  BTN_L1,
  BTN_L2,
  BTN_L3,
  BTN_R1,
  BTN_R2,
  BTN_R3,
  BTN_START,
  BTN_KEY_1,
  BTN_KEY_2,
  BTN_KEY_3,
  BTN_KEY_4,
  BTN_KEY_5,
  BTN_KEY_6,
  BTN_KEY_7,
  BTN_KEY_8,
  BTN_KEY_9,
  BTN_KEY_0,
  BTN_ENTER,
  BTN_RETURN,
  BTN_CLEAR,
  BTN_EJECT,
  BTN_TOPMENU,
  BTN_TIME,
  BTN_PREV,
  BTN_NEXT,
  BTN_PLAY,
  BTN_SCAN_REV,
  BTN_SCAN_FWD,
  BTN_STOP,
  BTN_PAUSE,
  BTN_POPUP_MENU,
  BTN_SLOW_REV,
  BTN_SLOW_FWD,
  BTN_SUBTITLE,
  BTN_AUDIO,
  BTN_ANGLE,
  BTN_DISPLAY,
  BTN_BLUE,
  BTN_RED,
  BTN_GREEN,
  BTN_YELLOW,
  BTN_max
} buttoncode_t;

#define AVEC(x...) (const action_type_t []){x, ACTION_NONE}
const static action_type_t *btn_to_action[BTN_max] = {
  [BTN_LEFT]       = AVEC(ACTION_LEFT),
  [BTN_UP]         = AVEC(ACTION_UP),
  [BTN_RIGHT]      = AVEC(ACTION_RIGHT),
  [BTN_DOWN]       = AVEC(ACTION_DOWN),
  [BTN_CROSS]      = AVEC(ACTION_ACTIVATE),
  [BTN_CIRCLE]     = AVEC(ACTION_NAV_BACK),
  [BTN_TRIANGLE]   = AVEC(ACTION_MENU),
  [BTN_SQUARE]     = AVEC(ACTION_ITEMMENU, ACTION_SHOW_MEDIA_STATS),

  [BTN_L1]         = AVEC(ACTION_SKIP_BACKWARD),
  [BTN_R1]         = AVEC(ACTION_SKIP_FORWARD),

  [BTN_L3]         = AVEC(ACTION_SYSINFO),
  [BTN_R3]         = AVEC(ACTION_LOGWINDOW),
  [BTN_START]      = AVEC(ACTION_PLAYPAUSE),
  [BTN_ENTER]      = AVEC(ACTION_ACTIVATE, ACTION_ENTER),
  [BTN_RETURN]     = AVEC(ACTION_NAV_BACK),
  [BTN_EJECT]      = AVEC(ACTION_EJECT),
  [BTN_TOPMENU]    = AVEC(ACTION_HOME),
  [BTN_PREV]       = AVEC(ACTION_SKIP_BACKWARD),
  [BTN_NEXT]       = AVEC(ACTION_SKIP_FORWARD),
  [BTN_PLAY]       = AVEC(ACTION_PLAY),
  [BTN_SCAN_REV]   = AVEC(ACTION_SEEK_BACKWARD),
  [BTN_SCAN_FWD]   = AVEC(ACTION_SEEK_FORWARD),
  [BTN_STOP]       = AVEC(ACTION_STOP),
  [BTN_PAUSE]      = AVEC(ACTION_PAUSE),
  [BTN_POPUP_MENU] = AVEC(ACTION_MENU),
  [BTN_SUBTITLE]   = AVEC(ACTION_CYCLE_SUBTITLE),
  [BTN_AUDIO]      = AVEC(ACTION_CYCLE_AUDIO),
};


const static action_type_t *btn_to_action_sel[BTN_max] = {
  [BTN_LEFT]       = AVEC(ACTION_MOVE_LEFT),
  [BTN_UP]         = AVEC(ACTION_MOVE_UP),
  [BTN_RIGHT]      = AVEC(ACTION_MOVE_RIGHT),
  [BTN_DOWN]       = AVEC(ACTION_MOVE_DOWN),
  [BTN_TRIANGLE]   = AVEC(ACTION_SWITCH_VIEW),
  [BTN_CIRCLE]     = AVEC(ACTION_STOP),
  [BTN_START]      = AVEC(ACTION_PLAYQUEUE),
  [BTN_SQUARE]     = AVEC(ACTION_ENABLE_SCREENSAVER),
};




static int16_t button_counter[MAX_PADS][BTN_max];
static PadData paddata[MAX_PADS];


static void
clear_btns(void)
{
  memset(button_counter, 0, sizeof(button_counter));
}

#define KEY_REPEAT_DELAY 30 // in frames
#define KEY_REPEAT_RATE  3  // in frames


static void
handle_seek(glw_ps3_t *gp, int pad, int sign, int pressed, int pre)
{
  if(pressed && pre > 10) {
    event_t *e = event_create_int3(EVENT_DELTA_SEEK_REL, pre, sign, 
				   gp->gr.gr_framerate);
    event_dispatch(e);
  }
}



static void
handle_btn(glw_ps3_t *gp, int pad, int code, int pressed, int sel, int pre)
{
  if(gp->button_assign == 0) {
    // Swap X and O
    if(code == BTN_CROSS)
      code = BTN_CIRCLE;
    else if(code == BTN_CIRCLE)
      code = BTN_CROSS;
  }

  int16_t *store = &button_counter[pad][code];
  int rate = KEY_REPEAT_RATE;
  int xrep = 0;
  if(code == 0)
    return;
  
  if(pressed) {

    if(pre > 200 && *store > KEY_REPEAT_DELAY)
      xrep = 1;
    if(pre > 150)
      rate = 1;
    else if(pre > 100)
      rate = 2;

    if(*store == 0 ||
       (*store > KEY_REPEAT_DELAY && (*store % rate == 0))) {
      int uc = 0;
      event_t *e = NULL;

      if(code >= BTN_KEY_1 && code <= BTN_KEY_9) {
	uc = code - BTN_KEY_1 + '1';
      } else if(code == BTN_KEY_0) {
	uc = '0';
      }
      
      if(uc != 0)
	e = event_create_int(EVENT_UNICODE, uc);

      if(e == NULL) {
	const action_type_t *avec =
	  sel ? btn_to_action_sel[code] : btn_to_action[code];
	if(avec) {
	  int i = 0;
	  while(avec[i] != 0)
	    i++;
	  e = event_create_action_multi(avec, i);
	}
      }

      if(e != NULL) {
        e->e_flags |= EVENT_KEYPRESS;
	event_addref(e);
	event_to_ui(e);

	if(xrep) {
	  event_addref(e);
	  event_to_ui(e);
	}

	event_release(e);
      }
    }
    (*store)++;
  } else {
    *store = 0;
  }
}





static const uint8_t bd_to_local_map[256] = {
  [BTN_BD_KEY_1] = BTN_KEY_1,
  [BTN_BD_KEY_2] = BTN_KEY_2,
  [BTN_BD_KEY_3] = BTN_KEY_3,
  [BTN_BD_KEY_4] = BTN_KEY_4,
  [BTN_BD_KEY_5] = BTN_KEY_5,
  [BTN_BD_KEY_6] = BTN_KEY_6,
  [BTN_BD_KEY_7] = BTN_KEY_7,
  [BTN_BD_KEY_8] = BTN_KEY_8,
  [BTN_BD_KEY_9] = BTN_KEY_9,
  [BTN_BD_KEY_0] = BTN_KEY_0,
  [BTN_BD_ENTER] = BTN_ENTER,
  [BTN_BD_RETURN] = BTN_RETURN,
  [BTN_BD_CLEAR] = BTN_CLEAR,
  [BTN_BD_EJECT] = BTN_EJECT,
  [BTN_BD_TOPMENU] = BTN_TOPMENU,
  [BTN_BD_TIME] = BTN_TIME,
  [BTN_BD_PREV] = BTN_PREV,
  [BTN_BD_NEXT] = BTN_NEXT,
  [BTN_BD_PLAY] = BTN_PLAY,
  [BTN_BD_SCAN_REV] = BTN_SCAN_REV,
  [BTN_BD_SCAN_FWD] = BTN_SCAN_FWD,
  [BTN_BD_STOP] = BTN_STOP,
  [BTN_BD_PAUSE] = BTN_PAUSE,
  [BTN_BD_POPUP_MENU] = BTN_POPUP_MENU,
  [BTN_BD_L3] = BTN_L3,
  [BTN_BD_R3] = BTN_R3,
  [BTN_BD_START] = BTN_START,
  [BTN_BD_UP] = BTN_UP,
  [BTN_BD_RIGHT] = BTN_RIGHT,
  [BTN_BD_DOWN] = BTN_DOWN,
  [BTN_BD_LEFT] = BTN_LEFT,
  [BTN_BD_L2] = BTN_L2,
  [BTN_BD_R2] = BTN_R2,
  [BTN_BD_L1] = BTN_L1,
  [BTN_BD_R1] = BTN_R1,
  [BTN_BD_TRIANGLE] = BTN_TRIANGLE,
  [BTN_BD_CIRCLE] = BTN_CIRCLE,
  [BTN_BD_CROSS] = BTN_CROSS,
  [BTN_BD_SQUARE] = BTN_SQUARE,
  [BTN_BD_SLOW_REV] = BTN_SLOW_REV,
  [BTN_BD_SLOW_FWD] = BTN_SLOW_FWD,
  [BTN_BD_SUBTITLE] = BTN_SUBTITLE,
  [BTN_BD_AUDIO] = BTN_AUDIO,
  [BTN_BD_ANGLE] = BTN_ANGLE,
  [BTN_BD_DISPLAY] = BTN_DISPLAY,
  [BTN_BD_BLUE] = BTN_BLUE,
  [BTN_BD_RED] = BTN_RED,
  [BTN_BD_GREEN] = BTN_GREEN,
  [BTN_BD_YELLOW] = BTN_YELLOW,
};


static uint16_t remote_last_btn[MAX_PADS] =
  {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/**
 *
 */
static void
handle_pads(glw_ps3_t *gp)
{
  PadInfo2 padinfo2;
  int i;

  if(gp->osk_widget) {
    clear_btns();
    return;
  }

  // Check the pads.
  ioPadGetInfo2(&padinfo2);
  for(i=0; i<7; i++){
    if(!padinfo2.port_status[i])
      continue;

    if(padinfo2.device_type[i] == 4) {
      uint32_t type = 4;
      int r = ioPadGetDataExtra(i, &type, &paddata[i]);

      if(r == 0) {
	int btn = paddata[i].button[25];
	if(btn != remote_last_btn[i]) {
	  if(remote_last_btn[i] < 0xff)
	    handle_btn(gp, i, bd_to_local_map[remote_last_btn[i]], 0, 0, 0);
	  remote_last_btn[i] = btn;
	}

	if(btn != 0xff)
	  handle_btn(gp, i, bd_to_local_map[btn], 1, 0, 0);
      }
      continue;
    }


    ioPadGetData(i, &paddata[i]);
    PadData *pd = &paddata[i];
    int sel = !!pd->BTN_SELECT;
    handle_btn(gp, i, BTN_LEFT,     pd->BTN_LEFT,     sel, pd->PRE_LEFT);
    handle_btn(gp, i, BTN_UP,       pd->BTN_UP,       sel, pd->PRE_UP);
    handle_btn(gp, i, BTN_RIGHT,    pd->BTN_RIGHT,    sel, pd->PRE_RIGHT);
    handle_btn(gp, i, BTN_DOWN,     pd->BTN_DOWN,     sel, pd->PRE_DOWN);
    handle_btn(gp, i, BTN_CROSS,    pd->BTN_CROSS,    sel, pd->PRE_CROSS);
    handle_btn(gp, i, BTN_CIRCLE,   pd->BTN_CIRCLE,   sel, pd->PRE_CIRCLE);
    handle_btn(gp, i, BTN_TRIANGLE, pd->BTN_TRIANGLE, sel, pd->PRE_TRIANGLE);
    handle_btn(gp, i, BTN_SQUARE,   pd->BTN_SQUARE,   sel, pd->PRE_SQUARE);
    handle_btn(gp, i, BTN_START,    pd->BTN_START,    sel, 0);
    handle_btn(gp, i, BTN_R1,       pd->BTN_R1,       sel, pd->PRE_R1);
    handle_btn(gp, i, BTN_L1,       pd->BTN_L1,       sel, pd->PRE_L1);
    handle_btn(gp, i, BTN_R3,       pd->BTN_R3,       sel, 0);
    handle_btn(gp, i, BTN_L3,       pd->BTN_L3,       sel, 0);


    if(gp->gp_seekmode == 0 || (gp->gp_seekmode == 1 && sel)) {
      handle_seek(gp, i, 1,        pd->BTN_R2,       pd->PRE_R2);
      handle_seek(gp, i, -1,       pd->BTN_L2,       pd->PRE_L2);
    }
  }
}


#define KB_SHIFTMASK 0x1
#define KB_ALTMASK   0x2
#define KB_CTRLMASK  0x4

/**
 *
 */
static const struct {
  int code;
  int modifier;
  const char *sym;
  int action1;
  int action2;
  int action3;
} kb2action[] = {

  { KB_RAWDAT|KB_RAWKEY_LEFT_ARROW,         0,      NULL, ACTION_LEFT },
  { KB_RAWDAT|KB_RAWKEY_RIGHT_ARROW,        0,      NULL, ACTION_RIGHT },
  { KB_RAWDAT|KB_RAWKEY_UP_ARROW,           0,      NULL, ACTION_UP },
  { KB_RAWDAT|KB_RAWKEY_DOWN_ARROW,         0,      NULL, ACTION_DOWN },

  { 9,     0,             NULL, ACTION_FOCUS_NEXT },
  { 9,     KB_SHIFTMASK,  NULL, ACTION_FOCUS_PREV },
  { 8,     0,             NULL, ACTION_BS, ACTION_NAV_BACK },
  { 10,    0,             NULL, ACTION_ACTIVATE, ACTION_ENTER },
  { 27,    0,             NULL, ACTION_CANCEL },

  { KB_RAWDAT|KB_RAWKEY_F1, -1,   "F1" },
  { KB_RAWDAT|KB_RAWKEY_F2, -1,   "F2" },
  { KB_RAWDAT|KB_RAWKEY_F3, -1,   "F3" },
  { KB_RAWDAT|KB_RAWKEY_F4, -1,   "F4" },
  { KB_RAWDAT|KB_RAWKEY_F5, -1,   "F5" },
  { KB_RAWDAT|KB_RAWKEY_F6, -1,   "F6" },
  { KB_RAWDAT|KB_RAWKEY_F7, -1,   "F7" },
  { KB_RAWDAT|KB_RAWKEY_F8, -1,   "F8" },
  { KB_RAWDAT|KB_RAWKEY_F9, -1,   "F9" },
  { KB_RAWDAT|KB_RAWKEY_F10, -1,   "F10" },
  { KB_RAWDAT|KB_RAWKEY_F11, -1,   "F11" },
  { KB_RAWDAT|KB_RAWKEY_F12, -1,   "F12" },

  { KB_RAWDAT|KB_RAWKEY_PAGE_UP,   -1,   "Prior" },
  { KB_RAWDAT|KB_RAWKEY_PAGE_DOWN, -1,   "Next" },
  { KB_RAWDAT|KB_RAWKEY_HOME,      -1,   "Home" },
  { KB_RAWDAT|KB_RAWKEY_END,       -1,   "End" },

  { KB_RAWDAT|KB_RAWKEY_LEFT_ARROW,  -1,   "Left" },
  { KB_RAWDAT|KB_RAWKEY_RIGHT_ARROW, -1,   "Right" },
  { KB_RAWDAT|KB_RAWKEY_UP_ARROW,    -1,   "Up" },
  { KB_RAWDAT|KB_RAWKEY_DOWN_ARROW,  -1,   "Down" },
};


/**
 *
 */
static void
handle_kb(glw_ps3_t *gp)
{
  KbInfo kbinfo;
  KbData kbdata;
  int i, j;
  int uc;
  event_t *e;
  action_type_t av[3];
  int mods;

  if(ioKbGetInfo(&kbinfo))
    return;

  for(i=0; i<MAX_KEYBOARDS; i++) {
    if(kbinfo.status[i] == 0) {
      if(gp->kb_present[i])
	TRACE(TRACE_INFO, "PS3", "Keyboard %d disconnected", i);

    } else {
      if(!gp->kb_present[i]) {

	ioKbGetConfiguration(i, &gp->kb_config[i]);

	TRACE(TRACE_INFO, "PS3",
	      "Keyboard %d connected, mapping=%d, rmode=%d, codetype=%d",
	      i, gp->kb_config[i].mapping, gp->kb_config[i].rmode,
	      gp->kb_config[i].codetype);

	ioKbSetCodeType(i, KB_CODETYPE_RAW);
      }

      if(!ioKbRead(i, &kbdata)) {
	for(j = 0; j < kbdata.nb_keycode; j++) {

	  if(0) TRACE(TRACE_DEBUG, "PS3", "Keystrike %x %x %x %x",
		      gp->kb_config[i].mapping,
		      kbdata.mkey._KbMkeyU.mkeys,
		      kbdata.led._KbLedU.leds,
		      kbdata.keycode[j]);

	  uc = ioKbCnvRawCode(gp->kb_config[i].mapping, kbdata.mkey,
			      kbdata.led, kbdata.keycode[j]);

	  mods = 0;
	  if(kbdata.mkey._KbMkeyU._KbMkeyS.l_shift || kbdata.mkey._KbMkeyU._KbMkeyS.r_shift)
	    mods |= KB_SHIFTMASK;
	  if(kbdata.mkey._KbMkeyU._KbMkeyS.l_alt || kbdata.mkey._KbMkeyU._KbMkeyS.r_alt)
	    mods |= KB_ALTMASK;
	  if(kbdata.mkey._KbMkeyU._KbMkeyS.l_ctrl || kbdata.mkey._KbMkeyU._KbMkeyS.r_ctrl)
	    mods |= KB_CTRLMASK;

	  for(i = 0; i < sizeof(kb2action) / sizeof(*kb2action); i++) {
	    if(kb2action[i].code == uc &&
	       (kb2action[i].modifier == -1 || kb2action[i].modifier == mods)) {

	      av[0] = kb2action[i].action1;
	      av[1] = kb2action[i].action2;
	      av[2] = kb2action[i].action3;

	      if(kb2action[i].action3 != ACTION_NONE)
		e = event_create_action_multi(av, 3);
	      else if(kb2action[i].action2 != ACTION_NONE)
		e = event_create_action_multi(av, 2);
	      else if(kb2action[i].action1 != ACTION_NONE)
		e = event_create_action_multi(av, 1);
	      else if(kb2action[i].sym != NULL) {
		char buf[128];

		snprintf(buf, sizeof(buf),
			 "%s%s%s%s",
			 mods & KB_SHIFTMASK   ? "Shift+" : "",
			 mods & KB_ALTMASK     ? "Alt+"   : "",
			 mods & KB_CTRLMASK    ? "Ctrl+"  : "",
			 kb2action[i].sym);
		e = event_create_str(EVENT_KEYDESC, buf);
	      } else {
		e = NULL;
	      }

	      if(e != NULL) {
                e->e_flags |= EVENT_KEYPRESS;
		event_to_ui(e);
		break;
	      }
	    }
	  }

	  if(i == sizeof(kb2action) / sizeof(*kb2action) && uc < 0x8000 && uc) {
	    e = event_create_int(EVENT_UNICODE, uc);
	    event_to_ui(e);
	  }
	}
      }
    }
    gp->kb_present[i] = kbinfo.status[i];
  }

}


/**
 * @brief Core render, input dispatch, and event pump loop for PS3 user interface.
 *
 * Implements non-blocking first-frame flip status priming followed by double-buffered
 * frame rasterization and VSYNC pacing. Coordinates Sixaxis/DS3 controller polling,
 * USB keyboard input dispatch, and PSL1GHT v2 system utility callback processing.
 *
 * @param gp Pointer to active PS3 UI context state.
 *
 * Algorithmic & Timing Invariants:
 * - On the first frame (first_fb == 1), gcmResetFlipStatus() primes the display register
 *   without blocking in waitFlip(), allowing drawFrame() to queue the initial rasterization commands.
 * - On subsequent frames (first_fb == 0), waitFlip() ensures hardware VSYNC display scanout
 *   synchronization before next frame rasterization begins.
 *
 * Complexity:
 * - Time Complexity: O(F * N) where F is the number of rendered frames and N is widget tree size.
 * - Space Complexity: O(1) stack overhead.
 */
static void
glw_ps3_mainloop(glw_ps3_t *gp)
{
  int currentBuffer = 0;
  TRACE(TRACE_DEBUG, "GLW", "Entering mainloop");

  /* Reset first-frame guard flag to ensure proper initial flip initialization */
  first_fb = 1;

  /* Register PSL1GHT v2 system utility callback */
  sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, eventHandle, gp);
  while(gp->gp_stop != 10) {

    if(gp->gp_stop)
      gp->gp_stop++;

    handle_pads(gp);
    handle_kb(gp);

    /* Render and flip single frame; flip() handles VSYNC synchronization and command buffer reset */
    drawFrame(gp, currentBuffer, 1);
    flip(gp, currentBuffer);
    currentBuffer = !currentBuffer;
    sysUtilCheckCallback();
  }

  /* Render final blank frame on shutdown */
  drawFrame(gp, currentBuffer, 0);
  flip(gp, currentBuffer);
  currentBuffer = !currentBuffer;

  /* Unregister callback before teardown */
  sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
}


static void
set_seekmode(void *opaque, const char *str)
{
  glw_ps3_t *gp = opaque;
  gp->gp_seekmode = atoi(str);
}


int glw_ps3_start(void);

/**
 *
 */
int
glw_ps3_start(void)
{
  glw_ps3_t *gp = calloc(1, sizeof(glw_ps3_t));
  glwps3 = gp;
  prop_t *root = gp->gr.gr_prop_ui = prop_create(prop_get_global(), "ui");
  gp->gr.gr_prop_nav = nav_spawn();

  prop_set_int(prop_create(root, "fullscreen"), 1);

  if(glw_ps3_init(gp))
     return 1;

  gp->gr.gr_prop_maxtime = 10000;

  glw_root_t *gr = &gp->gr;

  if(glw_init2(gr,
               GLW_INIT_KEYBOARD_MODE |
               GLW_INIT_OVERSCAN |
               GLW_INIT_IN_FULLSCREEN))
    return 1;

  settings_create_separator(glw_settings.gs_settings, _p("Dual-Shock Remote"));

  setting_create(SETTING_MULTIOPT, glw_settings.gs_settings,
                 SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Seek using L2 and R2 button")),
                 SETTING_OPTION("0", _p("Yes")),
                 SETTING_OPTION("1", _p("Yes with Select button")),
                 SETTING_OPTION("2", _p("No")),
                 SETTING_COURIER(gr->gr_courier),
                 SETTING_CALLBACK(set_seekmode, gp),
                 SETTING_STORE("glw", "analogseekmode"),
                 NULL);

  gr->gr_open_osk = osk_open;

  TRACE(TRACE_DEBUG, "GLW", "loading universe");

  glw_load_universe(gr);
  glw_ps3_mainloop(gp);
  glw_unload_universe(gr);
  glw_reap(gr);
  glw_reap(gr);
  return 0;
}

int
arch_stop_req(void)
{
  glwps3->gp_stop = 1;
  return 0;
}
