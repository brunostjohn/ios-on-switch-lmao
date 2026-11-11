// SPDX-License-Identifier: MIT
// A draw that fetches a color it writes through the texture unit (not inside the pixel interlock) keeps Metal's raster
// order on the raw path only where the CPU can tell its triangles apart (RenderEncoder::selfRuns, raw_overlap.h). The
// draws refused before any vertex is read are decided by fetchRefusal as each draw is recorded, so only their encoder
// goes to m2v; FetchRefusal::Geometry, which depends on what the CPU can read at submission, is selfRuns' alone.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "raw_key_tally.h"
#include "raw_storage_form.h"

namespace raw {

enum class FetchRefusal : uint8_t {
    None,
    Indirect,   // its primitive count is in GPU memory
    Topology,   // points or lines
    Instances,  // more than one instance
    Transform,  // not Core Animation's transform: nothing bounds its vertices on the CPU
    Geometry,   // its vertices unreadable or too many, or its runs would restart a fragment primitive ID
    Count
};

constexpr size_t kFetchRefusals = size_t(FetchRefusal::Count);
constexpr const char *kFetchRefusalNames[kFetchRefusals] = {"-",         "indirect",  "topology",
                                                            "instances", "transform", "geometry"};

// Fetching draws by how they kept raster order since the device opened, counted in debug builds only: drawn whole on the
// CPU's proof that no two of their triangles share a pixel, with the vertices and bytes the proof read; drawn a triangle
// at a time without one (selfRuns' small direct Core Animation draws) and those triangles; fetched inside the pixel
// interlock; refused, by reason, as recorded (the encoder to m2v) and at encoding
struct FetchCounts {
    std::atomic<uint64_t> provenWhole{0}, proofVertices{0}, proofBytes{0}, sequential{0}, sequentialTriangles{0},
        interlocked{0};
    std::atomic<uint64_t> refusedAtRecord[kFetchRefusals]{}, refusedAtEncode[kFetchRefusals]{};
    std::atomic<uint64_t> storageFallback[kStorageFallbacks]{};
    // the pipeline key hashes behind refusedAtRecord, refusedAtEncode and storageFallback; and every interlocked draw's
    // and generic merge state draw's (COMPILER.md E5: which states a glass frame's interlock and generic draws are)
    KeyTally recordKeys, encodeKeys, fallbackKeys, interlockKeys, genericKeys;
};

struct FetchDraw {
    uint32_t primitive = 3;  // MTLPrimitiveType
    uint32_t count = 0;      // vertices or indices
    uint32_t instances = 1;
    bool indirect = false;
};

constexpr bool fetchesOwnColor(uint8_t fetchBits, uint8_t writeBits, uint8_t storageBits)
{
    return fetchBits & writeBits & ~storageBits;
}

constexpr bool singlePrimitive(const FetchDraw &d)
{
    const uint32_t vertices = d.primitive == 0 ? 1 : d.primitive <= 2 ? 2 : 3;
    return !d.count || !d.instances || (d.count <= vertices && d.instances == 1);
}

constexpr FetchRefusal fetchRefusal(bool fetchesOwn, bool caTransform, const FetchDraw &d)
{
    if (!fetchesOwn)
        return FetchRefusal::None;
    if (d.indirect)
        return FetchRefusal::Indirect;
    if (singlePrimitive(d))
        return FetchRefusal::None;
    if (d.primitive != 3 && d.primitive != 4)
        return FetchRefusal::Topology;
    if (d.instances != 1)
        return FetchRefusal::Instances;
    return caTransform ? FetchRefusal::None : FetchRefusal::Transform;
}

constexpr const char *fetchRefusalWhy(FetchRefusal r)
{
    return r == FetchRefusal::Indirect
               ? "indirect framebuffer-fetch draws require an interlocked program"
               : "framebuffer-fetch ordering requires an interlocked program or available bounded geometry";
}

}  // namespace raw
