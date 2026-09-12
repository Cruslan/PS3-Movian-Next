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
 * Maps source color channels to GPU shader registers without CPU pixel conversion overhead.
 */
#define REMAP_IDENTITY \
  GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, \
                         GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, \
                         GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

#define REMAP_ABGR \
  GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, \
                         GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_B, \
                         GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_R, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

#define REMAP_RGBA \
  GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, \
                         GCM_TEXTURE_REMAP_COLOR_B, GCM_TEXTURE_REMAP_COLOR_A, \
                         GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_G, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

#define REMAP_IA8 \
  GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, \
                         GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R, \
                         GCM_TEXTURE_REMAP_COLOR_R, GCM_TEXTURE_REMAP_COLOR_R, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, \
                         GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP)

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
static void *
realloc_tex(glw_root_t *gr, glw_backend_texture_t *tex, int size)
{
  if(tex->size != (uint32_t)size) {
    if(tex->size != 0)
      rsx_free(tex->tex.offset, tex->size);

    tex->size = size;

    if(tex->size != 0)
      tex->tex.offset = rsx_alloc(tex->size, 16);
  }
  return tex->size ? rsx_to_ppu(tex->tex.offset) : NULL;
}

/**
 * @brief Initializes and uploads an ABGR pixel buffer to RSX VRAM.
 */
static void
init_abgr(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  void *mem = realloc_tex(gr, tex, linesize * height);
  if(mem == NULL)
    return;

  memcpy(mem, src, tex->size);
  init_tex(&tex->tex, tex->tex.offset, width, height, linesize,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_ABGR);
}

/**
 * @brief Initializes and uploads an RGBA pixel buffer to RSX VRAM.
 */
static void
init_rgba(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  void *mem = realloc_tex(gr, tex, linesize * height);
  if(mem == NULL)
    return;

  memcpy(mem, src, tex->size);
  init_tex(&tex->tex, tex->tex.offset, width, height, linesize,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_RGBA);
}

/**
 * @brief Converts 24-bit packed RGB pixels to 32-bit ARGB in RSX VRAM.
 */
static void
init_rgb(glw_root_t *gr, glw_backend_texture_t *tex,
         const uint8_t *src, int linesize,
         int width, int height, int repeat)
{
  uint32_t *dst = (uint32_t *)realloc_tex(gr, tex, width * height * 4);
  if(dst == NULL)
    return;

  for(int y = 0; y < height; y++) {
    const uint8_t *s = src;
    for(int x = 0; x < width; x++) {
      uint8_t r = *s++;
      uint8_t g = *s++;
      uint8_t b = *s++;
      *dst++ = 0xff000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    src += linesize;
  }

  init_tex(&tex->tex, tex->tex.offset, width, height, width * 4,
           GCM_TEXTURE_FORMAT_A8R8G8B8, repeat, REMAP_IDENTITY);
}

/**
 * @brief Initializes an 8-bit Intensity + 8-bit Alpha luminance texture.
 */
static void
init_i8a8(glw_root_t *gr, glw_backend_texture_t *tex,
          const uint8_t *src, int linesize,
          int width, int height, int repeat)
{
  void *mem = realloc_tex(gr, tex, linesize * height);
  if(mem == NULL)
    return;

  memcpy(mem, src, tex->size);
  init_tex(&tex->tex, tex->tex.offset, width, height, linesize,
           GCM_TEXTURE_FORMAT_COMPRESSED_HILO8, repeat, REMAP_IA8);
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
  int size;

  glt->glt_s = 1.0f;
  glt->glt_t = 1.0f;

  switch(pm->pm_type) {
  case PIXMAP_BGR32:
    size = pm->pm_linesize * pm->pm_height;
    init_abgr(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_RGBA:
    size = pm->pm_linesize * pm->pm_height;
    init_rgba(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_RGB24:
    size = pm->pm_width * pm->pm_height * 4;
    init_rgb(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
             pm->pm_width, pm->pm_height, repeat);
    break;

  case PIXMAP_IA:
    size = pm->pm_linesize * pm->pm_height;
    init_i8a8(gr, &glt->glt_texture, pm->pm_data, pm->pm_linesize,
              pm->pm_width, pm->pm_height, repeat);
    break;

  default:
    return 0;
  }
  return size;
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
