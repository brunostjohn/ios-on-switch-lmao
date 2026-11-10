// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>
#include "raw_compute_key.h"

namespace raw {

inline bool operator<(const BoundTexture &a, const BoundTexture &b)
{
    return std::tie(a.slot, a.pixelFormat, a.textureType, a.levels, a.arrayLength, a.swizzle[0], a.swizzle[1],
                    a.swizzle[2], a.swizzle[3]) < std::tie(b.slot, b.pixelFormat, b.textureType, b.levels,
                                                           b.arrayLength, b.swizzle[0], b.swizzle[1], b.swizzle[2],
                                                           b.swizzle[3]);
}

inline uint32_t computeLodBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline bool operator<(const BoundSampler &a, const BoundSampler &b)
{
    return std::make_tuple(a.slot, a.minFilter, a.magFilter, a.mipFilter, a.sAddress, a.tAddress, a.rAddress,
                           a.maxAnisotropy, a.compare, a.border, computeLodBits(a.lodMin), computeLodBits(a.lodMax),
                           a.normalized, a.lodAverage) <
           std::make_tuple(b.slot, b.minFilter, b.magFilter, b.mipFilter, b.sAddress, b.tAddress, b.rAddress,
                           b.maxAnisotropy, b.compare, b.border, computeLodBits(b.lodMin), computeLodBits(b.lodMax),
                           b.normalized, b.lodAverage);
}

struct ComputeBindingText {
    std::string memory, textures, samplers;
};

struct ComputeSpecialization {
    std::array<uint32_t, 3> threads = {};
    std::vector<std::pair<uint32_t, uint32_t>> memory;
    std::vector<BoundTexture> textures;
    std::vector<BoundSampler> samplers;

    bool operator<(const ComputeSpecialization &b) const
    {
        return std::tie(threads, memory, textures, samplers) < std::tie(b.threads, b.memory, b.textures, b.samplers);
    }

    bool operator==(const ComputeSpecialization &b) const
    {
        return !(*this < b) && !(b < *this);
    }

    bool bindTexture(uint32_t slot, const BoundTexture *value)
    {
        return bind(textures, slot, value);
    }

    bool bindSampler(uint32_t slot, const BoundSampler *value)
    {
        return bind(samplers, slot, value);
    }

    bool bindMemory(uint32_t slot, uint32_t bytes)
    {
        const auto at = std::lower_bound(memory.begin(), memory.end(), slot, [](const auto &v, uint32_t i) {
            return v.first < i;
        });
        if (at != memory.end() && at->first == slot) {
            if (at->second == bytes)
                return false;
            if (bytes)
                at->second = bytes;
            else
                memory.erase(at);
        } else {
            if (!bytes)
                return false;
            memory.insert(at, {slot, bytes});
        }
        return true;
    }

    ComputeBindingText text() const
    {
        ComputeBindingText result;
        for (const auto &[slot, bytes] : memory)
            result.memory += (result.memory.empty() ? "" : ",") + std::to_string(slot) + ":" + std::to_string(bytes);
        result.textures = computeTextures(textures.data(), textures.size());
        result.samplers = computeSamplers(samplers.data(), samplers.size());
        return result;
    }

private:
    template <typename T> static bool bind(std::vector<T> &values, uint32_t slot, const T *value)
    {
        const auto at = std::lower_bound(values.begin(), values.end(), slot, [](const T &v, uint32_t i) {
            return v.slot < i;
        });
        if (at != values.end() && at->slot == slot) {
            if (!value)
                values.erase(at);
            else if (*at < *value || *value < *at)
                *at = *value;
            else
                return false;
        } else {
            if (!value)
                return false;
            values.insert(at, *value);
        }
        return true;
    }
};

template <typename T> struct ComputeKernelResult {
    const T *state = nullptr;
    bool retry = false;
};

template <typename T> class ComputeKernelCache {
public:
    template <typename Make> ComputeKernelResult<T> get(const ComputeSpecialization &key, Make &&make)
    {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock<std::mutex> hold(lock_);
            const auto at = entries_.find(key);
            if (at != entries_.end()) {
                entry = at->second;
                entry->changed.wait(hold, [&] {
                    return !entry->pending;
                });
                return entry->result;
            }
            entry = std::make_shared<Entry>();
            entries_.emplace(key, entry);
        }
        Pending pending{*this, key, entry};
        pending.result = make();
        return pending.result;
    }

private:
    struct Entry {
        std::condition_variable changed;
        ComputeKernelResult<T> result;
        bool pending = true;
    };

    struct Pending {
        ComputeKernelCache &cache;
        const ComputeSpecialization &key;
        const std::shared_ptr<Entry> &entry;
        ComputeKernelResult<T> result{nullptr, true};

        ~Pending()
        {
            {
                std::lock_guard<std::mutex> hold(cache.lock_);
                entry->result = result;
                entry->pending = false;
                if (result.retry)
                    cache.entries_.erase(key);
            }
            entry->changed.notify_all();
        }
    };

    std::mutex lock_;
    std::map<ComputeSpecialization, std::shared_ptr<Entry>> entries_;
};

struct ComputeDispatch {
    enum Reason {
        Ready,
        Edge,
        Range
    } reason = Ready;

    std::array<uint64_t, 3> groups;
    uint8_t axis = 0;
};

inline ComputeDispatch computeDispatch(std::array<uint64_t, 3> grid, const std::array<uint32_t, 3> &threads,
                                       bool perThread)
{
    ComputeDispatch result{ComputeDispatch::Ready, grid, 0};
    for (uint8_t i = 0; perThread && i < 3; ++i) {
        if (!threads[i] || grid[i] % threads[i])
            return {ComputeDispatch::Edge, result.groups, i};
        result.groups[i] /= threads[i];
    }
    if (result.groups[0] > 0x7fffffff || result.groups[1] > 0xffff || result.groups[2] > 0xffff)
        result.reason = ComputeDispatch::Range;
    return result;
}

}  // namespace raw
