// SPDX-License-Identifier: MIT
// A render encoder's vertex amplification and bind strides (raw_gpu.h RenderEncoder). A state of vertex amplification is
// m2v's variant of one draw per amplification (bind/graphics.rs amplification_draws): each draw is drawn once per
// amplification, in order, its id in the state's AmplificationId push word, as Metal renders every primitive of one
// amplification before the next's. The GPU repeats it: one call of an amplify macro (raw_mme.h) per Metal draw, which
// writes each amplification's words from the scratch words the encoder keeps (the count where it changes, the state's
// words where the bound state's differ) and draws, a pixel barrier between two amplifications of a state that fetches its
// own colour through the texture unit. That state's draw of more than one primitive is drawn a run of primitives at a
// time with barriers between: it is drawn once per amplification. Raw passes have one layer and set one viewport, so a
// mapping's render target offset renders the first layer, as Metal's does in such a pass; a viewport offset sends the
// draw elsewhere. A state of a dynamic stride reads its bind's stride: its vertex stream's at each draw, a pulled
// layout's from its VertexBufferStride word.
#include <algorithm>
#include "raw_gpu.h"

namespace raw {
namespace {

// NVB197_SET_VERTEX_STREAM_A_FORMAT_STRIDE is bits 11:0
constexpr uint32_t kStreamStrideLimit = 0xfff;
constexpr uint32_t kRootPushBytes = 0x700;  // the root table's push words, m2v's push ranges
constexpr uint32_t kAmplificationWords = mme::kAmplificationScratchWords + mme::kAmplifyCallWords;

}  // namespace

void RenderEncoder::setVertexStride(uint32_t slot, uint32_t stride)
{
    if (slot < 31)
        vertexStrides_[slot] = stride;
}

void RenderEncoder::setAmplification(uint32_t count, bool viewportOffsets)
{
    amplificationScratchDirty_ |= std::max(1u, count) != amplifications_;
    amplifications_ = std::max(1u, count);
    amplificationOffsets_ = viewportOffsets && amplifications_ > 1;
    amplificationDirty_ = true;
}

const char *RenderEncoder::drawnWordsRefusal(const PackPipeline &r) const
{
    const uint32_t most = std::max<uint32_t>(1, r.amplificationMax);
    if (amplifications_ > most)
        return "a vertex amplification count above the state's maxVertexAmplificationCount";
    if (r.vertexStrideDynamic && vertexStride(r.vertexSlot) > kStreamStrideLimit)
        return "a vertex buffer bind stride past the vertex stream's 12-bit stride";
    for (uint32_t k = 1; k < kPackVertexStreams && r.vertexStreams[k - 1]; k++) {
        const uint8_t stream = r.vertexStreams[k - 1], slot = streamSlot(stream);
        if (slot >= 31)
            return "a vertex stream of no Metal buffer";
        if ((stream & kStreamBindStride) && vertexStride(slot) > kStreamStrideLimit)
            return "a vertex buffer bind stride past the vertex stream's 12-bit stride";
    }
    return nullptr;
}

void RenderEncoder::icbAmplification(uint32_t a)
{
    const PackPipeline &r = *prepared_->rec;
    amplificationDirty_ = true;
    if (!r.amplificationIdWord || r.amplificationIdWord > 32 || cb_->rootPush_[r.amplificationIdWord - 1] == a)
        return;
    if (p_.room() < 8)
        reserve();
    p_.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0x700 + 4 * (r.amplificationIdWord - 1u));
    p_.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), &a, 1);
    cb_->rootPush_[r.amplificationIdWord - 1] = a;
}

bool RenderEncoder::runsPerAmplification(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed)
{
    const Pipeline *pl = pipeline_ ? formFor(pipeline_) : nullptr;
    return pl && fetchesOwnColor(pl->fetchBits, pl->writeBits, pl->storageBits) &&
           !singlePrimitive({primitive, count, instances, false, indexed});
}

void RenderEncoder::amplificationScratch(Push &p, const Pipeline &pl)
{
    if (amplificationScratchDirty_) {
        const uint32_t none[SWITCH_GM20B_AMP_MAX] = {};
        mme::amplification(p, amplifications_, none, none);
        amplificationScratchDirty_ = false;
    }
    const PackPipeline &r = *pl.rec;
    mme::AmplificationWords words;
    words.id = r.amplificationIdWord ? kRootPushBytes + 4 * (r.amplificationIdWord - 1u) : 0;
    words.count = r.amplificationCountWord ? kRootPushBytes + 4 * (r.amplificationCountWord - 1u) : 0;
    words.barrier = fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits);
    if (!words.same(heldAmplification_)) {
        mme::amplificationWords(p, words);
        heldAmplification_ = words;
    }
}

void RenderEncoder::amplifyCall(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                                uint32_t root, uint32_t baseInstance)
{
    if (p_.room() < kAmplificationWords)
        reserve();
    amplificationScratch(p_, pl);
    mme::amplify(p_, params_, indexed, first, count, instances, root, baseInstance);
    heldAmplifyWords(*pl.rec);
}

void RenderEncoder::heldAmplifyWords(const PackPipeline &r)
{
    if (r.amplificationIdWord && r.amplificationIdWord <= 32)
        cb_->rootPush_[r.amplificationIdWord - 1] = amplifications_ - 1;
    if (r.amplificationCountWord && r.amplificationCountWord <= 32)
        cb_->rootPush_[r.amplificationCountWord - 1] = amplifications_;
}

void RenderEncoder::drawnWords(const PackPipeline &r, uint32_t (&want)[32])
{
    if (r.amplificationIdWord && r.amplificationIdWord <= 32) {
        // an amplify call and an indirect command buffer's execute write each amplification's id themselves
        want[r.amplificationIdWord - 1] = amplifies() ? cb_->rootPush_[r.amplificationIdWord - 1] : amplification_;
        amplificationDirty_ = false;
    }
    if (r.amplificationCountWord && r.amplificationCountWord <= 32)
        want[r.amplificationCountWord - 1] = amplifications_;
    for (const auto &pulled : r.pulledStride)
        if (pulled[1] && pulled[1] <= 32)
            want[pulled[1] - 1] = vertexStride(pulled[0]);
}

}  // namespace raw
