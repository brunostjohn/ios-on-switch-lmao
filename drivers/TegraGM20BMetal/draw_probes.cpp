// SPDX-License-Identifier: MIT
// DEVELOPMENT: the glass and blur-texture probes of draws (glass_probe.h, glass_mem.h).
#include "lock.h"
#include "glass_probe.h"
#include "gpu.h"
#include "stamps.h"
#include "tiled_cache.h"

#include <mach/mach_time.h>
#include <os/log.h>
#include <set>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

#if SWITCH_METAL_DIAGNOSTICS
void RenderEncoder::probeDraw(const Pipeline &bound, Pipeline &drawn, uint32_t, int32_t, uint32_t, bool,
                              const PixelBox &limit)
{
    if (glassProbeArmed())
        probeGlassDraw(bound, drawn, limit);
    if (dev_->glassMemLive_)
        glassMemSample(bound, drawn);
}

// The glass probe's line for a Core Animation draw (glass_probe.h): the form drawn as probeDraw names it, each
// colour attachment as the pass bound it (format/bytes a texel/samples/layout[+comp][+ml]/size/load), the textures its
// record samples and the window of each uniform buffer its program reads (Core Animation binds one ring of every draw's
// uniforms: past the window it holds other draws')
void RenderEncoder::probeGlassDraw(const Pipeline &bound, const Pipeline &drawn, const PixelBox &limit)
{
    const PackPipeline &rec = *drawn.rec;
    const Pipeline *barrierTwin = __atomic_load_n(&bound.barrierTwin, __ATOMIC_ACQUIRE);
    const char *caName = __atomic_load_n(&bound.caName, __ATOMIC_ACQUIRE);
    const char *form = drawn.splitOf                     ? "split"
                       : drawn.storageBits               ? "storage"
                       : &drawn == barrierTwin           ? "barrier"
                       : rec.flags & PipelineBarrierForm ? "barrier-record"
                                                         : "whole";
    char attachments[8 * 48] = "-", sampled[16 * 56] = "-", uniforms[8 * 32] = "-";
    size_t at = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8 && at + 1 < sizeof attachments; j++)
        if (const Texture *t = pass_.color[j].texture) {
            const int n =
                snprintf(attachments + at, sizeof attachments - at, "%sc%u:%u/%u/%u/%s%s%s/%ux%u/%c", at ? " " : "", j,
                         t->format, t->bpp, t->samples, t->blockLinear ? "bl" : "pitch", t->comptags ? "+comp" : "",
                         memoryless_[j] ? "+ml" : "", t->width, t->height, "dlc"[pass_.color[j].load]);
            at += std::min(sizeof attachments - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < drawn.entries.size() && at + 1 < sizeof sampled; k++)
        if (const PackEntry &e = drawn.entries[k];
            e.kind == EntryTexture && e.stage == 2 && e.slot < 16 && textures_[1][e.slot]) {
            at += at ? size_t(snprintf(sampled + at, sizeof sampled - at, " ")) : 0;
            const int n = glassProbeTexture(sampled + at, sizeof sampled - at, e.slot, *textures_[1][e.slot]);
            at += std::min(sizeof sampled - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < drawn.entries.size() && at + 1 < sizeof uniforms; k++)
        if (const PackEntry &e = drawn.entries[k]; e.kind == EntryUbo && !bufferStorage(e)) {
            const Bound &b = bufs_[e.stage][e.slot];
            at += at ? size_t(snprintf(uniforms + at, sizeof uniforms - at, " ")) : 0;
            const uint64_t window = (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16;
            const int n = glassProbeUniform(uniforms + at, sizeof uniforms - at, e.stage, e.slot, b.cpu,
                                            window ? std::min<uint64_t>(window, b.size) : b.size);
            at += std::min(sizeof uniforms - at - 1, size_t(std::max(n, 0)));
        }
    char taps[160] = "";
    if (glassProbeBlurState(caName)) {
        const Bound &u = bufs_[2][1];
        taps[0] = ' ';
        glassProbeTaps(taps + 1, sizeof taps - 1, u.cpu, u.size);
    }
    char line[1000];
    snprintf(line, sizeof line, "%s k%016llx %s att %s tex %s uni %s box %u,%u-%u,%u%s", caName ? caName : "-",
             (unsigned long long)rec.keyHash, form, attachments, sampled, uniforms, limit.x0, limit.y0, limit.x1,
             limit.y1, taps);
    GlassProbeTally ended;
    const bool first = glassProbeFirst(line, ended);
    if (ended.distinct)
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
               "SwitchMetal: glass draws g%u: %u distinct lines, %u repeats left out", ended.generation, ended.distinct,
               ended.repeats);
    if (!first || gGlassProbeLines.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
           "SwitchMetal: glass draw g%u cb %06llx %{public}s", gGlassProbeGeneration.load(std::memory_order_relaxed),
           (unsigned long long)(uintptr_t(cb_) & 0xffffff), line);
}

// The blur-texture probe's rows (glass_mem.h) of what a Core Animation draw samples: each texture's level 0 and, for a
// mip chain, its middle level, one row each, read at the command buffer's end. Surfaces are left out (other processes write
// them), and the copy engine reads plain memory only: a compressed binding hands it compressed bytes (comp.h).
void RenderEncoder::glassMemSample(const Pipeline &bound, const Pipeline &drawn)
{
    for (uint32_t k = 0; k < drawn.entries.size(); k++) {
        const PackEntry &e = drawn.entries[k];
        if (e.kind != EntryTexture || e.stage != 2 || e.slot >= 16 || !textures_[1][e.slot])
            continue;
        const Texture &t = *textures_[1][e.slot];
        if (t.import)
            continue;
        const uint32_t levels = t.chain ? t.chain->layout.levels : 1;
        const bool compressed = t.comptags && !t.zeta && copyEngineReadsCompressed(compressedKind(t.bpp, 0, t.samples));
        const char unread = t.comptags && !compressed ? 'c'
                            : t.memoryless            ? 'm'
                            : t.zeta                  ? 'z'
                            : t.converted()           ? 'v'
                            : t.sampled               ? 'p'
                            : t.standIn               ? 's'
                            : t.blockW == 1 && t.blockH == 1 && t.bpp && t.bpp <= 16 && t.samples == 1 &&
                                    t.layers == 1 && t.type == fx::kTexTwoD && t.va
                                ? 0
                                : 'o';
        uint64_t header = 0xcbf29ce484222325ull;
        if (t.tic)
            for (uint32_t b = 0; b < 32; b++)
                header = (header ^ dev_->tic_.cpu[32 * t.tic + b]) * 0x100000001b3ull;
        const uint32_t picks[2] = {0, levels > 1 ? (levels + 1) / 2 : 0};
        for (uint32_t p = 0; p < (picks[1] ? 2u : 1u); p++) {
            Texture level;
            if (picks[p] && !chainLevelImage(t, picks[p], level))
                continue;
            const Texture &image = picks[p] ? level : t;
            GlassMemItem it;
            it.key = t.storageRange().va ? t.storageRange().va : t.va;
            it.image = image.va;
            it.width = image.width;
            it.height = image.height;
            it.format = t.format;
            it.levels = levels;
            it.level = picks[p];
            it.bpp = t.bpp;
            it.texels = std::min(kGlassMemTexels, image.width);
            it.flags =
                uint8_t((t.arena ? kGlassMemHeap : 0) | (t.blockLinear ? 0 : kGlassMemPitch) |
                        (compressed ? kGlassMemCompressed : 0) | (unread || !image.height ? kGlassMemUnsampled : 0));
            it.unread = unread ? unread : image.height ? 0 : 'o';
            const int index = dev_->glassMem->sampled(it, __atomic_load_n(&bound.caName, __ATOMIC_ACQUIRE), t.tic,
                                                      header, dev_->commitCount_);
            if (index >= 0) {
                cb_->glassMemImages_[index] = image;
                cb_->glassMemSources_[index] = &t;
            }
        }
    }
}
#endif

#if SWITCH_METAL_DIAGNOSTICS
// The blur-texture probe's reads, after everything else the command buffer does: its draws' writes reach memory (the tiled
// cache's flush, the wait for idle) before the copy engine reads each row into a ring slot, read at a later begin() once
// this submission's fence passed. A full ring (no begin() between eight submissions) drops the record.
void CommandBuffer::glassMemTail()
{
    Device &d = *dev_;
    GlassMemCb record =
        d.glassMem->close(d.commitCount_, mach_absolute_time(), d.stats.memEvents.load(std::memory_order_relaxed));
    glassMemSlot_ = -1;
    for (uint32_t i = 0; i < record.count; i++) {
        GlassMemItem &it = record.items[i];
        if (const Texture *t = glassMemSources_[i]; t && t->arena && !t->view && !t->span.valid())
            it.flags |= kGlassMemAliased | kGlassMemUnsampled;
    }
    const GlassMemTailPlan plan = glassMemTailPlan(d.glassMemLive_, record);
    if (!plan.ring)
        return;
    record.launches = plan.launches;
    if (!d.glassMemRing_.va) {
        if (!d.k_.allocControl(uint64_t(kGlassMemSlots) * kGlassMemSlotBytes, d.glassMemRing_,
                               ControlUse::TransferSlot))
            return;
        memset(d.glassMemRing_.cpu, 0, uint64_t(kGlassMemSlots) * kGlassMemSlotBytes);
    }
    int slot = -1;
    for (uint32_t s = 0; s < kGlassMemSlots && slot < 0; s++)
        if (!d.glassMemBusy_[s])
            slot = int(s);
    if (slot < 0)
        return;
    const uint32_t words = 24 + record.count * 256;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, words * 4, 256, va));
    Push p{start, start + words};
    tiledCacheDrain(p, d.k_.info());
    p.m(WAIT_FOR_IDLE, 0);
    const uint64_t base = d.glassMemRing_.va + uint64_t(slot) * kGlassMemSlotBytes;
    const uint64_t cookie = base + uint64_t(kGlassMemItems) * kGlassMemItemBytes, begun = cookie + kGlassMemStartAt;
    p.inc(HOST_SEMAPHORE_A, {uint32_t(begun >> 32) & 0xff, uint32_t(begun), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    memset(d.glassMemRing_.cpu + uint64_t(slot) * kGlassMemSlotBytes, kGlassMemPoison,
           uint64_t(record.count) * kGlassMemItemBytes);
    for (uint32_t i = 0; i < record.count; i++) {
        const GlassMemItem &it = record.items[i];
        if (it.flags & kGlassMemUnsampled)
            continue;
        const Texture &image = glassMemImages_[i];
        const uint64_t row = base + uint64_t(i) * kGlassMemItemBytes;
        copySides(p, sideOf(image, it.inX, it.inY, 0), {row, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
        if (it.outside)
            copySides(p, sideOf(image, it.outX, it.outY, 0), {row + kGlassMemRowBytes, it.texels * it.bpp}, it.texels,
                      1, it.bpp, d.stats);
        for (uint32_t q = 0; it.quarters && q < 2; q++)
            copySides(p, sideOf(image, it.inX, it.height * (1 + 2 * q) / 4, 0),
                      {row + (2 + q) * kGlassMemRowBytes, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
    }
    p.inc(HOST_SEMAPHORE_A, {uint32_t(cookie >> 32) & 0xff, uint32_t(cookie), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    checkRoom(p, "the glass memory probe's reads");
    segment(start, p.at);
    if (d.k_.exactJobs())
        surfaces_.add(d.glassMemRing_.handle, true);
    d.glassMemCbs_[slot] = record;
    glassMemSlot_ = slot;
}

// At begin(), where the probe is armed or was: armed again after a pause it starts with no history (what was written
// meanwhile went unseen); still armed, the command buffer starts its record; disarmed, the rows in flight are read out
// and nothing else runs until it is armed again.
void Device::glassMemFrame()
{
    const uint64_t now = mach_absolute_time();
    bool on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
    if (!on) {
        glassMemOn.store(false, std::memory_order_relaxed);
        on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
        if (on)
            glassMemOn.store(true, std::memory_order_relaxed);
    }
    if (on && !glassMemLive_)
        glassMem->reset();
    glassMemLive_ = on;
    glassMemRead(now);
    if (on)
        glassMem->begin();
}

// The probe's records whose rows landed, oldest first (a reused texture's row is judged against the one its last write
// left): a line for each that found something, or for each while HW Test armed it, and a summary every 5 s, each with
// what the probe's own reads cost. A killed submission stops the probe; a record a reopened channel's fence never
// reaches is dropped after 120 command buffers.
void Device::glassMemRead(uint64_t now)
{
    const os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    const bool every = glassMemArming.every(now);
    for (;;) {
        int next = -1;
        for (uint32_t s = 0; s < kGlassMemSlots; s++)
            if (glassMemBusy_[s] && (next < 0 || glassMemCbs_[s].serial < glassMemCbs_[next].serial))
                next = int(s);
        if (next < 0)
            break;
        if (!done(glassMemCbs_[next].seqno)) {
            if (commitCount_ - glassMemCbs_[next].serial <= 120)
                break;
            glassMemBusy_[next] = false;
            glassMemBusyCount_--;
            continue;
        }
        glassMemBusy_[next] = false;
        glassMemBusyCount_--;
        if (lost(glassMemCbs_[next].seqno)) {
            if (!glassMemStopped.exchange(true))
                os_log_error(
                    log,
                    "SwitchMetal: glassmem: command buffer %llu carrying the probe's reads was killed: the probe is "
                    "off for this process",
                    (unsigned long long)glassMemCbs_[next].serial);
            continue;
        }
        uint8_t rows[kGlassMemSlotBytes];
        memcpy(rows, glassMemRing_.cpu + uint64_t(next) * kGlassMemSlotBytes, sizeof rows);
        const uint32_t cookie = glassMemCookie(glassMemCbs_[next].serial);
        StampRelease begun, ended;
        memcpy(&ended, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes, sizeof ended);
        memcpy(&begun, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes + kGlassMemStartAt, sizeof begun);
        const uint64_t probeNs = begun.payload == cookie && ended.payload == cookie && ended.ptimer >= begun.ptimer
                                     ? ptimerNanoseconds(ended.ptimer - begun.ptimer)
                                     : 0;
        for (const std::string &line : glassMem->finish(glassMemCbs_[next], rows, every, probeNs))
            os_log(log, "SwitchMetal: glassmem %{public}s", line.c_str());
    }
    if (!glassMemLive_ || every || now - glassMemSummaryAt_ < glassProbeTicks(5000))
        return;
    const GlassMemCounts &c = glassMem->counts;
    const uint64_t cbs = c.cbs.load() - glassMemSummaryCbs_, per = cbs ? cbs : 1;
    if (cbs)
        os_log(log,
               "SwitchMetal: glassmem totals cbs %llu rebuilt %llu reused %llu zero-reused %llu nf %llu chg %llu mv "
               "%llu ev "
               "%llu probe ce %.1f/cb gpu %.0fus/cb",
               (unsigned long long)c.cbs.load(), (unsigned long long)c.rebuilt.load(),
               (unsigned long long)c.reused.load(), (unsigned long long)c.black.load(),
               (unsigned long long)c.nonFinite.load(), (unsigned long long)c.changed.load(),
               (unsigned long long)c.moved.load(), (unsigned long long)stats.memEvents.load(),
               double(c.launches.load() - glassMemSummaryLaunches_) / double(per),
               double(c.probeNs.load() - glassMemSummaryNs_) / 1000.0 / double(per));
    glassMemSummaryAt_ = now;
    glassMemSummaryCbs_ = c.cbs.load();
    glassMemSummaryLaunches_ = c.launches.load();
    glassMemSummaryNs_ = c.probeNs.load();
}

// A GPU-memory event, numbered: the render server's lines are what a reused texture's /e<n> counts
void Device::glassMemEvent(const char *what, uint64_t bytes)
{
    const uint64_t n = ++stats.memEvents;
    if (glassMemOn.load(std::memory_order_relaxed))
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b"),
               "SwitchMetal: glassmem event %llu %{public}s %llu KiB", (unsigned long long)n, what,
               (unsigned long long)(bytes >> 10));
}
#endif

}  // namespace gm20b
