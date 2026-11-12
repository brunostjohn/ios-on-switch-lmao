// SPDX-License-Identifier: MIT
// An indirect command buffer of draws that inherit their pipeline and buffers: each command is kSlotWords push words in GPU
// memory, written once as the CPU encodes the command, and a range of commands is one run of words the channel fetches by
// reference (RenderEncoder::executeSlots), after the encoder prepared the state they inherit. A command is its topology,
// its index buffer, and the gated draw macro (raw_mme.h) called with its index and Metal's indirect arguments inline: it
// draws as an indirect draw does (root draw parameters, base registers) where its index is in the range the execute put in
// the macros' scratch, and draws nothing elsewhere. A command with nothing encoded is no-ops. The macro call is at the
// same words in every command, so a copy to another index rewrites one word (kIndexWord).
#pragma once
#include <cstdint>
#include "raw_mme.h"
#include "raw_push.h"

namespace raw::icb {

constexpr uint32_t kSlotWords = 18, kCallWord = 10, kIndexWord = kCallWord + 1;
constexpr uint32_t kNop = Push::hdr(4, SUBC_3D, NVB197_NO_OPERATION, 0);

struct Draw {
    uint32_t primitive;  // MTLPrimitiveType
    uint32_t count, instances, first, baseInstance;
};

struct IndexedDraw {
    uint32_t primitive;
    uint32_t indexSize;  // 2 or 4
    uint64_t indexVa, indexLast;
    uint32_t count, instances, firstIndex;
    int32_t baseVertex;
    uint32_t baseInstance;
};

inline uint32_t topology(uint32_t primitive)
{
    constexpr uint32_t kTopology[] = {1, 2, 3, 4, 5};  // point, line, line strip, triangle, triangle strip
    return primitive < 5 ? kTopology[primitive] : 4;
}

inline bool restarts(uint32_t primitive)
{
    return primitive == 2 || primitive == 4;
}

// Metal biases the depth of triangles alone (RenderEncoder::rasterFor): commands of both kinds need different raster state
inline bool triangles(uint32_t primitive)
{
    return primitive == 3 || primitive == 4;
}

inline void empty(uint32_t *slot)
{
    for (uint32_t i = 0; i < kSlotWords; i++)
        slot[i] = kNop;
}

// The words before the call (`p` at the slot's first) padded to it
inline void padToCall(uint32_t *slot, Push &p)
{
    while (p.at < slot + kCallWord)
        *p.at++ = kNop;
}

inline void draw(uint32_t *slot, uint32_t index, const Draw &d)
{
    Push p{slot, slot + kSlotWords};
    p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, topology(d.primitive));
    p.m(NVB197_SET_DA_PRIMITIVE_RESTART, restarts(d.primitive));
    padToCall(slot, p);
    mme::icbDrawHeader(p, false);
    const uint32_t arguments[] = {index, d.count, d.instances, d.first, d.baseInstance};
    p.raw(arguments, 5);
    while (p.at < p.end)
        *p.at++ = kNop;
}

inline void indexed(uint32_t *slot, uint32_t index, const IndexedDraw &d)
{
    Push p{slot, slot + kSlotWords};
    p.inc(NVB197_SET_INDEX_BUFFER_A,
          {uint32_t(d.indexVa >> 32), uint32_t(d.indexVa), uint32_t(d.indexLast >> 32), uint32_t(d.indexLast)});
    *p.at++ = Push::hdr(1, SUBC_3D, NVB197_SET_DA_PRIMITIVE_RESTART_INDEX, 1);
    *p.at++ = d.indexSize == 2 ? 0xffffu : 0xffffffffu;
    p.m(NVB197_SET_INDEX_BUFFER_E, d.indexSize == 2 ? 1 : 2);
    p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, topology(d.primitive));
    p.m(NVB197_SET_DA_PRIMITIVE_RESTART, restarts(d.primitive));
    padToCall(slot, p);
    mme::icbDrawHeader(p, true);
    const uint32_t arguments[] = {index, d.count, d.instances, d.firstIndex, uint32_t(d.baseVertex), d.baseInstance};
    p.raw(arguments, 6);
    while (p.at < p.end)
        *p.at++ = kNop;
}

}  // namespace raw::icb
