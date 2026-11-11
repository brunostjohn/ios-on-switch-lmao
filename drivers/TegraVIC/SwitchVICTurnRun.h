// SPDX-License-Identifier: BSD-2-Clause-Patent
// What HW Test's display.x1.landscape_video reads of TegraVIC (debug kernels only): each IOSurfaceAccelerator request
// shape once (who asks, the transform, the formats and sizes, whether it carries events or a callback), the jobs and
// recorded quarter turns, and TegraVIC's CPU time.
#pragma once
#include <stdint.h>
#include "TegraVIC.h"

#if TEGRA_VIC_STATS
#include "TegraVICRequest.h"
#include "TegraVICSurface.h"

struct SwitchVICTurnCounters {
    uint32_t jobs;        // submitted to VIC
    uint32_t turns;       // quarter turns recorded instead of copied
    uint32_t eventTurns;  // quarter turns copied because the request carries events
    uint64_t ticks;       // mach_absolute_time in requests and on the engine thread
};

void switch_vic_turn_request_seen(const TegraVICRequest &request, const TegraVICSurfaceDesc &source,
                                  const TegraVICSurfaceDesc &destination, int pid);
void switch_vic_turn_count(bool submitted, bool turned, bool eventTurn, uint64_t ticks);
extern "C" void switch_vic_turn_counters(SwitchVICTurnCounters *out);
#endif
