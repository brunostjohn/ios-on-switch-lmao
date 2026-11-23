// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchExternalScreen.h"
#include <mach/mach_time.h>
#include "TegraDisplayB.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"
#include "../NativeMobileDisplay/SwitchSurfaceUse.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUPublish.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" int tegra_gm20b_surface_layout(uint64_t, struct tgm_layout *);

bool SwitchExternalScreen::mapDomain()
{
    if (mapped)
        return true;
    if (!tables.memory && !switch_dma_alloc(&tables, kTablePages * 4096u))
        return false;
    if (!domain.va && (!domain.init(tables.va, tables.pa, kTablePages) ||
                      !domain.reserve(kSlotBase, kSlots * kSlotSpan)))
        return false;
    cleanDomain();
    char why[128];
    mapped = tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY_B, domain.ptb(), TEGRA_SMMU_DCB_ASID,
                              TEGRA_SMMU_TRANSLATION_ENABLE_0, (1u << 2) | (1u << 4) | (1u << 6) | (1u << 17),
                              why, sizeof(why)) == 0;
    if (!mapped)
        IOLog("SwitchDisplayPort: external scanout: %s\n", why);
    return mapped;
}

void SwitchExternalScreen::cleanDomain()
{
    switch_smmu_publish(domain, TEGRA_SMMU_ASID_DISPLAY_B, SWITCH_DCACHE_CLEAN);
}

void SwitchExternalScreen::releaseRead(unsigned index)
{
    auto &slot = slots[index];
    if (slot.counted)
        switch_surface_use_decrement(slot.surface, 0);
    slot.counted = false;
    switch_native_scanout_deliver(&slot.read);
    switch_native_scanout_release(&slot.read);
}

void SwitchExternalScreen::releaseSlot(unsigned index)
{
    auto &slot = slots[index];
    releaseRead(index);
    if (slot.backing) {
        switch_gpu_backing_scanout(slot.backing, 0);
        switch_gpu_backing_release(slot.backing);
    }
    if (slot.layout)
        tegra_gm20b_surface_unhold(slot.layout);
    if (slot.pages) {
        domain.unmap(kSlotBase + index * kSlotSpan, slot.pages);
        cleanDomain();
    }
    if (slot.memory) {
        slot.memory->complete(kIODirectionOut);
        slot.memory->release();
    }
    OSSafeReleaseNULL(slot.surface);
    slot = {};
}

bool SwitchExternalScreen::applyTiledLayout(Slot &slot, uint64_t firstPage, uint64_t bytes)
{
    int tiled = 0;
    void *held = tegra_gm20b_surface_hold(firstPage, &tiled);
    if (!tiled) {
        slot.layout = held;
        return true;
    }
    tgm_layout layout = {};
    if (tegra_gm20b_surface_layout(firstPage, &layout) != 1 || layout.bytes > bytes ||
        layout.row_bytes < mode.hactive * 4u || layout.row_bytes > 0xffff) {
        if (held)
            tegra_gm20b_surface_unhold(held);
        return false;
    }
    slot.layout = held;
    slot.kind = 2u | (static_cast<uint32_t>(layout.y_log2) << 4);
    slot.stride = layout.row_bytes;
    return true;
}

int SwitchExternalScreen::prepareSlot(OSObject *surface)
{
    auto *memory = native_legacy_surface_descriptor(surface);
    SwitchNativeScanout snapshot = {};
    switch_gpu_backing *held = nullptr;
    for (const auto &slot : slots)
        if (slot.surface == surface && slot.memory == memory)
            held = slot.backing;
    if (!held || switch_native_scanout_pitch_held(surface, held, &snapshot)) {
        const int error = switch_native_scanout_pitch(surface, &snapshot);
        if (error)
            return error;
    }
    const auto &allocation = snapshot.allocation;
    const uint64_t offset = snapshot.pitchOffset;
    const uint64_t bytes = native_legacy_surface_allocation(surface);
    const uint32_t pages = static_cast<uint32_t>((bytes + 4095) / 4096);
    if (!memory || !allocation.pages || !bytes || (offset & 4095) || bytes > kSlotSpan ||
        offset / 4096 > allocation.page_count || pages > allocation.page_count - offset / 4096 || !mapDomain()) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    int victim = -1;
    for (unsigned i = 0; i < kSlots; ++i) {
        auto &slot = slots[i];
        if (slot.memory == memory && slot.surface == surface && slot.pages == pages && slot.offset == offset &&
            slot.allocationId == allocation.allocation_id && slot.generation == allocation.allocation_generation) {
            if (!slot.layout && !applyTiledLayout(slot, allocation.pages[0], bytes)) {
                switch_native_scanout_release(&snapshot);
                return -1;
            }
            if (slot.read.lease) {
                if (slot.read.allocation.content_generation != snapshot.allocation.content_generation) {
                    switch_native_scanout_release(&snapshot);
                    return -1;
                }
                switch_native_scanout_release(&snapshot);
            } else {
                slot.read = snapshot;
            }
            slot.used = ++slotUses;
            if (!slot.counted && switch_display::surfaceUseCounted(surface)) {
                switch_surface_use_increment(surface, 0);
                slot.counted = true;
            }
            return static_cast<int>(i);
        }
        if (static_cast<int>(i) != front && static_cast<int>(i) != drawing &&
            (victim < 0 || slot.used < slots[victim].used))
            victim = static_cast<int>(i);
    }
    // A swap validated before a modeset can reach scanout after it. Cached slots belong to one geometry.
    const uint64_t pitch = native_legacy_surface_stride(surface);
    if (victim < 0 || native_legacy_surface_width(surface) != mode.hactive ||
        native_legacy_surface_height(surface) != mode.vactive || pitch > 0xffff || (pitch & 63u) ||
        !native_legacy_surface_bounds(mode.hactive, mode.vactive, pitch, bytes, memory->getLength())) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    const unsigned index = static_cast<unsigned>(victim);
    releaseSlot(index);
    if (memory->prepare(kIODirectionOut) != kIOReturnSuccess) {
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    if (!domain.map(kSlotBase + index * kSlotSpan, allocation.pages + offset / 4096, pages, false)) {
        domain.unmap(kSlotBase + index * kSlotSpan, pages);
        cleanDomain();
        memory->complete(kIODirectionOut);
        switch_native_scanout_release(&snapshot);
        return -1;
    }
    auto &slot = slots[index];
    slot.read = snapshot;
    slot.memory = memory;
    memory->retain();
    slot.surface = surface;
    surface->retain();
    slot.pages = pages;
    slot.offset = offset;
    slot.allocationId = allocation.allocation_id;
    slot.generation = allocation.allocation_generation;
    slot.used = ++slotUses;
    slot.backing = snapshot.view.native;
    switch_gpu_backing_retain(slot.backing);
    switch_gpu_backing_scanout(slot.backing, 1);
    slot.counted = switch_display::surfaceUseCounted(surface);
    if (slot.counted)
        switch_surface_use_increment(surface, 0);
    slot.stride = static_cast<uint32_t>(pitch);
    if (!applyTiledLayout(slot, allocation.pages[0], bytes)) {
        releaseSlot(index);
        return -1;
    }
    cleanDomain();
    return victim;
}

void SwitchExternalScreen::kickSwapQueue()
{
    if (dcb && switch_abi_load<const void *>(this, vsyncClientList))
        dcb->frameInterrupt(true);
    if (kicking || !dcb || drawing >= 0 || pending || device != 2)
        return;
    kicking = true;
    requestFrame();
    kicking = false;
}

void SwitchExternalScreen::requestFrame()
{
    const auto *request = pendingSwapRequest();
    if (!request)
        return;
    OSObject *surface = nullptr;
    memcpy(&surface, reinterpret_cast<const uint8_t *>(request) + swapEffectiveSurfaces, sizeof(surface));
    if (!surface) {
        dcb->windowOff();
        pending = request;
        latched = true;
        dcb->frameInterrupt(true);
        return;
    }
    const int prepared = prepareSlot(surface);
    if (prepared < 0) {
        if (prepared == -16 || prepared == SWITCH_GPU_BACKING_STALE)
            retryWhenReady();
        else
            dropFrame(request);
        return;
    }
    const unsigned index = static_cast<unsigned>(prepared);
    drawing = prepared;
    pending = request;
    gpuGeneration = linkGeneration;
    const auto &producer = slots[index].read.view.producer;
    if (!switch_gpu_backing_promised(slots[index].backing) &&
        (!producer.engine || switch_gpu_backing_token_external_status(&producer) == 1)) {
        latchFrame();
        return;
    }
    switch_gpu_backing_token tokens[SWITCH_SCANOUT_MAX_DEPENDENCIES] = {};
    TegraScanoutCopy offer = {};
    offer.version = TEGRA_SCANOUT_COPY_VERSION;
    offer.width = mode.hactive;
    offer.height = mode.vactive;
    offer.sequence = sequence + 1;
    offer.display = this;
    offer.source = native_legacy_surface_descriptor(surface);
    offer.sourceStride = slots[index].stride;
    offer.sourceBytes = native_legacy_surface_allocation(surface);
    const bool tiled = slots[index].kind != 0;
    offer.flags = TEGRA_SCANOUT_COPY_FENCE_ONLY |
                  (slots[index].presented ? TEGRA_SCANOUT_COPY_PRODUCER_WATCH : 0u) |
                  (tiled ? TEGRA_SCANOUT_COPY_SOURCE_BLOCK : 0u);
    offer.sourceBlockHeightLog2 = tiled ? ((slots[index].kind >> 4) & 7u) : 0u;
    offer.dependencies = tokens;
    offer.dependencyCount = switch_native_scanout_dependencies(slots[index].read, tokens,
                                                               SWITCH_SCANOUT_MAX_DEPENDENCIES);
    gpuPending = true;
    const int result = tegra_gm20b_scanout_copy(&offer);
    if (result) {
        gpuPending = false;
        pending = nullptr;
        drawing = -1;
        if (front != prepared)
            releaseRead(index);
        if (result == kIOReturnCannotLock || result == kIOReturnBusy)
            retryWhenReady();
        else
            dropFrame(request);
        return;
    }
    ++sequence;
}

void SwitchExternalScreen::gpuComplete(int status)
{
    __atomic_store_n(&gpuStatus, status, __ATOMIC_RELEASE);
    completion->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchExternalScreen::completionFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    self->gpuPending = false;
    if (!self->dcb || self->gpuGeneration != self->linkGeneration ||
        __atomic_load_n(&self->gpuStatus, __ATOMIC_ACQUIRE) != 0) {
        if (self->drawing >= 0 && self->drawing != self->front) {
            if (!self->connected || self->gpuGeneration != self->linkGeneration)
                self->releaseSlot(static_cast<unsigned>(self->drawing));
            else
                self->releaseRead(static_cast<unsigned>(self->drawing));
        }
        self->drawing = -1;
        self->retire(self->pending, false);
        self->pending = nullptr;
        if (!self->connected || self->selectedMode < 0 || self->device != 2)
            self->drainSwapQueue();
        else
            self->kickSwapQueue();
        return;
    }
    self->latchFrame();
}

void SwitchExternalScreen::latchFrame()
{
    const unsigned index = static_cast<unsigned>(drawing);
    const auto &slot = slots[index];
    if (dcb->scanout(static_cast<uint32_t>(kSlotBase + index * kSlotSpan), slot.stride, slot.kind)) {
        latched = true;
        return;
    }
    if (drawing != front)
        releaseRead(index);
    drawing = -1;
    const auto *request = pending;
    pending = nullptr;
    dropFrame(request);
}

void SwitchExternalScreen::dropFrame(const OriginalLegacySwapRequest *request)
{
    if (retire(request, false) && pendingSwapRequest())
        readySource->interruptOccurred(nullptr, nullptr, 0);
}

bool SwitchExternalScreen::retire(const OriginalLegacySwapRequest *request, bool shown)
{
    if (!activateOnlyPendingSwap(request))
        return false;
    switch_external_complete_current(this, shown);
    return true;
}

bool SwitchExternalScreen::frameEnd()
{
    if (!dcb)
        return true;
    if (latched) {
        const auto receipt = dcb->windowLatch(drawing < 0 ? 0 : static_cast<uint32_t>(kSlotBase + drawing * kSlotSpan));
        if (receipt == TegraDisplayB::Latch::Missed)
            return false;
        if (receipt == TegraDisplayB::Latch::Active) {
            latched = false;
            if (front >= 0 && front != drawing)
                releaseRead(static_cast<unsigned>(front));
            front = drawing;
            drawing = -1;
            retire(pending, true);
            pending = nullptr;
            if (front >= 0) {
                slots[front].presented = true;
                switch_native_scanout_deliver(&slots[front].read);
            }
        }
    }
    const auto *clients = switch_abi_load<const void *>(this, vsyncClientList);
    if (clients)
        native_legacy_vsync_notify(this, mach_absolute_time(), mach_continuous_time(), interval);
    kickSwapQueue();
    if (dcb)
        dcb->frameInterrupt(latched || switch_abi_load<const void *>(this, vsyncClientList));
    return true;
}
