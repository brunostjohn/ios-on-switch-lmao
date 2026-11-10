// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

enum class ExecutionKind : uint8_t {
    native,
    compatibility,
    wait,
    signal
};

struct ExecutionRange {
    size_t from, to;
    ExecutionKind kind;
    bool gpuWait = false;
};

struct ExecutionAttribution {
    size_t native = 0, compatibility = 0;

    void add(bool raw, bool event)
    {
        if (!event)
            ++(raw ? native : compatibility);
    }

    bool mixed() const
    {
        return native && compatibility;
    }

    bool onlyCompatibility() const
    {
        return !native && compatibility;
    }
};

// A range never crosses an event. Adjacent passes of one backend can share a submission, but an event's exact position
// is retained even when no encoder exists on either side. The caller supplies complete encoder ranges, not draw fragments.
class ExecutionPlan {
    std::vector<ExecutionRange> ranges_;

public:
    void append(size_t from, size_t to, ExecutionKind kind)
    {
        if (from == to)
            return;
        const bool work = kind == ExecutionKind::native || kind == ExecutionKind::compatibility;
        if (work && !ranges_.empty() && ranges_.back().kind == kind && ranges_.back().to == from)
            ranges_.back().to = to;
        else {
            ranges_.push_back({from, to, kind});
            if (kind == ExecutionKind::native)
                for (size_t at = ranges_.size() - 1; at && ranges_[at - 1].kind == ExecutionKind::wait; --at)
                    ranges_[at - 1].gpuWait = true;
        }
    }

    const std::vector<ExecutionRange> &ranges() const
    {
        return ranges_;
    }
};

}  // namespace raw
