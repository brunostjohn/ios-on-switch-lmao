// SPDX-License-Identifier: BSD-2-Clause-Patent
int planeNativeSlotFor(OSObject *surface, const native_planes::Layer &layer, uint32_t width, uint32_t height,
                       native_planes::Image &image, bool &resolved)
{
    SwitchNativeScanout scanout = {};
    int error = switch_native_scanout_acquire(surface, &scanout);
    resolved = true;
    // Only a surface with no native picture, one the window cannot take, or a retired one falls back to its pitch
    // representation; a busy one (-16) keeps the frame on screen rather than read the representation being replaced.
    if (error && error != -2 && error != -95 && error != SWITCH_GPU_BACKING_STALE) {
        switch_native_scanout_release(&scanout);
        return error;
    }
    if (error || !switch_native_plane_image(layer, width, height, scanout.view.layout,
                                            scanout.view.crop.x, scanout.view.crop.y, image)) {
        switch_native_scanout_release(&scanout);
        error = switch_native_scanout_pitch(surface, &scanout);
        tgm_layout block = {};
        if (!error && !native_planes::imageOf(layer, width, height, image, surfaceLayout(surface, block)))
            error = -95;
        if (!error && scanout.pitchOffset) {
            const uint64_t offset = scanout.pitchOffset;
            if (offset > UINT64_MAX - image.readHigh - 4095) {
                error = -22;
            } else {
                image.start[0] += offset;
                image.start[1] += offset;
                image.readLow = (image.readLow + offset) & ~4095ULL;
                image.readHigh = (image.readHigh + offset + 4095) & ~4095ULL;
            }
        }
    }
    if (error) {
        switch_native_scanout_release(&scanout);
        return error;
    }
    return planeUseNoted(planeMapSnapshot(scanout, image), surface);
}

int planeMapSnapshot(SwitchNativeScanout &scanout, const native_planes::Image &image, int avoid = -1)
{
    const auto &allocation = scanout.allocation;
    if (!image.visible) {
        switch_native_scanout_release(&scanout);
        return -2;
    }
    if (!allocation.pages || image.readHigh <= image.readLow ||
        image.readHigh - image.readLow > kPlaneSlotSpan ||
        image.readHigh > (uint64_t(allocation.page_count) << 12)) {
        switch_native_scanout_release(&scanout);
        return -95;
    }
    int victim = -1, mapped = -1;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (int(k) == avoid)
            continue;
        auto &slot = planeSlots[k];
        const bool samePages = slot.native && slot.mappedAllocation == allocation.allocation_id &&
                               slot.mappedGeneration == allocation.allocation_generation &&
                               slot.low == image.readLow && slot.high == image.readHigh;
        if (samePages && slot.native->lease &&
            slot.native->allocation.content_generation == allocation.content_generation) {
            slot.used = ++planeSlotUses;
            switch_native_scanout_release(&scanout);
            return int(k);
        }
        const bool busy = planeSlotBusy(int(k));
        if (samePages && !busy && mapped < 0)
            mapped = int(k);
        if (!busy && (victim < 0 || slot.used < planeSlots[victim].used))
            victim = int(k);
    }
    if (mapped >= 0) {
        auto &slot = planeSlots[mapped];
        planeUseRelease(unsigned(mapped));
        switch_native_scanout_release(slot.native);
        *slot.native = scanout;
        scanout = {};
        slot.tiled = image.surfaceKind != 0;
        slot.used = ++planeSlotUses;
        return mapped;
    }
    if (victim < 0) {
        switch_native_scanout_release(&scanout);
        return -11;
    }
    auto &slot = planeSlots[victim];
    if (!slot.native)
        slot.native = IONewZero(SwitchNativeScanout, 1);
    if (!slot.native) {
        switch_native_scanout_release(&scanout);
        return -1;
    }
    planeDropSlot(unsigned(victim));
    const uint32_t first = uint32_t(image.readLow >> 12), count = uint32_t((image.readHigh - image.readLow) >> 12);
    if (!probeDomain.map(planeSlotIova(victim), allocation.pages + first, count, false)) {
        probeDomain.unmap(planeSlotIova(victim), count);
        cleanDomain();
        switch_native_scanout_release(&scanout);
        return -1;
    }
    slot.mappedAllocation = allocation.allocation_id;
    slot.mappedGeneration = allocation.allocation_generation;
    *slot.native = scanout;
    scanout = {};
    slot.low = image.readLow;
    slot.high = image.readHigh;
    slot.tiled = image.surfaceKind != 0;
    slot.used = ++planeSlotUses;
    cleanDomain();
    if (stats)
        ++stPlaneMaps;
    return victim;
}

unsigned planeDependencies(const PlaneSet &set, switch_gpu_backing_token tokens[2]) const
{
    unsigned count = 0;
    for (const auto &overlay : set.window) {
        if (!overlay.on)
            continue;
        const auto *native = planeSlots[overlay.slot].native;
        if (native && native->lease && native->view.producer.engine)
            tokens[count++] = native->view.producer;
    }
    return count;
}

void planeNextDependencies(TegraScanoutCopy &copy, switch_gpu_backing_token tokens[2]) const
{
    if (!planeNextSet)
        return;
    copy.dependencyCount = planeDependencies(planeNext, tokens);
    copy.dependencies = copy.dependencyCount ? tokens : nullptr;
}
