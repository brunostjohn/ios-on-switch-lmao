// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planesResume()
{
    if (!planesOn)
        return true;
    const bool recovering = planeRecovering;
    const PlaneSet now = recovering ? planeRecoveryTarget : planeSetOf(planeLatch.active);
    uint32_t update = kGeneralUpdate | kWinAUpdate | kWinBUpdate | kWinCUpdate;
    uint32_t request = kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq;
    if (!planeReserveBandwidth(now))
        return false;
    if (recovering) {
        planeLatch.reset();
        if (planeLatch.write(now) < 0)
            panic("SwitchDisplay: SC7 recovery target could not be restored");
        planeTarget = now;
        planeRecovering = false;
        planeRecoveryTarget = {};
        planeRequestedAt = frameEnds;
    }
    planeStartRegisters();
    for (unsigned window = 0; window != 2; ++window)
        planeWriteWindowRegisters(window, now.window[window], false);
    const bool over = now.on();
    planeBlending = !over;  // force the write: the cold init put hekate's values back
    planeBlendA(over, update, request);
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    if (planeHeldSet) {
        planeHeldSet = false;
        planeFlipOnly(planeHeld);
    }
    return true;
}
