// SPDX-License-Identifier: MIT
// The signals of a command buffer that no raw range's job carries (a signal after an m2v range, a command buffer of signals
// alone) go to the GPU in jobs of no pushes on the raw channel (raw::Device::eventJob): released after everything the channel
// ran before them, and after the m2v work committed before them through the kernel's Order.
#pragma once
#include <IOKit/IOReturn.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

// A command buffer's submission range as an event job reads it (RMSegment and its operation)
struct EventRangeView {
    bool event = false;     // an event operation's range, else an encoder's
    bool signal = false;    // a signal, else a wait
    bool leasable = false;  // its event is a kernel event (SharedEventLease)
    bool carried = false;   // a job carries its signal already (RMSegment::gpuSignal)
};

// The ranges one job carries from `at`, a signal no job carries: it and the leasable signals right after it. Empty where the
// signal at `at` is not a kernel event's (its value is set on the CPU after the work before it).
inline std::vector<size_t> signalJobRanges(const std::vector<EventRangeView> &ranges, size_t at)
{
    std::vector<size_t> carried;
    for (size_t i = at; i < ranges.size(); i++) {
        const EventRangeView &r = ranges[i];
        if (!r.event || !r.signal || !r.leasable)
            break;
        if (!r.carried)
            carried.push_back(i);
    }
    return carried;
}

// What a refused event job asks of its command buffer: to fail, or (its channel made again after the caller ordered the job
// and checked its waits there) to order it again on the new channel and submit it then
enum class EventJobAnswer { failed, reordered };

constexpr EventJobAnswer eventJobAnswer(int result)
{
    return result == kIOReturnNotResponding ? EventJobAnswer::reordered : EventJobAnswer::failed;
}

}  // namespace raw
