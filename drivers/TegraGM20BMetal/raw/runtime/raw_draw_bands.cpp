// SPDX-License-Identifier: MIT
// RenderEncoder's draws in scissor bands (raw_draw_bands.h), priced by their state's learned rate (raw_band_rates.h).
#include "raw_gpu.h"
#include "raw_mme.h"
#include "raw_stamps.h"
#include "raw_tiled_cache.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"

namespace raw {

// a draw's shortcuts around its words (raw_draw_shortcuts.cpp), as raw_gpu.cpp reserves them
constexpr uint32_t kShortcutWords = 2 * (SWITCH_GM20B_CONSTANT_COLOR_WORDS + SWITCH_GM20B_FILL_VIA_TRIANGLE_WORDS);

constexpr uint32_t kMeasureWords = kTiledCacheFlushWords + kDrawStampWords;

uint64_t RenderEncoder::drawUnits(uint32_t instances) const
{
    return laneWork(laneWork(drawPixels_, instances), emitted_->samplePasses);
}

uint32_t RenderEncoder::drawBands(uint32_t vertices, uint32_t instances, uint64_t units) const
{
    const Pipeline &pl = *emitted_;
    if (dev_->compositorClass() || pl.vertexStores || cb_->bands >= kBandsPerCommandBuffer)
        return 1;
    BandDraw d;
    d.fragmentNs = estimatedNs(pl.bandRate, units, workNs(laneWork(units, pl.fragmentInstructions)));
    d.vertexNs = workNs(laneWork(laneWork(vertices, instances), pl.vertexInstructions));
    d.rows = drawBox_.empty() ? 0 : drawBox_.y1 - drawBox_.y0;
    return bandCount(d, dev_->bandBudgetNs(), kBandsPerCommandBuffer - cb_->bands);
}

// The render server is never banded, so its draws teach nothing. A banded draw's first band is the one measured: a later
// band's time could hold a compositor's frame, which preempts at a band's end.
bool RenderEncoder::measureDraw(uint64_t units, DrawTiming &t)
{
    if (dev_->compositorClass() || cb_->timings.size() >= kMeasuredPerCommandBuffer ||
        !measure(emitted_->bandRate, units))
        return false;
    const uint32_t n = dev_->drawStamps_.fetch_add(1, std::memory_order_relaxed);
    t = {&emitted_->bandRate, units, n % kDrawStampSlots, n + 1 ? n + 1 : 1, emitted_->rec ? emitted_->rec->keyHash : 0,
         emitted_->fragmentInstructions};
    return true;
}

void RenderEncoder::stampDraw(Push &p, const DrawTiming &t, bool end)
{
    tiledCacheDrain(p, dev_->k_.info());
    emitDrawStamp(p, dev_->stamps_.va, t.slot, end, t.payload);
}

void RenderEncoder::emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                             uint32_t baseInstance, const Shortcut &cut)
{
    const auto words = [&](Push &p) {
        shortcutBegin(cut);
        mme::draw(p, params_, indexed, first, cut.bbox ? 3 : count, instances, root, baseInstance);
        shortcutEnd(cut);
    };
    const uint64_t units = drawUnits(instances);
    const uint32_t bands = drawBands(count, instances, units);
    DrawTiming timing;
    const bool measured = measureDraw(bands < 2 ? units : units / bands, timing);
    if (bands < 2) {
        if (p_.room() < kBandWords + 2 * kMeasureWords + kShortcutWords)
            reserve();
        if (measured)
            stampDraw(p_, timing, false);
        words(p_);
        if (measured) {
            stampDraw(p_, timing, true);
            cb_->timings.push_back(timing);
        }
        finishDraw(count, instances);
        return;
    }
    if (cb_->batchWork_.operations)
        cutBatch();
    const PixelBox area = {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]};
    bool firstBand = true;
    emitBands(
        p_, rot_, area, drawBox_, bands,
        [&] {
            if (p_.room() < kBandWords + 2 * kMeasureWords + kShortcutWords)
                reserve();
        },
        [&](Push &p) {
            if (measured && firstBand)
                stampDraw(p, timing, false);
            words(p);
            if (measured && firstBand)
                stampDraw(p, timing, true);
            firstBand = false;
        },
        [&] {
            cutBatch();
        });
    if (measured)
        cb_->timings.push_back(timing);
    cb_->bands += bands;
    RAW_DIAGNOSTIC(dev_->stats.bandedDraws++; dev_->stats.bands += bands);
}

void Device::learnDrawTimes(const std::vector<DrawTiming> &timings)
{
    for (const DrawTiming &t : timings) {
        const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + drawStampOffset(t.slot, false));
        uint64_t ns = 0;
        if (!stampedNanoseconds(*slot, t.payload, ns))
            continue;
        learn(*t.rate, ns, t.units);
        RAW_DIAGNOSTIC(stats.learnedDraws++);
#if SWITCH_RAW_DIAGNOSTICS
        if (ns >= kLongDrawNs)
            os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                   "raw: long draw %llu us k%016llx %llu kunits %u instr, learned %u ps a unit",
                   (unsigned long long)(ns / 1000), (unsigned long long)t.key, (unsigned long long)(t.units / 1000),
                   t.instructions, t.rate->psPerUnit.load(std::memory_order_relaxed));
#endif
    }
}

}  // namespace raw
