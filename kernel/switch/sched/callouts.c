// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thread call functions and timer actions by CPU (callouts.h), debug kernels: one line with each 10 s scheduling
 * report. A hook runs with interrupts masked from its read of the thread's on-core time to its row update, which also
 * keeps it on the CPU whose table it writes, so each table has one writer.
 */

#include <kern/misc_protos.h>
#include <switch/callouts.h>
#include <switch/sched.h>

#include "callouts.h"

#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/recount.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <pexpert/arm64/board_config.h>

#if DEVELOPMENT || DEBUG

static struct switch_callout_table callout_tables[SWITCH_CALLOUT_KINDS][MAX_CPUS] __attribute__((aligned(64)));
static struct switch_callout_sum callout_seen[SWITCH_CALLOUT_KINDS];
static struct switch_callout_sum callout_now;

uint64_t switch_callout_begin(void)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t t = recount_current_thread_time_mach();

    ml_set_interrupts_enabled(enabled);
    return t;
}

void switch_callout_end(enum switch_callout_kind kind, const void *function, uint64_t begin)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t t = recount_current_thread_time_mach();

    switch_callout_add(&callout_tables[kind][cpu_number()], (uint64_t)(uintptr_t)function, t - MIN(t, begin));
    ml_set_interrupts_enabled(enabled);
}

static uint64_t unslide(uint64_t address)
{
    return (uint64_t)VM_KERNEL_UNSLIDE((uintptr_t)address);
}

void switch_sched_callout_report(char *line, size_t size, uint64_t window_ns)
{
    static const char *const kinds[SWITCH_CALLOUT_KINDS] = {"functions", "timer actions"};
    mach_timebase_info_data_t tb;
    size_t at = 0;

    clock_timebase_info(&tb);
    at = switch_callout_put(size, 0,
                            snprintf(line, size, "SwitchSched callouts (%llu ms; on-core ms (calls), unslid): ",
                                     window_ns / NSEC_PER_MSEC));
    for (unsigned int kind = 0; kind < SWITCH_CALLOUT_KINDS; kind++) {
        __builtin_memset(&callout_now, 0, sizeof(callout_now));
        for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
            switch_callout_sum_table(&callout_now, &callout_tables[kind][cpu]);
        }
        at = switch_callout_put(size, at, snprintf(line + at, size - at, "%s%s ", kind ? "; " : "", kinds[kind]));
        at += switch_callout_format(line + at, size - at, &callout_now, &callout_seen[kind], unslide, tb.numer,
                                    tb.denom);
    }
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
