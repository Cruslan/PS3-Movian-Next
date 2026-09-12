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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * PSL1GHT v2 RSX Subsystem Headers:
 * Supplies hardware command buffer interfaces, NV40/G70 GCM register controls,
 * and offline-compiled shader introspection bindings.
 */
#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <rsx/commands.h>
#include <rsx/nv40.h>

#include "glw.h"
#include "glw_renderer.h"
#include "fileaccess/fileaccess.h"

#define RSX_TRACE(fmt...) // TRACE(TRACE_DEBUG, "RSX", fmt)

extern s32 rsxContextCallback(gcmContextData *context, u32 count);


/**
 * 4x4 Identity matrix in row-major layout for eyespace rendering passes
 */
static float identitymtx[16] = {
  1.0f, 0.0f, 0.0f, 0.0f,
  0.0f, 1.0f, 0.0f, 0.0f,
  0.0f, 0.0f, 1.0f, 0.0f,
  0.0f, 0.0f, 0.0f, 1.0f
};

/**
 * @brief Helper to resolve vertex program constant register index by uniform name.
 *
 * Searches the compiled vertex program constant table for a matching identifier
 * and retrieves the hardware constant register index.
 *
 * @param vp Pointer to the compiled vertex program structure.
 * @param name Null-terminated uniform identifier string.
 * @return int Hardware constant index on success, or -1 if unresolved.
 *
 * @complexity Time: O(K) where K is number of uniforms in shader. Space: O(1).
 */
static int
vp_get_vector_const(const rsxVertexProgram *vp, const char *name)
{
  rsxProgramConst *c = rsxVertexProgramGetConst(vp, name);
  return c != NULL ? (int)c->index : -1;
}

/**
 * @brief Helper to resolve vertex program attribute register index by attribute identifier.
 *
 * PSL1GHT v2 declares rsxVertexProgramGetAttribIndex in <rsx/rsx_program.h>
 * but omits the symbol implementation in librsx.a. This helper interrogates the
 * rsxProgramAttrib structure directly via rsxVertexProgramGetAttrib.
 *
 * @param vp Pointer to the compiled vertex program structure.
 * @param name Null-terminated attribute identifier string (e.g. "a_position").
 * @return s32 Hardware attribute index on success, or -1 if unresolved.
 *
 * @complexity Time: O(A) where A is number of attributes in shader. Space: O(1).
 */
static s32
glw_rsx_vp_get_attrib_index(const rsxVertexProgram *vp, const char *name)
{
  rsxProgramAttrib *attrib = rsxVertexProgramGetAttrib(vp, name);
  return attrib != NULL ? (s32)attrib->index : -1;
}

/**
 * @brief Helper to resolve fragment program attribute register index by identifier.
 *
 * PSL1GHT v2 declares rsxFragmentProgramGetAttribIndex in <rsx/rsx_program.h>
 * but omits the symbol implementation in librsx.a. This helper interrogates the
 * rsxProgramAttrib structure directly via rsxFragmentProgramGetAttrib.
 *
 * @param fp Pointer to the compiled fragment program structure.
 * @param name Null-terminated attribute identifier string (e.g. "u_t0").
 * @return s32 Hardware sampler/attribute index on success, or -1 if unresolved.
 *
 * @complexity Time: O(A) where A is number of attributes in shader. Space: O(1).
 */
static s32
glw_rsx_fp_get_attrib_index(const rsxFragmentProgram *fp, const char *name)
{
  rsxProgramAttrib *attrib = rsxFragmentProgramGetAttrib(fp, name);
  return attrib != NULL ? (s32)attrib->index : -1;
}

/**
 * ============================================================================
 * Legacy PSL1GHT v1 (libreality) Shader Binary Specification
 * ============================================================================
 * Precompiled shaders bundled under res/shaders/rsx/ were authored using vintage
 * 2011-era libreality compiler pipelines (cgc -> cgcomp).
 *
 * In libreality v1:
 * - Attribute records are 8 bytes: { uint32_t name_off; uint32_t index; }
 *   (PSL1GHT v2 expects 12 bytes: { uint32_t name_off; uint32_t index; uint8_t type; uint8_t _pad0[3]; })
 * - The v1 vertex program header stores num_attrib (u16) at byte offset 0x02,
 *   attrib_off (u32) at 0x04, num_const (u32) at 0x08, and const_off (u32) at 0x14.
 *   (PSL1GHT v2 stores _pad0 at 0x02, num_attr at 0x06, num_const at 0x08, attr_off at 0x0c, const_off at 0x10)
 * - The v1 fragment program header stores num_attrib (u16) at 0x02, attrib_off (u32) at 0x04,
 *   num_regs (u32) at 0x08, num_const (u32) at 0x10, const_off (u32) at 0x14, and num_insn (u16) at 0x18.
 *   (PSL1GHT v2 stores _pad0 at 0x02, num_attr at 0x06, attr_off at 0x0c, and const_off at 0x10)
 *
 * Passing raw v1 shader payloads directly to PSL1GHT v2's introspection routines
 * causes out-of-bounds pointer dereferences, resulting in unmapped memory faults
 * (e.g. strcasecmp segfaulting on unmapped memory at 0x8661a70c).
 * The helpers below transparently detect v1 binaries and reconstruct valid
 * PSL1GHT v2 rsxVertexProgram and rsxFragmentProgram structures in heap memory.
 */

/**
 * @brief Legacy PSL1GHT v1 (libreality) Vertex Program binary header.
 *
 * In 2011-era libreality binary vertex shader files (*.vp), the 32-bit field order
 * following attrib_off (0x04) is:
 * - 0x08: input_mask (bitmask of active input attributes, e.g. 0x07 for 3 attributes)
 * - 0x0c: output_mask (bitmask of output interpolators, e.g. 0x0003c000)
 * - 0x10: num_const (number of uniform constants, e.g. 9 for v1.vp, 6 for yuv2rgb_v.vp)
 * - 0x14: const_off (byte offset to constant descriptors, e.g. 0x38)
 * - 0x18: num_insn (number of 16-byte instructions, e.g. 14)
 * - 0x1c: ucode_off (byte offset to microcode, e.g. 0x180)
 *
 * An earlier inversion placed num_const at 0x08 and input_mask at 0x10, causing
 * num_const to be read as 7 instead of 9, which truncated constants 7 and 8 (the 3D
 * projection matrix) during modernization.
 */
typedef struct reality_vertex_program {
  uint16_t magic;        /**< Magic header signature (0x5650 == 'VP') (0x00) */
  uint16_t num_attrib;   /**< Number of input vertex attributes (0x02) */
  uint32_t attrib_off;   /**< Byte offset to attribute record table (0x04) */
  uint32_t input_mask;   /**< Bitmask of active vertex attribute slots (0x08) */
  uint32_t output_mask;  /**< Bitmask of interpolator output slots (0x0c) */
  uint32_t num_const;    /**< Number of uniform constant descriptors (0x10) */
  uint32_t const_off;    /**< Byte offset to uniform constant record table (0x14) */
  uint32_t num_insn;     /**< Number of 16-byte NV40 microcode instructions (0x18) */
  uint32_t ucode_off;    /**< Byte offset to hardware microcode instruction stream (0x1c) */
} realityVertexProgram;

/**
 * @brief Legacy PSL1GHT v1 (libreality) Fragment Program binary header.
 */
typedef struct reality_fragment_program {
  uint16_t magic;        /**< Magic header signature (0x4650 == 'FP') */
  uint16_t num_attrib;   /**< Number of attribute/sampler records */
  uint32_t attrib_off;   /**< Byte offset to attribute record table */
  uint32_t num_regs;     /**< Number of temporary vector registers utilized */
  uint32_t pad0;         /**< Reserved padding field */
  uint32_t num_const;    /**< Number of uniform parameter records */
  uint32_t const_off;    /**< Byte offset to uniform constant record table */
  uint16_t num_insn;     /**< Number of 16-byte fragment microcode instructions */
  uint16_t pad1;         /**< Alignment padding word */
  uint32_t ucode_off;    /**< Byte offset to hardware microcode stream */
} realityFragmentProgram;

/**
 * @brief Legacy PSL1GHT v1 8-byte Attribute Record.
 */
typedef struct reality_program_attrib {
  uint32_t name_off;     /**< Offset to null-terminated identifier string */
  uint32_t index;        /**< Hardware attribute register or sampler unit index */
} realityProgramAttrib;

/**
 * @brief Modernizes a vertex program binary from legacy libreality format to PSL1GHT v2.
 *
 * Detects whether the binary payload loaded from dataroot:// is a 2011-era libreality
 * vertex program. If legacy format is detected, reconstructs an authentic PSL1GHT v2
 * rsxVertexProgram structure in dynamically allocated heap memory, expanding 8-byte
 * attribute entries to 12-byte rsxProgramAttrib records, relocating constant tables
 * and string pools, and ensuring strict 16-byte microcode instruction alignment.
 *
 * @param raw_data Pointer to the raw binary shader payload loaded from disk.
 * @param raw_size Byte size of the raw binary shader payload.
 * @return rsxVertexProgram* Modernized PSL1GHT v2 vertex program descriptor, or NULL on error.
 *
 * @complexity Time: O(N) where N is shader binary byte length. Space: O(N) allocated buffer.
 */
static rsxVertexProgram *
rsx_modernize_vp(const void *raw_data, size_t raw_size)
{
  if(raw_data == NULL || raw_size < sizeof(realityVertexProgram))
    return NULL;

  const uint16_t magic = *(const uint16_t *)raw_data;
  if(magic != 0x5650)
    return NULL;

  /* In PSL1GHT v2 rsxVertexProgram, offset 2 is _pad0 which is strictly 0.
   * In legacy libreality, offset 2 is num_attrib which is non-zero (>= 2). */
  const uint16_t pad_or_num_attrib = ((const uint16_t *)raw_data)[1];
  if(pad_or_num_attrib == 0) {
    /* Already in modern PSL1GHT v2 format; clone into independent heap buffer */
    rsxVertexProgram *vp = malloc(raw_size);
    if(vp != NULL)
      memcpy(vp, raw_data, raw_size);
    return vp;
  }

  const realityVertexProgram *old_vp = (const realityVertexProgram *)raw_data;
  const uint16_t num_attrib = old_vp->num_attrib;
  const uint32_t num_const  = old_vp->num_const;
  const uint32_t num_insn   = old_vp->num_insn;
  const uint32_t attrib_off = old_vp->attrib_off;
  const uint32_t const_off  = old_vp->const_off;
  const uint32_t ucode_off  = old_vp->ucode_off;

  /* Basic safety boundary validation */
  if(attrib_off >= raw_size || (num_const > 0 && const_off >= raw_size) || ucode_off >= raw_size)
    return NULL;

  const realityProgramAttrib *old_attrs = (const realityProgramAttrib *)((const char *)raw_data + attrib_off);
  const rsxProgramConst *old_consts = (const rsxProgramConst *)((const char *)raw_data + const_off);

  /* Find minimum string offset in the payload to copy strings and microcode */
  uint32_t min_str_off = ucode_off;
  for(uint16_t i = 0; i < num_attrib; i++) {
    if(old_attrs[i].name_off != 0 && old_attrs[i].name_off < min_str_off)
      min_str_off = old_attrs[i].name_off;
  }
  for(uint32_t i = 0; i < num_const; i++) {
    if(old_consts[i].name_off != 0 && old_consts[i].name_off < min_str_off)
      min_str_off = old_consts[i].name_off;
  }

  if(min_str_off > raw_size)
    min_str_off = ucode_off;

  /* Layout modern PSL1GHT v2 structure:
   * 0x00 .. 0x3f: rsxVertexProgram header (padded to 64 bytes)
   * 0x40 .. : rsxProgramAttrib array (num_attrib * 12 bytes)
   * Followed by: rsxProgramConst array (num_const * 28 bytes)
   * Followed by: string table
   * Followed by: hardware microcode aligned to 16 bytes */
  const uint32_t new_attr_off    = 64;
  const uint32_t new_const_off   = new_attr_off + (num_attrib * sizeof(rsxProgramAttrib));
  const uint32_t new_strings_off = new_const_off + (num_const * sizeof(rsxProgramConst));
  const uint32_t str_len         = (ucode_off > min_str_off) ? (ucode_off - min_str_off) : 0;
  const uint32_t new_ucode_off   = (new_strings_off + str_len + 15) & ~15;
  const uint32_t ucode_len       = num_insn * 16;
  const uint32_t new_total_size  = new_ucode_off + ucode_len;

  rsxVertexProgram *new_vp = calloc(1, new_total_size);
  if(new_vp == NULL)
    return NULL;

  char *dst_base = (char *)new_vp;
  const char *src_base = (const char *)raw_data;

  /* Copy string table and microcode blocks */
  if(str_len > 0)
    memcpy(dst_base + new_strings_off, src_base + min_str_off, str_len);
  if(ucode_len > 0 && ucode_off + ucode_len <= raw_size)
    memcpy(dst_base + new_ucode_off, src_base + ucode_off, ucode_len);

  const int32_t str_delta = (int32_t)new_strings_off - (int32_t)min_str_off;

  /* Populate converted 12-byte rsxProgramAttrib array */
  rsxProgramAttrib *new_attrs = (rsxProgramAttrib *)(dst_base + new_attr_off);
  for(uint16_t i = 0; i < num_attrib; i++) {
    new_attrs[i].name_off = (old_attrs[i].name_off != 0) ? (old_attrs[i].name_off + str_delta) : 0;
    new_attrs[i].index    = old_attrs[i].index;
    new_attrs[i].type     = 0;
    new_attrs[i]._pad0[0] = 0;
    new_attrs[i]._pad0[1] = 0;
    new_attrs[i]._pad0[2] = 0;
  }

  /* Populate converted 28-byte rsxProgramConst array */
  rsxProgramConst *new_consts = (rsxProgramConst *)(dst_base + new_const_off);
  for(uint32_t i = 0; i < num_const; i++) {
    new_consts[i] = old_consts[i];
    if(new_consts[i].name_off != 0)
      new_consts[i].name_off += str_delta;
  }

  /* Fill modern PSL1GHT v2 vertex program header:
   * Map input and output masks from the libreality header.
   * Furthermore, actively ensure that every declared input attribute slot
   * is guaranteed to be enabled in input_mask so that NV40 register 0x1ff4
   * (NV40_3D_VP_ATTRIB_EN) is fully active, particularly Attribute 0 (a_position)
   * which triggers the hardware vertex assembly pipe.
   * Allocate 32 temporary vector registers for NV40 vertex calculations. */
  uint32_t active_input_mask = old_vp->input_mask;
  for(uint16_t i = 0; i < num_attrib; i++) {
    active_input_mask |= (1u << old_attrs[i].index);
  }

  new_vp->magic       = 0x5650;
  new_vp->_pad0       = 0;
  new_vp->num_regs    = 32;
  new_vp->num_attr    = num_attrib;
  new_vp->num_const   = (uint16_t)num_const;
  new_vp->num_insn    = (uint16_t)num_insn;
  new_vp->attr_off    = new_attr_off;
  new_vp->const_off   = new_const_off;
  new_vp->ucode_off   = new_ucode_off;
  new_vp->input_mask  = active_input_mask;
  new_vp->output_mask = old_vp->output_mask;
  new_vp->const_start = 0;
  new_vp->insn_start  = 0;

  return new_vp;
}

/**
 * @brief Modernizes a fragment program binary from legacy libreality format to PSL1GHT v2.
 *
 * Detects whether the binary payload loaded from dataroot:// is a 2011-era libreality
 * fragment program. If legacy format is detected, reconstructs an authentic PSL1GHT v2
 * rsxFragmentProgram structure in dynamically allocated heap memory, expanding 8-byte
 * attribute entries to 12-byte rsxProgramAttrib records, relocating constant offset
 * patch tables, constant descriptors, and string pools, and guaranteeing 16-byte
 * alignment for the NV40 fragment microcode.
 *
 * @param raw_data Pointer to the raw binary shader payload loaded from disk.
 * @param raw_size Byte size of the raw binary shader payload.
 * @return rsxFragmentProgram* Modernized PSL1GHT v2 fragment program descriptor, or NULL on error.
 *
 * @complexity Time: O(N) where N is shader binary byte length. Space: O(N) allocated buffer.
 */
static rsxFragmentProgram *
rsx_modernize_fp(const void *raw_data, size_t raw_size)
{
  if(raw_data == NULL || raw_size < sizeof(realityFragmentProgram))
    return NULL;

  const uint16_t magic = *(const uint16_t *)raw_data;
  if(magic != 0x4650)
    return NULL;

  /* In PSL1GHT v2 rsxFragmentProgram, offset 2 is _pad0 which is strictly 0.
   * In legacy libreality, offset 2 is num_attrib which is non-zero (>= 2). */
  const uint16_t pad_or_num_attrib = ((const uint16_t *)raw_data)[1];
  if(pad_or_num_attrib == 0) {
    /* Already in modern PSL1GHT v2 format; clone into independent heap buffer */
    rsxFragmentProgram *fp = malloc(raw_size);
    if(fp != NULL)
      memcpy(fp, raw_data, raw_size);
    return fp;
  }

  const realityFragmentProgram *old_fp = (const realityFragmentProgram *)raw_data;
  const uint16_t num_attrib = old_fp->num_attrib;
  const uint32_t num_const  = old_fp->num_const;
  const uint16_t num_insn   = old_fp->num_insn;
  const uint32_t num_regs   = old_fp->num_regs;
  const uint32_t attrib_off = old_fp->attrib_off;
  const uint32_t const_off  = old_fp->const_off;
  const uint32_t ucode_off  = old_fp->ucode_off;

  /* Basic safety boundary validation */
  if(attrib_off >= raw_size || (num_const > 0 && const_off >= raw_size) || ucode_off >= raw_size)
    return NULL;

  const realityProgramAttrib *old_attrs = (const realityProgramAttrib *)((const char *)raw_data + attrib_off);

  /* In libreality fragment shaders, all non-attribute data (const offset tables,
   * constant descriptors, and string table) resides between old_payload_start and ucode_off.
   * old_payload_start = attrib_off + (num_attrib * 8). */
  const uint32_t old_payload_start = attrib_off + (num_attrib * sizeof(realityProgramAttrib));
  if(old_payload_start > raw_size || old_payload_start > ucode_off)
    return NULL;

  /* Layout modern PSL1GHT v2 structure:
   * 0x00 .. 0x3f: rsxFragmentProgram header (padded to 64 bytes)
   * 0x40 .. : rsxProgramAttrib array (num_attrib * 12 bytes)
   * Followed by: intermediate payload (offset tables + constants + strings)
   * Followed by: hardware microcode aligned to 16 bytes */
  const uint32_t new_attr_off    = 64;
  const uint32_t new_payload_off = new_attr_off + (num_attrib * sizeof(rsxProgramAttrib));
  const uint32_t payload_len     = ucode_off - old_payload_start;
  const uint32_t new_ucode_off   = (new_payload_off + payload_len + 15) & ~15;
  const uint32_t ucode_len       = num_insn * 16;
  const uint32_t new_total_size  = new_ucode_off + ucode_len;

  rsxFragmentProgram *new_fp = calloc(1, new_total_size);
  if(new_fp == NULL)
    return NULL;

  char *dst_base = (char *)new_fp;
  const char *src_base = (const char *)raw_data;

  /* Copy intermediate payload and microcode */
  if(payload_len > 0)
    memcpy(dst_base + new_payload_off, src_base + old_payload_start, payload_len);
  if(ucode_len > 0 && ucode_off + ucode_len <= raw_size)
    memcpy(dst_base + new_ucode_off, src_base + ucode_off, ucode_len);

  const int32_t delta = (int32_t)new_payload_off - (int32_t)old_payload_start;

  /* Populate converted 12-byte rsxProgramAttrib array */
  rsxProgramAttrib *new_attrs = (rsxProgramAttrib *)(dst_base + new_attr_off);
  for(uint16_t i = 0; i < num_attrib; i++) {
    new_attrs[i].name_off = (old_attrs[i].name_off != 0) ? (old_attrs[i].name_off + delta) : 0;
    new_attrs[i].index    = old_attrs[i].index;
    new_attrs[i].type     = 0;
    new_attrs[i]._pad0[0] = 0;
    new_attrs[i]._pad0[1] = 0;
    new_attrs[i]._pad0[2] = 0;
  }

  /* If constants exist, adjust their internal pointers (name_off and offset table index) */
  const uint32_t new_const_off = (num_const > 0) ? (const_off + delta) : 0;
  if(num_const > 0 && new_const_off < new_total_size) {
    rsxProgramConst *new_consts = (rsxProgramConst *)(dst_base + new_const_off);
    for(uint32_t i = 0; i < num_const; i++) {
      if(new_consts[i].name_off != 0)
        new_consts[i].name_off += delta;
      /* In fragment programs, c->index holds byte offset from fp to rsxConstOffsetTable */
      new_consts[i].index += delta;

      /* Remap legacy libreality shader parameter types to PSL1GHT v2 constants.
       * In legacy libreality (2011 cgcomp):
       * - type 4 with count 4 represents a 4x4 float matrix uniform ('u_colormtx').
       *   In PSL1GHT v2, PARAM_FLOAT4x4 is 11, whereas type 4 is PARAM_BOOL4.
       *   If left as type 4, rsxSetFragmentProgramParameter only writes 1 vector (row 0),
       *   leaving rows 1, 2, 3 uninitialized (zeros) in RSX GDDR3 microcode, causing the
       *   fragment shader to evaluate green and blue components to 0 (completely black video).
       * - type 3 with count 1 represents a float4 vector uniform ('u_color').
       *   In PSL1GHT v2, PARAM_FLOAT4 is 9, whereas type 3 is PARAM_BOOL3.
       * - type 0 with count 1 represents a scalar float uniform ('u_blend').
       *   In PSL1GHT v2, PARAM_FLOAT is 5, whereas type 0 is PARAM_BOOL.
       */
      if(new_consts[i].count == 4 && (new_consts[i].type == 4 || new_consts[i].type == PARAM_FLOAT4x4)) {
        new_consts[i].type = PARAM_FLOAT4x4;
      } else if(new_consts[i].type == 3 || (new_consts[i].count == 1 && new_consts[i].type == PARAM_FLOAT4)) {
        new_consts[i].type = PARAM_FLOAT4;
      } else if(new_consts[i].type == 0 && new_consts[i].count == 1) {
        new_consts[i].type = PARAM_FLOAT;
      }
    }
  }

  /**
   * Hardware RSX/NV40 Texture Coordinate Routing & Control (NV40_3D_TEX_COORD_CONTROL):
   * In NV40 architecture, rsxLoadFragmentProgramLocation programs register 0x0B40 + unit*4
   * for each bit set in fp->texcoords (units 0 through 9). The written payload is:
   *   (texcoord3D_bit ? 0x10 : 0) | (texcoord2D_bit ? 1 : 0)
   * If a bit in fp->texcoords is 0, the write to that unit is completely bypassed,
   * leaving whatever stale state was previously programmed in that hardware register.
   *
   * Architectural Requirements:
   * 1. fp->texcoords MUST have all 10 interpolator units enabled (0x3FF). This guarantees
   *    that rsxLoadFragmentProgramLocation will explicitly update NV40_3D_TEX_COORD_CONTROL(0..9)
   *    on every shader bind, deterministically resetting inactive units to 0.
   * 2. In NV40 / RPCS3, when an interpolator is marked 2D in texcoord2D, the hardware
   *    forces the 3rd vector component (Z) to 0.0f. In Movian shaders, interpolator 0
   *    (TEX0, attribute index 4) is 'f_col_mul' (RGBA color multiplier) and interpolator 1
   *    (TEX1, attribute index 5) is 'f_col_off' (RGBA color offset). Marking unit 0 as 2D
   *    eradicates the Blue channel (Z) from all UI drawing, turning white and grey theme
   *    colors into pure yellow/olive (R == G, B == 0).
   * 3. The authentic 2D texture coordinates in UI shaders ('f_tex.fp', 'f_tex_blur.fp', etc.)
   *    reside at attribute index 6 ('f_tex'), which corresponds to interpolator unit 2
   *    (unit = index - 4 = 6 - 4 = 2). In video shaders ('yuv2rgb_1f_norm.fp'), 'f_tex0' is
   *    at index 4 (unit 0) and 'f_tex1' is at index 5 (unit 1).
   * 4. Texture sampler bindings ('u_t0'..'u_t15') are uniforms, not interpolators. They must
   *    never be used to compute texcoords or texcoord2D bitmasks.
   */
  uint16_t active_texcoords = 0x3ff;
  uint16_t active_texcoord2D = 0;

  for(uint16_t i = 0; i < num_attrib; i++) {
    if(old_attrs[i].name_off != 0 && old_attrs[i].name_off < raw_size) {
      const char *name = (const char *)raw_data + old_attrs[i].name_off;
      if((strncmp(name, "f_tex", 5) == 0 || strncmp(name, "f_Tex", 5) == 0) &&
         old_attrs[i].index >= 4 && old_attrs[i].index < 14) {
        uint32_t unit = old_attrs[i].index - 4;
        active_texcoord2D |= (1u << unit);
      }
    }
  }

  /* Fill v2 fragment program header */
  new_fp->magic       = 0x4650;
  new_fp->_pad0       = 0;
  new_fp->num_regs    = (uint16_t)(num_regs >= 2 ? num_regs : 2);
  new_fp->num_attr    = num_attrib;
  new_fp->num_const   = (uint16_t)num_const;
  new_fp->num_insn    = num_insn;
  new_fp->attr_off    = new_attr_off;
  new_fp->const_off   = new_const_off;
  new_fp->ucode_off   = new_ucode_off;
  new_fp->fp_control  = 0;
  new_fp->texcoords   = active_texcoords;
  new_fp->texcoord2D  = active_texcoord2D;
  new_fp->texcoord3D  = 0;
  new_fp->_pad1       = 0;

  return new_fp;
}

/**
 * @brief Loads and parses a precompiled RSX vertex shader from the dataroot bundle.
 *
 * Fetches the binary shader blob created by cgcomp offline, transparently modernizes
 * legacy libreality payloads into PSL1GHT v2 rsxVertexProgram structures, extracts microcode
 * references, and binds uniform/attribute indices for high-performance dispatch.
 *
 * @param filename File basename within dataroot://res/shaders/rsx/ (e.g. "v1.vp").
 * @return rsx_vp_t* Allocated vertex shader descriptor or NULL on failure.
 *
 * @complexity Time: O(U + A + S) where U is uniform count, A is attribute count,
 *                  and S is shader file size. Space: O(S) heap allocation.
 */
static rsx_vp_t *
load_vp(const char *filename)
{
  char errmsg[100];
  buf_t *b;
  char url[512];

  snprintf(url, sizeof(url), "dataroot://res/shaders/rsx/%s", filename);

  /* Load compiled shader binary payload from file access VFS */
  if((b = fa_load(url, FA_LOAD_ERRBUF(errmsg, sizeof(errmsg)), NULL)) == NULL) {
    TRACE(TRACE_ERROR, "glw", "Unable to load shader %s -- %s\n", url, errmsg);
    return NULL;
  }

  /* Modernize legacy libreality format if needed */
  rsxVertexProgram *vp = rsx_modernize_vp(b->b_ptr, b->b_size);
  buf_release(b);
  if(vp == NULL) {
    TRACE(TRACE_ERROR, "glw", "Failed to parse/modernize vertex shader %s\n", url);
    return NULL;
  }

  rsx_vp_t *rvp = calloc(1, sizeof(rsx_vp_t));
  if(rvp == NULL) {
    free(vp);
    return NULL;
  }

  rvp->rvp_binary = vp;

  /* Extract microcode pointer and byte length */
  rsxVertexProgramGetUCode(vp, &rvp->rvp_ucode, &rvp->rvp_ucode_size);

  /* Cache vertex program uniform register indices */
  rvp->rvp_u_modelview    = vp_get_vector_const(vp, "u_modelview");
  rvp->rvp_u_color        = vp_get_vector_const(vp, "u_color");
  rvp->rvp_u_color_offset = vp_get_vector_const(vp, "u_color_offset");
  rvp->rvp_u_blur         = vp_get_vector_const(vp, "u_blur");

  /* Cache vertex program attribute input slot indices */
  rvp->rvp_a_position = glw_rsx_vp_get_attrib_index(vp, "a_position");
  rvp->rvp_a_color    = glw_rsx_vp_get_attrib_index(vp, "a_color");
  rvp->rvp_a_texcoord = glw_rsx_vp_get_attrib_index(vp, "a_texcoord");

  return rvp;
}

/**
 * @brief Loads and parses a precompiled RSX fragment shader from the dataroot bundle.
 *
 * Allocates 256-byte aligned GDDR3 VRAM memory for fragment microcode, transparently
 * modernizes legacy libreality payloads into PSL1GHT v2 rsxFragmentProgram structures,
 * uploads the microcode payload into RSX memory space, and caches uniform descriptor pointers.
 *
 * @param gr Graphics root context pointer.
 * @param filename File basename within dataroot://res/shaders/rsx/ (e.g. "f_tex.fp").
 * @return rsx_fp_t* Allocated fragment shader descriptor or NULL on failure.
 *
 * @complexity Time: O(M) where M is microcode length. Space: O(M) in RSX GDDR3 RAM.
 */
static rsx_fp_t *
load_fp(glw_root_t *gr, const char *filename)
{
  char errmsg[100];
  buf_t *b;
  char url[512];

  snprintf(url, sizeof(url), "dataroot://res/shaders/rsx/%s", filename);

  if((b = fa_load(url, FA_LOAD_ERRBUF(errmsg, sizeof(errmsg)), NULL)) == NULL) {
    TRACE(TRACE_ERROR, "glw", "Unable to load shader %s -- %s\n", url, errmsg);
    return NULL;
  }

  /* Modernize legacy libreality format if needed */
  rsxFragmentProgram *fp = rsx_modernize_fp(b->b_ptr, b->b_size);
  buf_release(b);
  if(fp == NULL) {
    TRACE(TRACE_ERROR, "glw", "Failed to parse/modernize fragment shader %s\n", url);
    return NULL;
  }

  /* Query microcode offset and length */
  void *ucode = NULL;
  uint32_t ucode_size = 0;
  rsxFragmentProgramGetUCode(fp, &ucode, &ucode_size);

  /* Allocate 256-byte aligned memory in RSX GDDR3 VRAM for fragment shader microcode */
  int offset = rsx_alloc(ucode_size, 256);
  if(offset == -1) {
    TRACE(TRACE_ERROR, "glw", "Unable to allocate RSX VRAM for shader %s", url);
    free(fp);
    return NULL;
  }

  uint32_t *buf = rsx_to_ppu(offset);
  memcpy(buf, ucode, ucode_size);

  rsx_fp_t *rfp = calloc(1, sizeof(rsx_fp_t));
  if(rfp == NULL) {
    rsx_free(offset, ucode_size);
    free(fp);
    return NULL;
  }

  rfp->rfp_binary = fp;
  rfp->rfp_rsx_location = offset;

  /* Resolve uniform parameter pointers for microcode uniform patching */
  rfp->rfp_u_color        = rsxFragmentProgramGetConst(fp, "u_color");
  rfp->rfp_u_color_matrix = rsxFragmentProgramGetConst(fp, "u_colormtx");
  rfp->rfp_u_blend        = rsxFragmentProgramGetConst(fp, "u_blend");

  /* Cache texture sampler binding attribute indices */
  for(int i = 0; i < 6; i++) {
    char name[8];
    snprintf(name, sizeof(name), "u_t%d", i);
    rfp->rfp_texunit[i] = glw_rsx_fp_get_attrib_index(fp, name);
  }

  return rfp;
}

/**
 * @brief Directly emits NV40 vertex program constants into the RSX command stream.
 *
 * Bypasses the defective remainder loop in librsx.a's rsxSetVertexProgramConstants,
 * which erroneously performs fctidz (float-to-integer conversion) on remainder counts
 * (< 32 floats), corrupting modelview matrices, colors, offsets, and projection parameters.
 *
 * In NV40/G70 GCM architecture, vertex program constants are uploaded via register
 * NV40TCL_VP_UPLOAD_CONST_ID (0x1efc). The method packet format consists of:
 * - Header word: ((num_words + 1) << 18) | NV40TCL_VP_UPLOAD_CONST_ID
 * - Start index: target constant vector register index (start)
 * - Payload words: raw IEEE-754 32-bit floating point components
 *
 * @param ctx Pointer to active GCM context descriptor.
 * @param start Starting hardware constant vector register index.
 * @param values Pointer to 4-component float vector array.
 * @param num_vectors Number of 4-float vectors to upload.
 *
 * Complexity:
 * - Time Complexity: O(num_vectors) memory copy.
 * - Space Complexity: O(1) stack allocation.
 */
static inline void
glw_rsx_set_vp_constant_4f(gcmContextData *ctx, u32 start, const float *values, u32 num_vectors)
{
  /* Validate register start offset, values pointer, and vector count against unmapped constants */
  if(unlikely(start == (u32)-1 || values == NULL || num_vectors == 0))
    return;

  while(num_vectors > 0) {
    /* NV40 constant upload registers accommodate batches of up to 8 vectors (32 floats) */
    const u32 batch_vectors = num_vectors > 8 ? 8 : num_vectors;
    const u32 num_floats = batch_vectors * 4;
    const u32 total_words = 1 + 1 + num_floats; /* header + start index + float payload */

    if(unlikely(ctx->current + total_words > ctx->end)) {
      if(rsxContextCallback(ctx, total_words) != 0)
        return;
    }

    /* Method packet header specifying constant count + 1 and target register NV40TCL_VP_UPLOAD_CONST_ID */
    *(ctx->current++) = ((num_floats + 1) << 18) | NV40TCL_VP_UPLOAD_CONST_ID;

    /* Start vector constant register index */
    *(ctx->current++) = start;

    /* Copy raw IEEE-754 32-bit floats directly into command buffer without numeric conversion */
    memcpy(ctx->current, values, num_floats * sizeof(float));
    ctx->current += num_floats;

    start += batch_vectors;
    values += num_floats;
    num_vectors -= batch_vectors;
  }
}

/**
 * @brief Activates a vertex program on the RSX command stream if not already active.
 *
 * Loads the vertex program ucode instruction block into the RSX hardware using
 * rsxLoadVertexProgramBlock, and uploads all internal shader constants directly
 * via glw_rsx_set_vp_constant_4f. This prevents librsx's rsxLoadVertexProgram from
 * routing internal constants through rsxSetVertexProgramConstants where fctidz truncates
 * the 3D projection matrix.
 *
 * @param root Graphics root context pointer.
 * @param rvp Vertex program to activate.
 *
 * Complexity:
 * - Time Complexity: O(U + C) where U is ucode instructions and C is constant count.
 * - Space Complexity: O(1) stack allocation.
 */
static void
rsx_set_vp(glw_root_t *root, rsx_vp_t *rvp)
{
  if(root->gr_be.be_vp_current == rvp)
    return;

  root->gr_be.be_vp_current = rvp;

  const rsxVertexProgram *vp = rvp->rvp_binary;
  rsxLoadVertexProgramBlock(root->gr_be.be_ctx, vp, rvp->rvp_ucode);

  /* Upload internal shader constants using raw float emitter to avoid librsx fctidz truncation */
  const u16 num_consts = rsxVertexProgramGetNumConst(vp);
  const rsxProgramConst *consts = rsxVertexProgramGetConsts(vp);
  if(consts != NULL) {
    for(u16 i = 0; i < num_consts; i++) {
      if(consts[i].is_internal) {
        glw_rsx_set_vp_constant_4f(root->gr_be.be_ctx, consts[i].index, (const float *)consts[i].values, 1);
      }
    }
  }
}

/**
 * @brief Activates a fragment program on the RSX command stream if not already active.
 *
 * @param root Graphics root context pointer.
 * @param rfp Fragment program to activate.
 */
static void
rsx_set_fp(glw_root_t *root, rsx_fp_t *rfp)
{
  if(root->gr_be.be_fp_current == rfp)
    return;

  root->gr_be.be_fp_current = rfp;
  rsxLoadFragmentProgramLocation(root->gr_be.be_ctx, rfp->rfp_binary,
                                 rfp->rfp_rsx_location, GCM_LOCATION_RSX);
}

/**
 * @brief Binds texture surface and sampler parameters to an RSX texture stage.
 *
 * @param ctx GCM command buffer context pointer.
 * @param unit Texture unit index (0-15).
 * @param tex Pointer to GCM texture descriptor.
 */
static inline void
rsx_bind_texture(gcmContextData *ctx, int unit, const gcmTexture *tex)
{
  rsxLoadTexture(ctx, (u8)unit, tex);
  /* Configure texture LOD clamping: allow full range up to 12.0 (12 << 8 in 8.8 fixed-point format) */
  rsxTextureControl(ctx, (u8)unit, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
  /* Standard bilinear minification and magnification filtering without convolution */
  rsxTextureFilter(ctx, (u8)unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, 0);
  rsxTextureWrapMode(ctx, (u8)unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                     GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/**
 * @brief Dispatches the scheduled list of 2D rendering jobs into the RSX command buffer.
 *
 * Iterates through the sorted render job queue, binds pipeline state (blending, face culling,
 * vertex/fragment programs, uniform constants), maps vertex attributes, and submits
 * draw calls using immediate-mode vertex generation (rsxDrawVertexBegin / rsxDrawVertex4f / rsxDrawVertexEnd).
 *
 * @param gr Graphics root context pointer.
 *
 * @complexity Time: O(J + V) where J is render job count and V is total vertex count.
 *                  Space: O(1) in-place command buffer queuing.
 */
static void
rsx_render_unlocked(glw_root_t *gr)
{
  gcmContextData *ctx = gr->gr_be.be_ctx;
  const float *vertices = gr->gr_vertex_buffer;

  int current_blendmode = GLW_BLEND_NORMAL;

  /* Standard alpha blending: Source * As + Dest * (1 - As) */
  rsxSetBlendFunc(ctx,
                  GCM_SRC_ALPHA,
                  GCM_ONE_MINUS_SRC_ALPHA,
                  GCM_SRC_ALPHA,
                  GCM_ZERO);

  int current_frontface = GLW_CCW;
  rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);

  for(int j = 0; j < gr->gr_num_render_jobs; j++) {
    const glw_render_order_t *ro = gr->gr_render_order + j;
    const glw_render_job_t *rj = ro->job;

    /* Skip clipped or empty render jobs to prevent redundant GPU state emission and zero-primitive draws */
    if(unlikely(rj->num_indices <= 0))
      continue;

    const struct glw_backend_texture *t0 = rj->t0;
    const struct glw_backend_texture *t1 = rj->t1;
    rsx_vp_t *rvp = gr->gr_be.be_vp_1;
    rsx_fp_t *rfp;
    float rgba[4];

    if(unlikely(rj->gpa != NULL)) {
      glw_program_args_t *gpa = rj->gpa;

      if(t1 != NULL && gpa->gpa_load_texture != NULL)
        gpa->gpa_load_texture(gr, gpa->gpa_prog, gpa->gpa_aux, t1, 1);

      if(t0 != NULL && gpa->gpa_load_texture != NULL)
        gpa->gpa_load_texture(gr, gpa->gpa_prog, gpa->gpa_aux, t0, 0);

      rfp = gpa->gpa_prog->gp_fragment_program;
      rvp = gpa->gpa_prog->gp_vertex_program;

      rsx_set_vp(gr, rvp);

      if(gpa->gpa_load_uniforms != NULL)
        gpa->gpa_load_uniforms(gr, gpa->gpa_prog, gpa->gpa_aux, rj);

    } else {

      if(t0 == NULL) {
        if(t1 != NULL) {
          rfp = gr->gr_be.be_fp_flat_stencil;
          if(t1->tex.offset == 0 || t1->size == 0)
            continue;
          rsx_bind_texture(ctx, 0, &t1->tex);
        } else {
          rfp = gr->gr_be.be_fp_flat;
        }

      } else {
        if(t0->tex.offset == 0 || t0->size == 0)
          continue;

        const int doblur = rj->blur > 0.05f || (rj->flags & GLW_RENDER_BLUR_ATTRIBUTE);

        rsx_bind_texture(ctx, 0, &t0->tex);

        if(t1 != NULL) {
          rfp = doblur ? gr->gr_be.be_fp_tex_stencil_blur : gr->gr_be.be_fp_tex_stencil;
          if(t1->tex.offset == 0 || t1->size == 0)
            continue;
          rsx_bind_texture(ctx, 1, &t1->tex);
        } else {
          rfp = doblur ? gr->gr_be.be_fp_tex_blur : gr->gr_be.be_fp_tex;
        }
      }
      rsx_set_vp(gr, rvp);
    }

    /* Submit 4-vector ModelView projection matrix to vertex program via raw float emitter */
    if(likely(rvp->rvp_u_modelview != -1)) {
      glw_rsx_set_vp_constant_4f(ctx, rvp->rvp_u_modelview,
                                 rj->eyespace ? identitymtx : (const float *)&rj->m, 4);
    }

    const float alpha = rj->alpha;

    rgba[0] = rj->rgb_mul.r;
    rgba[1] = rj->rgb_mul.g;
    rgba[2] = rj->rgb_mul.b;
    rgba[3] = alpha;

    if(likely(rvp->rvp_u_color != -1))
      glw_rsx_set_vp_constant_4f(ctx, rvp->rvp_u_color, rgba, 1);

    /* Update blend equation when transitioning between normal and additive */
    if(unlikely(current_blendmode != rj->blendmode)) {
      current_blendmode = rj->blendmode;
      switch(rj->blendmode) {
      case GLW_BLEND_ADDITIVE:
        rsxSetBlendFunc(ctx,
                        GCM_SRC_COLOR,
                        GCM_ONE,
                        GCM_SRC_ALPHA,
                        GCM_ONE);
        break;
      case GLW_BLEND_NORMAL:
        rsxSetBlendFunc(ctx,
                        GCM_SRC_ALPHA,
                        GCM_ONE_MINUS_SRC_ALPHA,
                        GCM_SRC_ALPHA,
                        GCM_ZERO);
        break;
      }
    }

    /* Update polygon winding orientation when changed */
    if(unlikely(current_frontface != rj->frontface)) {
      current_frontface = rj->frontface;
      rsxSetFrontFace(ctx,
                      current_frontface == GLW_CW ? GCM_FRONTFACE_CW :
                      GCM_FRONTFACE_CCW);
    }

    if(rvp->rvp_u_color_offset != -1) {
      rgba[0] = rj->rgb_off.r;
      rgba[1] = rj->rgb_off.g;
      rgba[2] = rj->rgb_off.b;
      rgba[3] = 0.0f;
      glw_rsx_set_vp_constant_4f(ctx, rvp->rvp_u_color_offset, rgba, 1);
    }

    if(rfp == gr->gr_be.be_fp_tex_blur && t0 != NULL && rvp->rvp_u_blur != -1) {
      float v[4];
      v[0] = rj->blur;
      v[1] = 1.5f / (float)t0->tex.width;
      v[2] = 1.5f / (float)t0->tex.height;
      v[3] = 0.0f;
      glw_rsx_set_vp_constant_4f(ctx, rvp->rvp_u_blur, v, 1);
    }

    rsx_set_fp(gr, rfp);

    /* Begin hardware primitive generation */
    rsxDrawVertexBegin(ctx, rj->primitive_type);

    const uint16_t *idx = gr->gr_index_buffer + rj->index_offset;

    for(int i = 0; i < rj->num_indices; i++) {
      const float *v = &vertices[idx[i] * VERTEX_SIZE];

      /* Attribute stream: texcoord (guarded against unmapped attribute slot index -1) */
      if(likely(rvp->rvp_a_texcoord != -1))
        rsxDrawVertex4f(ctx, (u8)rvp->rvp_a_texcoord, &v[8]);

      /* Attribute stream: vertex color */
      if(unlikely(rvp->rvp_a_color != -1))
        rsxDrawVertex4f(ctx, (u8)rvp->rvp_a_color, &v[4]);

      /* Primary vertex position slot 0 (emits vertex in hardware) */
      rsxDrawVertex4f(ctx, 0, &v[0]);
    }

    rsxDrawVertexEnd(ctx);
  }
}

/**
 * @brief Pre-compiles and initializes all core RSX UI shaders.
 *
 * @param gr Graphics root context pointer.
 * @return int 0 on success.
 */
int
glw_rsx_init_context(glw_root_t *gr)
{
  glw_backend_root_t *be = &gr->gr_be;

  gr->gr_be_render_unlocked = rsx_render_unlocked;

  be->be_vp_1          = load_vp("v1.vp");
  be->be_fp_tex        = load_fp(gr, "f_tex.fp");
  be->be_fp_flat       = load_fp(gr, "f_flat.fp");
  be->be_fp_tex_blur   = load_fp(gr, "f_tex_blur.fp");
  be->be_fp_tex_stencil  = load_fp(gr, "f_tex_stencil.fp");
  be->be_fp_flat_stencil = load_fp(gr, "f_flat_stencil.fp");
  be->be_fp_tex_stencil_blur = load_fp(gr, "f_tex_stencil_blur.fp");

  be->be_yuv2rgb_1f.gp_vertex_program =
  be->be_yuv2rgb_2f.gp_vertex_program = load_vp("yuv2rgb_v.vp");

  be->be_yuv2rgb_1f.gp_fragment_program = load_fp(gr, "yuv2rgb_1f_norm.fp");
  be->be_yuv2rgb_2f.gp_fragment_program = load_fp(gr, "yuv2rgb_2f_norm.fp");

  return 0;
}

void
glw_rtt_init(glw_root_t *gr, glw_rtt_t *grtt, int width, int height, int alpha)
{
}

void
glw_rtt_enter(glw_root_t *gr, glw_rtt_t *grtt, glw_rctx_t *rc)
{
}

void
glw_rtt_restore(glw_root_t *gr, glw_rtt_t *grtt)
{
}

void
glw_rtt_destroy(glw_root_t *gr, glw_rtt_t *grtt)
{
}

struct glw_program *
glw_make_program(struct glw_root *gr,
                 const char *vertex_shader,
                 const char *fragment_shader)
{
  return NULL;
}

void
glw_destroy_program(struct glw_root *gr, struct glw_program *gp)
{
}
