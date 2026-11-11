// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchVICTurnRun.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <string.h>
#include "SwitchVICTurn.h"
#include "../NativeMobileDisplay/SwitchLandscapeRunABI.h"

extern "C" void proc_selfname(char *buf, int size);
extern "C" IOReturn switch_display_landscape_run_call(const IOExternalMethodArguments *) __attribute__((weak));

namespace {
SwitchVICTurnCounters gCounters;
constexpr unsigned kShapes = 32;
uint64_t gShapes[kShapes];

uint64_t shapeOf(const TegraVICRequest &r, const TegraVICSurfaceDesc &s, const TegraVICSurfaceDesc &d, int pid)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t words[] = {uint64_t(pid),     r.flags & 0x7ff,  s.fourcc, uint64_t(s.width) << 32 | s.height,
                              d.fourcc,          uint64_t(d.width) << 32 | d.height,
                              uint64_t(!!r.src_event_port) << 2 | uint64_t(!!r.dst_event_port) << 1 | !!r.cb_fn};
    for (const uint64_t w : words)
        h = (h ^ w) * 1099511628211ull;
    return h | 1;
}

// True the first time a shape is seen (the table full: never again).
bool firstOf(uint64_t shape)
{
    for (auto &slot : gShapes) {
        uint64_t seen = __atomic_load_n(&slot, __ATOMIC_ACQUIRE);
        if (seen == shape)
            return false;
        if (!seen && __atomic_compare_exchange_n(&slot, &seen, shape, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
        if (seen == shape)
            return false;
    }
    return false;
}

void fourcc(char out[5], uint32_t f)
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = char(f >> (24 - 8 * i));
    out[4] = 0;
}
}  // namespace

void switch_vic_turn_request_seen(const TegraVICRequest &r, const TegraVICSurfaceDesc &s,
                                  const TegraVICSurfaceDesc &d, int pid)
{
    if (!firstOf(shapeOf(r, s, d, pid)))
        return;
    char name[32] = {}, from[5], to[5];
    proc_selfname(name, sizeof(name));
    fourcc(from, s.fourcc);
    fourcc(to, d.fourcc);
    char line[256];
    snprintf(line, sizeof(line),
             "TegraVIC: request shape: %s[%d] transform %llu '%s' %ux%u src %llu,%llu %llux%llu (16.16) -> '%s' %ux%u "
             "dst %u,%u %ux%u; events src %u (wait %llu signal %llu) dst %u (wait %llu signal %llu); callback %d skip %d",
             name, pid, r.flags & TEGRA_VIC_REQ_TRANSFORM, from, s.width, s.height, r.src_x, r.src_y, r.src_w, r.src_h,
             to, d.width, d.height, r.dst_x, r.dst_y, r.dst_w, r.dst_h, r.src_event_port, r.src_wait, r.src_signal,
             r.dst_event_port, r.dst_wait, r.dst_signal, r.cb_fn != 0, (r.flags & TEGRA_VIC_REQ_SKIP_CALLBACK) != 0);
    os_log(OS_LOG_DEFAULT, "%s", line);
}

void switch_vic_turn_count(bool submitted, bool turned, bool eventTurn, uint64_t ticks)
{
    if (submitted)
        __atomic_add_fetch(&gCounters.jobs, 1u, __ATOMIC_RELAXED);
    if (turned)
        __atomic_add_fetch(&gCounters.turns, 1u, __ATOMIC_RELAXED);
    if (eventTurn)
        __atomic_add_fetch(&gCounters.eventTurns, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gCounters.ticks, ticks, __ATOMIC_RELAXED);
}

extern "C" void switch_vic_turn_counters(SwitchVICTurnCounters *out)
{
    out->jobs = __atomic_load_n(&gCounters.jobs, __ATOMIC_RELAXED);
    out->turns = __atomic_load_n(&gCounters.turns, __ATOMIC_RELAXED);
    out->eventTurns = __atomic_load_n(&gCounters.eventTurns, __ATOMIC_RELAXED);
    out->ticks = __atomic_load_n(&gCounters.ticks, __ATOMIC_RELAXED);
}

// FORCE is held for the calling connection (TegraVIC drops it when that connection closes); SAMPLE and QUERY are the
// display's.
IOReturn switch_vic_landscape_call(const void *client, const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const SwitchDisplayLandscapeRequest *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != sizeof(*request) || !request ||
        request->magic != SWITCH_DISPLAY_LANDSCAPE_RUN_MAGIC || request->reserved)
        return kIOReturnBadArgument;
    if (request->op == SWITCH_DISPLAY_LANDSCAPE_FORCE) {
        if (request->value & ~(SWITCH_DISPLAY_TRAIT_OVERLAY_BLOCK_YUV | SWITCH_DISPLAY_TRAIT_COLUMN_SCAN_YUV |
                               SWITCH_DISPLAY_TRAIT_OFFSET_CROP))
            return kIOReturnBadArgument;
        if (request->value)
            switch_display_traits_force(request->value, client);
        else
            switch_display_traits_release(client);
        return kIOReturnSuccess;
    }
    return switch_display_landscape_run_call ? switch_display_landscape_run_call(a) : kIOReturnUnsupported;
}
#endif
