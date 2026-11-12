// SPDX-License-Identifier: MIT
// A tessellation state's draws (raw_gpu.h RenderEncoder): each patch one control point of a PATCH_LIST of one (m2v
// link/tessellation), drawn by the hardware tessellator through the state's control and evaluation programs. The control
// stage reads the encoder's factor buffer and the draw's patch and control-point index buffers through set 0
// (EntryTessBuffer), and its push words (PackPipeline::controlWord) as m2v's binder writes them (bind/graphics.rs
// patch_draw). An indirect draw's MTLDrawPatchIndirectArguments are a non-indexed draw's: the channel's MME reads them.
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"
#include "raw_icb.h"

namespace raw {
namespace {

constexpr uint16_t kHalfInfinity = 0x7c00;
// m2v INFINITE_SCALE: takes every positive half factor past the largest level, finitely
constexpr float kInfiniteScale = 4294967296.0f;

}  // namespace

bool tessellationScaleBits(float scale, uint32_t &bits)
{
    const _Float16 half = static_cast<_Float16>(scale);
    uint16_t halfBits;
    memcpy(&halfBits, &half, sizeof halfBits);
    if (halfBits < 1 || halfBits > kHalfInfinity)
        return false;
    const float pushed = halfBits == kHalfInfinity ? kInfiniteScale : static_cast<float>(half);
    memcpy(&bits, &pushed, sizeof bits);
    return true;
}

RenderEncoder::Bound RenderEncoder::boundFrom(const Buffer *buffer, uint64_t offset)
{
    if (!buffer || !buffer->va || offset >= buffer->length)
        return {};
    return {buffer->va + offset, uint32_t(std::min<uint64_t>(buffer->length - offset, 0xffffffffu)), buffer, offset,
            buffer->cpu ? buffer->cpu + offset : nullptr};
}

void RenderEncoder::setTessellationFactors(const Buffer *buffer, uint64_t offset, uint64_t instanceStride)
{
    const Bound next = boundFrom(buffer, offset & ~uint64_t(1));
    const uint32_t stride = next.va ? uint32_t(instanceStride & ~uint64_t(3)) : 0;
    Bound &factors = tess_.buffers[TessFactors];
    if (factors.va == next.va && factors.size == next.size && factors.buffer == next.buffer &&
        tess_.instanceStride == stride)
        return;
    factors = next;
    tess_.instanceStride = stride;
    tessWordsDirty_ = true;
    invalidateDescriptors();
}

void RenderEncoder::setTessellationFactorScale(float scale)
{
    uint32_t bits = tess_.scaleBits;
    const bool valid = tessellationScaleBits(scale, bits);
    if (valid == tess_.scaleValid && bits == tess_.scaleBits)
        return;
    tess_.scaleValid = valid;
    tess_.scaleBits = bits;
    tessWordsDirty_ = true;
}

// The draw's patch and control-point index buffers into set 0, and whether it binds a patch index buffer into the control
// stage's words; false: a draw the state does not take (`why`), as m2v's binder refuses it
bool RenderEncoder::bindPatches(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why)
{
    const Pipeline *pl = formFor(pipeline_);
    if (!pl || !tessellates(*pl->rec)) {
        *why = "patches drawn with a state of no post-tessellation vertex function";
        return false;
    }
    const PackPipeline &r = *pl->rec;
    if (r.controlPoints && controlPoints != r.controlPoints) {
        *why = "patches of another control point count than the function's";
        return false;
    }
    if (bool(pointIndices) != bool(r.controlPointIndexBytes) ||
        (pointIndices && pointIndexOffset % r.controlPointIndexBytes) || (patchIndices && patchIndexOffset % 4)) {
        *why = "patch or control-point indices the state does not draw";
        return false;
    }
    if (!tess_.scaleValid) {
        *why = "a tessellation factor scale that is not a positive half";
        return false;
    }
    const Bound patches = boundFrom(patchIndices, patchIndexOffset), points = boundFrom(pointIndices, pointIndexOffset);
    const uint32_t indexed = patches.va ? 1 : 0;
    Bound &p = tess_.buffers[TessPatchIndices], &c = tess_.buffers[TessControlPointIndices];
    if (p.va != patches.va || p.size != patches.size || c.va != points.va || c.size != points.size) {
        p = patches;
        c = points;
        invalidateDescriptors();
    }
    if (indexed != tess_.indexed) {
        tess_.indexed = indexed;
        tessWordsDirty_ = true;
    }
    return true;
}

bool RenderEncoder::drawPatches(uint32_t controlPoints, uint32_t start, uint32_t count, uint32_t instances,
                                uint32_t baseInstance, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why)
{
    if (!pipeline_)
        return true;
    if (!bindPatches(controlPoints, patchIndices, patchIndexOffset, pointIndices, pointIndexOffset, why))
        return false;
    return draw(icb::kPatchPrimitive, start, count, instances, baseInstance, why);
}

bool RenderEncoder::drawPatchesIndirect(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                        const Buffer *pointIndices, uint64_t pointIndexOffset, const Buffer *arguments,
                                        uint64_t offset, const char **why)
{
    if (!pipeline_)
        return true;
    if (!bindPatches(controlPoints, patchIndices, patchIndexOffset, pointIndices, pointIndexOffset, why))
        return false;
    return indirect(icb::kPatchPrimitive, false, arguments, offset, why);
}

// The set-0 descriptor source of tessellation buffer `which`: the zero page where none is bound, whose factors cull every
// patch and whose indices read 0, as m2v binds its zero buffer there
RenderEncoder::Bound RenderEncoder::tessBuffer(uint8_t which) const
{
    if (which < TessBufferCount && tess_.buffers[which].va)
        return tess_.buffers[which];
    Bound zero;
    zero.va = dev_->zero_.va;
    zero.size = uint32_t(dev_->zero_.bytes);
    return zero;
}

// The control stage's words m2v's binder pushes besides the buffers' residuals
void RenderEncoder::controlWords(const PackPipeline &r, uint32_t (&want)[32]) const
{
    const uint32_t first = r.controlWord - 1u;
    want[first + kTessInstanceStrideWord] = tess_.instanceStride;
    want[first + kTessScaleWord] = tess_.scaleBits;
    want[first + kTessIndexedWord] = tess_.indexed;
}

}  // namespace raw
