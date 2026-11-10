// SPDX-License-Identifier: MIT
#include "nvk_switch_compile.h"
#include "nvk_shader.h"

#define SWITCH_EXPORT __attribute__((visibility("default")))

SWITCH_EXPORT bool
switch_nvk_stage_metrics(const struct switch_nvk_stage *stage,
                         struct switch_nvk_stage_metrics *out, size_t bytes)
{
   if (!stage || !stage->owner || !out || bytes != sizeof(*out))
      return false;
   const struct nvk_shader *shader = container_of(stage->owner, struct nvk_shader, vk);
   const struct nak_shader_info *info = &shader->info;
   *out = (struct switch_nvk_stage_metrics) {
      .version = 1,
      .code_bytes = stage->code_bytes,
      .instructions = info->num_instrs,
      .gprs = info->num_gprs,
      .max_warps = info->max_warps_per_sm,
      .scratch_bytes = info->slm_size,
      .memory_spills = info->num_spills_to_mem,
      .memory_fills = info->num_fills_from_mem,
      .register_spills = info->num_spills_to_reg,
      .register_fills = info->num_fills_from_reg,
      .static_cycles = info->num_static_cycles,
   };
   return true;
}

static bool
dominates(const struct switch_nvk_stage_metrics *a,
          const struct switch_nvk_stage_metrics *b)
{
   return a->code_bytes <= b->code_bytes && a->instructions <= b->instructions &&
      a->gprs <= b->gprs && a->max_warps >= b->max_warps && a->scratch_bytes <= b->scratch_bytes &&
      a->memory_spills <= b->memory_spills && a->memory_fills <= b->memory_fills &&
      a->register_spills <= b->register_spills && a->register_fills <= b->register_fills &&
      a->static_cycles <= b->static_cycles &&
      (a->code_bytes < b->code_bytes || a->instructions < b->instructions || a->gprs < b->gprs ||
       a->max_warps > b->max_warps || a->scratch_bytes < b->scratch_bytes ||
       a->memory_spills < b->memory_spills || a->memory_fills < b->memory_fills ||
       a->register_spills < b->register_spills || a->register_fills < b->register_fills ||
       a->static_cycles < b->static_cycles);
}

SWITCH_EXPORT VkResult
switch_nvk_compile_ranked_stage(VkDevice device,
                                const struct switch_nvk_stage_info *info,
                                struct switch_nvk_stage *out)
{
   VkResult result = switch_nvk_compile_stage(device, info, out);
   if (result != VK_SUCCESS || !out->semantic_version)
      return result;
   struct switch_nvk_stage_info portable_info = *info;
   portable_info.semantic = NULL;
   if ((!portable_info.spirv || !portable_info.spirv_bytes) &&
       (!info->portable || !info->portable(info->portable_context,
                                          &portable_info.spirv, &portable_info.spirv_bytes)))
      return VK_SUCCESS;
   struct switch_nvk_stage portable = {0};
   result = switch_nvk_compile_stage(device, &portable_info, &portable);
   if (result != VK_SUCCESS)
      return VK_SUCCESS;
   struct switch_nvk_stage_metrics native_cost, portable_cost;
   if (switch_nvk_stage_metrics(out, &native_cost, sizeof(native_cost)) &&
       switch_nvk_stage_metrics(&portable, &portable_cost, sizeof(portable_cost)) &&
       dominates(&portable_cost, &native_cost)) {
      switch_nvk_stage_finish(device, out);
      *out = portable;
   } else {
      switch_nvk_stage_finish(device, &portable);
   }
   return VK_SUCCESS;
}
