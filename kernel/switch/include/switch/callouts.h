// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thread call functions and IOTimerEventSource actions by the CPU they used (kernel/switch/sched/callouts.c, debug
 * kernels; a release kernel compiles both hooks to nothing). thread_call_invoke brackets each callout's function, and
 * IOTimerEventSource's thread call timeouts bracket the driver action they run, so a "thread call kernel-high #N" in
 * the scheduling report has owners by name: begin returns the thread's on-core time, end adds the difference to the
 * function's row.
 */
#ifndef SWITCH_CALLOUTS_HOOKS_H
#define SWITCH_CALLOUTS_HOOKS_H

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

enum switch_callout_kind {
    SWITCH_CALLOUT_FUNCTION,
    SWITCH_CALLOUT_TIMER_ACTION,
    SWITCH_CALLOUT_KINDS
};

#if DEVELOPMENT || DEBUG
uint64_t switch_callout_begin(void);
void switch_callout_end(enum switch_callout_kind kind, const void *function, uint64_t begin);
#else
static inline uint64_t switch_callout_begin(void)
{
    return 0;
}

static inline void switch_callout_end(__unused enum switch_callout_kind kind, __unused const void *function,
                                      __unused uint64_t begin)
{}
#endif

__END_DECLS

#endif /* SWITCH_CALLOUTS_HOOKS_H */
