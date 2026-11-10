// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct SwitchVICQueue {
    static constexpr uint32_t kSlots = 16;
    using Completion = void (*)(void *, int);
    struct Job {
        uint32_t fence = 0, boundUs = 0;
        uint64_t submittedUs = 0;
        Completion completion = nullptr;
        void *context = nullptr;
    };
    Job jobs[kSlots] = {};
    uint32_t head = 0, count = 0, submitted = 0, completed = 0;

    static bool reached(uint32_t value, uint32_t threshold)
    {
        return static_cast<int32_t>(value - threshold) >= 0;
    }

    uint32_t tail() const { return (head + count) % kSlots; }
    bool full() const { return count == kSlots; }
    bool canPublish(uint32_t amount) const { return amount && amount <= kSlots - count; }
    uint32_t configurationSlot(uint32_t job) const { return (tail() + job) % kSlots; }

    bool publish(Job job)
    {
        if (full())
            return false;
        job.fence = ++submitted;
        jobs[tail()] = job;
        ++count;
        return true;
    }

    bool retire(uint32_t value)
    {
        if (!count || !reached(value, jobs[head].fence))
            return false;
        completed = jobs[head].fence;
        jobs[head] = {};
        head = (head + 1) % kSlots;
        --count;
        return true;
    }

    bool reset(uint32_t value)
    {
        if (count)
            return false;
        submitted = completed = value;
        return true;
    }
};
