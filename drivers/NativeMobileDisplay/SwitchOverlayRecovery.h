// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeRecovering = false;
uint64_t planeRecoveryAt = 0;
PlaneSet planeRecoveryTarget = {};

void holdPlaneRecovery(const PlaneSet &p)
{
    uint64_t first = planeRecoveryTarget.pendingSeq ? planeRecoveryTarget.pendingSeq : planeRecoveryTarget.seq;
    const uint64_t incoming = p.pendingSeq ? p.pendingSeq : p.seq;
    if (!first || (incoming && incoming < first))
        first = incoming;
    if (p.seq >= planeRecoveryTarget.seq)
        planeRecoveryTarget = p;
    planeRecoveryTarget.pendingSeq = first;
    planeTarget = planeRecoveryTarget;
}

void writePlaneRecoveryOff()
{
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    scanoutMemoryBarrier();
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq | kWinCActReq;
    planeRecoveryAt = frameEnds;
    armFlipDeadline();
}

void beginPlaneRecovery(const char *why)
{
    if (planeRecovering)
        return;
    IOLog("SwitchDisplay: recovering overlay windows: %s\n", why);
    planeRecovering = true;
    planeRecoveryTarget = {};
    const int states[] = {planeLatch.active, planeLatch.superseded, planeLatch.armed};
    for (int state : states)
        if (state >= 0)
            holdPlaneRecovery(planeSetOf(state));
    if (planeHeldSet) {
        holdPlaneRecovery(planeHeld);
        planeHeldSet = false;
    }
    writePlaneRecoveryOff();
}

bool planeRecoveryFrameEnd()
{
    if (!planeRecovering)
        return false;
    const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcStateAccess] = access | kReadMuxActive;
    const bool disabled = !(dc[kWinBBase + native_planes::kOptions] & native_planes::kWinEnable) &&
                          !(dc[kWinCBase + native_planes::kOptions] & native_planes::kWinEnable);
    dc[kDcStateAccess] = access;
    if (!disabled || (dc[kDcStateControl] & (kWinBActReq | kWinCActReq))) {
        if (frameEnds - planeRecoveryAt >= kLatchFrames)
            panic("SwitchDisplay: overlay recovery could not stop both windows in %u frames", kLatchFrames);
        return true;
    }
    // Only observed inactive windows permit replacing their live CSC/filter state or releasing old snapshots.
    planeLatch.reset();
    for (unsigned window = 0; window != 2; ++window) {
        planeCsc[window][0] = planeCsc[window][1] = -1;
        planeFiltersWritten[window] = false;
    }
    planeUnderflowRun = 0;
    planeRecovering = false;
    const PlaneSet restore = planeRecoveryTarget;
    planeRecoveryTarget = {};
    planeFlipOnly(restore);
    planeReleaseIdle();
    return true;
}
