// SPDX-License-Identifier: MIT
#pragma once
#include "raw_push.h"
#include <array>

namespace raw::mme {

// Native GM20B macro ABI v1. This channel owns macro slots 0..3 and their contiguous instruction words from zero. Arguments:
// instance count (preloaded R1), first vertex/index, element count. R1-R4 are invocation-local, no shadow scratch is used.
// The caller owns topology, restart, index binding, root draw parameters and base vertex/instance registers. The macro
// changes only BEGIN, vertex/index first+count and END, exactly as the compact direct path does. It never changes CB0.
// This is the pre-Pascal loop in NVK's nvk_mme_build_draw[_indexed]_loop. Encodings use Mesa's mme_fermi_encode layout;
// tests decode and execute these exact words with Mesa's independent Fermi simulator.
constexpr uint32_t kAbi = 1, kDraw = 0, kDrawIndexed = 1, kIndirect = 2, kIndexedIndirect = 3;
constexpr uint32_t kBegin = 0x1618, kEnd = 0x1614, kVertexFirst = 0x0d74, kIndexFirst = 0x17dc;
constexpr uint32_t kMacroCall = 0x3800, kRamPointer = 0x0114, kRam = 0x0118, kStartPointer = 0x011c, kStart = 0x0120;

constexpr uint32_t add(uint32_t assign, uint32_t dst, uint32_t source, int32_t immediate)
{
    return 1u | assign << 4 | dst << 8 | source << 11 | (uint32_t(immediate) & 0x3ffff) << 14;
}

constexpr uint32_t branch(uint32_t source, int32_t displacement, bool nonzero)
{
    return 7u | uint32_t(nonzero) << 4 | 1u << 5 | source << 11 | (uint32_t(displacement) & 0x3ffff) << 14;
}

constexpr std::array<uint32_t, 17> program(uint32_t firstMethod)
{
    return {{
        add(0, 2, 0, 0),           // load first
        add(0, 3, 0, 0),           // load count (consume all args even for zero instances)
        add(1, 4, 0, 0),           // BEGIN: instance FIRST
        branch(1, 12, false),      // zero instances -> exit at 15
        add(2, 0, 0, kBegin / 4),  // 4: BEGIN method, no increment
        add(4, 0, 4, 0),           // emit BEGIN
        add(2, 0, 0, 0x1000 | firstMethod / 4),
        add(4, 0, 2, 0),  // first vertex/index
        add(4, 0, 3, 0),  // count, next method
        add(2, 0, 0, kEnd / 4),
        add(4, 0, 0, 0),  // END
        add(1, 4, 0, 1),
        3u | 1u << 4 | 4u << 8 | 4u << 14 | 1u << 22 | 26u << 27,  // R4 = bit 26: instance SUBSEQUENT
        add(1, 1, 1, -1),                                          // instances remaining
        branch(1, -10, true),                                      // nonzero -> BEGIN at 4, no delay slot
        add(1, 0, 0, 0) | 1u << 7,                                 // exit after one delay slot
        add(1, 0, 0, 0),
    }};
}

constexpr auto kDrawProgram = program(kVertexFirst);
constexpr auto kIndexedProgram = program(kIndexFirst);

// Metal indirect arguments have exactly Vulkan's draw/indexed-draw word ordering. Unlike Turing, X1 reads them through a
// non-prefetched GPFIFO continuation, not an MME memory load. The CPU supplies no copy and never reads GPU-produced counts.
struct IndirectProgram {
    std::array<uint32_t, 48> words{};
    uint32_t size = 0;

    constexpr void emit(uint32_t word)
    {
        words[size++] = word;
    }

    constexpr void method(uint32_t address, uint32_t reg)
    {
        emit(add(2, 0, 0, int32_t(address / 4)));
        emit(add(4, 0, reg, 0));
    }
};

constexpr IndirectProgram indirectProgram(bool indexed)
{
    IndirectProgram p;
    // R1=count (preloaded), R2=instances, R3=first, R4=base vertex or base instance, R5=indexed base instance, R6=BEGIN.
    p.emit(add(0, 2, 0, 0));
    p.emit(add(0, 3, 0, 0));
    p.emit(add(0, 4, 0, 0));
    if (indexed)
        p.emit(add(0, 5, 0, 0));
    p.method(0x238c, 0);  // LOAD_CONSTANT_BUFFER_OFFSET = 0; root CB0 remains selected
    p.emit(add(2, 0, 0, 0x2390 / 4));
    p.emit(add(4, 0, indexed ? 4 : 3, 0));  // root base vertex / first vertex
    p.emit(add(4, 0, indexed ? 5 : 4, 0));  // root base instance
    p.method(0x1438, indexed ? 5 : 4);      // GLOBAL_BASE_INSTANCE_INDEX
    p.method(0x1434, indexed ? 4 : 0);      // GLOBAL_BASE_VERTEX_INDEX
    p.method(0x1118, indexed ? 4 : 0);      // VERTEX_ID_BASE
    p.emit(add(1, 6, 0, 0));
    const uint32_t emptyCount = p.size;
    p.emit(0);
    const uint32_t emptyInstances = p.size;
    p.emit(0);
    const uint32_t loop = p.size;
    p.method(kBegin, 6);
    p.emit(add(2, 0, 0, 0x1000 | (indexed ? kIndexFirst : kVertexFirst) / 4));
    p.emit(add(4, 0, 3, 0));
    p.emit(add(4, 0, 1, 0));
    p.method(kEnd, 0);
    p.emit(add(1, 6, 0, 1));
    p.emit(3u | 1u << 4 | 6u << 8 | 6u << 14 | 1u << 22 | 26u << 27);
    p.emit(add(1, 2, 2, -1));
    const uint32_t back = p.size;
    p.emit(branch(2, int32_t(loop) - int32_t(back), true));
    p.words[emptyCount] = branch(1, int32_t(p.size - emptyCount), false);
    p.words[emptyInstances] = branch(2, int32_t(p.size - emptyInstances), false);
    p.emit(add(1, 0, 0, 0) | 1u << 7);
    p.emit(add(1, 0, 0, 0));
    return p;
}

constexpr auto kIndirectProgram = indirectProgram(false);
constexpr auto kIndexedIndirectProgram = indirectProgram(true);
constexpr size_t kContextWords =
    4 * 5 + kDrawProgram.size() + kIndexedProgram.size() + kIndirectProgram.size + kIndexedIndirectProgram.size;
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
    load(kDraw, kDrawProgram.data(), uint32_t(kDrawProgram.size()));
    load(kDrawIndexed, kIndexedProgram.data(), uint32_t(kIndexedProgram.size()));
    load(kIndirect, kIndirectProgram.words.data(), kIndirectProgram.size);
    load(kIndexedIndirect, kIndexedIndirectProgram.words.data(), kIndexedIndirectProgram.size);
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
