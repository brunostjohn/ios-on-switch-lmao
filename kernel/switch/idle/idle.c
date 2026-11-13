// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU idle states (<switch/idle.h>, kits/SwitchIdle). Each idle period either waits in WFI or, once ArmCpu has started
 * the platform half, powers the core down (C7) when kits/SwitchIdle/switch_idle_governor.h says the period is long
 * enough. A powered-down core returns through XNU's own deep-idle path: the PSCI entry, start.s resume_idle_cpu
 * (cpu_machine_idle_init made it the core's reset handler) and arm_init_idle_cpu, which calls switch_idle_resumed and
 * then cpu_idle_exit(TRUE) and Idle_load_context, back into machine_idle's caller. Nothing on the idle stack survives
 * or needs to: Idle_context saved the idle thread's registers in its machine state.
 */

#include <switch/idle.h>

#include <arm/cpu_data_internal.h>
#include <arm/rtclock.h>
#include <arm64/switch_smp.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/atomic_private.h>
#include <os/log.h>

#include "../../../kits/SwitchIdle/switch_idle_governor.h"

struct idle_core {
    uint64_t predicted;
    uint64_t entered;
    uint64_t wake_at;
    uint64_t lead;
    uint32_t state;
    struct switch_idle_c7_retry retry;
    /* For idle_report, from the idle thread: a refusal count to report and its PSCI error, and the refusals in a row a
     * request the firmware took ended. 0: nothing to report. */
    uint32_t report_refusals;
    uint32_t report_taken;
    int64_t report_rc;
#if DEVELOPMENT || DEBUG
    struct switch_idle_counts counts;
    bool from_c7; /* back from C7 and no idle entry since: the next frame line paid the exit */
#endif
} __attribute__((aligned(64)));

static struct idle_core idle_cores[SWITCH_SMP_CORE_COUNT];
static const struct switch_idle_platform *idle_platform;
static bool idle_c7;
static uint64_t idle_frame_at;
static void (*idle_frames_started)(void);
static thread_call_t idle_report_call;

static void idle_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; cpu++) {
        struct idle_core *core = &idle_cores[cpu];
        const uint32_t refusals = os_atomic_xchg(&core->report_refusals, 0, acquire);
        if (refusals) {
            os_log_error(OS_LOG_DEFAULT,
                         "SwitchIdle: cpu%u: PSCI CPU_SUSPEND refused C7, %u in a row (last returned %lld); C7 is "
                         "requested again at later idles",
                         cpu, refusals, os_atomic_load(&core->report_rc, relaxed));
        }
        const uint32_t taken = os_atomic_xchg(&core->report_taken, 0, relaxed);
        if (taken) {
            os_log_error(OS_LOG_DEFAULT, "SwitchIdle: cpu%u: C7 taken again; refusals in a row before it: %u", cpu,
                         taken);
        }
    }
}

/* The idle thread, interrupts masked: the lines are logged from a thread call. */
static void idle_refused(struct idle_core *core, int64_t rc)
{
    if (switch_idle_c7_refused(&core->retry)) {
        os_atomic_store(&core->report_rc, rc, relaxed);
        os_atomic_store(&core->report_refusals, core->retry.refusals, release);
        thread_call_enter(idle_report_call);
    }
}

static void idle_taken(struct idle_core *core)
{
    const uint32_t ended = switch_idle_c7_taken(&core->retry);
    if (ended) {
        os_atomic_store(&core->report_taken, ended, relaxed);
        thread_call_enter(idle_report_call);
    }
}

static void idle_exit(struct idle_core *core, uint64_t now)
{
    const uint64_t idle = now - core->entered;
    core->predicted = switch_idle_learn(core->predicted, idle, switch_idle_timer_bound(now, core->wake_at));
#if DEVELOPMENT || DEBUG
    core->counts.residency[core->state] += idle;
    core->counts.entries[core->state]++;
#endif
}

void switch_idle_wfi(struct cpu_data *cdp)
{
    const uint32_t cpu = (uint32_t)cdp->cpu_number;
    if (__improbable(cpu >= SWITCH_SMP_CORE_COUNT)) {
        __builtin_arm_wfi();
        return;
    }
    struct idle_core *core = &idle_cores[cpu];
    const uint64_t now = mach_absolute_time();
    const uint64_t deadline = cdp->rtcPop;
#if DEVELOPMENT || DEBUG
    core->from_c7 = false;
#endif
    core->entered = now;
    core->state = os_atomic_load(&idle_c7, acquire)
                      ? switch_idle_choose(core->predicted, now, deadline, os_atomic_load(&idle_frame_at, relaxed))
                      : SWITCH_IDLE_WFI;
    if (core->state == SWITCH_IDLE_C7 && !switch_idle_c7_try(&core->retry)) {
        core->state = SWITCH_IDLE_WFI;
    }
    if (core->state == SWITCH_IDLE_C7) {
        core->wake_at = switch_idle_c7_wake_at(deadline, core->lead);
        const int64_t rc =
            idle_platform->power_down(cpu, switch_idle_timer_ticks(now, core->wake_at, idle_platform->wake_ticks_max));
        if (rc != 0) {
            idle_refused(core, rc);
        } else {
            idle_taken(core);
#if DEVELOPMENT || DEBUG
            core->counts.skipped++;
#endif
        }
    } else {
        core->wake_at = deadline;
        __builtin_arm_wfi();
    }
    idle_exit(core, mach_absolute_time());
}

void switch_idle_resumed(struct cpu_data *cdp)
{
    const uint32_t cpu = (uint32_t)cdp->cpu_number;
    switch_secondary_vectors_leave();
    idle_platform->resumed(cpu);
    ClearIdlePop(FALSE);
    struct idle_core *core = &idle_cores[cpu];
    const uint64_t now = mach_absolute_time();
    const uint64_t exit_latency = switch_idle_c7_exit(now, core->wake_at);
#if DEVELOPMENT || DEBUG
    if (exit_latency != 0) {
        core->counts.timer_exits++;
        core->counts.exit_ticks += exit_latency;
        if (exit_latency > core->counts.exit_max) {
            core->counts.exit_max = exit_latency;
        }
        if (exit_latency > core->lead) {
            core->counts.exits_late++;
        }
    }
    core->from_c7 = true;
#endif
    core->lead = switch_idle_c7_lead(core->lead, exit_latency);
    idle_taken(core);
    idle_exit(core, now);
}

void switch_idle_start(const struct switch_idle_platform *platform)
{
    idle_report_call =
        thread_call_allocate_with_options(idle_report, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    idle_platform = platform;
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; cpu++) {
        idle_cores[cpu].lead = switch_idle_c7_lead(0, 0);
    }
    os_atomic_store(&idle_c7, true, release);
}

void switch_idle_note_frame(void)
{
#if DEVELOPMENT || DEBUG
    const uint32_t cpu = (uint32_t)getCpuDatap()->cpu_number;
    if (cpu < SWITCH_SMP_CORE_COUNT && idle_cores[cpu].from_c7) {
        idle_cores[cpu].from_c7 = false;
        idle_cores[cpu].counts.frame_wakes++;
    }
#endif
    const uint64_t now = mach_absolute_time();
    const uint64_t before = os_atomic_xchg(&idle_frame_at, now, relaxed);
    if (now - before >= SWITCH_IDLE_FRAME_WINDOW) {
        void (*started)(void) = os_atomic_load(&idle_frames_started, acquire);
        if (started) {
            started();
        }
    }
}

uint64_t switch_idle_last_frame(void)
{
    return os_atomic_load(&idle_frame_at, relaxed);
}

void switch_idle_set_frames_started(void (*started)(void))
{
    os_atomic_store(&idle_frames_started, started, release);
}

int switch_idle_read_counts(uint32_t cpu, struct switch_idle_counts *out)
{
#if DEVELOPMENT || DEBUG
    if (cpu >= SWITCH_SMP_CORE_COUNT) {
        return -1;
    }
    *out = idle_cores[cpu].counts;
    return 0;
#else
    (void)cpu;
    (void)out;
    return -1;
#endif
}
