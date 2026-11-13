// SPDX-License-Identifier: BSD-2-Clause-Patent
// A display route that a failure turns off comes back after a clean run of the events it hangs off (frame ends, flips,
// sets), never on a timer. A failure before the route has stayed up for `forgive` clean events since it came back
// doubles the run, up to `cap`. Plain state: its owner keeps it under its own gate or lock.
#pragma once
#include <stdint.h>

struct SwitchRouteBackoff {
    uint32_t first, cap, forgive;
    uint32_t run = 0;   // the clean run the last failure asked for; 0 once the route is forgiven
    uint32_t left = 0;  // clean events still to go before the route is tried again; 0 while it is on
    uint32_t up = 0;    // clean events since it came back
    uint32_t failures = 0, retries = 0;

    constexpr SwitchRouteBackoff(uint32_t firstRun, uint32_t capRun, uint32_t forgiveRun)
        : first(firstRun), cap(capRun), forgive(forgiveRun)
    {}

    bool on() const
    {
        return left == 0;
    }

    void fail()
    {
        ++failures;
        if (!run)
            run = first;
        else
            run = run > cap / 2 ? cap : run * 2;
        left = run;
        up = 0;
    }

    // True when this clean event ends the run: the route is tried again from here.
    bool clean()
    {
        if (left) {
            if (--left)
                return false;
            ++retries;
            return true;
        }
        if (run && ++up >= forgive) {
            run = 0;
            up = 0;
        }
        return false;
    }

    // An event that was not clean while the route is off: its run starts over.
    void dirty()
    {
        if (left)
            left = run;
    }
};
