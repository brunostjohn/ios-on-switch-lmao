// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple's blocking CPU limits, replayed beside the ones this port lifts (limit_shadow.h), debug kernels: one line with
 * each 10 s scheduling report naming how often each would have blocked its thread, and for how long.
 *
 * A thread whose limit was lifted gets a slot, claimed on the thread itself in thread_set_cpulimit with preemption off;
 * its fields are written before its id is published (release). After that only the thread's own dispatch hooks
 * (thread_invoke and thread_dispatch, under the thread's lock, on the one CPU it is running on or leaving) write the
 * slot, and a thread is dispatched only after its departure, so the slot has one writer at a time. A thread that asks
 * again updates its limit the same way, on itself. The report reads the counts. Slots are never freed: thread ids are
 * not reused, and a boot asks for a handful.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include "limit_shadow.h"

#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <os/log.h>

#if DEVELOPMENT || DEBUG

#define SHADOWS 16u

struct shadow_slot {
    uint64_t tid; /* 0: free */
    int32_t pid;
    char name[17];
    uint8_t percentage;
    uint32_t interval_ms;
    uint64_t on_since; /* when the thread went on a core, 0 while off */
    struct switch_limit_shadow s;
    uint64_t blocks_seen, over_seen; /* the report's */
};

uint32_t switch_sched_limit_watching;
static struct shadow_slot shadows[SHADOWS];
static uint32_t shadows_dropped;

static struct shadow_slot *shadow_of(uint64_t tid)
{
    for (unsigned int i = 0; i < SHADOWS; i++) {
        if (os_atomic_load(&shadows[i].tid, acquire) == tid) {
            return &shadows[i];
        }
    }
    return NULL;
}

/* thread_set_cpulimit, on the thread itself, for a limit switch_cpulimit_lift lifted. */
void switch_sched_limit_shadow(uint8_t percentage, uint64_t interval_ns)
{
    thread_t thread = current_thread();
    const uint64_t tid = thread->thread_id;
    uint64_t interval = 0;

    nanoseconds_to_absolutetime(interval_ns, &interval);
    disable_preemption();
    struct shadow_slot *slot = shadow_of(tid);
    if (slot == NULL) {
        for (unsigned int i = 0; i < SHADOWS && slot == NULL; i++) {
            if (os_atomic_load(&shadows[i].tid, relaxed) == 0 &&
                os_atomic_cmpxchg(&shadows[i].tid, 0, UINT64_MAX, relaxed)) {
                slot = &shadows[i];
            }
        }
        if (slot == NULL) {
            os_atomic_inc(&shadows_dropped, relaxed);
            enable_preemption();
            return;
        }
        slot->pid = task_pid(get_threadtask(thread));
        switch_task_name(get_threadtask(thread), slot->name, sizeof(slot->name));
    }
    if (slot->percentage != percentage || slot->s.interval != interval) {
        slot->percentage = percentage;
        slot->interval_ms = (uint32_t)(interval_ns / NSEC_PER_MSEC);
        switch_limit_shadow_init(&slot->s, interval, percentage);
        slot->blocks_seen = slot->over_seen = 0;
    }
    slot->on_since = mach_absolute_time();
    os_atomic_store(&slot->tid, tid, release);
    os_atomic_store(&switch_sched_limit_watching, 1, relaxed);
    enable_preemption();
}

void switch_sched_limit_dispatched(thread_t thread, uint64_t now)
{
    struct shadow_slot *slot = shadow_of(thread->thread_id);

    if (slot != NULL) {
        slot->on_since = now;
    }
}

void switch_sched_limit_departed(thread_t thread, uint64_t now)
{
    struct shadow_slot *slot = shadow_of(thread->thread_id);

    if (slot != NULL && slot->on_since != 0 && now > slot->on_since) {
        switch_limit_shadow_ran(&slot->s, slot->on_since, now);
        slot->on_since = 0;
    }
}

void switch_sched_limit_report(char *line, size_t size, uint64_t window_ns)
{
    size_t at = 0;
    unsigned int watched = 0;

    if (os_atomic_load(&switch_sched_limit_watching, relaxed) == 0) {
        return;
    }
    at = (size_t)snprintf(line, size,
                          "SwitchSched CPU limits lifted (%llu ms; what Apple's limit would have done: blocks, ms "
                          "run past the budget):",
                          window_ns / NSEC_PER_MSEC);
    for (unsigned int i = 0; i < SHADOWS && at < size - 1; i++) {
        struct shadow_slot *slot = &shadows[i];
        const uint64_t tid = os_atomic_load(&slot->tid, acquire);

        if (tid == 0 || tid == UINT64_MAX) {
            continue;
        }
        watched++;
        const uint64_t blocks = os_atomic_load(&slot->s.blocks, relaxed);
        const uint64_t over = os_atomic_load(&slot->s.over, relaxed);
        const uint64_t window_blocks = blocks - MIN(blocks, slot->blocks_seen);
        uint64_t over_ns = 0;

        absolutetime_to_nanoseconds(over - MIN(over, slot->over_seen), &over_ns);
        slot->blocks_seen = blocks;
        slot->over_seen = over;
        if (window_blocks == 0) {
            continue;
        }
        const int n = snprintf(line + at, size - at, " %s[%d] 0x%llx %u%%/%u: %llu, %llu;", slot->name, slot->pid, tid,
                               slot->percentage, slot->interval_ms, window_blocks, over_ns / NSEC_PER_MSEC);
        at = n < 0 ? at : MIN(at + (size_t)n, size - 1);
    }
    (void)snprintf(line + at, size - at, " %u threads watched, %u not", watched,
                   os_atomic_load(&shadows_dropped, relaxed));
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
