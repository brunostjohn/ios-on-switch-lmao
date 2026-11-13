// SPDX-License-Identifier: BSD-2-Clause-Patent
// A display route that a failure turns off comes back after a clean run of the events it hangs off (frame ends, flips,
// sets), never on a timer. The first failure asks for First clean events; a failure before the route has stayed up for
// Forgive clean events since it came back doubles the run, up to Cap. Plain state: its owner keeps it under its own gate
// or lock. Small, as some owners live in the display's instance (at the 16 KiB a kalloc_type class may have).
#pragma once
#include <stdint.h>

template <uint16_t First, uint16_t Cap, uint16_t Forgive> struct SwitchRouteBackoff {
    static_assert(First > 0 && First <= Cap && Cap <= 0x8000, "a run doubles within 16 bits");
    uint16_t run = 0;   // the clean run the last failure asked for; 0 once the route is forgiven
    uint16_t left = 0;  // clean events still to go before the route is tried again; 0 while it is on
    uint16_t up = 0;    // clean events since it came back
    uint32_t failures = 0, retries = 0;

    bool on() const
    {
        return left == 0;
    }

    void fail()
    {
        ++failures;
        if (!run)
            run = First;
        else
            run = run > Cap / 2 ? Cap : uint16_t(run * 2);
        left = run;
        up = 0;
    }

    // True when this clean event ends the run: the route is tried again from the next event.
    bool clean()
    {
        if (left) {
            if (--left)
                return false;
            ++retries;
            return true;
        }
        if (run && ++up >= Forgive) {
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
