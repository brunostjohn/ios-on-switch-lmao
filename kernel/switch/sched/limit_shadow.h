// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple's blocking per-thread CPU limit, replayed beside one this port lifted (<switch/cpulimit.h>; limit_shadow.c,
 * debug kernels). XNU's ledger refills a thread's allowance every refill interval; once the thread has run its budget
 * (the percentage of the interval) in an interval, it sleeps to the refill. Here it runs on: an interval in which it
 * passed its budget is a block Apple's limit would have made, and the time it ran past the budget is what that block
 * would have taken. Intervals start where the thread first ran and follow on whole, as the ledger's refills do. Plain
 * C, so kernel/tests runs it on the host. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_LIMIT_SHADOW_H
#define SWITCH_SCHED_LIMIT_SHADOW_H

#include <stdint.h>

struct switch_limit_shadow {
    uint64_t interval, budget;
    uint64_t start, used; /* the current interval's start (0 before the first run) and the time run in it */
    uint64_t blocks, over;
};

static inline void switch_limit_shadow_init(struct switch_limit_shadow *s, uint64_t interval, uint8_t percentage)
{
    s->interval = interval ? interval : 1;
    s->budget = interval * percentage / 100u;
    s->start = s->used = 0;
    s->blocks = s->over = 0;
}

/* The thread ran over [from, to). */
static inline void switch_limit_shadow_ran(struct switch_limit_shadow *s, uint64_t from, uint64_t to)
{
    while (from < to) {
        if (s->start == 0) {
            s->start = from;
            s->used = 0;
        } else if (from >= s->start + s->interval) {
            s->start += (from - s->start) / s->interval * s->interval;
            s->used = 0;
        }
        const uint64_t end = to < s->start + s->interval ? to : s->start + s->interval;
        const uint64_t before = s->used;

        s->used += end - from;
        if (before <= s->budget && s->used > s->budget) {
            s->blocks++;
        }
        if (s->used > s->budget) {
            s->over += s->used - (before > s->budget ? before : s->budget);
        }
        from = end;
    }
}

#endif /* SWITCH_SCHED_LIMIT_SHADOW_H */
