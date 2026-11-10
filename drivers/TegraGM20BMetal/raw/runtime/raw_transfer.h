// SPDX-License-Identifier: MIT
#pragma once
#include "raw_copy.h"
#include "raw_kernel.h"
#include <array>
#include <memory>
#include <mutex>

namespace raw {

enum class TextureTransferResult : uint8_t { Unsupported, Complete, Failed };

struct TextureTransfer {
    CopySurface texture;
    uint32_t width = 0, height = 0, planes = 0, bpp = 0;
    uint64_t layerBytes = 0;
};

class TextureTransfers {
public:
    bool copy(Kernel &kernel, std::mutex &allocationLock, const TextureTransfer &region,
              void *bytes, uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner);
    void close(Kernel &kernel, std::mutex &allocationLock);

private:
    struct Slot {
        Bo memory;
        uint32_t sequence = 0;
        bool pending = false;
        std::shared_ptr<void> owner;
    };

    bool retire(Kernel &kernel, Slot &slot);
    void reset(Kernel &kernel);
    std::mutex lock_;
    std::array<Slot, 4> slots_;
    uint32_t channel_ = 0, last_ = 0, next_ = 0;
    const volatile uint32_t *fence_ = nullptr;
};

} // namespace raw
