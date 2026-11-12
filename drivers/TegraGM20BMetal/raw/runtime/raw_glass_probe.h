// SPDX-License-Identifier: MIT
// DEVELOPMENT: what each Core Animation draw reads while HW Test's glass tests hold the glass probe armed in every
// process (com.brunostjohn.switch.debug.glass-probe, RawMetalGlassProbe.mm). A draw's line (raw_gpu.cpp probeGlassDraw)
// names its state, its program's form, its colour attachments, each texture it samples with whether this process ever
// wrote it (a pass, copy, dispatch or upload) and the non-finite floats and halves in its uniform buffers' CPU view. A
// command buffer logs each distinct line once; the window ends after kGlassProbeMs or kGlassProbeLines lines.
#pragma once
#include <mach/mach_time.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include "raw_nonfinite.h"

namespace raw {

constexpr uint64_t kGlassProbeMs = 20000;
constexpr int32_t kGlassProbeLines = 2000;
// A uniform buffer's bytes the probe scans at most
constexpr uint64_t kGlassProbeUniformBytes = 64 << 10;

inline std::atomic<uint32_t> gGlassProbeGeneration{0};
inline std::atomic<int32_t> gGlassProbeLines{0};
inline std::atomic<uint64_t> gGlassProbeUntil{0};

inline uint64_t glassProbeTicks(uint64_t ms)
{
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    return ms * 1000000 * base.denom / base.numer;
}

inline void armGlassProbe()
{
    gGlassProbeLines.store(kGlassProbeLines, std::memory_order_relaxed);
    gGlassProbeUntil.store(mach_absolute_time() + glassProbeTicks(kGlassProbeMs), std::memory_order_relaxed);
    gGlassProbeGeneration.fetch_add(1, std::memory_order_relaxed);
}

inline void disarmGlassProbe()
{
    gGlassProbeLines.store(0, std::memory_order_relaxed);
}

inline bool glassProbeArmed()
{
    return gGlassProbeLines.load(std::memory_order_relaxed) > 0 &&
           mach_absolute_time() < gGlassProbeUntil.load(std::memory_order_relaxed);
}

// A command buffer of the render server's record (RawMetalGlassProbe.mm) in one notify state word: draws in bits 0-9,
// encoders 10-17, passes into a mip level above 0 18-23 (each count saturating), the commit's mach time's low 40 bits
// above; hw_test_app's GlassProbe.swift unpacks it
struct GlassCbEntry {
    uint32_t draws = 0, encoders = 0, levels = 0;
    uint64_t ticks = 0;

    uint64_t packed() const
    {
        return uint64_t(std::min(draws, 1023u)) | uint64_t(std::min(encoders, 255u)) << 10 |
               uint64_t(std::min(levels, 63u)) << 18 | (ticks & ((1ull << 40) - 1)) << 24;
    }

    static GlassCbEntry unpack(uint64_t word)
    {
        GlassCbEntry e;
        e.draws = uint32_t(word & 1023);
        e.encoders = uint32_t(word >> 10 & 255);
        e.levels = uint32_t(word >> 18 & 63);
        e.ticks = word >> 24;
        return e;
    }
};

// True the first time `line` comes in command buffer `cb` this generation (FNV-1a of its text)
inline bool glassProbeFirst(const void *cb, const char *line)
{
    struct Seen {
        const void *cb = nullptr;
        uint32_t generation = 0;
        std::unordered_set<uint64_t> lines;
    };

    static thread_local Seen seen;
    const uint32_t generation = gGlassProbeGeneration.load(std::memory_order_relaxed);
    if (seen.cb != cb || seen.generation != generation) {
        seen.cb = cb;
        seen.generation = generation;
        seen.lines.clear();
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char *c = line; *c; ++c)
        hash = (hash ^ uint8_t(*c)) * 0x100000001b3ull;
    return seen.lines.insert(hash).second;
}

// "t<slot>:<w>x<h>/f<format>/<bl|pitch>[/L<levels>][+ml][+heap][+imp][+comp][+si][+pi][+sh]/<w|nw>": a sampled texture
// (+heap: placed in an MTLHeap, whose pages a purge takes), w where this process wrote it (an imported surface's other
// writers are not seen)
template <class Texture> int glassProbeTexture(char *out, size_t size, uint32_t slot, const Texture &t)
{
    char levels[16] = "";
    if (t.chain)
        snprintf(levels, sizeof levels, "/L%u", unsigned(t.chain->layout.levels));
    return snprintf(out, size, "t%u:%ux%u/f%u/%s%s%s%s%s%s%s%s%s/%s", slot, t.width, t.height, t.format,
                    t.blockLinear ? "bl" : "pitch", levels, t.memoryless ? "+ml" : "", t.arena ? "+heap" : "",
                    t.import ? "+imp" : "", t.comptags ? "+comp" : "", t.standIn ? "+si" : "", t.sampled ? "+pi" : "",
                    t.shadowTarget ? "+sh" : "", t.probeWritten ? "w" : "nw");
}

// "u<stage>.<slot>:<bytes>/<words>f<halves>h": a uniform buffer's non-finite 32-bit floats and halves, "nocpu" without
// a CPU view
inline int glassProbeUniform(char *out, size_t size, uint32_t stage, uint32_t slot, const uint8_t *cpu, uint64_t bytes)
{
    if (!cpu)
        return snprintf(out, size, "u%u.%u:%llu/nocpu", stage, slot, (unsigned long long)bytes);
    const NonFiniteUniforms found = nonFiniteUniforms(cpu, std::min(bytes, kGlassProbeUniformBytes));
    return snprintf(out, size, "u%u.%u:%llu/%uf%uh", stage, slot, (unsigned long long)bytes, found.words, found.halves);
}

}  // namespace raw
