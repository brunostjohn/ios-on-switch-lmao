/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * The raw path's draw macros (ABI v2: the draws at slots 11-16, the indirect command buffers' at 4-7, R1 the first
 * argument, shadow scratch read by the indirect command buffer macros only, constant buffer 0 left selected), built with
 * Mesa's MME builder. The draw slots are the ones NVK's draw macros take, so a push profile that counts NVK's draw calls
 * counts the raw path's too. The instance loops are NVK's pre-Pascal nvk_mme_build_draw_loop and
 * nvk_mme_build_draw_indexed_loop (src/nouveau/vulkan/nvk_cmd_draw.c, commit 77452b9092cc), with their Turing and
 * Pascal branches dropped; the calling convention around them is raw's (drivers/TegraGM20BMetal/raw/runtime/raw_mme.h),
 * and a zero instance count skips the loop, which on Fermi's MME tests its condition at the end.
 *
 * Writes the macros' words as a C header: gen_macros <out.h>.
 */

#include <stdio.h>
#include <stdlib.h>

#include "drf.h"
#include "mme_builder.h"
#include "nv_push_cl9097.h"
#include "nv_push_clb197.h"
#include "nv_push_clc597.h"

/* Shadow scratch words no NVK macro the channel runs reads (NVK's own end at NVK_MME_NUM_SCRATCH, about 100) */
enum {
    SWITCH_GM20B_SCRATCH_ICB_LOCATION = 120,
    SWITCH_GM20B_SCRATCH_ICB_LENGTH = 121
};

/* mme_tu104.c's disassembler names Turing's methods; this tool builds Fermi macros and prints none */
const char *P_PARSE_NVC597_MTHD(uint16_t idx)
{
    (void)idx;
    return "?";
}

static void nvk_mme_build_draw_loop(struct mme_builder *b, struct mme_value instance_count, struct mme_value first,
                                    struct mme_value count, uint16_t first_mthd)
{
    uint32_t begin_initial_value;
    V_NVB197_BEGIN(begin_initial_value, {
                                            .primitive_id = PRIMITIVE_ID_FIRST,
                                            .instance_id = INSTANCE_ID_FIRST,
                                            .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
                                        });
    struct mme_value begin = mme_mov(b, mme_imm(begin_initial_value));

    mme_start_loop(b, instance_count);

    mme_mthd(b, NV9097_BEGIN);
    mme_emit(b, begin);

    mme_mthd(b, first_mthd);
    mme_emit(b, first);
    mme_emit(b, count);

    mme_mthd(b, NV9097_END);
    mme_emit(b, mme_zero());

    mme_set_field_enum(b, begin, NV9097_BEGIN_INSTANCE_ID, SUBSEQUENT);
    mme_end_loop(b);

    mme_free_reg(b, begin);
}

static void build_direct(struct mme_builder *b, uint16_t first_mthd)
{
    struct mme_value instances = mme_load(b);
    struct mme_value first = mme_load(b);
    struct mme_value count = mme_load(b);
    mme_if(b, ine, instances, mme_zero())
    {
        nvk_mme_build_draw_loop(b, instances, first, count, first_mthd);
    }
}

/* A draw that writes its draw parameters first: the root table's base vertex and instance and the base registers, as
 * nvk_mme_build_set_draw_params writes them before Turing. Arguments: instances, first vertex or index, count, then the
 * base instance (a draw of vertices: its first vertex is the root's base vertex, the base registers 0), or the base
 * vertex and the base instance (an indexed draw). */
static void build_direct_params(struct mme_builder *b, bool indexed)
{
    struct mme_value instances = mme_load(b);
    struct mme_value first = mme_load(b);
    struct mme_value count = mme_load(b);
    struct mme_value base = mme_load(b);
    struct mme_value base_instance = indexed ? mme_load(b) : base;
    struct mme_value base_vertex = indexed ? base : mme_zero();

    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, mme_zero());
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    mme_emit(b, indexed ? base_vertex : first);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX);
    mme_emit(b, base_vertex);
    mme_mthd(b, NVB197_SET_VERTEX_ID_BASE);
    mme_emit(b, base_vertex);

    mme_free_reg(b, base);
    if (indexed)
        mme_free_reg(b, base_instance);

    mme_if(b, ine, instances, mme_zero())
    {
        nvk_mme_build_draw_loop(b, instances, first, count,
                                indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
    }
}

/* Metal's indirect arguments after their count and instances, in Vulkan's order: first vertex or index, (vertex offset,)
 * first instance. The root table's base vertex and instance, then the base registers, then the draws. */
static void build_indirect_body(struct mme_builder *b, bool indexed, struct mme_value count, struct mme_value instances)
{
    struct mme_value first = mme_load(b);
    struct mme_value base = mme_load(b);
    struct mme_value base_instance = indexed ? mme_load(b) : base;
    struct mme_value base_vertex = indexed ? base : mme_zero();

    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, mme_zero());
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    mme_emit(b, indexed ? base_vertex : first);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX);
    mme_emit(b, base_vertex);
    mme_mthd(b, NVB197_SET_VERTEX_ID_BASE);
    mme_emit(b, base_vertex);

    /* Fermi's MME has seven registers: the loop needs three more than the arguments it keeps */
    mme_free_reg(b, base);
    if (indexed)
        mme_free_reg(b, base_instance);

    mme_if(b, ine, count, mme_zero())
    {
        mme_if(b, ine, instances, mme_zero())
        {
            nvk_mme_build_draw_loop(b, instances, first, count,
                                    indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
        }
    }
}

static void build_indirect(struct mme_builder *b, bool indexed)
{
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    build_indirect_body(b, indexed, count, instances);
}

/* An indirect command buffer's range, as an MTLIndirectCommandBufferExecutionRange (location, length), into the shadow
 * scratch words the gated draws read */
static void build_icb_range(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value location = mme_load(b);
    struct mme_value length = mme_load(b);
    mme_mthd(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LOCATION));
    mme_emit(b, location);
    mme_emit(b, length);
}

/* An indirect command: its index in its buffer, then an indirect draw's arguments. Outside the range in scratch (index -
 * location unsigned not below length) its instance count is zero, so it loads its arguments and draws nothing: the mask is
 * the borrow of (index - location) - length, all ones where it is below. Fermi's MME compares for equality alone. */
static void build_icb_draw(struct mme_builder *b, bool indexed)
{
    struct mme_value index = mme_load(b);
    struct mme_value location = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LOCATION));
    mme_sub_to(b, index, index, location);
    mme_free_reg(b, location);
    struct mme_value length = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LENGTH));
    struct mme_value64 below = mme_sub64(b, mme_value64(index, mme_zero()), mme_value64(length, mme_zero()));
    mme_free_reg(b, index);
    mme_free_reg(b, length);
    mme_free_reg(b, below.lo);
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    mme_and_to(b, instances, instances, below.hi);
    mme_free_reg(b, below.hi);
    build_indirect_body(b, indexed, count, instances);
}

/* An indirect command buffer's dispatch: its index. The global render enable, which the compute class's launches obey
 * (NVK's conditional rendering of dispatches), is TRUE inside the range in scratch and FALSE outside it, from the same
 * borrow as build_icb_draw's; the execute sets it TRUE again after its commands. */
static void build_icb_gate(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value index = mme_load(b);
    struct mme_value location = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LOCATION));
    mme_sub_to(b, index, index, location);
    mme_free_reg(b, location);
    struct mme_value length = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LENGTH));
    struct mme_value64 below = mme_sub64(b, mme_value64(index, mme_zero()), mme_value64(length, mme_zero()));
    mme_free_reg(b, index);
    mme_free_reg(b, length);
    mme_free_reg(b, below.lo);
    mme_and_to(b, below.hi, below.hi, mme_imm(NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    mme_mthd(b, NVB197_SET_GLOBAL_RENDER_ENABLE_C);
    mme_emit(b, below.hi);
    mme_free_reg(b, below.hi);
}

static void write_macro(FILE *out, const char *name, void (*build)(struct mme_builder *, bool), bool indexed,
                        const struct nv_device_info *dev)
{
    struct mme_builder b;
    mme_builder_init(&b, dev);
    build(&b, indexed);
    size_t size;
    uint32_t *words = mme_builder_finish(&b, &size);
    fprintf(out, "\nstatic const uint32_t switch_gm20b_macro_%s[] = {", name);
    for (size_t i = 0; i < size / 4; i++)
        fprintf(out, "%s0x%08x,", i % 6 ? " " : "\n    ", words[i]);
    fprintf(out, "\n};\n");
    free(words);
}

static void draw(struct mme_builder *b, bool indexed)
{
    build_direct(b, indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *out = fopen(argv[1], "w");
    if (!out)
        return 1;
    const struct nv_device_info dev = {
        .type = NV_DEVICE_TYPE_SOC,
        .chipset = 0x12b,
        .cls_eng3d = MAXWELL_B,
    };
    fprintf(out,
            "// Generated by kits/SwitchGM20BEmit/mme/switch_gm20b_macros.c with Mesa's MME builder: do not edit.\n"
            "#ifndef SWITCH_GM20B_MACROS_H\n#define SWITCH_GM20B_MACROS_H\n\n#include <stdint.h>\n\n"
            "// The raw path's macro slots (ABI v2): NVK's draw macros' slots, and the indirect command buffers' below them\nenum {\n"
            "    SWITCH_GM20B_MACRO_ICB_RANGE = 4,\n    SWITCH_GM20B_MACRO_ICB_DRAW,\n    SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED,\n"
            "    SWITCH_GM20B_MACRO_ICB_GATE,\n"
            "    SWITCH_GM20B_MACRO_DRAW = 11,\n    SWITCH_GM20B_MACRO_DRAW_INDEXED,\n"
            "    SWITCH_GM20B_MACRO_INDIRECT,\n    SWITCH_GM20B_MACRO_INDEXED_INDIRECT,\n"
            "    SWITCH_GM20B_MACRO_DRAW_PARAMS,\n    SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS,\n};\n\n"
            "// The shadow scratch words the indirect command buffer macros keep their range in\n"
            "enum {\n    SWITCH_GM20B_SCRATCH_ICB_LOCATION = %u,\n    SWITCH_GM20B_SCRATCH_ICB_LENGTH = %u,\n};\n",
            SWITCH_GM20B_SCRATCH_ICB_LOCATION, SWITCH_GM20B_SCRATCH_ICB_LENGTH);
    write_macro(out, "draw", draw, false, &dev);
    write_macro(out, "draw_indexed", draw, true, &dev);
    write_macro(out, "indirect", build_indirect, false, &dev);
    write_macro(out, "indexed_indirect", build_indirect, true, &dev);
    write_macro(out, "draw_params", build_direct_params, false, &dev);
    write_macro(out, "draw_indexed_params", build_direct_params, true, &dev);
    write_macro(out, "icb_range", build_icb_range, false, &dev);
    write_macro(out, "icb_draw", build_icb_draw, false, &dev);
    write_macro(out, "icb_draw_indexed", build_icb_draw, true, &dev);
    write_macro(out, "icb_gate", build_icb_gate, false, &dev);
    fprintf(out, "\n#endif\n");
    return fclose(out) ? 1 : 0;
}
