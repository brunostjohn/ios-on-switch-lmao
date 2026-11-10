// SPDX-License-Identifier: BSD-2-Clause-Patent
void reattachDirect()
{
    char why[160];
    if (switch_smmu_restore(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID, why, sizeof(why)))
        panic("SwitchDisplay: SC7 retained scanout domain could not be restored: %s", why);
}

bool finishResume()
{
    if (!getWorkLoop()->inGate())
        panic("SwitchDisplay: SC7 restore outside the display gate");
    if (!hwSuspended)
        panic("SwitchDisplay: tegra_display_resume without tegra_display_suspend");
    if (!captureDisplayMemory(switch_display::TimingBank::Programmed))
        IOLog("SwitchDisplay: SC7 left no valid display timing: display memory admission off\n");
    if (directAttached || planesAttached)
        reattachDirect();  // SC7 reset the MC: the dc swgroup is in bypass again
    dc[kDcStateAccess] = 0;
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinOptions] = 0;
    for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
        dc[kWindowAState[i]] = savedWindow[i];
    dc[kDcDispWinOptions] = savedDispWinOptions;
    dc[kDcRegActControl] = savedActControl;
    dc[kDcScratchB] = savedScratchB;
    if (vpulseProgrammed)
        writePhaseRegisters();  // r17-touch: SC7 reset them; they latch with the request below
    color->restoreAfterPowerUp();
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
    dc[kDcIntStatus] = kVblankInt | kFrameEndInt | kWinAUnderflowInt;
    dc[kDcIntEnable] = savedIntEnable;
    writeIntMask(~0u, savedIntMask);
    __atomic_store_n(&dcPending, 0u, __ATOMIC_RELAXED);
    planesResume();
    hwSuspended = false;
    scanoutStopped = true;
    vblankSource->enable();
    if (rotated) {
        rotationLatched = false;
        rotationRequested = frameEnds;
        underflowRun = 0;
        __asm__ volatile("dmb ish" ::: "memory");
        gPanicRestore.armed = true;
    }
    if (resumeLit)
        setScanout(true);
    if (presentDeferred) {
        presentDeferred = false;
        presentCopied();
    }  // an off-gate copy that finished during the suspend
    if (gpuPresentDeferred) {
        gpuPresentDeferred = false;
        presentGpuCopy();
    }  // likewise a GPU frame copy
    if (const int held = ring.flipHeld(); held != kNoBuffer)
        writeFlip(held);  // a frame held for the flip slot
    if (kickDeferred) {
        kickDeferred = false;
        kickSwapQueue();
    }
    return resumeLit;
}
