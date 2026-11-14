// SPDX-License-Identifier: BSD-2-Clause-Patent
// The DFLL ceiling check (SwitchThermal.cpp) as a step machine on the thermal work loop's timer: the clock measured
// against the governor's ceiling; over it by half a DFLL step or more, the ceiling asked again from the row below; still
// over, that row held down and the next row down asked. Each settle (2 ms between the re-asks, the DFLL's closed-loop
// settle after a request, the settle before a move to the ceiling is measured) is a timer shot, so governor signals,
// ALERT and SOC_THERM events run between the steps instead of waiting behind an IOSleep under the loop's gate.
// test/switch_thermal_ceiling_test.cpp drives it against a model DFLL.
//
// `Dfll` provides: probeKhz() (the cluster's clock, measured), ceiling() and khzAt(index) (the governor's), appliedKhz()
// (what the governor last applied), landed(index), hold(index, &again), applied(index) (switchperf), setRate(khz)
// (TegraDFLL; 0 or an error), arm(ms) (the step timer), and the three log lines.
#ifndef SWITCH_THERMAL_CEILING_H
#define SWITCH_THERMAL_CEILING_H

#include <stdint.h>
#include "SwitchGovernor.h"

namespace switchceiling {

constexpr uint32_t kReaskBelowMS = 2;  // between the request for the row below and the ceiling's again

enum class Step {
    Idle,
    Settling,  // a governor move to the ceiling, measured once it has settled
    Below,     // the row below asked; the ceiling is asked again next
    Ceiling,   // the ceiling asked again; measured once it has settled
    Lowered,   // the ceiling held down, the next row asked; applied once it has settled
};

struct Report {
    uint32_t khz, ceiling, below, again, lower;
    int err, moved;
    unsigned wait;
    bool retried, restored;
};

template <class Dfll> class Check {
public:
    Check(uint32_t stepKhz, uint32_t settleMs) : stepKhz_(stepKhz), settleMs_(settleMs)
    {}

    bool busy() const
    {
        return step_ != Step::Idle;
    }

    Step step() const
    {
        return step_;
    }

    // A governor move just landed on the ceiling: measured after `settleMs`.
    void afterMove(Dfll &dfll, uint32_t settleMs)
    {
        step_ = Step::Settling;
        dfll.arm(settleMs);
    }

    // The check, from its start: the probe, and whether it moved the clock (the caller then leaves the DFLL alone until
    // busy() is false).
    uint32_t start(Dfll &dfll, bool *moved)
    {
        report_ = {};
        report_.khz = dfll.probeKhz();
        hi_ = dfll.ceiling();
        report_.ceiling = dfll.khzAt(hi_);
        *moved = false;
        if (!switch_gov_over_step(report_.khz, report_.ceiling, stepKhz_)) {
            if (dfll.appliedKhz() == report_.ceiling && dfll.landed(hi_))
                dfll.logRestored(report_);
            step_ = Step::Idle;
            return report_.khz;
        }
        *moved = true;
        report_.below = hi_ ? dfll.khzAt(hi_ - 1) : report_.ceiling;
        report_.err = dfll.setRate(report_.below);
        if (!report_.err) {
            step_ = Step::Below;
            dfll.arm(kReaskBelowMS);
        } else {
            decide(dfll);
        }
        return report_.khz;
    }

    // The step timer fired. True when the check has ended.
    bool fired(Dfll &dfll)
    {
        switch (step_) {
        case Step::Settling: {
            bool moved = false;
            (void)start(dfll, &moved);
            break;
        }
        case Step::Below:
            report_.err = dfll.setRate(report_.ceiling);
            if (!report_.err) {
                step_ = Step::Ceiling;
                dfll.arm(settleMs_);
            } else {
                decide(dfll);
            }
            break;
        case Step::Ceiling:
            decide(dfll);
            break;
        case Step::Lowered:
            dfll.applied(lower_);
            dfll.logHeld(report_);
            step_ = Step::Idle;
            break;
        case Step::Idle:
            break;
        }
        return !busy();
    }

    // The DFLL is going (the CPU back on PLLX, system sleep): no step runs after this.
    void cancel()
    {
        step_ = Step::Idle;
    }

private:
    // The ceiling asked again (or the re-ask refused): kept if it measures right, else held down.
    void decide(Dfll &dfll)
    {
        report_.again = dfll.probeKhz();
        if (!report_.err && !switch_gov_over_step(report_.again, report_.ceiling, stepKhz_)) {
            dfll.applied(hi_);
            report_.restored = dfll.landed(hi_);
            dfll.logReasked(report_);
            step_ = Step::Idle;
            return;
        }
        report_.wait = dfll.hold(hi_, &report_.retried);
        lower_ = dfll.ceiling();
        report_.lower = dfll.khzAt(lower_);
        report_.moved = dfll.setRate(report_.lower);
        if (!report_.moved) {
            step_ = Step::Lowered;
            dfll.arm(settleMs_);
            return;
        }
        dfll.logHeld(report_);
        step_ = Step::Idle;
    }

    uint32_t stepKhz_, settleMs_;
    Step step_ = Step::Idle;
    unsigned hi_ = 0, lower_ = 0;
    Report report_ = {};
};

}  // namespace switchceiling

#endif
