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
#pragma once

/**
 * PSL1GHT v2 RSX Reality Synthesizer Acceleration Headers:
 * Supplies hardware command buffer interfaces, NV40/G70 GCM data structures,
 * and offline-compiled shader introspection bindings.
 */
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <rsx/commands.h>

struct glw_rgb;
struct glw_rctx;
struct glw_root;
struct glw_backend_root;
struct glw_renderer;
struct glw_backend_texture;

/**
 * RSX Hardware Primitive Topology Enums:
 * Maps Movian internal GLW draw modes to native GCM command values.
 */
#define GLW_DRAW_TRIANGLES GCM_TYPE_TRIANGLES
#define GLW_DRAW_LINE_LOOP GCM_TYPE_LINE_LOOP
#define GLW_DRAW_LINES     GCM_TYPE_LINES

/**
 * @brief RSX Vertex Program Shader Descriptor.
 *
 * Holds the compiled vertex shader binary header, microcode pointer, and resolved
 * uniform/attribute hardware register indices for fast per-batch binding.
 */
typedef struct glw_rsx_vp {
  rsxVertexProgram *rvp_binary;     /**< Parsed vertex program binary structure */
  void             *rvp_ucode;      /**< Extracted hardware microcode payload */
  uint32_t          rvp_ucode_size; /**< Size in bytes of vertex shader microcode */

  int rvp_u_modelview;              /**< ModelView matrix constant slot index */
  int rvp_u_color;                  /**< Primary diffuse color constant slot index */
  int rvp_u_color_offset;           /**< Additive color offset constant slot index */
  int rvp_u_blur;                   /**< Gaussian blur kernel constant slot index */

  int rvp_a_position;               /**< Vertex position attribute index (NV40 TCL slot) */
  int rvp_a_color;                  /**< Vertex color attribute index */
  int rvp_a_texcoord;               /**< Texture coordinate attribute index */

} rsx_vp_t;

/**
 * @brief RSX Fragment Program Shader Descriptor.
 *
 * Holds the compiled fragment shader binary header, GDDR3 memory offset, resolved
 * uniform pointers, and texture sampler unit assignments.
 */
typedef struct glw_rsx_fp {
  rsxFragmentProgram *rfp_binary;          /**< Parsed fragment program binary structure */
  int                 rfp_rsx_location;    /**< Microcode memory offset in RSX GDDR3 RAM */

  rsxProgramConst    *rfp_u_color;         /**< Color modulation constant descriptor */
  rsxProgramConst    *rfp_u_color_matrix;  /**< Color conversion matrix constant descriptor */
  rsxProgramConst    *rfp_u_blend;         /**< Video blend parameter constant descriptor */

  int                 rfp_texunit[6];      /**< Texture sampler unit binding slots */

} rsx_fp_t;

/**
 * @brief Combined RSX Graphics Pipeline Shader Pair.
 */
struct glw_program {
  rsx_vp_t *gp_vertex_program;
  rsx_fp_t *gp_fragment_program;
};

/**
 * @brief RSX Backend Root Context.
 *
 * Tracks currently bound vertex/fragment shaders and holds pre-compiled
 * core UI and video conversion shaders.
 */
typedef struct glw_backend_root {
  gcmContextData *be_ctx;                  /**< GCM RSX command buffer execution context */

  rsx_vp_t *be_vp_current;                 /**< Currently bound vertex program */
  rsx_fp_t *be_fp_current;                 /**< Currently bound fragment program */

  rsx_vp_t *be_vp_1;                       /**< Standard 2D UI vertex program */
  rsx_fp_t *be_fp_tex;                     /**< Textured fragment program */
  rsx_fp_t *be_fp_flat;                    /**< Untextured flat-color fragment program */
  rsx_fp_t *be_fp_tex_blur;                /**< Textured box-blur fragment program */

  struct glw_program be_yuv2rgb_1f;        /**< 1-field YUV to RGB color conversion shader */
  struct glw_program be_yuv2rgb_2f;        /**< 2-field deinterlacing YUV to RGB shader */

  rsx_fp_t *be_fp_tex_stencil;             /**< Textured stencil mask fragment program */
  rsx_fp_t *be_fp_flat_stencil;            /**< Flat stencil mask fragment program */
  rsx_fp_t *be_fp_tex_stencil_blur;        /**< Textured blurred stencil fragment program */

} glw_backend_root_t;

/**
 * @brief RSX Backend Texture Container.
 *
 * Wraps the native PSL1GHT v2 gcmTexture structure with allocated GDDR3 byte size.
 */
typedef struct glw_backend_texture {
  gcmTexture tex;                          /**< Native GCM texture parameter structure */
  uint32_t   size;                         /**< Total allocated VRAM size in bytes */
} glw_backend_texture_t;

#define glw_tex_width(gbt)  ((gbt)->tex.width)
#define glw_tex_height(gbt) ((gbt)->tex.height)

#define glw_can_tnpo2(gr) 1
#define glw_is_tex_inited(n) ((n)->size != 0)

int glw_rsx_init_context(struct glw_root *gr);

/**
 * Render to texture support
 */
typedef struct {
  glw_backend_texture_t grtt_texture;
  int                   grtt_width;
  int                   grtt_height;
  char                  grtt_opaque;
} glw_rtt_t;

void glw_rtt_init(struct glw_root *gr, glw_rtt_t *grtt, int width, int height, int alpha);
void glw_rtt_enter(struct glw_root *gr, glw_rtt_t *grtt, struct glw_rctx *rc0);
void glw_rtt_restore(struct glw_root *gr, glw_rtt_t *grtt);
void glw_rtt_destroy(struct glw_root *gr, glw_rtt_t *grtt);

#define glw_rtt_texture(grtt) ((grtt)->grtt_texture)

/**
 * RSX VRAM Memory Pool Allocator:
 * Manages GDDR3 memory allocations with alignment constraints.
 */
int rsx_alloc(int size, int alignment);
void rsx_free(int pos, int size);

extern char *rsx_address;
#define rsx_to_ppu(pos) ((void *)(rsx_address + (pos)))
