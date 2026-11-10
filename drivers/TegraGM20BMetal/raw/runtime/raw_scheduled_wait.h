// SPDX-License-Identifier: MIT
// When waitUntilScheduled returns (RawMetalCommandBuffer.mm), and which waits of a queue's head let the command buffers
// committed behind it count as scheduled before their work is submitted (RawMetalSchedule.mm).
#pragma once

namespace raw {

enum class HeadWait {
    event,    // a shared event value nothing reached yet
    compile,  // a pipeline being made at the committers' priority, which waits for nothing its caller does
    signal,   // its own earlier work, before a signal after it is published
    order,    // the other driver's work, ordered on the CPU
};

// The kernel orders a present only after the writers it already has, so a command buffer counts as scheduled before its
// work is submitted only where waiting could deadlock: behind a shared event, which the waiting thread may be about to
// commit the signaller of.
constexpr bool holds(HeadWait wait)
{
    return wait == HeadWait::event;
}

// `scheduled`: its scheduled handlers ran; `held`: its queue's head waits on a shared event; `failed`: no more of it will
// be submitted
constexpr bool scheduledWaitReturns(bool scheduled, bool held, bool failed)
{
    return scheduled || held || failed;
}

}  // namespace raw
