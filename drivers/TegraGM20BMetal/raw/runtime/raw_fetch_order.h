// SPDX-License-Identifier: MIT
// A draw that fetches a color it writes through the texture unit (not inside the pixel interlock) keeps Metal's raster
// order on the raw path by drawing one triangle at a time behind the fetch barrier (TriangleRuns::sequential), which
// reads no vertex; Core Animation's transform first lets RenderEncoder::selfRuns try to prove larger runs (raw_overlap.h).
// The draws refused before any vertex is read are decided by fetchRefusal as each draw is recorded, so only their encoder
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
    Indirect,     // its primitive count is in GPU memory
    Topology,     // points or lines
    Instances,    // more than one instance
    Restart,      // an indexed strip: where its triangles restart only its indices say
    Long,         // more triangles than a draw is split into (kSequentialTriangles)
    PrimitiveId,  // its fragments read the primitive ID, which each triangle's draw would restart
    Geometry,     // its vertices unreadable or too many, or its runs would restart a fragment primitive ID
    Count
};

constexpr size_t kFetchRefusals = size_t(FetchRefusal::Count);
constexpr const char *kFetchRefusalNames[kFetchRefusals] = {"-",       "indirect", "topology",    "instances",
                                                            "restart", "long",     "primitiveid", "geometry"};

// The triangles a draw is split into at most, one draw and one fetch barrier each
constexpr uint32_t kSequentialTriangles = 96;

// Fetching draws by how they kept raster order since the device opened, counted in debug builds only: drawn whole on the
// CPU's proof that no two of their triangles share a pixel, with the vertices and bytes the proof read; drawn a triangle
// at a time (no vertex read) and those triangles; fetched inside the pixel interlock; refused, by reason, as recorded
// (the encoder to m2v) and at encoding
struct FetchCounts {
    std::atomic<uint64_t> provenWhole{0}, proofVertices{0}, proofBytes{0}, sequential{0}, sequentialTriangles{0},
        interlocked{0};
    std::atomic<uint64_t> refusedAtRecord[kFetchRefusals]{}, refusedAtEncode[kFetchRefusals]{};
    std::atomic<uint64_t> storageFallback[kStorageFallbacks]{};
    // the pipeline key hashes behind refusedAtRecord, refusedAtEncode and storageFallback; and those of every draw in the
    // pixel interlock and of every generic merge state draw, the glass frame's slowest kinds
    KeyTally recordKeys, encodeKeys, fallbackKeys, interlockKeys, genericKeys;
};

struct FetchDraw {
    uint32_t primitive = 3;  // MTLPrimitiveType
    uint32_t count = 0;      // vertices or indices
    uint32_t instances = 1;
    bool indirect = false;
    bool indexed = false;
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

constexpr uint32_t triangles(const FetchDraw &d)
{
    return d.primitive == 4 ? (d.count > 2 ? d.count - 2 : 0) : d.count / 3;
}

// What keeps a list or strip of one instance from being drawn a triangle at a time
constexpr FetchRefusal sequentialRefusal(bool primitiveId, const FetchDraw &d)
{
    if (d.indexed && d.primitive == 4)
        return FetchRefusal::Restart;
    if (triangles(d) > kSequentialTriangles)
        return FetchRefusal::Long;
    return primitiveId ? FetchRefusal::PrimitiveId : FetchRefusal::None;
}

constexpr FetchRefusal fetchRefusal(bool fetchesOwn, bool caTransform, bool primitiveId, const FetchDraw &d)
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
    return caTransform ? FetchRefusal::None : sequentialRefusal(primitiveId, d);
}

constexpr const char *fetchRefusalWhy(FetchRefusal r)
{
    return r == FetchRefusal::Indirect
               ? "indirect framebuffer-fetch draws require an interlocked program"
               : "framebuffer-fetch ordering requires an interlocked program or available bounded geometry";
}

}  // namespace raw
