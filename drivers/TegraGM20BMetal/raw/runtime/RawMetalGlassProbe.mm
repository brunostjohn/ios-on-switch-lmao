// SPDX-License-Identifier: MIT
// DEVELOPMENT: the switches HW Test's glass tests (hw_test_app app27/GlassProbe.swift) post to every process's raw
// path, and the render server's record of its command buffers they read back.
// Each name below follows com.brunostjohn.switch.debug.:
//   glass-probe[-off]           the draw lines (raw_glass_probe.h) and the record below
//   glass-record                the record alone (no lines to slow a moving scene)
//   whole-programs-on|-off      every command buffer draws whole programs (switchRawWholePrograms)
//   zero-new-buffers-on|-off    every new buffer starts zeroed
// glass-probe and glass-record also arm the blur-texture probe's per-command-buffer lines (raw_glass_mem.h), whose counts
// and the pool's shape this file publishes as glass-mem and glass-mem.pool.
// A switch left on turns itself off after kForcedSeconds: a test that died with it on leaves no process forced.
// The record: each command buffer the render server commits for kRecordSeconds after either arms it, as its API made it
// (draws, encoders, passes into a mip level above 0, the commit's mach time), in notify state, which crosses HW Test's
// sandbox where a file would not: glass-cb holds generation << 32 | count, glass-cb.<count % kRecordSlots> the latest
// entries (raw::GlassCbEntry). Either arming starts the next generation, which the render server's draw lines carry
// too.
#import "RawMetalInternal.h"

#include <notify.h>
#include <mutex>
#include "raw_glass_probe.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
namespace {

constexpr int64_t kForcedSeconds = 60;
constexpr uint64_t kRecordSeconds = 20;
constexpr uint32_t kRecordSlots = 16;
constexpr const char *kRecordName = "com.brunostjohn.switch.debug.glass-cb";

struct Forced {
    std::atomic<bool> on{false};
    std::atomic<uint32_t> generation{0};
};

Forced gWholePrograms, gZeroNewBuffers;

struct Record {
    std::mutex lock;
    std::atomic<bool> armed{false};
    std::atomic<uint64_t> until{0};
    uint32_t generation = 0, count = 0;
    int countToken = -1;
    int slotTokens[kRecordSlots];
};

Record &record()
{
    static Record *r = new Record;
    return *r;
}

void watchForced(Forced &f, const char *name)
{
    for (const bool on : {true, false}) {
        char full[96];
        snprintf(full, sizeof full, "com.brunostjohn.switch.debug.%s-%s", name, on ? "on" : "off");
        int token;
        notify_register_dispatch(full, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            f.on.store(on, std::memory_order_relaxed);
            const uint32_t generation = f.generation.fetch_add(1, std::memory_order_relaxed) + 1;
            os_log(rawLog(), "raw: %{public}s %{public}s (debug notification)", name, on ? "on" : "off");
            if (!on)
                return;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kForcedSeconds * NSEC_PER_SEC),
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                               if (f.generation.load(std::memory_order_relaxed) != generation)
                                   return;
                               f.on.store(false, std::memory_order_relaxed);
                               os_log(rawLog(), "raw: %{public}s off after %lld s", name, kForcedSeconds);
                           });
        });
    }
}

void armRecord(bool armed)
{
    Record &r = record();
    std::lock_guard<std::mutex> l(r.lock);
    if (r.countToken < 0) {
        if (notify_register_check(kRecordName, &r.countToken) != NOTIFY_STATUS_OK)
            return;
        for (uint32_t i = 0; i < kRecordSlots; i++) {
            char name[96];
            snprintf(name, sizeof name, "%s.%u", kRecordName, i);
            notify_register_check(name, &r.slotTokens[i]);
        }
    }
    r.armed.store(armed, std::memory_order_relaxed);
    if (armed) {
        r.until.store(mach_absolute_time() + raw::glassProbeTicks(kRecordSeconds * 1000), std::memory_order_relaxed);
        r.generation = raw::gGlassProbeGeneration.load(std::memory_order_relaxed);
        r.count = 0;
    }
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
}

// The blur-texture probe's lines for every command buffer for the next kGlassMemArmMs (raw_glass_mem.h), and the probe
// back on where a killed submission had stopped it
void armGlassMem(raw::Device *dev)
{
    if (!dev->glassMem)
        return;
    dev->glassMemArmedUntil.store(mach_absolute_time() + raw::glassProbeTicks(raw::kGlassMemArmMs),
                                  std::memory_order_relaxed);
    dev->glassMemStopped.store(false, std::memory_order_relaxed);
}

// The probe's counts and the pool's shape in notify state for HW Test (ui.glass_sidebar): each commit while armed, else
// once a second
void publishGlassMem(raw::Device &d)
{
    static std::atomic<uint64_t> last{0};
    static int memToken = -1, poolToken = -1;
    static std::once_flag registered;
    if (!d.glassMem)
        return;
    const uint64_t now = mach_absolute_time();
    const bool armed = now < d.glassMemArmedUntil.load(std::memory_order_relaxed);
    if (!armed && now - last.load(std::memory_order_relaxed) < raw::glassProbeTicks(1000))
        return;
    last.store(now, std::memory_order_relaxed);
    std::call_once(registered, [] {
        notify_register_check("com.brunostjohn.switch.debug.glass-mem", &memToken);
        notify_register_check("com.brunostjohn.switch.debug.glass-mem.pool", &poolToken);
    });
    const raw::Device::Stats &s = d.stats;
    if (memToken >= 0)
        notify_set_state(memToken, raw::glassMemWord(d.glassMem->counts));
    if (poolToken >= 0)
        notify_set_state(poolToken,
                         raw::glassMemPoolWord(s.poolBytes.load() >> 20, s.poolChunks.load(), s.backings.load(),
                                               s.backingBytes.load() >> 20, s.compHeld.load(), s.compMade.load()));
}

}  // namespace

bool rmWholePrograms()
{
    return gWholePrograms.on.load(std::memory_order_relaxed);
}

bool rmZeroNewBuffers()
{
    return gZeroNewBuffers.on.load(std::memory_order_relaxed);
}

void watchGlassProbe(raw::Device *dev)
{
    const bool renderServer = dev->role() == raw::Device::Role::RenderServer;
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 raw::armGlassProbe();
                                 if (renderServer)
                                     armRecord(true);
                                 armGlassMem(dev);
                                 os_log(rawLog(), "raw: glass probe armed g%u (debug notification)",
                                        raw::gGlassProbeGeneration.load(std::memory_order_relaxed));
                             });
    if (renderServer)
        notify_register_dispatch("com.brunostjohn.switch.debug.glass-record", &token,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                     raw::gGlassProbeGeneration.fetch_add(1, std::memory_order_relaxed);
                                     armRecord(true);
                                     armGlassMem(dev);
                                 });
    notify_register_dispatch("com.brunostjohn.switch.debug.glass-probe-off", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 raw::disarmGlassProbe();
                                 if (renderServer)
                                     armRecord(false);
                             });
    watchForced(gWholePrograms, "whole-programs");
    watchForced(gZeroNewBuffers, "zero-new-buffers");
}

void glassProbeCommitted(RMCommandBuffer *cb)
{
    publishGlassMem(*gRM->raw);
    Record &r = record();
    raw::GlassCbEntry e;
    e.ticks = mach_absolute_time();
    if (!r.armed.load(std::memory_order_relaxed) || e.ticks >= r.until.load(std::memory_order_relaxed))
        return;
    for (size_t at = 0; at < cb->_plan.size(); at++) {
        const Cmd c = cb->_plan.legacy(at);
        e.draws += isPlanDraw(c.op);
        e.encoders += c.op == OpRender || c.op == OpCompute || c.op == OpBlit;
        if (c.op != OpRender || c.x >= cb->_passes.size())
            continue;
        const RMPass &pass = cb->_passes[c.x];
        bool level = false;
        for (const RMPass::Color &a : pass.color)
            level |= a.texture && a.level;
        e.levels += level;
    }
    std::lock_guard<std::mutex> l(r.lock);
    if (!r.armed || r.countToken < 0)
        return;
    notify_set_state(r.slotTokens[r.count % kRecordSlots], e.packed());
    r.count++;
    notify_set_state(r.countToken, uint64_t(r.generation) << 32 | r.count);
    os_log(rawLog(), "raw: glass cb g%u #%u draws %u encoders %u levels %u (mach %llu)", r.generation, r.count, e.draws,
           e.encoders, e.levels, (unsigned long long)e.ticks);
}
#endif
