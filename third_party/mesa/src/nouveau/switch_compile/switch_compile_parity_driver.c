// SPDX-License-Identifier: MIT
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWITCH_XR_PLANE_WORDS 6
#define SWITCH_SPLIT_MAX_UBOS 6

#define VK_SUCCESS 0
#define VK_SHADER_STAGE_VERTEX_BIT 0x00000001u
#define VK_SHADER_STAGE_FRAGMENT_BIT 0x00000010u
#define VK_SHADER_STAGE_COMPUTE_BIT 0x00000020u

#define VK_DESCRIPTOR_TYPE_SAMPLER 0
#define VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE 2
#define VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER 6
#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 7

struct parity_push_range {
   uint32_t stage_flags;
   uint32_t offset;
   uint32_t size;
};

struct parity_stage_info {
   const uint32_t *spirv;
   size_t spirv_bytes;
   const char *entry;
   uint32_t stage;
   const void *specialization;
   uint64_t layout;
   uint64_t pipeline_flags;
   const void *pipeline_pnext;
   const void *stage_pnext;
   bool sample_shading;
   bool split;
   uint32_t half_targets;
   uint32_t const_push_mask;
   uint32_t const_push_words[32];
   uint64_t center_frag_depth;
   const void *semantic;
   const void *portable_context;
   bool (*portable)(const void *context, const uint32_t **words, size_t *bytes);
   bool texs_f16;
   bool turned;
   uint64_t xr_plane_bindings[SWITCH_XR_PLANE_WORDS];
   uint32_t unorm_targets;
};

struct parity_stage {
   uint8_t header[80];
   const void *code;
   uint32_t code_bytes;
   const void *data;
   uint32_t data_bytes;
   uint8_t cbuf_types[16];
   uint32_t cbuf_count;
   uint8_t num_gprs;
   bool early_fragment_tests;
   bool writes_depth;
   bool post_depth_coverage;
   const void *split;
   void *owner;
   uint32_t interlock_control;
   uint64_t uses_sample_shading;
   uint32_t semantic_version;
   uint8_t bound_textures;
   uint8_t num_barriers;
   uint16_t local_size[3];
   uint16_t smem_size;
   uint32_t slm_size, crs_size;
   uint8_t cbuf_sets[16];
   uint32_t cbuf_offsets[16];
};

struct parity_metrics {
   uint32_t version, code_bytes, instructions, gprs, max_warps;
   uint32_t scratch_bytes, memory_spills, memory_fills, register_spills, register_fills;
   uint64_t static_cycles;
};

struct parity_split_layout {
   uint32_t frontier_bytes;
   uint8_t frontier_cbuf;
   uint8_t prologue_cbufs;
   uint8_t shader_data_cbuf;
   uint8_t ubo_count;
   uint8_t ubo_cbuf[SWITCH_SPLIT_MAX_UBOS];
   uint16_t batch_version;
   uint16_t batch_threads;
   uint32_t frontier_values;
   uint32_t prologue_instrs;
};

struct parity_split_cost {
   uint16_t version, reserved;
   uint32_t launch_cycles, drain_cycles;
   uint32_t prologue_warp_cycles, prologue_instructions;
   uint32_t saved_pixel_instructions, saved_pixel_cycles, frontier_values;
};

struct parity_split_parts {
   struct parity_stage fs;
   struct parity_stage prologue;
   struct parity_split_layout layout;
   uint32_t qmd[64];
   uint32_t threshold_pixels;
   struct parity_split_cost cost;
};

struct parity_binding {
   uint32_t binding;
   int32_t type;
   uint32_t count;
   uint32_t flags;
   uint32_t offset, stride;
};

struct parity_set {
   const struct parity_binding *bindings;
   uint32_t binding_count;
   uint32_t flags;
};

struct parity_layout {
   const struct parity_set *sets;
   uint32_t set_count;
   uint32_t create_flags;
   const struct parity_push_range *push_ranges;
   uint32_t push_range_count;
};

extern void switch_compile_abi(uint32_t sizes[3]);
extern int32_t switch_compile_stage(const void *frozen,
                                    const struct parity_stage_info *info,
                                    const struct parity_layout *layout,
                                    struct parity_stage *out);
extern void switch_compile_stage_finish(struct parity_stage *stage);
extern void switch_compile_split_parts(const void *split,
                                       struct parity_split_parts *out);
extern bool switch_compile_stage_metrics(const struct parity_stage *stage,
                                         struct parity_metrics *out,
                                         size_t bytes);

static const _Alignas(8) uint8_t kFrozenGm20b[600] = {
    0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2e, 0x01, 0x4e, 0x56, 0x49, 0x44, 0x49, 0x41, 0x20, 0x54, 0x65, 0x67,
    0x72, 0x61, 0x20, 0x58, 0x31, 0x20, 0x28, 0x47, 0x4d, 0x32, 0x30, 0x42, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x4d, 0x32, 0x30, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x35, 0x01, 0x02, 0x00, 0x01, 0x40, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0xb5, 0xb0, 0x2d, 0x90, 0x97, 0xb1, 0x40, 0xa1,
    0xc0, 0xb1, 0x6f, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint32_t *
read_file_words(const char *path, size_t *out_bytes)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "failed to open %s\n", path);
      return NULL;
   }
   if (fseek(f, 0, SEEK_END) != 0) {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz <= 0 || (sz % 4) != 0) {
      fclose(f);
      return NULL;
   }
   rewind(f);
   uint32_t *buf = malloc((size_t)sz);
   if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
      free(buf);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *out_bytes = (size_t)sz;
   return buf;
}

static void
write_exact(FILE *out, const void *ptr, size_t n)
{
   if (n > 0 && fwrite(ptr, 1, n, out) != n) {
      fprintf(stderr, "write failed\n");
      exit(2);
   }
}

static void
write_u8(FILE *out, uint8_t v)
{
   write_exact(out, &v, sizeof(v));
}

static void
write_u16(FILE *out, uint16_t v)
{
   write_exact(out, &v, sizeof(v));
}

static void
write_u32(FILE *out, uint32_t v)
{
   write_exact(out, &v, sizeof(v));
}

static void
write_u64(FILE *out, uint64_t v)
{
   write_exact(out, &v, sizeof(v));
}

static void
dump_stage(FILE *out, const struct parity_stage *s)
{
   write_exact(out, s->header, sizeof(s->header));
   write_u32(out, s->code_bytes);
   write_exact(out, s->code, s->code_bytes);
   write_u32(out, s->data_bytes);
   write_exact(out, s->data, s->data_bytes);
   write_u32(out, s->cbuf_count);
   write_exact(out, s->cbuf_types, sizeof(s->cbuf_types));
   write_exact(out, s->cbuf_sets, sizeof(s->cbuf_sets));
   write_exact(out, s->cbuf_offsets, sizeof(s->cbuf_offsets));
   write_u8(out, s->num_gprs);
   write_u8(out, s->early_fragment_tests ? 1 : 0);
   write_u8(out, s->writes_depth ? 1 : 0);
   write_u8(out, s->post_depth_coverage ? 1 : 0);
   write_u32(out, s->interlock_control);
   write_u64(out, s->uses_sample_shading);
   write_u32(out, s->semantic_version);
   write_u8(out, s->bound_textures);
   write_u8(out, s->num_barriers);
   write_u16(out, s->local_size[0]);
   write_u16(out, s->local_size[1]);
   write_u16(out, s->local_size[2]);
   write_u16(out, s->smem_size);
   write_u32(out, s->slm_size);
   write_u32(out, s->crs_size);
}

static int
run_case(FILE *out, const char *name, const char *spv_path,
         const struct parity_stage_info *tmpl,
         const struct parity_layout *layout, bool require_split)
{
   size_t bytes = 0;
   uint32_t *words = read_file_words(spv_path, &bytes);
   if (!words)
      return 1;

   struct parity_stage_info info = *tmpl;
   info.spirv = words;
   info.spirv_bytes = bytes;
   info.entry = "main";

   struct parity_stage stage;
   memset(&stage, 0, sizeof(stage));
   int32_t res = switch_compile_stage(kFrozenGm20b, &info, layout, &stage);
   free(words);
   if (res != VK_SUCCESS || !stage.code || stage.code_bytes == 0) {
      fprintf(stderr, "case %s failed: res=%d code_bytes=%u\n", name, res, stage.code_bytes);
      return 1;
   }

   struct parity_metrics metrics;
   memset(&metrics, 0, sizeof(metrics));
   if (!switch_compile_stage_metrics(&stage, &metrics, sizeof(metrics))) {
      fprintf(stderr, "case %s metrics failed\n", name);
      switch_compile_stage_finish(&stage);
      return 1;
   }

   if (require_split && !stage.split) {
      fprintf(stderr, "case %s expected split != NULL\n", name);
      switch_compile_stage_finish(&stage);
      return 1;
   }

   uint32_t name_len = (uint32_t)strlen(name);
   write_u32(out, name_len);
   write_exact(out, name, name_len);
   dump_stage(out, &stage);
   write_u32(out, metrics.version);
   write_u32(out, metrics.code_bytes);
   write_u32(out, metrics.instructions);
   write_u32(out, metrics.gprs);
   write_u32(out, metrics.max_warps);
   write_u32(out, metrics.scratch_bytes);
   write_u32(out, metrics.memory_spills);
   write_u32(out, metrics.memory_fills);
   write_u32(out, metrics.register_spills);
   write_u32(out, metrics.register_fills);
   write_u64(out, metrics.static_cycles);

   write_u8(out, stage.split ? 1 : 0);
   uint32_t split_fs_bytes = 0, split_prologue_bytes = 0;
   if (stage.split) {
      struct parity_split_parts parts;
      memset(&parts, 0, sizeof(parts));
      switch_compile_split_parts(stage.split, &parts);
      dump_stage(out, &parts.fs);
      dump_stage(out, &parts.prologue);
      write_u32(out, parts.layout.frontier_bytes);
      write_u8(out, parts.layout.frontier_cbuf);
      write_u8(out, parts.layout.prologue_cbufs);
      write_u8(out, parts.layout.shader_data_cbuf);
      write_u8(out, parts.layout.ubo_count);
      write_exact(out, parts.layout.ubo_cbuf, sizeof(parts.layout.ubo_cbuf));
      write_u16(out, parts.layout.batch_version);
      write_u16(out, parts.layout.batch_threads);
      write_u32(out, parts.layout.frontier_values);
      write_u32(out, parts.layout.prologue_instrs);
      write_exact(out, parts.qmd, sizeof(parts.qmd));
      write_u32(out, parts.threshold_pixels);
      write_u16(out, parts.cost.version);
      write_u16(out, parts.cost.reserved);
      write_u32(out, parts.cost.launch_cycles);
      write_u32(out, parts.cost.drain_cycles);
      write_u32(out, parts.cost.prologue_warp_cycles);
      write_u32(out, parts.cost.prologue_instructions);
      write_u32(out, parts.cost.saved_pixel_instructions);
      write_u32(out, parts.cost.saved_pixel_cycles);
      write_u32(out, parts.cost.frontier_values);
      split_fs_bytes = parts.fs.code_bytes;
      split_prologue_bytes = parts.prologue.code_bytes;
   }

   printf("case=%s code=%u data=%u instrs=%u gprs=%u cbufs=%u split=%d(fs=%u,pro=%u)\n",
          name, stage.code_bytes, stage.data_bytes, metrics.instructions,
          stage.num_gprs, stage.cbuf_count, stage.split ? 1 : 0,
          split_fs_bytes, split_prologue_bytes);

   switch_compile_stage_finish(&stage);
   return 0;
}

int
switch_parity_run(int argc, char **argv)
{
   if (argc != 8) {
      fprintf(stderr,
              "usage: %s <out.bin> <vert.spv> <frag_half.spv> <frag_texs.spv> "
              "<frag_turned.spv> <frag_split.spv> <comp.spv>\n",
              argv[0]);
      return 2;
   }

   uint32_t sizes[3] = {0, 0, 0};
   switch_compile_abi(sizes);
   if (sizes[0] != sizeof(struct parity_stage_info) ||
       sizes[1] != sizeof(struct parity_stage) ||
       sizes[2] != sizeof(struct parity_split_parts)) {
      fprintf(stderr, "ABI mismatch: got [%u,%u,%u] expected [%zu,%zu,%zu]\n",
              sizes[0], sizes[1], sizes[2],
              sizeof(struct parity_stage_info),
              sizeof(struct parity_stage),
              sizeof(struct parity_split_parts));
      return 1;
   }

   FILE *out = fopen(argv[1], "wb");
   if (!out) {
      fprintf(stderr, "failed to open output %s\n", argv[1]);
      return 1;
   }
   write_exact(out, sizes, sizeof(sizes));

   const struct parity_binding ubo_bind[] = {
       {.binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .offset = 0, .stride = 16},
   };
   const struct parity_set ubo_set[] = {
       {.bindings = ubo_bind, .binding_count = 1},
   };
   const struct parity_push_range vert_push[] = {
       {.stage_flags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 32},
   };
   const struct parity_layout vert_layout = {
       .sets = ubo_set,
       .set_count = 1,
       .push_ranges = vert_push,
       .push_range_count = 1,
   };
   struct parity_stage_info vert_info = {.stage = VK_SHADER_STAGE_VERTEX_BIT};
   if (run_case(out, "vert", argv[2], &vert_info, &vert_layout, false) != 0)
      return 1;

   const struct parity_push_range frag_push[] = {
       {.stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 32},
   };
   const struct parity_layout frag_half_layout = {
       .sets = ubo_set,
       .set_count = 1,
       .push_ranges = frag_push,
       .push_range_count = 1,
   };
   struct parity_stage_info frag_half_info = {
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .half_targets = 0x1u,
       .unorm_targets = 0x2u,
       .const_push_mask = 0x3u,
       .const_push_words = {0x3f800000u, 0x3f000000u},
   };
   if (run_case(out, "frag_half_unorm", argv[3], &frag_half_info, &frag_half_layout, false) != 0)
      return 1;

   const struct parity_binding tex_binds[] = {
       {.binding = 0, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1, .offset = 0, .stride = 16},
       {.binding = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1, .offset = 16, .stride = 16},
       {.binding = 2, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .count = 1, .offset = 32, .stride = 16},
   };
   const struct parity_set tex_set[] = {
       {.bindings = tex_binds, .binding_count = 3},
   };
   const struct parity_layout tex_layout = {
       .sets = tex_set,
       .set_count = 1,
   };
   struct parity_stage_info tex_info = {
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .half_targets = 0x1u,
       .texs_f16 = true,
   };
   if (run_case(out, "frag_texs_f16", argv[4], &tex_info, &tex_layout, false) != 0)
      return 1;

   const struct parity_layout empty_layout = {0};
   struct parity_stage_info turned_info = {
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .center_frag_depth = 1,
       .turned = true,
   };
   if (run_case(out, "frag_turned_depth", argv[5], &turned_info, &empty_layout, false) != 0)
      return 1;

   const struct parity_layout split_layout = {
       .sets = ubo_set,
       .set_count = 1,
   };
   struct parity_stage_info split_info = {
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .split = true,
   };
   if (run_case(out, "frag_split", argv[6], &split_info, &split_layout, true) != 0)
      return 1;

   const struct parity_binding comp_binds[] = {
       {.binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .offset = 0, .stride = 16},
       {.binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1, .offset = 16, .stride = 16},
   };
   const struct parity_set comp_set[] = {{.bindings = comp_binds, .binding_count = 2}};
   const struct parity_push_range comp_push[] = {{.stage_flags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 8}};
   const struct parity_layout comp_layout = {
       .sets = comp_set, .set_count = 1, .push_ranges = comp_push, .push_range_count = 1,
   };
   struct parity_stage_info comp_info = {.stage = VK_SHADER_STAGE_COMPUTE_BIT};
   if (run_case(out, "comp", argv[7], &comp_info, &comp_layout, false) != 0)
      return 1;
   fclose(out);
   return 0;
}
#ifdef SWITCH_PARITY_HAS_MAIN
int main(int argc, char **argv) { return switch_parity_run(argc, argv); }
#endif
