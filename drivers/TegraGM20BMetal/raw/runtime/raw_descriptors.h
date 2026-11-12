// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>

namespace raw {

using DescriptorWords = std::array<uint32_t, 64>;

// The upload allocations must remain immutable and live until every draw using this encoder has retired.
class DescriptorSnapshot {
public:
    template <typename Upload> uint64_t resolve(const DescriptorWords &words, Upload upload)
    {
        if (!address_ || words_ != words) {
            const uint64_t address = upload(words);
            if (!address)
                return 0;
            words_ = words;
            address_ = address;
        }
        return address_;
    }

private:
    DescriptorWords words_ = {};
    uint64_t address_ = 0;
};

struct PreparedDescriptorWords {
    DescriptorWords words{};
    std::array<uint32_t, 32> residuals{};
    uint32_t residualMask = 0;
    uint64_t address = 0;
};

class PreparedDescriptors {
public:
    template <typename Build, typename Upload>
    const PreparedDescriptorWords *resolve(const void *pipeline, uint64_t revision, Build build, Upload upload)
    {
        if (value_.address && pipeline_ == pipeline && revision_ == revision)
            return &value_;
        PreparedDescriptorWords value;
        if (!build(value))
            return nullptr;
        value.address = snapshot_.resolve(value.words, upload);
        if (!value.address)
            return nullptr;
        value_ = value;
        pipeline_ = pipeline;
        revision_ = revision;
        return &value_;
    }

private:
    DescriptorSnapshot snapshot_;
    PreparedDescriptorWords value_;
    const void *pipeline_ = nullptr;
    uint64_t revision_ = 0;
};

// What the last draw bound of set 0: its address, and each uniform buffer's constant buffer (the descriptor's base and
// range). A draw of another pipeline binds all of them again; one of the same pipeline only those that moved.
class DescriptorBinding {
public:
    static constexpr uint32_t kUbos = 32;

    bool changed(const void *pipeline, uint64_t address)
    {
        if (pipeline_ != pipeline)
            ubosBound_ = 0;
        if (pipeline_ == pipeline && address_ == address)
            return false;
        pipeline_ = pipeline;
        address_ = address;
        return true;
    }

    bool uboMoved(uint32_t ubo, const uint32_t *descriptor)
    {
        const std::array<uint32_t, 3> bound = {descriptor[0], descriptor[1], descriptor[2]};
        if (ubo >= kUbos)
            return true;
        if ((ubosBound_ >> ubo & 1) && ubos_[ubo] == bound)
            return false;
        ubos_[ubo] = bound;
        ubosBound_ |= 1u << ubo;
        return true;
    }

private:
    const void *pipeline_ = nullptr;
    uint64_t address_ = 0;
    uint32_t ubosBound_ = 0;
    std::array<std::array<uint32_t, 3>, kUbos> ubos_{};
};

}  // namespace raw
