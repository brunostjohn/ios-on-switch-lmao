// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "SwitchVICMapping.h"

struct SwitchVICMapping {
    switch_gpu_backing *backing = nullptr;
    uint64_t allocation = 0, generation = 0, address = 0, used = 0;
    uint32_t pages = 0, users = 0;
    bool writable = false;
};

namespace {
constexpr uint32_t kMappings = 32;
constexpr uint64_t kPinnedBytes = 128ULL << 20;
SwitchVICMapping gMappings[kMappings];
IOLock *gLock;
uint64_t gClock, gBytes;

void evict(SwitchVICMapping &mapping)
{
    if (!mapping.backing)
        return;
    switch_vic_mapping_destroy(mapping.address, mapping.pages);
    switch_gpu_backing_release(mapping.backing);
    gBytes -= static_cast<uint64_t>(mapping.pages) << 12;
    mapping = {};
}

SwitchVICMapping *oldest(bool occupied = false)
{
    SwitchVICMapping *candidate = nullptr;
    for (auto &mapping : gMappings) {
        if (!mapping.users && (!occupied || mapping.backing) && (!candidate || mapping.used < candidate->used))
            candidate = &mapping;
    }
    return candidate;
}
}  // namespace

bool switch_vic_mapping_start()
{
    gLock = IOLockAlloc();
    return gLock != nullptr;
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable)
{
    if (!gLock || !backing)
        return nullptr;
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    const uint64_t bytes = static_cast<uint64_t>(info.page_count) << 12;
    if (!bytes || bytes > kPinnedBytes)
        return nullptr;
    IOLockLock(gLock);
    for (auto &mapping : gMappings) {
        if (mapping.backing && mapping.allocation == info.allocation_id &&
            mapping.generation == info.allocation_generation && mapping.writable == writable) {
            ++mapping.users;
            mapping.used = ++gClock;
            IOLockUnlock(gLock);
            return &mapping;
        }
    }
    while (gBytes + bytes > kPinnedBytes) {
        SwitchVICMapping *mapping = oldest(true);
        if (!mapping || !mapping->backing)
            break;
        evict(*mapping);
    }
    SwitchVICMapping *mapping = gBytes + bytes <= kPinnedBytes ? oldest() : nullptr;
    if (mapping) {
        evict(*mapping);
        const uint64_t address = switch_vic_mapping_create(info.pages, info.page_count, writable);
        if (address) {
            switch_gpu_backing_retain(backing);
            *mapping = {backing, info.allocation_id, info.allocation_generation, address, ++gClock,
                        info.page_count, 1, writable};
            gBytes += bytes;
        } else {
            mapping = nullptr;
        }
    }
    IOLockUnlock(gLock);
    return mapping;
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (!mapping)
        return;
    IOLockLock(gLock);
    if (!mapping->users)
        panic("SwitchVIC: mapping lease released twice");
    --mapping->users;
    mapping->used = ++gClock;
    IOLockUnlock(gLock);
}

uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping)
{
    return mapping ? mapping->address : 0;
}
