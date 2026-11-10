// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

struct SwitchNVDECQueue {
    static constexpr uint32_t kSlots = 8;
    struct Job {
        uint64_t sequence = 0;
        uint64_t submittedUs = 0;
    };
    Job jobs[kSlots] = {};
    uint64_t submitted = 0, completed = 0;
    uint32_t head = 0, count = 0;

    uint32_t tail() const { return (head + count) % kSlots; }
    bool full() const { return count == kSlots; }
    uint64_t next() const { return submitted + 1; }

    static bool reached(uint32_t value, uint32_t threshold)
    {
        return static_cast<int32_t>(value - threshold) >= 0;
    }

    bool publish(uint64_t submittedUs)
    {
        if (full() || submitted == UINT64_MAX)
            return false;
        jobs[tail()] = {++submitted, submittedUs};
        ++count;
        return true;
    }

    bool retire(uint32_t hardware)
    {
        if (!count || !reached(hardware, static_cast<uint32_t>(jobs[head].sequence)))
            return false;
        completed = jobs[head].sequence;
        jobs[head] = {};
        head = (head + 1) % kSlots;
        --count;
        return true;
    }

    bool rebase(uint32_t hardware)
    {
        const uint32_t advance = hardware - static_cast<uint32_t>(submitted);
        if (count || advance > INT32_MAX || submitted > UINT64_MAX - advance)
            return false;
        submitted += advance;
        completed = submitted;
        return true;
    }

    bool legacySequence(uint32_t threshold, uint64_t *sequence) const
    {
        const uint32_t behind = static_cast<uint32_t>(submitted) - threshold;
        if (!sequence || behind > INT32_MAX || behind > submitted)
            return false;
        *sequence = submitted - behind;
        return true;
    }
};
