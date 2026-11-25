// SPDX-License-Identifier: GPL-2.0-only
/*
 * User threads' kernel time by what entered the kernel (systime.h), debug kernels: one line with each 10 s scheduling
 * report (stats.c). Each CPU's counts have one writer, that CPU: sleh_synchronous counts an entry before it enables
 * interrupts, and recount absorbs a slice with interrupts masked. The report reads every CPU's counts from its own
 * thread, so a count it reads may miss the slice being added at that moment; the next window has it.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>

#include "systime.h"

#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/arm/thread_status.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <pexpert/arm64/board_config.h>

#if DEVELOPMENT || DEBUG

struct systime_cpu {
    struct switch_systime counts;
} __attribute__((aligned(64)));

static struct systime_cpu systime_cpu[MAX_CPUS];
static struct switch_systime systime_seen;

static enum switch_systime_bucket bucket_of(thread_t thread)
{
    const uint16_t code = thread->machine.exception_trace_code;
    int64_t svc = 0;

    if ((code & SWITCH_SYSTIME_TRACE_EC) == SWITCH_SYSTIME_EC_SVC_64 && thread->machine.upcb != NULL) {
        svc = (int64_t)(int)get_saved_state_svc_number(thread->machine.upcb);
    }
    return switch_systime_trace_bucket(code, svc);
}

static void add(uint64_t *count, uint64_t n)
{
    os_atomic_store(count, os_atomic_load(count, relaxed) + n, relaxed);
}

void switch_systime_entered(thread_t thread)
{
    add(&systime_cpu[cpu_number()].counts.entries[bucket_of(thread)], 1);
}

void switch_systime_absorb(thread_t thread, task_t task, uint64_t mach)
{
    if (task == kernel_task || mach == 0) {
        return;
    }
    add(&systime_cpu[cpu_number()].counts.time[bucket_of(thread)], mach);
}

void switch_sched_systime_report(char *line, size_t size, uint64_t window_ns)
{
    struct switch_systime now = {{0}};
    struct switch_systime window = {{0}};
    mach_timebase_info_data_t tb;
    size_t at = 0;

    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        for (unsigned int b = 0; b < SWITCH_SYSTIME_BUCKETS; b++) {
            now.time[b] += os_atomic_load(&systime_cpu[cpu].counts.time[b], relaxed);
            now.entries[b] += os_atomic_load(&systime_cpu[cpu].counts.entries[b], relaxed);
        }
    }
    for (unsigned int b = 0; b < SWITCH_SYSTIME_BUCKETS; b++) {
        window.time[b] = now.time[b] - MIN(now.time[b], systime_seen.time[b]);
        window.entries[b] = now.entries[b] - MIN(now.entries[b], systime_seen.entries[b]);
    }
    systime_seen = now;
    clock_timebase_info(&tb);
    at = switch_systime_put(size, 0,
                            snprintf(line, size,
                                     "SwitchSched system (%llu ms; user threads' kernel time by what entered the "
                                     "kernel, with entries): ",
                                     window_ns / NSEC_PER_MSEC));
    switch_systime_format(line + at, size - at, &window, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
