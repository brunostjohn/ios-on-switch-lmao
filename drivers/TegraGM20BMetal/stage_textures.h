// SPDX-License-Identifier: MIT
// The texture slots a render stage binds on this build's identity (MetalABI.h's kind), as its GPU's Metal argument tables
// have them (device_answers.h maxVertexTextures, maxFragmentTextures): 31 on 21H461's A10X, 128 on 24B119's A13. GM20B
// bounds neither: a texture's handle is a set-0 word its program reads.
#pragma once
#include <cstdint>
#include <string_view>
#if defined(__has_include)
#if __has_include("MetalABI.h")
#include "MetalABI.h"
#endif
#endif
#include "set_entries.h"
#include "slot_mask.h"

namespace gm20b {

constexpr uint32_t stageTexturesOf(std::string_view kind)
{
    return kind == "24B119" ? kStageTextureArgumentsA13 : kind == "21H461" ? kStageTextureArgumentsA10X : 0;
}

#ifdef SWITCH_METAL_KIND
constexpr uint32_t kStageTextures = stageTexturesOf(SWITCH_METAL_KIND);
static_assert(kStageTextures, "no render stage texture slots for this kind (stage_textures.h)");
#endif

// The raw compute encoder's texture slots, on every identity
constexpr uint32_t kKernelTextures = 31;

#ifdef SWITCH_METAL_KIND
using StageTextureMask = SlotMask<kStageTextures>;
#endif

}  // namespace gm20b
