// SPDX-License-Identifier: MIT
#include "raw_transfer.h"
#include "raw_fence.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace raw {

void TextureTransfers::reset(Kernel &kernel)
{
    if (channel_)
        kernel.closeChannel(channel_);
    channel_ = last_ = next_ = 0;
    fence_ = nullptr;
    for (auto &slot : slots_) {
        slot.pending = false;
        slot.owner.reset();
    }
}

bool TextureTransfers::retire(Kernel &kernel, Slot &slot)
{
    if (!slot.pending)
        return true;
    const auto *fence = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_);
    bool wait = int32_t(*fence_ - slot.sequence) < 0;
    if (!wait && (kernel.info().flags & TegraGM20BInfoSemanticFence)) {
        const auto *semantic = reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(fence + 1);
        const auto status = readSemanticFence(semantic, slot.sequence).status;
        if (status == SemanticFenceStatus::Failed)
            return false;
        wait = status == SemanticFenceStatus::Pending;
    }
    if (wait && !kernel.wait(channel_, slot.sequence))
        return false;
    if (kernel.killRecords()) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (fence->error) {
            std::atomic_thread_fence(std::memory_order_acquire);
            if (int32_t(slot.sequence - fence->killed_first) >= 0 && int32_t(fence->killed_last - slot.sequence) >= 0)
                return false;
        }
    } else if (kernel.lost(channel_)) {
        return false;
    }
    slot.pending = false;
    slot.owner.reset();
    return true;
}

bool TextureTransfers::copy(Kernel &kernel, std::mutex &allocationLock, const TextureTransfer &region,
                           void *bytes, uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner)
{
    if (!kernel.exactJobs() || !bytes || !region.width || !region.height || !region.planes ||
        !region.bpp || region.bpp > 16 || (region.bpp & (region.bpp - 1)))
        return false;
    const uint64_t line = uint64_t(region.width) * region.bpp;
    if (!row && region.height == 1)
        row = line;
    if (row < line || row > std::numeric_limits<size_t>::max() / region.height)
        return false;
    if (!image)
        image = row * region.height;
    if (image < row * region.height || image > std::numeric_limits<size_t>::max() / region.planes)
        return false;
    const uint64_t pitch = (line + 31) & ~uint64_t(31);
    if (pitch > UINT32_MAX || pitch > UINT64_MAX / region.height)
        return false;
    const uint64_t plane = pitch * region.height;
    if (plane > UINT64_MAX / region.planes)
        return false;
    const uint64_t dataBytes = plane * region.planes;
    const uint64_t commandOffset = (dataBytes + 255) & ~uint64_t(255);
    const uint64_t commandBytes = uint64_t(region.planes) * 32 * sizeof(uint32_t);
    if (commandOffset < dataBytes ||
        commandOffset > UINT64_MAX - commandBytes || commandBytes / 4 >= (1u << 21))
        return false;
    std::lock_guard<std::mutex> serial(lock_);
    const bool killed = channel_ && kernel.killRecords() &&
                        reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_)->error;
    if (channel_ && (killed || kernel.lost(channel_) || !retire(kernel, slots_[next_])))
        reset(kernel);
    if (!channel_) {
        std::lock_guard<std::mutex> allocate(allocationLock);
        if (!kernel.channel(TegraGM20BEngineCopy, channel_, fence_))
            return false;
    }
    Slot &slot = slots_[next_];
    const uint64_t need = commandOffset + commandBytes;
    if (slot.memory.bytes < need) {
        std::lock_guard<std::mutex> allocate(allocationLock);
        Bo replacement;
        if (!kernel.allocControl(std::max<uint64_t>(0x10000, need), replacement))
            return false;
        kernel.free(slot.memory);
        slot.memory = replacement;
    }
    if (write)
        for (uint32_t z = 0; z < region.planes; ++z)
            for (uint32_t y = 0; y < region.height; ++y)
                std::memcpy(slot.memory.cpu + z * plane + y * pitch,
                            static_cast<const uint8_t *>(bytes) + z * image + y * row, line);
    auto *start = reinterpret_cast<uint32_t *>(slot.memory.cpu + commandOffset);
    Push push{start, start + commandBytes / sizeof(uint32_t)};
    SurfaceList uses;
    uses.add(slot.memory.handle, !write);
    for (uint32_t z = 0; z < region.planes; ++z) {
        CopySurface texture = region.texture;
        if (region.layerBytes)
            texture.va += z * region.layerBytes;
        else
            texture.layer += z;
        const CopySurface staging{slot.memory.va + z * plane, uint32_t(pitch)};
        uses.addAddress(texture.va, 1, write);
        emitCopy(push, write ? staging : texture, write ? texture : staging,
                 region.width, region.height, copyTexelRemap(region.bpp), true);
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    const TegraGM20BAbiPush commands{slot.memory.va + commandOffset, uint32_t(push.at - start), 0};
    uint32_t sequence = 0;
    if (!kernel.exec(channel_, &commands, 1, last_, sequence, &uses, nullptr,
                     write ? JobVisibility::Internal : JobVisibility::External))
        return false;
    slot.sequence = sequence;
    slot.pending = true;
    slot.owner = std::move(owner);
    last_ = sequence;
    next_ = (next_ + 1) % slots_.size();
    if (write)
        return true;
    if (!retire(kernel, slot))
        return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    for (uint32_t z = 0; z < region.planes; ++z)
        for (uint32_t y = 0; y < region.height; ++y)
            std::memcpy(static_cast<uint8_t *>(bytes) + z * image + y * row,
                        slot.memory.cpu + z * plane + y * pitch, line);
    return true;
}

void TextureTransfers::close(Kernel &kernel, std::mutex &allocationLock)
{
    std::lock_guard<std::mutex> serial(lock_);
    reset(kernel);
    for (auto &slot : slots_) {
        std::lock_guard<std::mutex> allocate(allocationLock);
        kernel.free(slot.memory);
    }
}

} // namespace raw
