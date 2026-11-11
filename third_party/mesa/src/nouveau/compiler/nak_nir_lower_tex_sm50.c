/*
 * Copyright © 2026 Bruno St John
 * SPDX-License-Identifier: MIT
 *
 * Maxwell's bound textures and its scalar texture forms (TEXS, TLDS, TLD4S),
 * which read the texture handle from the constant buffer
 * SET_BINDLESS_TEXTURE names, at a 13-bit word index, and take their
 * coordinates in two register operands rather than one vector.  The legal
 * forms and their operand layouts are nv50_ir's
 * (nv50_ir_ra.cpp isScalarTexGM107/handleScalarTexGM107,
 * nv50_ir_emit_gm107.cpp emitTEXS, getTEXSTarget, getTLDSTarget; MIT).
 */

#include "nak_private.h"
#include "nir_builder.h"

#define SM50_BOUND_INDEX_MAX (1u << 13)

static bool
handle_cbuf(nir_def *tex_h, uint32_t *idx, uint32_t *offset)
{
   if (tex_h == NULL || !nir_def_is_intrinsic(tex_h))
      return false;

   nir_intrinsic_instr *intrin = nir_def_as_intrinsic(tex_h);
   if (intrin->intrinsic != nir_intrinsic_ldc_nv ||
       !nir_src_is_const(intrin->src[0]) || !nir_src_is_const(intrin->src[1]))
      return false;

   *idx = nir_src_as_uint(intrin->src[0]);
   *offset = nir_intrinsic_base(intrin) + nir_src_as_uint(intrin->src[1]);
   return *offset % 4 == 0 && *offset / 4 < SM50_BOUND_INDEX_MAX;
}

static bool
tex_handles(nir_tex_instr *tex, nir_def **tex_h, nir_def **samp_h)
{
   *tex_h = *samp_h = NULL;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_texture_handle)
         *tex_h = tex->src[i].src.ssa;
      else if (tex->src[i].src_type == nir_tex_src_sampler_handle)
         *samp_h = tex->src[i].src.ssa;
   }
   return *tex_h != NULL;
}

static bool
bound_candidate(const nir_tex_instr *tex, nir_def *tex_h, nir_def *samp_h,
                uint32_t *idx, uint32_t *offset)
{
   if (tex->op == nir_texop_lod || tex->is_sparse ||
       (samp_h != NULL && samp_h != tex_h))
      return false;
   return handle_cbuf(tex_h, idx, offset);
}

/*
 * The constant buffer a fragment shader's bound texture instructions read
 * their handles from: of the buffers whose handles are single words at
 * constant offsets, the one most of its samples read.  SET_BINDLESS_TEXTURE
 * names one buffer for every stage, so only fragment shaders take bound
 * forms.  -1: none.
 */
int
nak_nir_sm50_bound_texture_cbuf(nir_shader *nir, const struct nak_compiler *nak)
{
   if (nak->sm < 50 || nak->sm >= 70 || nir->info.stage != MESA_SHADER_FRAGMENT)
      return -1;

   unsigned uses[32] = { 0 };
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            nir_def *tex_h, *samp_h;
            uint32_t idx, offset;
            if (tex_handles(tex, &tex_h, &samp_h) &&
                bound_candidate(tex, tex_h, samp_h, &idx, &offset) &&
                idx < ARRAY_SIZE(uses))
               uses[idx]++;
         }
      }
   }

   int best = -1;
   for (unsigned i = 0; i < ARRAY_SIZE(uses); i++) {
      if (uses[i] && (best < 0 || uses[i] > uses[best]))
         best = i;
   }
   return best;
}

/* The handle of tex (its texture and sampler handles, as they were) as
 * (cbuf << 16) | offset where it is a bound texture of `cbuf`,
 * NAK_NIR_TEX_REF_TYPE_CBUF's texture_index
 */
bool
nak_nir_sm50_bound_texture(const nir_tex_instr *tex, nir_def *tex_h,
                           nir_def *samp_h, int cbuf, uint32_t *index)
{
   uint32_t idx, offset;
   if (cbuf < 0 || !bound_candidate(tex, tex_h, samp_h, &idx, &offset) ||
       idx != (uint32_t)cbuf)
      return false;
   *index = (idx << 16) | offset;
   return true;
}

static void
move_comps(nir_def **from, unsigned *from_comps, nir_def **to,
           unsigned *to_comps, unsigned keep)
{
   nir_def *moved[4];
   unsigned n = *from_comps - keep;
   for (unsigned i = 0; i < n; i++)
      moved[i] = from[keep + i];
   for (unsigned i = *to_comps; i-- > 0;)
      to[i + n] = to[i];
   for (unsigned i = 0; i < n; i++)
      to[i] = moved[i];
   *from_comps = keep;
   *to_comps += n;
}

static bool
texs_form(nir_tex_instr *tex, enum nak_nir_lod_mode lod, bool array)
{
   bool zero = lod == NAK_NIR_LOD_MODE_ZERO;
   bool level = lod == NAK_NIR_LOD_MODE_LOD;
   if (lod != NAK_NIR_LOD_MODE_AUTO && !zero && !level)
      return false;

   switch (tex->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
      return !array && !tex->is_shadow && zero;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
      if (array)
         return tex->is_shadow ? zero : !level;
      return true;
   case GLSL_SAMPLER_DIM_3D:
      return !tex->is_shadow && !level;
   case GLSL_SAMPLER_DIM_CUBE:
      return !array && !tex->is_shadow && !zero;
   default:
      return false;
   }
}

static bool
tlds_form(nir_tex_instr *tex, enum nak_nir_lod_mode lod, bool array,
          bool offset)
{
   bool zero = lod == NAK_NIR_LOD_MODE_ZERO;
   switch (tex->sampler_dim) {
   case GLSL_SAMPLER_DIM_1D:
      return !array && !offset;
   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
      return !array || (zero && !offset);
   case GLSL_SAMPLER_DIM_MS:
      return !array && !offset;
   case GLSL_SAMPLER_DIM_3D:
      return zero && !offset;
   default:
      return false;
   }
}

/*
 * Whether a bound texture instruction of these sources has a scalar form, and
 * its sources rearranged as that form takes them: src0 is register A (one
 * value, or an aligned pair), src1 register B.  Before: src0 the array index
 * then the coordinates, src1 the level, the offset, the reference and the
 * sample index (nak_nir_lower_tex.c lower_tex).
 */
bool
nak_nir_sm50_scalarize_tex(nir_tex_instr *tex, enum nak_nir_lod_mode lod,
                           enum nak_nir_offset_mode offset, bool array,
                           nir_def **src0, unsigned *src0_comps,
                           nir_def **src1, unsigned *src1_comps)
{
   switch (tex->op) {
   case nir_texop_tex:
   case nir_texop_txl:
      if (offset != NAK_NIR_OFFSET_MODE_NONE || !texs_form(tex, lod, array))
         return false;
      break;
   case nir_texop_txf:
   case nir_texop_txf_ms:
      if (offset == NAK_NIR_OFFSET_MODE_PER_PX ||
          !tlds_form(tex, lod, array, offset != NAK_NIR_OFFSET_MODE_NONE))
         return false;
      /* TLDS.A2D: register A the array index, B the coordinates */
      if (array) {
         move_comps(src0, src0_comps, src1, src1_comps, 1);
         return true;
      }
      break;
   case nir_texop_tg4:
      if (array || offset == NAK_NIR_OFFSET_MODE_PER_PX ||
          (lod != NAK_NIR_LOD_MODE_AUTO && lod != NAK_NIR_LOD_MODE_ZERO) ||
          (tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
           tex->sampler_dim != GLSL_SAMPLER_DIM_RECT))
         return false;
      break;
   default:
      return false;
   }

   /* One coordinate in each register when B has nothing else, otherwise the
    * first two in A as a pair
    */
   if (*src0_comps == 2 && *src1_comps == 0)
      move_comps(src0, src0_comps, src1, src1_comps, 1);
   else if (*src0_comps > 2)
      move_comps(src0, src0_comps, src1, src1_comps, 2);

   return *src0_comps <= 2 && *src1_comps <= 2;
}
