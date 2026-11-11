// SPDX-License-Identifier: MIT
#pragma once
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "SwitchGM20BMacros.h"
#include "raw_push.h"
#include <iterator>

namespace raw::mme {

// Native GM20B macro ABI v1. This channel owns macro slots 0..4 and their contiguous instruction words from zero. Arguments:
// instance count (preloaded R1), first vertex/index, element count. The caller owns topology, restart, index binding,
// root draw parameters and base vertex/instance registers. The macros change only BEGIN, vertex/index first+count and
// END, exactly as the compact direct path does, and never CB0. They are NVK's pre-Pascal draw loops, built with Mesa's
// MME builder (kits/SwitchGM20BEmit: gen/SwitchGM20BMacros.h).
constexpr uint32_t kAbi = 1, kDraw = SWITCH_GM20B_MACRO_DRAW, kDrawIndexed = SWITCH_GM20B_MACRO_DRAW_INDEXED,
                   kIndirect = SWITCH_GM20B_MACRO_INDIRECT, kIndexedIndirect = SWITCH_GM20B_MACRO_INDEXED_INDIRECT,
                   kMatrix = SWITCH_GM20B_MACRO_VCM_CONSTANTS;
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
                  kMatrixProgram = program(switch_gm20b_macro_vcm_constants);
constexpr size_t kContextWords = 5 * 5 + std::size(switch_gm20b_macro_draw) +
                                 std::size(switch_gm20b_macro_draw_indexed) + std::size(switch_gm20b_macro_indirect) +
                                 std::size(switch_gm20b_macro_indexed_indirect) +
                                 std::size(switch_gm20b_macro_vcm_constants);
constexpr size_t kCallWords = 4;

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
    load(kMatrix, kMatrixProgram.words, kMatrixProgram.size);
}

// The colour matrix macro's call: its arguments are the next GPFIFO entry's words (the uniform's)
inline void matrixHeader(Push &push)
{
    *push.at++ = Push::hdr(5, SUBC_3D, kMacroCall + 8 * kMatrix, SWITCH_GM20B_MACRO_VCM_ARGUMENTS);
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

}  // namespace raw::mme
