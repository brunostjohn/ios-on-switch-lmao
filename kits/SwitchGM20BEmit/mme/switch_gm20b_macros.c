/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * The raw path's draw macros (ABI v2: slots 11-16, R1 the first argument, no shadow scratch, constant buffer 0 left
 * selected), built with Mesa's MME builder. The slots are the ones NVK's draw macros take, so a push profile that counts
 * NVK's draw calls counts the raw path's too. The instance loops are NVK's pre-Pascal nvk_mme_build_draw_loop and
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

/* Metal's indirect arguments, in Vulkan's order: the count, instances, first vertex or index, (vertex offset,) first
 * instance. The root table's base vertex and instance, then the base registers, then the draws. */
static void build_indirect(struct mme_builder *b, bool indexed)
{
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
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
            "// The raw path's macro slots (ABI v2): NVK's draw macros' slots\nenum {\n"
            "    SWITCH_GM20B_MACRO_DRAW = 11,\n    SWITCH_GM20B_MACRO_DRAW_INDEXED,\n"
            "    SWITCH_GM20B_MACRO_INDIRECT,\n    SWITCH_GM20B_MACRO_INDEXED_INDIRECT,\n"
            "    SWITCH_GM20B_MACRO_DRAW_PARAMS,\n    SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS,\n};\n");
    write_macro(out, "draw", draw, false, &dev);
    write_macro(out, "draw_indexed", draw, true, &dev);
    write_macro(out, "indirect", build_indirect, false, &dev);
    write_macro(out, "indexed_indirect", build_indirect, true, &dev);
    write_macro(out, "draw_params", build_direct_params, false, &dev);
    write_macro(out, "draw_indexed_params", build_direct_params, true, &dev);
    fprintf(out, "\n#endif\n");
    return fclose(out) ? 1 : 0;
}
