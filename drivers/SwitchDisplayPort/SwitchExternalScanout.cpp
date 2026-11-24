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
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
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
                              TEGRA_SMMU_TRANSLATION_ENABLE_0, (1u << 2) | (1u << 4) | (1u << 6),
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
        layout.width < slot.width || layout.height < slot.height ||
        layout.row_bytes < slot.width * 4u || (layout.row_bytes & 63u) != 0u ||
        layout.row_bytes > 0xffffu || layout.y_log2 > 5u) {
        if (held)
            tegra_gm20b_surface_unhold(held);
        return false;
    }
    slot.layout = held;
    slot.kind = 2u | (static_cast<uint32_t>(layout.y_log2) << 4);
    slot.stride = layout.row_bytes;
    slot.blockBytes = layout.bytes;
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
            if (!slot.layout && !applyTiledLayout(slot, allocation.pages[offset / 4096], bytes)) {
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
    const uint64_t surfW = native_legacy_surface_width(surface);
    const uint64_t surfH = native_legacy_surface_height(surface);
    const uint64_t pitch = native_legacy_surface_stride(surface);
    const uint32_t colorDepth = switch_external::colorDepthOf(native_legacy_surface_format(surface),
                                                              native_legacy_surface_planes(surface));
    if (victim < 0 || !colorDepth || !surfW || !surfH || surfW > SWITCH_DP_MAX_WIDTH || surfH > SWITCH_DP_MAX_HEIGHT ||
        pitch > 0xffff || (pitch & 63u) ||
        !native_legacy_surface_bounds(static_cast<uint32_t>(surfW), static_cast<uint32_t>(surfH), pitch, bytes,
                                      memory->getLength())) {
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
    slot.allocationBytes = bytes;
    slot.blockBytes = 0;
    slot.width = static_cast<uint32_t>(surfW);
    slot.height = static_cast<uint32_t>(surfH);
    slot.colorDepth = colorDepth;
    slot.used = ++slotUses;
    slot.backing = snapshot.view.native;
    switch_gpu_backing_retain(slot.backing);
    switch_gpu_backing_scanout(slot.backing, 1);
    slot.counted = switch_display::surfaceUseCounted(surface);
    if (slot.counted)
        switch_surface_use_increment(surface, 0);
    slot.stride = static_cast<uint32_t>(pitch);
    if (!applyTiledLayout(slot, allocation.pages[offset / 4096], bytes)) {
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
    const auto *bytes = reinterpret_cast<const uint8_t *>(request);
    uint32_t updated = 0;
    OSObject *effectiveSurfaces[switch_external::kLayers] = {};
    memcpy(&updated, bytes + swapUpdated, sizeof(updated));
    memcpy(effectiveSurfaces, bytes + swapEffectiveSurfaces, sizeof(effectiveSurfaces));

    int activeLayer = -1;
    unsigned activeCount = 0;
    for (unsigned i = 0; i < switch_external::kLayers; ++i) {
        if ((updated & (1u << i)) != 0u) {
            if (effectiveSurfaces[i]) {
                switch_external::Rect src = {}, dst = {};
                memcpy(&src, bytes + swapSourceRects + i * sizeof(src), sizeof(src));
                memcpy(&dst, bytes + swapDestinationRects + i * sizeof(dst), sizeof(dst));
                layerRects[i] = {src, dst, true};
            } else {
                layerRects[i] = {};
            }
        }
        if (effectiveSurfaces[i]) {
            activeLayer = static_cast<int>(i);
            ++activeCount;
        }
    }
    if (activeCount == 0u) {
        dcb->windowOff();
        pending = request;
        latched = true;
        latchedAddr = 0;
        dcb->frameInterrupt(true);
        return;
    }
    if (activeCount > 1u || !layerRects[activeLayer].valid) {
#if DEVELOPMENT || DEBUG
        if (diagScanoutFailures < 8u) {
            ++diagScanoutFailures;
            IOLog("SwitchDisplayPort: requestFrame dropped activeCount=%u activeLayer=%d rectValid=%u\n",
                  activeCount, activeLayer, activeLayer >= 0 ? static_cast<unsigned>(layerRects[activeLayer].valid) : 0u);
        }
#endif
        dropFrame(request);
        return;
    }
    OSObject *surface = effectiveSurfaces[activeLayer];
    const int prepared = prepareSlot(surface);
    if (prepared < 0) {
        if (prepared == -16 || prepared == SWITCH_GPU_BACKING_STALE) {
            retryWhenReady();
        } else {
#if DEVELOPMENT || DEBUG
            if (diagScanoutFailures < 8u) {
                ++diagScanoutFailures;
                IOLog("SwitchDisplayPort: prepareSlot failed err=%d surf=%llux%llu stride=%llu alloc=%llu\n",
                      prepared, static_cast<unsigned long long>(native_legacy_surface_width(surface)),
                      static_cast<unsigned long long>(native_legacy_surface_height(surface)),
                      static_cast<unsigned long long>(native_legacy_surface_stride(surface)),
                      static_cast<unsigned long long>(native_legacy_surface_allocation(surface)));
            }
#endif
            dropFrame(request);
        }
        return;
    }
    const unsigned index = static_cast<unsigned>(prepared);
    const auto &slot = slots[index];
    const bool tiled = slot.kind != 0;
    switch_external::SurfaceLayout layout = {};
    layout.surfW = slot.width;
    layout.surfH = slot.height;
    layout.stride = slot.stride;
    layout.colorDepth = slot.colorDepth;
    layout.mappedBytes = slot.allocationBytes;
    layout.blockLinear = tiled;
    layout.yLog2 = tiled ? static_cast<uint8_t>((slot.kind >> 4) & 7u) : 0u;
    layout.rowBytes = tiled ? slot.stride : 0u;
    layout.blockBytes = tiled ? slot.blockBytes : 0u;
    const auto frame = switch_external::switch_external_frame(
        layout, layerRects[activeLayer].src, layerRects[activeLayer].dst, mode.hactive, mode.vactive);
    if (!frame.valid) {
#if DEVELOPMENT || DEBUG
        if (diagScanoutFailures < 8u) {
            ++diagScanoutFailures;
            const auto &rect = layerRects[activeLayer];
            IOLog("SwitchDisplayPort: frame geometry invalid surf=%ux%u stride=%u kind=%#x src=(%d,%d,%d,%d) "
                  "dst=(%d,%d,%d,%d) mode=%ux%u\n",
                  slot.width, slot.height, slot.stride, slot.kind, rect.src.x, rect.src.y, rect.src.w, rect.src.h,
                  rect.dst.x, rect.dst.y, rect.dst.w, rect.dst.h, mode.hactive, mode.vactive);
        }
#endif
        if (front != prepared)
            releaseRead(index);
        dropFrame(request);
        return;
    }
    if (!frame.visible) {
        if (front != prepared)
            releaseRead(index);
        dcb->windowOff();
        pending = request;
        latched = true;
        latchedAddr = 0;
        dcb->frameInterrupt(true);
        return;
    }
    pendingFrame = frame;
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
    offer.width = slots[index].width;
    offer.height = slots[index].height;
    offer.sequence = sequence + 1;
    offer.display = this;
    offer.source = native_legacy_surface_descriptor(surface);
    offer.sourceStride = slots[index].stride;
    offer.sourceBytes = native_legacy_surface_allocation(surface);
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
        if (result == kIOReturnCannotLock || result == kIOReturnBusy) {
            retryWhenReady();
        } else {
#if DEVELOPMENT || DEBUG
            if (diagScanoutFailures < 8u) {
                ++diagScanoutFailures;
                IOLog("SwitchDisplayPort: tegra_gm20b_scanout_copy failed ret=%#x surf=%ux%u stride=%u bytes=%llu kind=%#x\n",
                      result, slots[index].width, slots[index].height, slots[index].stride,
                      static_cast<unsigned long long>(offer.sourceBytes), slots[index].kind);
            }
#endif
            dropFrame(request);
        }
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
    const int status = __atomic_load_n(&self->gpuStatus, __ATOMIC_ACQUIRE);
    if (!self->dcb || self->gpuGeneration != self->linkGeneration || status != 0) {
#if DEVELOPMENT || DEBUG
        if (status != 0 && self->diagScanoutFailures < 8u) {
            ++self->diagScanoutFailures;
            IOLog("SwitchDisplayPort: gpuComplete failed status=%d\n", status);
        }
#endif
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
    const uint32_t iova = static_cast<uint32_t>(kSlotBase + index * kSlotSpan + pendingFrame.startOffset);
    const TegraDisplayB::Frame frame = {
        iova,
        pendingFrame.stride,
        pendingFrame.kind,
        pendingFrame.colorDepth,
        pendingFrame.position,
        pendingFrame.size,
        pendingFrame.prescaled,
        pendingFrame.hOffset,
        pendingFrame.vOffset,
    };
    if (dcb->scanout(frame)) {
        latched = true;
        latchedAddr = iova;
#if DEVELOPMENT || DEBUG
        const auto &slot = slots[index];
        if (diagFrameLogs < 16u &&
            (diagLastPos != frame.position || diagLastSize != frame.size || diagLastSurfW != slot.width ||
             diagLastSurfH != slot.height || diagLastKind != frame.kind)) {
            ++diagFrameLogs;
            diagLastPos = frame.position;
            diagLastSize = frame.size;
            diagLastSurfW = slot.width;
            diagLastSurfH = slot.height;
            diagLastKind = frame.kind;
            IOLog("SwitchDisplayPort: scanout iova=%#x surf=%ux%u pos=(%u,%u) size=%ux%u stride=%u kind=%#x depth=%u\n",
                  iova, slot.width, slot.height, frame.position & 0xffffu, frame.position >> 16,
                  frame.size & 0xffffu, frame.size >> 16, frame.stride, frame.kind, frame.colorDepth);
        }
#endif
        return;
    }
#if DEVELOPMENT || DEBUG
    if (diagScanoutFailures < 8u) {
        ++diagScanoutFailures;
        IOLog("SwitchDisplayPort: dcb->scanout failed iova=%#x pos=%#x size=%#x stride=%u kind=%#x\n",
              iova, frame.position, frame.size, frame.stride, frame.kind);
    }
#endif
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
        const auto receipt = dcb->windowLatch(latchedAddr);
        if (receipt == TegraDisplayB::Latch::Missed) {
#if DEVELOPMENT || DEBUG
            if (diagScanoutFailures < 8u) {
                ++diagScanoutFailures;
                IOLog("SwitchDisplayPort: window A missed latch for iova=%#x\n", latchedAddr);
            }
#endif
            dcb->resetWindowCache();
            latched = false;
            latchedAddr = 0;
            if (drawing >= 0 && drawing != front)
                releaseRead(static_cast<unsigned>(drawing));
            drawing = -1;
            retire(pending, false);
            pending = nullptr;
        } else if (receipt == TegraDisplayB::Latch::Active) {
            latched = false;
            latchedAddr = 0;
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
        dcb->frameInterrupt(true);
    return true;
}
