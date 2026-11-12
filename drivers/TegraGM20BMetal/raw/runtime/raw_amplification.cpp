// SPDX-License-Identifier: MIT
// A render encoder's vertex amplification and bind strides (raw_gpu.h RenderEncoder). A state of vertex amplification is
// m2v's variant of one draw per amplification (bind/graphics.rs amplification_draws): each draw is drawn once per
// amplification, in order, its id in the state's AmplificationId push word, as Metal renders every primitive of one
// amplification before the next's. Raw passes have one layer and set one viewport, so a mapping's render target offset
// renders the first layer, as Metal's does in such a pass; a viewport offset sends the draw elsewhere. A state of a dynamic
// stride reads its bind's stride: its vertex stream's at each draw, a pulled layout's from its VertexBufferStride word.
#include <algorithm>
#include "raw_gpu.h"

namespace raw {
namespace {

// NVB197_SET_VERTEX_STREAM_A_FORMAT_STRIDE is bits 11:0
constexpr uint32_t kStreamStrideLimit = 0xfff;

}  // namespace

void RenderEncoder::setVertexStride(uint32_t slot, uint32_t stride)
{
    if (slot < 31)
        vertexStrides_[slot] = stride;
}

void RenderEncoder::setAmplification(uint32_t count, bool viewportOffsets)
{
    amplifications_ = std::max(1u, count);
    amplificationOffsets_ = viewportOffsets && amplifications_ > 1;
    amplificationDirty_ = true;
}

const char *RenderEncoder::drawnWordsRefusal(const PackPipeline &r) const
{
    const uint32_t most = std::max<uint32_t>(1, r.amplificationMax);
    if (amplifications_ > most)
        return "a vertex amplification count above the state's maxVertexAmplificationCount";
    if (r.amplificationCountRead && amplifications_ != most)
        return "a vertex amplification count other than the one the state's [[amplification_count]] was made with";
    if (r.vertexStrideDynamic && vertexStride(r.vertexSlot) > kStreamStrideLimit)
        return "a vertex buffer bind stride past the vertex stream's 12-bit stride";
    return nullptr;
}

void RenderEncoder::drawnWords(const PackPipeline &r, uint32_t (&want)[32])
{
    if (r.amplificationIdWord && r.amplificationIdWord <= 32) {
        want[r.amplificationIdWord - 1] = amplification_;
        amplificationDirty_ = false;
    }
    for (const auto &pulled : r.pulledStride)
        if (pulled[1] && pulled[1] <= 32)
            want[pulled[1] - 1] = vertexStride(pulled[0]);
}

}  // namespace raw
