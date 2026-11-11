// SPDX-License-Identifier: MIT
// A parallel render encoder's sub-encoders, each recorded into a command buffer of its own (any thread, any order) and
// recorded again into the parallel encoder's in their creation order when it ends: one encoder each, a pass that continues
// the one before on the raw path (one hardware pass, no barrier, every state at Metal's default again) and a pass of its
// own in m2v's replay, which loads what the one before stored.
#pragma once
#include <cstddef>
#include <cstdint>
#include "RawMetalArguments.h"

namespace raw {

// MTLLoadAction and MTLStoreAction values
inline constexpr uint8_t kLoadActionLoad = 1, kStoreActionStore = 1;

// Sub-encoder `at` of `count` as a pass of its own: the first loads as the parallel pass does, the last stores as it does
inline uint8_t seamLoad(uint8_t passLoad, size_t at)
{
    return at ? kLoadActionLoad : passLoad;
}

inline uint8_t seamStore(uint8_t passStore, size_t at, size_t count)
{
    return at + 1 < count ? kStoreActionStore : passStore;
}

// The words of a command buffer's extra list a command reads from Cmd::x (its record: drops them once its plan has them)
inline size_t extraWords(Op op)
{
    switch (op) {
    case OpViewport:
    case OpDispatch:
        return 6;
    case OpBlendColor:
        return 4;
    case OpDepthBias:
    case OpDrawIndexed:
        return 3;
    default:
        return 0;
    }
}

// A command a sub-encoder's command buffer recorded, as the parallel encoder's records it: its pass, setBytes chunk and extra
// words where that command buffer holds them (its passes from `passBase`, its chunks from `chunkBase`, the words at
// `extraAt`). OpDrawIndirect's and OpDrawIndexedIndirect's x is an argument offset, not an extra word.
inline Cmd spliced(Cmd c, size_t passBase, size_t chunkBase, size_t extraAt)
{
    if (c.op == OpRender)
        c.x += passBase;
    else if (c.op == OpBytes)
        c.c += chunkBase;
    if (extraWords(c.op))
        c.x = extraAt;
    return c;
}

}  // namespace raw
