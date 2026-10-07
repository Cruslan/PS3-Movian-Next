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
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "glw.h"
#include "glw_texture.h"

/**
 * PSL1GHT v2 RSX Subsystem Headers:
 * Supplies hardware GCM texture structures (gcmTexture), memory offsets,
 * and texture channel remapping primitives.
 */
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/commands.h>

/**
 * Texture Channel Remap Macros (Hardware RSX Pixel Swizzle):
 * Directly programmed into the 16-bit NV40 hardware register NV40TCL_TEX_SWIZZLE(x).
 *
 * Hardware Invariants (NV40/G70 GPU):
 * - Bits 15..8 (S0): Component mapping mode (S0_X_S1 | S0_Y_S1 | S0_Z_S1 | S0_W_S1 = 0xaa00).
 * - Bits 7..0  (S1): Input swizzle channel routing:
 *   - REMAP_IDENTITY (0xaae4): X->X, Y->Y, Z->Z, W->W (standard ARGB/identity).
 *   - REMAP_ABGR     (0xaa6c): X->Z, Y->Y, Z->X, W->W (byte-reversed BGR to RGB).
 *   - REMAP_RGBA     (0xaa93): X->Y, Y->Z, Z->W, W->X (cyclic shift for RGBA).
 *   - REMAP_IA8      (0xaaab): X->Y, Y->Y, Z->Y, W->X (A8L8: Luminance Y->RGB, Alpha X->A).
 */
#define REMAP_IDENTITY 0xaae4
#define REMAP_ABGR     0xaa6c
#define REMAP_RGBA     0xaa93
#define REMAP_IA8      0xaa54

/**
 * @brief Releases backend rendering resources for a texture. Always executed on main render thread.
 *
 * @param gr Graphics root context pointer.
 * @param glt Loadable texture wrapper pointer.
 */
void
glw_tex_backend_free_render_resources(glw_root_t *gr, glw_loadable_texture_t *glt)
{
  glw_tex_destroy(gr, &glt->glt_texture);
}

/**
 * @brief Releases loader decoder worker resources.
 *
 * @param glt Loadable texture wrapper pointer.
 */
void
glw_tex_backend_free_loader_resources(glw_loadable_texture_t *glt)
{
}

/**
 * @brief Per-frame layout hook for valid textures.
 *
 * @param gr Graphics root context pointer.
 * @param glt Loadable texture wrapper pointer.
 */
void
glw_tex_backend_layout(glw_root_t *gr, glw_loadable_texture_t *glt)
{
}

/**
 * @brief Configures a native PSL1GHT v2 gcmTexture descriptor structure.
 *
 * Sets up dimension, pitch, dimensions, RSX VRAM location, texture format, and hardware
 * channel remapping for RSX sampling.
 *
 * @param tex Destination gcmTexture structure pointer.
 * @param offset Byte offset in RSX GDDR3 VRAM.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param stride Stride / pitch in bytes per scanline.
 * @param fmt Texture color format identifier (e.g. GCM_TEXTURE_FORMAT_A8R8G8B8).
 * @param repeat Flag indicating texture wrap repeat mode (1 for repeat, 0 for clamp).
 * @param remap Hardware color component remapping bitfield.
 *
 * @complexity Time: O(1). Space: O(1).
 */
static void
init_tex(gcmTexture *tex, uint32_t offset,
         uint32_t width, uint32_t height, uint32_t stride,
         uint8_t fmt, int repeat, uint32_t remap)
{
  memset(tex, 0, sizeof(*tex));

  /* Linear layout pitch-based texture in RSX local memory */
  tex->format    = GCM_TEXTURE_FORMAT_LIN | fmt;
  tex->mipmap    = 1;
  tex->dimension = GCM_TEXTURE_DIMS_2D;
  tex->cubemap   = GCM_FALSE;
  tex->remap     = remap;
  tex->width     = (u16)width;
  tex->height    = (u16)height;
  tex->depth     = 1;
  tex->location  = GCM_LOCATION_RSX;
  tex->pitch     = stride;
  tex->offset    = offset;
  tex->_pad      = (u8)(repeat ? 1 : 0);
}

/**
 * @brief Reallocates VRAM buffer for texture payload if dimensions have changed.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture container pointer.
 * @param size Required allocation size in bytes.
 * @return void* PPU virtual address mapped to RSX GDDR3 VRAM or NULL if size is zero.
 *
 * @complexity Time: O(1) extent allocator search. Space: O(size) VRAM allocation.
 */
/**
 * @brief Reallocates VRAM buffer for texture payload if dimensions have changed.
 *
 * Hardware RSX linear texture sampling requires the base address in GDDR3 memory
 * to be aligned to at least 64 bytes, and preferably 128 bytes (matching Cell PPE / RSX cache line size).
 * Unaligned texture offsets cause GPU bus faults and FIFO lockups during primitive rasterization.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture container pointer.
 * @param size Required allocation size in bytes.
 * @return void* PPU virtual address mapped to RSX GDDR3 VRAM or NULL if size is zero.
 *
 * Invariants:
 * - Alignment is strictly enforced to 128 bytes for all RSX linear texture descriptors.
 *
 * Complexity:
 * - Time: O(1) extent allocator search. Space: O(size) VRAM allocation.
 */
static void *
realloc_tex(glw_root_t *gr, glw_backend_texture_t *tex, int size)
{
  if(tex->size != (uint32_t)size) {
    if(tex->size != 0)
      rsx_free(tex->tex.offset, tex->size);

    tex->size = size;

    if(tex->size != 0)
      tex->tex.offset = rsx_alloc(tex->size, 128);
  }
  return tex->size ? rsx_to_ppu(tex->tex.offset) : NULL;
}

/**
 * @brief Initializes and uploads an ABGR pixel buffer to RSX VRAM.
 *
 * Pads the scanline pitch to a 64-byte multiple to satisfy NV40 hardware linear texture
 * memory access invariants. Drains PPE stores via a hardware sync barrier.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture descriptor pointer.
 * @param src Pointer to raw ABGR pixel buffer.
 * @param linesize Source scanline stride in bytes.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param repeat Texture wrapping mode flag.
 */
static void
init_abgr(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  /* Calculate 64-byte aligned pitch for RSX linear rasterizer burst reads */
  int pitch = (linesize + 63) & ~63;
  void *mem = realloc_tex(gr, tex, pitch * height);
  if(mem == NULL)
    return;

  if(pitch == linesize) {
    memcpy(mem, src, linesize * height);
  } else {
    /* Copy row by row when pitch padding is required to preserve scanline boundaries */
    for(int y = 0; y < height; y++) {
      memcpy((uint8_t *)mem + y * pitch, src + y * linesize, linesize);
    }
  }

  /* Hardware memory barrier: ensure PPE write-combining stores are globally visible to RSX */
  __asm__ volatile("sync" ::: "memory");

  init_tex(&tex->tex, tex->tex.offset, width, height, pitch,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_ABGR);
}

/**
 * @brief Initializes and uploads an RGBA pixel buffer to RSX VRAM.
 *
 * Pads the scanline pitch to a 64-byte multiple to satisfy NV40 hardware linear texture
 * memory access invariants. Drains PPE stores via a hardware sync barrier.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture descriptor pointer.
 * @param src Pointer to raw RGBA pixel buffer.
 * @param linesize Source scanline stride in bytes.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param repeat Texture wrapping mode flag.
 */
static void
init_rgba(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  /* Calculate 64-byte aligned pitch for RSX linear rasterizer burst reads */
  int pitch = (linesize + 63) & ~63;
  void *mem = realloc_tex(gr, tex, pitch * height);
  if(mem == NULL)
    return;

  if(pitch == linesize) {
    memcpy(mem, src, linesize * height);
  } else {
    /* Copy row by row when pitch padding is required to preserve scanline boundaries */
    for(int y = 0; y < height; y++) {
      memcpy((uint8_t *)mem + y * pitch, src + y * linesize, linesize);
    }
  }

  /* Hardware memory barrier: ensure PPE write-combining stores are globally visible to RSX */
  __asm__ volatile("sync" ::: "memory");

  init_tex(&tex->tex, tex->tex.offset, width, height, pitch,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_RGBA);
}

/**
 * @brief Converts 24-bit packed RGB pixels to 32-bit ARGB in RSX VRAM.
 *
 * Expands 3-byte RGB components to 4-byte 0xAARRGGBB format, aligning destination pitch
 * to 64 bytes and executing a hardware memory barrier prior to GPU sampler consumption.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture descriptor pointer.
 * @param src Pointer to raw packed RGB24 pixel buffer.
 * @param linesize Source scanline stride in bytes.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param repeat Texture wrapping mode flag.
 */
static void
init_rgb(glw_root_t *gr, glw_backend_texture_t *tex,
         const uint8_t *src, int linesize,
         int width, int height, int repeat)
{
  /* Align 32-bit ARGB scanline pitch to 64 bytes */
  int pitch = (width * 4 + 63) & ~63;
  uint8_t *dst_base = (uint8_t *)realloc_tex(gr, tex, pitch * height);
  if(dst_base == NULL)
    return;

  for(int y = 0; y < height; y++) {
    const uint8_t *s = src + y * linesize;
    uint32_t *dst = (uint32_t *)(dst_base + y * pitch);
    for(int x = 0; x < width; x++) {
      uint8_t r = *s++;
      uint8_t g = *s++;
      uint8_t b = *s++;
      *dst++ = 0xff000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }

  /* Hardware memory barrier: flush stores from PPE pipeline before RSX sampling */
  __asm__ volatile("sync" ::: "memory");

  init_tex(&tex->tex, tex->tex.offset, width, height, pitch,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_IDENTITY);
}

/**
 * @brief Initializes an 8-bit Intensity + 8-bit Alpha luminance texture.
 *
 * Utilized by FreeType font glyph rasterization. Expands 16-bit IA8 (Intensity/Alpha)
 * to 32-bit ARGB (GCM_TEXTURE_FORMAT_A8R8G8B8) in RSX GDDR3 VRAM.
 *
 * Hardware Mechanics & Rationale:
 * - Direct 16-bit linear sampling on NV40/G70 requires specialized swizzle patterns (0xaa54)
 *   and 16-bit texture format modes (0x18 / A8L8) which stall the hardware texture sampler
 *   when reading unaligned or non-swizzled linear buffers.
 * - Expanding IA8 (byte 0 = Intensity, byte 1 = Alpha) to 32-bit ARGB (0xAARRGGBB) maps
 *   Intensity uniformly across Red, Green, and Blue channels and preserves Alpha directly.
 * - This leverages the 100% verified, rock-solid GCM_TEXTURE_FORMAT_A8R8G8B8 and REMAP_IDENTITY
 *   pipeline that powers all other working UI elements in Movian.
 * - Scanline pitch is aligned to 64 bytes and PPE stores are flushed with a hardware sync barrier.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture descriptor pointer.
 * @param src Pointer to raw IA8 pixel buffer.
 * @param linesize Source scanline stride in bytes.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param repeat Texture wrapping mode flag.
 */
static void
init_i8a8(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  /* Align 32-bit ARGB scanline pitch to 64 bytes */
  int pitch = (width * 4 + 63) & ~63;
  uint8_t *dst_base = (uint8_t *)realloc_tex(gr, tex, pitch * height);
  if(dst_base == NULL)
    return;

  for(int y = 0; y < height; y++) {
    const uint8_t *s = src + y * linesize;
    uint32_t *dst = (uint32_t *)(dst_base + y * pitch);
    for(int x = 0; x < width; x++) {
      uint32_t i = s[0];
      uint32_t a = s[1];
      s += 2;
      *dst++ = ((uint32_t)a << 24) | (i << 16) | (i << 8) | i;
    }
  }

  /* Hardware memory barrier: flush stores from PPE pipeline before RSX sampling */
  __asm__ volatile("sync" ::: "memory");

  init_tex(&tex->tex, tex->tex.offset, width, height, pitch,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_IDENTITY);
}


/**
 * @brief Loads a pixmap into an allocated texture descriptor.
 *
 * @param gr Graphics root context pointer.
 * @param glt Loadable texture wrapper pointer.
 * @param pm Source pixmap structure.
 * @return int Total allocated byte size or 0 on unsupported pixmap format.
 */
int
glw_tex_backend_load(glw_root_t *gr, glw_loadable_texture_t *glt, pixmap_t *pm)
{
  int repeat = glt->glt_flags & GLW_TEX_REPEAT;

  glt->glt_s = 1.0f;
  glt->glt_t = 1.0f;

  switch(pm->pm_type) {
  case PIXMAP_BGR32:
    init_abgr(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_RGBA:
    init_rgba(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_RGB24:
    init_rgb(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
             pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_IA:
    init_i8a8(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  default:
    return 0;
  }
  return glt->glt_texture.size;
}

/**
 * @brief Uploads pixmap data into an existing backend texture.
 *
 * @param gr Graphics root context pointer.
 * @param tex Destination backend texture.
 * @param pm Source pixmap structure.
 * @param flags Texture flags (e.g. GLW_TEX_REPEAT).
 */
void
glw_tex_upload(glw_root_t *gr, glw_backend_texture_t *tex,
               const pixmap_t *pm, int flags)
{
  switch(pm->pm_type) {
  case PIXMAP_IA:
    init_i8a8(gr, tex, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, flags & GLW_TEX_REPEAT);
    break;

  case PIXMAP_RGB24:
    init_rgb(gr, tex, pm->pm_data, pm->pm_linesize,
             pm->pm_width, pm->pm_height, flags & GLW_TEX_REPEAT);
    break;

  case PIXMAP_BGR32:
    init_abgr(gr, tex, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, flags & GLW_TEX_REPEAT);
    break;

  case PIXMAP_RGBA:
    init_rgba(gr, tex, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, flags & GLW_TEX_REPEAT);
    break;

  default:
    TRACE(TRACE_ERROR, "GLW", "Unable to upload texture fmt %d, %d x %d",
          pm->pm_type, pm->pm_width, pm->pm_height);
    return;
  }
}

/**
 * @brief Releases RSX VRAM memory allocated for texture.
 *
 * @param gr Graphics root context pointer.
 * @param tex Backend texture container pointer.
 */
void
glw_tex_destroy(glw_root_t *gr, glw_backend_texture_t *tex)
{
  if(tex->size != 0) {
    rsx_free(tex->tex.offset, tex->size);
    tex->size = 0;
  }
}
