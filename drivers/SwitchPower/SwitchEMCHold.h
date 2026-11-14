// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * The EMC's hold (SwitchEMC.cpp, test/switch_emc_hold_test.cpp): while frames are being made (a frame line in the last
 * frame_ticks) or a hint's deadline has not passed, the EMC stays at its top rate with ACTMON's down watermark off. The
 * EMC's work loop decides at each program() whether the hold is on and when it must look again; the CPU governor's tick
 * may tell it the hold ended. Plain C.
 */
#ifndef SWITCH_EMC_HOLD_H
#define SWITCH_EMC_HOLD_H

#include <stdbool.h>
#include <stdint.h>

struct switch_emc_hold {
    uint64_t frame_ticks; /* a frame line holds the top rate this long */
};

/* What program() does: the down watermark off (keep), and the hold timer armed for arm_at (0: not armed). */
struct switch_emc_hold_step {
    bool keep;
    uint64_t arm_at;
};

/* When the hold ends: the later of the hint deadline and the last frame line's hold. */
static inline uint64_t switch_emc_hold_end(const struct switch_emc_hold *hold, uint64_t hint_until, uint64_t last_frame)
{
    const uint64_t frames = last_frame ? last_frame + hold->frame_ticks : 0;
    return frames > hint_until ? frames : hint_until;
}

/* The EMC's work loop, programming ACTMON at `now`; `ticked`: the CPU governor's tick runs. */
static inline struct switch_emc_hold_step switch_emc_hold_program(struct switch_emc_hold *hold, uint64_t now,
                                                                  uint64_t hint_until, uint64_t last_frame, bool ticked)
{
    struct switch_emc_hold_step step;
    const uint64_t end = switch_emc_hold_end(hold, hint_until, last_frame);
    (void)ticked;
    step.keep = now < end;
    step.arm_at = step.keep ? end : 0;
    return step;
}

/* The CPU governor's tick at `now`: whether it wakes the EMC's work loop because the hold ended. */
static inline bool switch_emc_hold_tick(struct switch_emc_hold *hold, uint64_t now, uint64_t hint_until,
                                        uint64_t last_frame)
{
    (void)hold;
    (void)now;
    (void)hint_until;
    (void)last_frame;
    return false;
}

#endif
