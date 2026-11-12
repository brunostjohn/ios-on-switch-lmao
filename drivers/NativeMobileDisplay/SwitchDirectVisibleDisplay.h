// SPDX-License-Identifier: BSD-2-Clause-Patent
// Direct scanout with no present job when the frame is already in DRAM. A slot holds its surface's backing as scanned
// (switch_gpu_backing_scanout), so the GPU driver ends every job that writes it External: its completion then means its
// writes left the GPU's L2 (FRAMECPU C6.1). A swap whose slot's producer token is externally visible at the offer (or
// that no engine wrote) flips from the kick itself; one whose producer is still running keeps the fence-only present.
// A window B change going out with the frame keeps the present when it has writers of its own to wait for.
// Included in the class body after SwitchDirectSlots.h; under the gate.

switch_gpu_backing *scanoutHeld[kSlots] = {};
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool directVisibleLogged = false;
#endif

void holdScanoutBacking(unsigned k)
{
    releaseScanoutBacking(k);
    auto *backing = slots[k].read ? slots[k].read->view.native : nullptr;
    if (!backing)
        return;
    switch_gpu_backing_retain(backing);
    switch_gpu_backing_scanout(backing, 1);
    scanoutHeld[k] = backing;
}

void releaseScanoutBacking(unsigned k)
{
    if (!scanoutHeld[k])
        return;
    switch_gpu_backing_scanout(scanoutHeld[k], 0);
    switch_gpu_backing_release(scanoutHeld[k]);
    scanoutHeld[k] = nullptr;
}

bool directFrameVisible(int entry) const
{
    const auto *read = slots[entry - kSlot0].read;
    if (!read || !read->lease || planeNextAlsoWritten() || (planeNextSet && planeFenced(planeNext)))
        return false;
    const auto &producer = read->view.producer;
    return !producer.engine || switch_gpu_backing_token_external_status(&producer) == 1;
}

// offerDirect's first step once the slot is mapped: true when the frame went to window A from here.
bool presentDirectVisible(const OriginalLegacySwapRequest *request, int entry, uint32_t width, uint32_t height,
                          uint64_t kickAt)
{
    if (!directFrameVisible(entry))
        return false;
    noteTaken(request, kickAt);
    gpuLength = static_cast<uint64_t>(width) * height * 4;
    flipRequest = request;
    presentEntry(entry);
    native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
    if (stats)
        ++stDirect;
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
    if (!directVisibleLogged) {
        directVisibleLogged = true;
        IOLog("SwitchDisplay: first direct frame flipped with no present job: its producer's writes were in DRAM\n");
    }
#endif
    return true;
}
