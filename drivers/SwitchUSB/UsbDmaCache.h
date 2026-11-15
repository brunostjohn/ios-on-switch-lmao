// SPDX-License-Identifier: BSD-2-Clause-Patent
// How the host mapper (UsbDmaMapper.cpp) gives a buffer to the XUSB host, which does not snoop the A57's caches. A
// mapper is called only at IODMACommand::prepare (iovmMapMemory) and complete (iovmUnmapMemory); synchronize never
// reaches it, and IODMACommand's own maintenance runs only for commands their creator made kNonCoherent
// (IODMACommand.cpp prepare/complete), which Apple's USB stack, written for IO-coherent SoCs, has no reason to do.
//
// A buffer the controller may write (any descriptor not prepared read-only: IOGeneralMemoryDescriptor::wireVirtual sets
// kIOMemoryPreparedReadOnly for an Out preparation) goes to the controller where it is only if every cache line it
// touches is wholly its own: cleaned and invalidated at prepare, so no line of it is cached while the controller writes
// and a read even before complete misses (unless a speculative fill brought the line back, which no mapper hook can
// undo), and again at complete for such fills. A line it shares with other data would be refilled and dirtied by the
// CPU's use of that data and written back over what the controller wrote (Linux bounces such kmalloc buffers on
// non-coherent arm64 for the same reason), so such a buffer is bounced through the non-cacheable pool instead and copied
// back at complete. A buffer the controller only reads is cleaned at prepare, at any alignment. Memory that is not
// cacheable needs no maintenance and never shares a cached line. No IOKit: test/usb_dma_cache_test.cpp drives the
// same rules against a model of a cache line with a neighbour in it.
//
// All of that holds only while the buffer's command is completed after each transfer. Apple keeps the command of a
// buffer it allocated prepared for the buffer's life (AppleUSBXHCIRequest remembers it): one map, transfers that never
// reach the mapper, an unmap only when the buffer goes. Such buffers are allocated write-combined (SwitchXHCI's client
// buffer options), so what the mapper still sees cacheable is caller-supplied memory, completed per transfer.
// UsbDmaKept counts the mappings that need that and are still mapped, so a held one says so instead of going stale.
#pragma once
#include <stdint.h>

enum class UsbDmaCacheOp {
    None,
    Clean,               // write dirty lines back to memory, keep them
    CleanAndInvalidate,  // write dirty lines back, then drop every line
};

struct UsbDmaPlan {
    bool bounce;
    UsbDmaCacheOp atMap, atUnmap;
};

// IOMemoryDescriptor's kIOMemoryBufferCacheMask and kIOMemoryBufferCacheShift, and IOMapTypes.h's cache modes (the
// same in both XNUs; UsbDmaMapper.cpp checks them against the SDK's).
constexpr uint32_t kUsbDmaBufferCacheMask = 0x70000000;
constexpr uint32_t kUsbDmaBufferCacheShift = 28;
constexpr uint32_t kUsbDmaDefaultCache = 0;
constexpr uint32_t kUsbDmaWriteThruCache = 2;
constexpr uint32_t kUsbDmaCopybackCache = 3;
constexpr uint32_t kUsbDmaCopybackInnerCache = 5;

// Whether a descriptor's memory is reached through the CPU's cache, from its flags: the cache mode an
// IOBufferMemoryDescriptor was allocated with, which every other descriptor leaves at default. Only those bits: the
// option-style cache bits (0x0f00) AppleUSBXHCIRequest puts on its IOSubMemoryDescriptor over a client's buffer change
// nothing about the client's pages, which stay cacheable and need their maintenance.
constexpr bool usbDmaCacheable(uint32_t descriptorFlags)
{
    const uint32_t mode = (descriptorFlags & kUsbDmaBufferCacheMask) >> kUsbDmaBufferCacheShift;
    return mode == kUsbDmaDefaultCache || mode == kUsbDmaWriteThruCache || mode == kUsbDmaCopybackCache ||
           mode == kUsbDmaCopybackInnerCache;
}

// [pa, pa + length) begins and ends on a cache line boundary (the line size from CTR_EL0.DminLine).
constexpr bool usbDmaWholeLines(uint64_t pa, uint64_t length, uint64_t line)
{
    return ((pa | (pa + length)) & (line - 1)) == 0;
}

constexpr UsbDmaPlan usbDmaPlan(bool cacheable, bool deviceMayWrite, bool wholeLines)
{
    if (!cacheable)
        return {false, UsbDmaCacheOp::None, UsbDmaCacheOp::None};
    if (!deviceMayWrite)
        return {false, UsbDmaCacheOp::Clean, UsbDmaCacheOp::None};
    // Even in a page-backed buffer: its owner may use the rest of the line meanwhile (a mass-storage driver keeping
    // its command and status blocks in one buffer), and copying less than a line costs next to nothing.
    if (!wholeLines)
        return {true, UsbDmaCacheOp::None, UsbDmaCacheOp::None};
    return {false, UsbDmaCacheOp::CleanAndInvalidate, UsbDmaCacheOp::CleanAndInvalidate};
}

// The mapping is right only if its command is completed after each transfer: a bounce is copied in at map and out at
// unmap, and a cacheable buffer the controller writes is maintained at map and unmap only. (A cacheable buffer the
// controller only reads is too, but a CPU write through IOMemoryDescriptor::writeBytes flushes its own lines.)
constexpr bool usbDmaNeedsCompletion(const UsbDmaPlan &plan, bool bounced)
{
    return bounced || plan.atUnmap != UsbDmaCacheOp::None;
}

// The mappings usbDmaNeedsCompletion says must be completed per transfer and are still mapped. Any thread.
class UsbDmaKept {
public:
    void mapped(bool needsCompletion)
    {
        if (needsCompletion)
            __atomic_add_fetch(&count_, 1, __ATOMIC_RELAXED);
    }

    void unmapped(bool needsCompletion)
    {
        if (needsCompletion)
            __atomic_sub_fetch(&count_, 1, __ATOMIC_RELAXED);
    }

    uint32_t kept() const
    {
        return __atomic_load_n(&count_, __ATOMIC_RELAXED);
    }

private:
    uint32_t count_ = 0;
};
