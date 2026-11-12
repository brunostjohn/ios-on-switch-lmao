// SPDX-License-Identifier: MIT
// What a state's reads through argument buffers take of a draw (raw_heap_tables.h), read from m2v's plans of the probe draws
// (NativeMetalDirect.rs holes_of, heap_of) for its record: each stage's root-table and ExecutionStatus address words, the
// buffer slots its root table holds, and its set-1 heap arrays in NVK's layout.
#pragma once
#include <cstring>
#include <string>
#include "bridge/NativeMetalDirect.h"
#include "raw_direct_nvk.h"
#include "raw_record_parts.h"

namespace raw {

inline bool sameAddressHoles(const NmbDirectDraw &a, const NmbDirectDraw &b)
{
    return !memcmp(a.rootTableWord, b.rootTableWord, sizeof a.rootTableWord) &&
           !memcmp(a.statusWord, b.statusWord, sizeof a.statusWord) && a.otherHoles == b.otherHoles;
}

inline bool sameHeap(const NmbDirectDraw &a, const NmbDirectDraw &b)
{
    return bool(a.set1) == bool(b.set1) && a.heapArrays == b.heapArrays &&
           !memcmp(a.heapClass, b.heapClass, sizeof a.heapClass);
}

// The record's address words and root slots (the bound run's: only its buffers have addresses), and its heap; null: why not
inline const char *directHeap(const DirectNvk &nvk, void *device, const NmbDirectState &state, RecordHeap &out)
{
    const NmbDirectDraw &draw = state.draws[0];
    if (draw.otherHoles)
        return "push words m2v fills with addresses other than its root and status tables";
    for (const NmbDirectDraw &d : state.draws)
        if (!sameAddressHoles(d, draw) || !sameHeap(d, draw))
            return "probe draws on variants of other address holes or heaps";
    memcpy(out.rootTableWord, draw.rootTableWord, sizeof out.rootTableWord);
    memcpy(out.statusWord, draw.statusWord, sizeof out.statusWord);
    memcpy(out.rootSlots, state.draws[3].rootSlots, sizeof out.rootSlots);
    for (uint32_t k = 0; k < 2; k++)
        if (out.rootSlots[k] && !out.rootTableWord[k])
            return "a root table's buffer slots in a stage that reads no root table";
    if (!draw.set1)
        return draw.heapArrays ? "heap arrays without their set" : nullptr;
    if (draw.heapArrays > kPackHeapArrays)
        return "more set-1 heap arrays than a record holds";
    for (uint32_t b = 0; b < draw.heapArrays; b++) {
        NvkSetBinding binding = {};
        if (!draw.heapClass[b])
            return "a set-1 heap array of a class the bridge cannot word";
        if (!nvk::setLayoutBinding(nvk, draw.set1, b, binding) || binding.stride != 4 ||
            binding.arraySize != kPackHeapElements)
            return "a set-1 heap array off the raw heap's 4-byte elements";
        out.array[b] = {draw.heapClass[b], binding.offset};
    }
    out.arrays = uint8_t(draw.heapArrays);
    out.bytes = nvk::setLayoutSize(nvk, device, draw.set1);
    return nullptr;
}

}  // namespace raw
