// SPDX-License-Identifier: MIT
#pragma once
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "SwitchGM20BMacros.h"
#include "raw_push.h"
#include <iterator>

namespace raw::mme {

// Native GM20B macro ABI v2. This channel owns macro slots 11..16 (NVK's draw macros' slots, which the kernel's push profile
// counts as draws) and their contiguous instruction words from zero. Arguments: instance count (preloaded R1), first
// vertex/index, element count, then for the parameter macros the base instance (a draw of vertices) or the base vertex and
// base instance (an indexed one). The caller owns topology, restart and index binding. The plain macros change only
// BEGIN, vertex/index first+count and END; the parameter macros first write the root table's draw parameters and the base
// registers as the direct path wrote them. They are NVK's pre-Pascal draw loops, built with Mesa's MME builder
// (kits/SwitchGM20BEmit: gen/SwitchGM20BMacros.h).
constexpr uint32_t kAbi = 2, kDraw = SWITCH_GM20B_MACRO_DRAW, kDrawIndexed = SWITCH_GM20B_MACRO_DRAW_INDEXED,
                   kIndirect = SWITCH_GM20B_MACRO_INDIRECT, kIndexedIndirect = SWITCH_GM20B_MACRO_INDEXED_INDIRECT,
                   kDrawParams = SWITCH_GM20B_MACRO_DRAW_PARAMS,
                   kIndexedParams = SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS;
constexpr uint32_t kMacroCall = NVB197_CALL_MME_MACRO(0), kRamPointer = NVB197_LOAD_MME_INSTRUCTION_RAM_POINTER,
                   kRam = NVB197_LOAD_MME_INSTRUCTION_RAM, kStartPointer = NVB197_LOAD_MME_START_ADDRESS_RAM_POINTER,
                   kStart = NVB197_LOAD_MME_START_ADDRESS_RAM;

struct Program {
    const uint32_t *words;
    uint32_t size;
};

template <size_t N> constexpr Program program(const uint32_t (&words)[N])
{
    return {words, uint32_t(N)};
}

// Metal indirect arguments have exactly Vulkan's draw/indexed-draw word ordering. Unlike Turing, X1 reads them through a
// non-prefetched GPFIFO continuation, not an MME memory load. The CPU supplies no copy and never reads GPU-produced counts.
constexpr Program kDrawProgram = program(switch_gm20b_macro_draw),
                  kIndexedProgram = program(switch_gm20b_macro_draw_indexed),
                  kIndirectProgram = program(switch_gm20b_macro_indirect),
                  kIndexedIndirectProgram = program(switch_gm20b_macro_indexed_indirect),
                  kDrawParamsProgram = program(switch_gm20b_macro_draw_params),
                  kIndexedParamsProgram = program(switch_gm20b_macro_draw_indexed_params);
constexpr size_t kContextWords = 6 * 5 + std::size(switch_gm20b_macro_draw) +
                                 std::size(switch_gm20b_macro_draw_indexed) + std::size(switch_gm20b_macro_indirect) +
                                 std::size(switch_gm20b_macro_indexed_indirect) +
                                 std::size(switch_gm20b_macro_draw_params) +
                                 std::size(switch_gm20b_macro_draw_indexed_params);
constexpr size_t kCallWords = 4, kParamsCallWords = 6;

inline void install(Push &push)
{
    uint32_t offset = 0;
    auto load = [&](uint32_t slot, const uint32_t *words, uint32_t count) {
        push.inc(kStartPointer, {slot, offset});
        push.m(kRamPointer, offset);
        push.nonInc(kRam, words, count);
        offset += count;
    };
    load(kDraw, kDrawProgram.words, kDrawProgram.size);
    load(kDrawIndexed, kIndexedProgram.words, kIndexedProgram.size);
    load(kIndirect, kIndirectProgram.words, kIndirectProgram.size);
    load(kIndexedIndirect, kIndexedIndirectProgram.words, kIndexedIndirectProgram.size);
    load(kDrawParams, kDrawParamsProgram.words, kDrawParamsProgram.size);
    load(kIndexedParams, kIndexedParamsProgram.words, kIndexedParamsProgram.size);
}

inline void indirectHeader(Push &push, bool indexed)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kIndexedIndirect : kIndirect), indexed ? 5 : 4);
}

inline void draw(Push &push, bool indexed, uint32_t first, uint32_t count, uint32_t instances)
{
    const uint32_t method = kMacroCall + 8 * (indexed ? kDrawIndexed : kDraw);
    // ONE_INC sends the first argument to CALL_MME_MACRO and all remaining arguments to its data method.
    *push.at++ = Push::hdr(5, SUBC_3D, method, 3);
    *push.at++ = instances;
    *push.at++ = first;
    *push.at++ = count;
}

// The draw parameters a channel holds: the root table's first word (a draw of vertices: its first vertex; an indexed
// draw: its base vertex) and base instance, and the base vertex registers. `unknown`: an indirect draw's macro set them.
struct DrawParams {
    uint32_t root = 0, baseInstance = 0, baseVertex = 0;
    bool unknown = false;
};

// A draw of `count` vertices or indices from `first`, `instances` times, as one macro call: the plain macro where the
// channel holds the draw's parameters, else the macro that writes them first
inline void draw(Push &push, DrawParams &held, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                 uint32_t root, uint32_t baseInstance)
{
    const uint32_t baseVertex = indexed ? root : 0;
    if (!held.unknown && held.root == root && held.baseInstance == baseInstance && held.baseVertex == baseVertex) {
        draw(push, indexed, first, count, instances);
        return;
    }
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * (indexed ? kIndexedParams : kDrawParams), indexed ? 5 : 4);
    *push.at++ = instances;
    *push.at++ = first;
    *push.at++ = count;
    if (indexed)
        *push.at++ = baseVertex;
    *push.at++ = baseInstance;
    held = {root, baseInstance, baseVertex, false};
}

}  // namespace raw::mme
