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
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "main.h"
#include "glw_video_common.h"

/**
 * PSL1GHT v2 RSX Subsystem Headers:
 * Hardware video surface rendering bindings for NV40/G70 GPU.
 */
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <rsx/commands.h>
#include <rsx/nv40.h>

#include "video/video_decoder.h"
#include "video/video_playback.h"


/**
 * gv_surface_mutex must be held
 */
/**
 * @brief Resets and releases RSX memory allocated for a video surface.
 *
 * This function releases any allocated RSX GDDR3 memory back to the RSX extent
 * memory pool if the offset is valid (> 0) and not shared by another active surface.
 * If offset is <= 0 (e.g. unallocated or previous allocation failure), it safely
 * clears the structure without invoking rsx_free on an invalid address.
 *
 * @param gv Pointer to GLW video context.
 * @param gvs Pointer to video surface structure to reset.
 *
 * @complexity Time: O(N) where N = GLW_VIDEO_MAX_SURFACES. Space: O(1).
 */
static void
surface_reset(glw_video_t *gv, glw_video_surface_t *gvs)
{
  int i;

  /* If surface has no valid RSX allocation, reset pointers and exit early */
  if(gvs->gvs_offset <= 0) {
    gvs->gvs_offset = 0;
    gvs->gvs_size = 0;
    gvs->gvs_data[0] = NULL;
    gvs->gvs_data[1] = NULL;
    gvs->gvs_data[2] = NULL;
    return;
  }

  /* Check if another surface shares the same RSX memory chunk (e.g. multi-view) */
  for(i = 0; i < GLW_VIDEO_MAX_SURFACES; i++) {
    if(gv->gv_surfaces[i].gvs_offset == gvs->gvs_offset &&
       gvs != &gv->gv_surfaces[i]) {
      /* Memory is shared with another surface; do not free pool block */
      gvs->gvs_offset = 0;
      gvs->gvs_size = 0;
      gvs->gvs_data[0] = NULL;
      gvs->gvs_data[1] = NULL;
      gvs->gvs_data[2] = NULL;
      return;
    }
  }

  /* Free the RSX GDDR3 extent pool block and nullify all pointers */
  if(gvs->gvs_offset > 0 && gvs->gvs_size > 0) {
    rsx_free(gvs->gvs_offset, gvs->gvs_size);
    gvs->gvs_offset = 0;
    gvs->gvs_size = 0;
    gvs->gvs_data[0] = NULL;
    gvs->gvs_data[1] = NULL;
    gvs->gvs_data[2] = NULL;
  }
}


/**
 *
 */
static void
yuvp_reset(glw_video_t *gv)
{
  int i;

  for(i = 0; i < GLW_VIDEO_MAX_SURFACES; i++)
    surface_reset(gv, &gv->gv_surfaces[i]);
}



/**
 * Luminance Texture Remap Macro:
 * Replicates single byte component into all shader vector channels (R, G, B, A).
 */
#define REMAP_L8 \
  GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, \
                         GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_COLOR_B, \
                         GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_COLOR_B, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

/**
 * @brief Configures an 8-bit planar YUV component texture descriptor.
 *
 * @param tex Destination gcmTexture structure pointer.
 * @param offset Byte offset in RSX GDDR3 VRAM.
 * @param width Plane width in pixels.
 * @param height Plane height in pixels.
 * @param stride Plane scanline pitch in bytes.
 *
 * @complexity Time: O(1). Space: O(1).
 */
static void
init_tex(gcmTexture *tex, uint32_t offset,
         uint32_t width, uint32_t height, uint32_t stride)
{
  memset(tex, 0, sizeof(*tex));
  tex->format    = GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_B8;
  tex->mipmap    = 1;
  tex->dimension = GCM_TEXTURE_DIMS_2D;
  tex->cubemap   = GCM_FALSE;
  tex->remap     = REMAP_L8;
  tex->width     = (u16)width;
  tex->height    = (u16)height;
  tex->depth     = 1;
  tex->location  = GCM_LOCATION_RSX;
  tex->pitch     = stride;
  tex->offset    = offset;
}

#define ROUND_UP(p, round) (((p) + (round) - 1) & ~((round) - 1))

/**
 * @brief Allocates and initializes RSX GDDR3 memory buffers for planar YUV video frames.
 *
 * @param gv GLW video context pointer.
 * @param gvs Video surface structure to populate.
 */
static void
surface_init(glw_video_t *gv, glw_video_surface_t *gvs)
{
  int i;
  int siz[3];

  /* First release any previous RSX allocation for this surface descriptor */
  surface_reset(gv, gvs);

  /* Compute byte size for Y, U, and V planar buffers aligned to 16-byte boundaries */
  for(i = 0; i < 3; i++)
    siz[i] = ROUND_UP(gvs->gvs_width[i] * gvs->gvs_height[i], 16);

  gvs->gvs_size = siz[0] + siz[1] + siz[2];
  gvs->gvs_offset = rsx_alloc(gvs->gvs_size, 16);

  /* Guard against VRAM allocation failure (e.g. low memory or high fragment pool exhaustion) */
  if(gvs->gvs_offset <= 0) {
    TRACE(TRACE_ERROR, "RSX",
          "Low VRAM: Failed to allocate %d bytes for YUVP surface (result offset=%d)",
          gvs->gvs_size, gvs->gvs_offset);
    gvs->gvs_offset = 0;
    gvs->gvs_size = 0;
    gvs->gvs_data[0] = NULL;
    gvs->gvs_data[1] = NULL;
    gvs->gvs_data[2] = NULL;
    return;
  }

  /* Map RSX GDDR3 physical video RAM offsets into Cell PPU effective address space */
  gvs->gvs_data[0] = rsx_to_ppu(gvs->gvs_offset);
  gvs->gvs_data[1] = rsx_to_ppu(gvs->gvs_offset + siz[0]);
  gvs->gvs_data[2] = rsx_to_ppu(gvs->gvs_offset + siz[0] + siz[1]);

  int offset = gvs->gvs_offset;
  for(i = 0; i < 3; i++) {
    init_tex(&gvs->gvs_tex[i],
             offset,
             gvs->gvs_width[i],
             gvs->gvs_height[i],
             gvs->gvs_width[i]);
    offset += siz[i];
  }
}



/**
 *
 */
/**
 * @brief Updates shader uniform parameters for video color space conversion and blending.
 *
 * @param gr Graphics root context pointer.
 * @param gp Video program shader pair.
 * @param args Video context pointer (glw_video_t*).
 * @param rj Render job descriptor.
 */
static void
glw_video_rsx_load_uniforms(glw_root_t *gr, glw_program_t *gp, void *args,
                            const glw_render_job_t *rj)
{
  glw_video_t *gv = (glw_video_t *)args;
  float f4[4];

  glw_backend_root_t *be = &gr->gr_be;
  gcmContextData *ctx = be->be_ctx;
  rsx_fp_t *rfp = gp->gp_fragment_program;

  if(rfp->rfp_u_blend != NULL) {
    f4[0] = gv->gv_blend;
    f4[1] = 0.0f;
    f4[2] = 0.0f;
    f4[3] = 0.0f;
    rsxSetFragmentProgramParameter(ctx, rfp->rfp_binary,
                                   rfp->rfp_u_blend, f4,
                                   rfp->rfp_rsx_location,
                                   GCM_LOCATION_RSX);
  }

  if(rfp->rfp_u_color_matrix != NULL)
    rsxSetFragmentProgramParameter(ctx, rfp->rfp_binary,
                                   rfp->rfp_u_color_matrix,
                                   gv->gv_cmatrix_cur,
                                   rfp->rfp_rsx_location,
                                   GCM_LOCATION_RSX);

  if(rfp->rfp_u_color != NULL) {
    f4[0] = 0.0f;
    f4[1] = 0.0f;
    f4[2] = 0.0f;
    f4[3] = rj->alpha;

    rsxSetFragmentProgramParameter(ctx, rfp->rfp_binary,
                                   rfp->rfp_u_color, f4,
                                   rfp->rfp_rsx_location,
                                   GCM_LOCATION_RSX);
  }
}

/**
 * @brief Binds a video planar texture stage to RSX hardware texture unit.
 */
static inline void
rsx_bind_video_texture(gcmContextData *ctx, int unit, const gcmTexture *tex)
{
  rsxLoadTexture(ctx, (u8)unit, tex);
  /* Configure single base mipmap level without out-of-bounds LOD fetching */
  rsxTextureControl(ctx, (u8)unit, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
  /* Linear video plane filtering without convolution */
  rsxTextureFilter(ctx, (u8)unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, 0);
  rsxTextureWrapMode(ctx, (u8)unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                     GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/**
 * @brief Binds planar YUV textures to fragment program sampler stages.
 */
static void
load_texture_yuv(glw_root_t *gr, glw_program_t *gp, void *args,
                 const glw_backend_texture_t *t, int num)
{
  glw_backend_root_t *be = &gr->gr_be;
  gcmContextData *ctx = be->be_ctx;
  rsx_fp_t *rfp = gp->gp_fragment_program;
  const glw_video_surface_t *s = (const glw_video_surface_t *)t;

  if(num == 1) {
    for(int i = 0; i < 3; i++)
      if(rfp->rfp_texunit[i+3] != -1)
        rsx_bind_video_texture(ctx, rfp->rfp_texunit[i+3], &s->gvs_tex[i]);
  } else {
    for(int i = 0; i < 3; i++)
      if(rfp->rfp_texunit[i] != -1)
        rsx_bind_video_texture(ctx, rfp->rfp_texunit[i], &s->gvs_tex[i]);
  }
}



/**
 * @brief Initializes the RSX planar YUV video rendering engine.
 *
 * Configures the pipeline callbacks, clears uniform color matrices,
 * and initializes the surface pool descriptors. The active surface queue is
 * capped at 4 surfaces instead of 10 to conserve valuable RSX GDDR3 VRAM
 * (~5.5MB working set vs ~13.8MB for 720p; ~12.4MB vs ~31.1MB for 1080p).
 *
 * @param gv Pointer to GLW video context.
 * @return 0 on success.
 *
 * @complexity Time: O(GLW_VIDEO_MAX_SURFACES). Space: O(1).
 */
static int
yuvp_init(glw_video_t *gv)
{
  int i;

  gv->gv_gpa.gpa_aux = gv;
  gv->gv_gpa.gpa_load_uniforms = glw_video_rsx_load_uniforms;
  gv->gv_gpa.gpa_load_texture = load_texture_yuv;

  memset(gv->gv_cmatrix_cur, 0, sizeof(float) * 16);

  /* Cleanly initialize all surface descriptors in the pool */
  for(i = 0; i < GLW_VIDEO_MAX_SURFACES; i++) {
    glw_video_surface_t *gvs = &gv->gv_surfaces[i];
    gvs->gvs_offset = 0;
    gvs->gvs_size = 0;
    gvs->gvs_data[0] = NULL;
    gvs->gvs_data[1] = NULL;
    gvs->gvs_data[2] = NULL;
    /* Limit the active surface queue pool to 4 to conserve RSX GDDR3 VRAM */
    if(i < 4)
      TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  }
  return 0;
}





/**
 *
 */
static void
gv_surface_pixmap_release(glw_video_t *gv, glw_video_surface_t *gvs,
			  struct glw_video_surface_queue *fromqueue)
{
  TAILQ_REMOVE(fromqueue, gvs, gvs_link);
  TAILQ_INSERT_TAIL(&gv->gv_avail_queue, gvs, gvs_link);
  hts_cond_signal(&gv->gv_avail_queue_cond);
}


static const float cmatrix_ITUR_BT_601[16] = {
  1.164400,   1.164400, 1.164400, 0,
  0.000000,  -0.391800, 2.017200, 0,
  1.596000,  -0.813000, 0.000000, 0, 
 -0.874190,   0.531702,-1.085616, 1
};

static const float cmatrix_ITUR_BT_709[16] = {
  1.164400,  1.164400,  1.164400, 0,
  0.000000, -0.213200,  2.112400, 0,
  1.792700, -0.532900,  0.000000, 0,
 -0.972926,  0.301453, -1.133402, 1
};

static const float cmatrix_SMPTE_240M[16] = {
  1.164400,  1.164400,  1.164400, 0,
  0.000000, -0.257800,  2.078700, 0,
  1.793900, -0.542500,  0.000000, 0,
 -0.973528,  0.328659, -1.116486, 1
};



/**
 *
 */
static void
gv_color_matrix_set(glw_video_t *gv, const struct frame_info *fi)
{
  const float *f;

  switch(fi->fi_color_space) {
  case COLOR_SPACE_BT_709:
    f = cmatrix_ITUR_BT_709;
    break;

  case COLOR_SPACE_BT_601:
    f = cmatrix_ITUR_BT_601;
    break;

  case COLOR_SPACE_SMPTE_240M:
    f = cmatrix_SMPTE_240M;
    break;

  default:
    f = fi->fi_height < 720 ? cmatrix_ITUR_BT_601 : cmatrix_ITUR_BT_709;
    break;
  }

  memcpy(gv->gv_cmatrix_tgt, f, sizeof(float) * 16);
  /* If current matrix is uninitialized (all zeros), populate immediately to prevent dark start */
  if(gv->gv_cmatrix_cur[0] == 0.0f)
    memcpy(gv->gv_cmatrix_cur, f, sizeof(float) * 16);
}


static void
gv_color_matrix_update(glw_video_t *gv)
{
  int i;
  for(i = 0; i < 16; i++)
    gv->gv_cmatrix_cur[i] = (gv->gv_cmatrix_cur[i] * 15.0 +
			     gv->gv_cmatrix_tgt[i]) / 16.0;
}


/**
 *
 */
static int64_t
yuvp_newframe(glw_video_t *gv, video_decoder_t *vd, int flags)
{
  gv_color_matrix_update(gv);
  return glw_video_newframe_blend(gv, vd, flags, &gv_surface_pixmap_release, 1);
}


/**
 *
 */
static void
yuvp_render(glw_video_t *gv, glw_rctx_t *rc)
{
  glw_root_t *gr = gv->w.glw_root;
  glw_video_surface_t *sa = gv->gv_sa, *sb = gv->gv_sb;
  glw_program_t *gp;
  glw_backend_root_t *gbr = &gr->gr_be;

  if(sa == NULL)
    return;

  gv->gv_width  = sa->gvs_width[0];
  gv->gv_height = sa->gvs_height[0];

  const float yshift_a = (-0.5 * sa->gvs_yshift) / (float)sa->gvs_height[0];

  glw_renderer_vtx_st(&gv->gv_quad,  0, 0, 1 + yshift_a);
  glw_renderer_vtx_st(&gv->gv_quad,  1, 1, 1 + yshift_a);
  glw_renderer_vtx_st(&gv->gv_quad,  2, 1, 0 + yshift_a);
  glw_renderer_vtx_st(&gv->gv_quad,  3, 0, 0 + yshift_a);

  if(sb != NULL) {

    gp = &gbr->be_yuv2rgb_2f;

    const float yshift_b = (-0.5 * sb->gvs_yshift) / (float)sb->gvs_height[0];

    glw_renderer_vtx_st2(&gv->gv_quad, 0, 0, 1 + yshift_b);
    glw_renderer_vtx_st2(&gv->gv_quad, 1, 1, 1 + yshift_b);
    glw_renderer_vtx_st2(&gv->gv_quad, 2, 1, 0 + yshift_b);
    glw_renderer_vtx_st2(&gv->gv_quad, 3, 0, 0 + yshift_b);

  } else {
    gp = &gbr->be_yuv2rgb_1f;
  }

  gv->gv_gpa.gpa_prog = gp;

  glw_renderer_draw(&gv->gv_quad, gr, rc,
                    (void *)sa, // Ugly
                    (void *)sb, // Ugly again
                    NULL, NULL,
                    rc->rc_alpha * gv->w.glw_alpha, 0, &gv->gv_gpa);
}


/**
 *
 */
static void
yuvp_blackout(glw_video_t *gv)
{
  memset(gv->gv_cmatrix_tgt, 0, sizeof(float) * 16);
}


/**
 *
 */
static int
yuvp_deliver(const frame_info_t *fi, glw_video_t *gv, glw_video_engine_t *gve)
{
  int hvec[3], wvec[3];
  int i, h, w;
  const uint8_t *src;
  uint8_t *dst;
  int tff;
  int hshift = fi->fi_hshift, vshift = fi->fi_vshift;
  glw_video_surface_t *s;
  const int parity = 0;

  wvec[0] = fi->fi_width;
  wvec[1] = fi->fi_width >> hshift;
  wvec[2] = fi->fi_width >> hshift;
  hvec[0] = fi->fi_height >> fi->fi_interlaced;
  hvec[1] = fi->fi_height >> (vshift + fi->fi_interlaced);
  hvec[2] = fi->fi_height >> (vshift + fi->fi_interlaced);

  glw_video_configure(gv, gve);
  
  gv_color_matrix_set(gv, fi);

  if((s = glw_video_get_surface(gv, wvec, hvec)) == NULL)
    return -1;

  /* Verify surface has valid RSX GDDR3 memory backing before copying pixel data */
  if(s->gvs_offset <= 0 || s->gvs_data[0] == NULL) {
    TAILQ_INSERT_TAIL(&gv->gv_avail_queue, s, gvs_link);
    hts_cond_signal(&gv->gv_avail_queue_cond);
    return -1;
  }

  if(!fi->fi_interlaced) {

    for(i = 0; i < 3; i++) {
      w = wvec[i];
      h = hvec[i];
      src = fi->fi_data[i];
      dst = s->gvs_data[i];
 
      while(h--) {
	memcpy(dst, src, w);
	dst += w;
	src += fi->fi_pitch[i];
      }
    }

    glw_video_put_surface(gv, s, fi->fi_pts, fi->fi_epoch, fi->fi_duration,
			  0, 0);

  } else {

    int duration = fi->fi_duration >> 1;

    tff = fi->fi_tff ^ parity;

    for(i = 0; i < 3; i++) {
      w = wvec[i];
      h = hvec[i];
      
      src = fi->fi_data[i]; 
      dst = s->gvs_data[i];
      
      while(h--) {
	memcpy(dst, src, w);
	dst += w;
	src += fi->fi_pitch[i] * 2;
      }
    }
    
    glw_video_put_surface(gv, s, fi->fi_pts, fi->fi_epoch, duration, 1, !tff);

    if((s = glw_video_get_surface(gv, wvec, hvec)) == NULL)
      return -1;

    /* Verify second field surface has valid RSX GDDR3 memory backing */
    if(s->gvs_offset <= 0 || s->gvs_data[0] == NULL) {
      TAILQ_INSERT_TAIL(&gv->gv_avail_queue, s, gvs_link);
      hts_cond_signal(&gv->gv_avail_queue_cond);
      return -1;
    }

    for(i = 0; i < 3; i++) {
      w = wvec[i];
      h = hvec[i];
      
      src = fi->fi_data[i] + fi->fi_pitch[i];
      dst = s->gvs_data[i];
      
      while(h--) {
	memcpy(dst, src, w);
	dst += w;
	src += fi->fi_pitch[i] * 2;
      }
    }
    
    glw_video_put_surface(gv, s, fi->fi_pts + duration,
			  fi->fi_epoch, duration, 1, tff);
  }
  return 0;
}

/**
 *
 */
static glw_video_engine_t glw_video_yuvp = {
  .gve_type = 'YUVP',
  .gve_newframe = yuvp_newframe,
  .gve_render = yuvp_render,
  .gve_reset = yuvp_reset,
  .gve_init = yuvp_init,
  .gve_deliver = yuvp_deliver,
  .gve_surface_init = surface_init,
  .gve_blackout = yuvp_blackout,
};

GLW_REGISTER_GVE(glw_video_yuvp);


/**
 *
 */
static int
rsx_deliver(const frame_info_t *fi, glw_video_t *gv, glw_video_engine_t *gve)
{
  int hvec[3], wvec[3];
  int i;
  int hshift = 1, vshift = 1;
  glw_video_surface_t *gvs;


  wvec[0] = fi->fi_width;
  wvec[1] = fi->fi_width >> hshift;
  wvec[2] = fi->fi_width >> hshift;
  hvec[0] = fi->fi_height >> fi->fi_interlaced;
  hvec[1] = fi->fi_height >> (vshift + fi->fi_interlaced);
  hvec[2] = fi->fi_height >> (vshift + fi->fi_interlaced);


  glw_video_configure(gv, gve);
  
  gv_color_matrix_set(gv, fi);

  if((gvs = glw_video_get_surface(gv, NULL, NULL)) == NULL)
    return -1;

  surface_reset(gv, gvs);

  gvs->gvs_offset = fi->fi_u32[0];
  gvs->gvs_size   = fi->fi_u32[3];
  gvs->gvs_width[0]  = fi->fi_width;
  gvs->gvs_height[0] = fi->fi_height;

  if(fi->fi_interlaced) {
    // Interlaced

    for(i = 0; i < 3; i++) {
      const int w = wvec[i];
      const int h = hvec[i];
      const int offset = fi->fi_u32[i];

      init_tex(&gvs->gvs_tex[i],
               offset + !fi->fi_tff * fi->fi_pitch[i],
               w, h, fi->fi_pitch[i] * 2);
    }
    glw_video_put_surface(gv, gvs, fi->fi_pts, fi->fi_epoch,
			  fi->fi_duration/2, 1, !fi->fi_tff);

    if((gvs = glw_video_get_surface(gv, NULL, NULL)) == NULL) {
      /* We have already taken ownership of the RSX memory, so we
         can only return 0 here. In fact we managed to put one surface
         out so it's not that much of an error really */
      return 0;
    }

    surface_reset(gv, gvs);

    gvs->gvs_offset = fi->fi_u32[0];
    gvs->gvs_size   = fi->fi_u32[3];
    gvs->gvs_width[0]  = fi->fi_width;
    gvs->gvs_height[0] = fi->fi_height;

    for(i = 0; i < 3; i++) {
      const int w = wvec[i];
      const int h = hvec[i];
      const int offset = fi->fi_u32[i];

      init_tex(&gvs->gvs_tex[i],
               offset + !!fi->fi_tff * fi->fi_pitch[i],
               w, h, fi->fi_pitch[i] * 2);
    }

    glw_video_put_surface(gv, gvs, fi->fi_pts + fi->fi_duration, fi->fi_epoch,
			  fi->fi_duration/2, 1, fi->fi_tff);

  } else {
    // Progressive

    for(i = 0; i < 3; i++) {
      const int w = wvec[i];
      const int h = hvec[i];
      const int offset = fi->fi_u32[i];

      init_tex(&gvs->gvs_tex[i],
               offset,
               w, h, fi->fi_pitch[i]);
    }
    glw_video_put_surface(gv, gvs, fi->fi_pts, fi->fi_epoch,
			  fi->fi_duration, 0, 0);
  }
  return 0;
}

/**
 *
 */
static glw_video_engine_t glw_video_rsxmem = {
  .gve_type = 'RSX',
  .gve_newframe = yuvp_newframe,
  .gve_render = yuvp_render,
  .gve_reset = yuvp_reset,
  .gve_init = yuvp_init,
  .gve_deliver = rsx_deliver,
  .gve_blackout = yuvp_blackout,
};

GLW_REGISTER_GVE(glw_video_rsxmem);
