// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace raw {

constexpr uint32_t kNoSnapshot = std::numeric_limits<uint32_t>::max();

template <typename T> class StateSnapshots {
    T current_{};
    uint64_t generation_ = 0;
    uint64_t captured_ = std::numeric_limits<uint64_t>::max();
    uint32_t index_ = kNoSnapshot;
    std::vector<T> values_;

public:
    const T &current() const
    {
        return current_;
    }

    const T &operator[](uint32_t index) const
    {
        return values_[index];
    }

    uint64_t generation() const
    {
        return generation_;
    }

    size_t size() const
    {
        return values_.size();
    }

    size_t storageBytes() const
    {
        return values_.capacity() * sizeof(T);
    }

    template <typename Edit> bool update(Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        return true;
    }

    uint32_t capture()
    {
        if (captured_ != generation_) {
            index_ = static_cast<uint32_t>(values_.size());
            values_.push_back(current_);
            captured_ = generation_;
        }
        return index_;
    }

    void resetCurrent()
    {
        current_ = {};
        ++generation_;
    }

    void clear()
    {
        values_.clear();
        current_ = {};
        generation_ = 0;
        captured_ = std::numeric_limits<uint64_t>::max();
        index_ = kNoSnapshot;
    }
};

template <typename T> bool assignState(T &to, const T &from)
{
    if (to == from)
        return false;
    to = from;
    return true;
}

struct BufferBinding {
    enum class Source : uint8_t {
        Unset,
        Buffer,
        Bytes,
        Offset,
        ClockAnchors  // offset: the kernel's clock anchor table's GPU address (OpKernelClockAnchors)
    };
    void *object = nullptr;
    uint64_t offset = 0;
    uint32_t chunk = 0;
    uint32_t length = 0;
    Source source = Source::Unset;

    bool operator==(const BufferBinding &b) const
    {
        return object == b.object && offset == b.offset && chunk == b.chunk && length == b.length && source == b.source;
    }

    bool operator!=(const BufferBinding &b) const
    {
        return !(*this == b);
    }
};

using BufferBindings = std::array<BufferBinding, 31>;

template <size_t N> struct ObjectBindings {
    std::array<void *, N> objects{};
    uint32_t set = 0;

    bool bind(uint32_t slot, void *object)
    {
        if (slot >= N)
            return false;
        const uint32_t bit = uint32_t(1) << slot;
        if ((set & bit) && objects[slot] == object)
            return false;
        objects[slot] = object;
        set |= bit;
        return true;
    }
};

using TextureBindings = ObjectBindings<31>;
using SamplerBindings = ObjectBindings<16>;

struct RasterState {
    enum Field : uint32_t {
        Pipeline = 1u << 0,
        DepthStencil = 1u << 1,
        Scissor = 1u << 2,
        Viewport = 1u << 3,
        Cull = 1u << 4,
        Winding = 1u << 5,
        Fill = 1u << 6,
        Blend = 1u << 7,
        Stencil = 1u << 8,
        DepthBias = 1u << 9,
        Visibility = 1u << 10,
    };

    uint32_t set = 0;
    void *pipeline = nullptr;
    void *nativePipeline = nullptr;
    void *nativeVariant = nullptr;
    void *depthStencil = nullptr;
    std::array<uint32_t, 4> scissor{};
    std::array<uint64_t, 6> viewport{};
    std::array<uint32_t, 4> blend{};
    std::array<uint32_t, 3> depthBias{};
    std::array<uint32_t, 2> stencil{};
    uint32_t cull = 0, winding = 0, fill = 0, visibility = 0;
    uint64_t visibilityOffset = 0;
};

struct KernelState {
    void *pipeline = nullptr;
    bool pipelineSet = false;
    std::array<uint32_t, 31> memory{};
    uint32_t memorySet = 0;
};

struct RenderStateRef {
    uint64_t epoch = 0;
    uint64_t resourceRevision = 0;
    uint32_t buffers[2]{};
    uint32_t textures[2]{};
    uint32_t samplers[2]{};
    uint32_t raster = 0;
    uint32_t uses = 0;
    bool usesFinal = false;
};

struct KernelStateRef {
    uint32_t buffers = 0, textures = 0, samplers = 0, state = 0, uses = 0;
    uint64_t epoch = 0;
    uint64_t resourceRevision = 0;
};

struct ResourceUse {
    enum : uint8_t {
        Read = 1,
        Write = 2,
        Sample = 4,
        Explicit = 8
    };

    void *object = nullptr;
    uint8_t access = 0;
};

struct StateCursor {
    uint32_t render = kNoSnapshot;
    uint32_t compute = kNoSnapshot;

    void reset()
    {
        render = compute = kNoSnapshot;
    }
};

}  // namespace raw
