// SPDX-License-Identifier: MIT
// Exact backing uses for one job. An older ABI refuses an oversized list instead of losing dependencies via All.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "TegraGM20BABI.h"

namespace raw {
class SurfaceList {
public:
    void clear()
    {
        uses_.clear();
        ranges_.clear();
        overflow_ = false;
        last_ = 0;
    }

    void add(uint64_t handle, bool write)
    {
        addAccess(handle, write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    }

    void addAddress(uint64_t va, uint64_t bytes, bool write)
    {
        if (!bytes || bytes > UINT64_MAX - va) {
            overflow_ = true;
            return;
        }
        const uint32_t access = TegraGM20BSurfaceGpuAddress | (write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
        for (const auto &range : ranges_)
            if (va >= range.begin && va + bytes <= range.end) {
                uses_[range.index].access |= access;
                return;
            }
        addAccess(va, access);
        if (valid() && bytes > 1)
            ranges_.push_back({va, va + bytes, last_});
    }

    void addAccess(uint64_t handle, uint32_t access)
    {
        if (!handle) {
            overflow_ = true;
            return;
        }
        const uint32_t kind = access & ~(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite);
        auto same = [&](const TegraGM20BAbiSurfaceUse &use) {
            return use.handle == handle && (use.access & ~(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite)) == kind;
        };
        if (last_ < uses_.size() && same(uses_[last_])) {
            uses_[last_].access |= access;
            return;
        }
        for (size_t i = 0; i < uses_.size(); ++i)
            if (same(uses_[i])) {
                uses_[i].access |= access;
                last_ = i;
                return;
            }
        if (uses_.size() == TegraGM20BMaxJobUses) {
            overflow_ = true;
            return;
        }
        last_ = uses_.size();
        uses_.push_back({handle, access, 0});
    }

    bool valid() const
    {
        return !overflow_;
    }

    const std::vector<TegraGM20BAbiSurfaceUse> &uses() const
    {
        return uses_;
    }

    const TegraGM20BAbiExecSurfaces *legacy(uint32_t channel, size_t &bytes)
    {
        bytes = 0;
        if (!valid() || uses_.size() > TegraGM20BMaxExecSurfaces)
            return nullptr;
        for (const auto &use : uses_)
            if (use.access & ~(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite))
                return nullptr;
        legacy_.channel = channel;
        legacy_.flags = 0;
        legacy_.count = uint32_t(uses_.size());
        if (!uses_.empty())
            memcpy(legacy_.uses, uses_.data(), uses_.size() * sizeof(uses_[0]));
        bytes = offsetof(TegraGM20BAbiExecSurfaces, uses) + uses_.size() * sizeof(uses_[0]);
        return &legacy_;
    }

private:
    struct Range {
        uint64_t begin, end;
        size_t index;
    };

    std::vector<Range> ranges_;
    std::vector<TegraGM20BAbiSurfaceUse> uses_;
    TegraGM20BAbiExecSurfaces legacy_ = {};
    size_t last_ = 0;
    bool overflow_ = false;
};
}  // namespace raw
