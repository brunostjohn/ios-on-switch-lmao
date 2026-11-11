// SPDX-License-Identifier: BSD-2-Clause-Patent
// Syncpoint fences: a caller-owned node that completes when its syncpoint reaches its threshold. Each syncpoint keeps
// its pending fences sorted, with its threshold interrupt armed at the earliest (SwitchHost1xFenceList.h), so any
// number of fences share one syncpoint and none needs a thread.
//
// A fence's callback runs once, on host1x's interrupt work loop (thread context, interrupts on), with status 0 when
// the syncpoint reached the threshold. It never runs inside switch_host1x_fence_add: one already reached there is
// handed to the work loop. The work loop serves every engine's syncpoints, so a callback takes locks only briefly and
// never sleeps, waits or takes another driver's command gate. A fence given a bound that it outlives fails with
// TEGRA_HOST1X_ETIMEDOUT when its syncpoint never got there; when the syncpoint got there but the interrupt never came,
// that is a panic once a client is strict (tegra_host1x_set_strict), TEGRA_HOST1X_ELOST before.
#ifndef SWITCH_HOST1X_FENCE_H
#define SWITCH_HOST1X_FENCE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_HOST1X_FENCE_IDLE = 0,
    SWITCH_HOST1X_FENCE_QUEUED = 1,      // in its syncpoint's list
    SWITCH_HOST1X_FENCE_HANDED_OFF = 2,  // expired, waiting for the work loop
    SWITCH_HOST1X_FENCE_DELIVERING = 3,  // its callback is about to run
};

struct switch_host1x_fence {
    struct switch_host1x_fence *prev, *next;
    void (*signaled)(struct switch_host1x_fence *fence, int status);
    uint64_t deadline;
    uint32_t syncpt, threshold;
    int status;
    uint8_t state, graced;
};

void switch_host1x_fence_init(struct switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                              void (*signaled)(struct switch_host1x_fence *fence, int status));
// Queues an idle fence. boundUs 0 leaves it unbounded until switch_host1x_fence_watch. TEGRA_HOST1X_OK: the callback
// will run (unless removed first); otherwise it will not.
int switch_host1x_fence_add(struct switch_host1x_fence *fence, uint32_t boundUs);
// Starts (or restarts) a queued fence's bound now; nothing once it has expired.
void switch_host1x_fence_watch(struct switch_host1x_fence *fence, uint32_t boundUs);
// True: the fence was taken back before its callback could run, and is idle. False: the callback has run or will.
bool switch_host1x_fence_remove(struct switch_host1x_fence *fence);

#ifdef __cplusplus
}
#endif
#endif
