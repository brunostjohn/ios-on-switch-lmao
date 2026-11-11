// SPDX-License-Identifier: MIT
// RenderEncoder's draws in scissor bands (raw_draw_bands.h).
#include "raw_gpu.h"
#include "raw_mme.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"

namespace raw {

uint32_t RenderEncoder::drawBands(uint32_t vertices, uint32_t instances) const
{
    const Pipeline &pl = *emitted_;
    if (dev_->compositorClass() || pl.vertexStores || cb_->bands >= kBandsPerCommandBuffer)
        return 1;
    BandDraw d;
    d.fragmentWork = laneWork(laneWork(drawPixels_, instances), uint64_t(pl.fragmentInstructions) * pl.samplePasses);
    d.vertexWork = laneWork(laneWork(vertices, instances), pl.vertexInstructions);
    d.rows = drawBox_.empty() ? 0 : drawBox_.y1 - drawBox_.y0;
    return bandCount(d, dev_->bandBudgetNs(), kBandsPerCommandBuffer - cb_->bands);
}

void RenderEncoder::emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances)
{
    const auto words = [&](Push &p) {
        if (instances == 1) {
            p.m(NVB197_BEGIN, 0);
            p.inc(indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START, {first, count});
            p.m(NVB197_END, 0);
        } else {
            mme::draw(p, indexed, first, count, instances);
        }
    };
    const uint32_t bands = drawBands(count, instances);
    if (bands < 2) {
        if (p_.room() < kBandWords)
            reserve();
        words(p_);
        finishDraw(count, instances);
        return;
    }
    if (cb_->batchWork_.operations)
        cutBatch();
    const PixelBox area = {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]};
    emitBands(
        p_, rot_, area, drawBox_, bands,
        [&] {
            if (p_.room() < kBandWords)
                reserve();
        },
        words,
        [&] {
            cutBatch();
        });
    cb_->bands += bands;
    RAW_DIAGNOSTIC(dev_->stats.bandedDraws++; dev_->stats.bands += bands);
}

}  // namespace raw
