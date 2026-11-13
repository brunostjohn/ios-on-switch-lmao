// SPDX-License-Identifier: BSD-2-Clause-Patent
// The scanout domain's attach (SwitchScanoutSetup.h) when firmware left window B or C on: the dc swgroup translates all
// three windows at once, so a stray window would fetch through the SMMU. Its OPTIONS are cleared and requested at the
// start, and the attach finishes at the first frame end that reads both windows off with no request out; while they do
// not, the request goes out again every kLatchFrames frame ends. Frame ends stay unmasked for the first request only: a
// window that does not stop is looked at again at the frame ends other work brings. Included in the class body before
// SwitchScanoutSetup.h; runs under the work-loop gate.

// Static: the instance is at the 16 KiB a kalloc_type class may have.
static inline bool scanoutAttachWaits = false;
static inline uint64_t scanoutAttachAskedAt = 0;
static inline unsigned scanoutAttachAsks = 0;

bool scanoutAttachHoldsFrameEnds() const
{
    return scanoutAttachWaits && scanoutAttachAsks <= 1;
}

void stopFirmwareWindows()
{
    dc[kWinBBase + native_planes::kOptions] = 0;
    dc[kWinCBase + native_planes::kOptions] = 0;
    dc[kDcStateControl] = kGeneralUpdate | kWinBUpdate | kWinCUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinBActReq | kWinCActReq;
    scanoutAttachAskedAt = frameEnds;
    ++scanoutAttachAsks;
}

// Every DC A frame end.
void scanoutAttachFrameEnd()
{
    if (!scanoutAttachWaits || hwSuspended)
        return;
    if (otherWindowEnabled() || (dc[kDcStateControl] & (kWinBActReq | kWinCActReq))) {
        if (frameEnds - scanoutAttachAskedAt >= kLatchFrames)
            stopFirmwareWindows();
        return;
    }
    scanoutAttachWaits = false;
    attachScanoutDomain();
}
