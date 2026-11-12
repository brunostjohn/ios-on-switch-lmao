// SPDX-License-Identifier: BSD-2-Clause-Patent
bool planeOnScreen(int window = -1) const
{
    const int ids[] = {planeLatch.active, planeLatch.armed, planeLatch.superseded};
    for (const int id : ids)
        if (id >= 0 && (window < 0 ? planeSetOf(id).on() : planeSetOf(id).window[window].on))
            return true;
    return false;
}

switch_display::WindowState planeWindowState(unsigned window) const
{
    return {planeOnScreen(int(window)), planeTarget.window[window].on ? planeTarget.window[window].slot : -1,
            planeCsc[window][0], planeCsc[window][1]};
}

static uint32_t planeWindowBase(unsigned window) { return window ? kWinCBase : kWinBBase; }
static uint32_t planeBufferBase(unsigned window) { return window ? kWinbufCBase : kWinbufBBase; }
static uint32_t planeControlBase(unsigned window) { return window ? kWincCBase : kWincBBase; }

void planeWriteCsc(unsigned window, const native_planes::Image &m)
{
    const uint32_t *k = native_planes::kCsc[m.matrix][m.fullRange ? 1 : 0];
    for (unsigned i = 0; i < 8; ++i)
        dc[planeControlBase(window) + native_planes::kCscYof + i] = k[i];
    planeCsc[window][0] = static_cast<int>(m.matrix);
    planeCsc[window][1] = m.fullRange ? 1 : 0;
}

// Filter taps are live, so only initialize a window whose previous scanout has retired.
void planeStartWindowRegisters(unsigned window)
{
    for (unsigned i = 0; i < 16; ++i) {
        dc[planeControlBase(window) + native_planes::kHFilter0 + i] = native_planes::kHFilterTaps[i];
        dc[planeControlBase(window) + native_planes::kVFilter0 + i] = native_planes::vFilterTap(i);
    }
    planeFiltersWritten[window] = true;
}

void planeStartRegisters()
{
    for (unsigned window = 0; window != 2; ++window)
        planeStartWindowRegisters(window);
    dc[kDcRegActControl] = dc[kDcRegActControl] & ~(kWinBActHcntrSel | kWinCActHcntrSel);
    // Every later write of hekate's REG_ACT_CONTROL keeps windows B and C on frame-boundary latching.
    pitchActControl &= ~(kWinBActHcntrSel | kWinCActHcntrSel);
    dc[kDcIntStatus] = kWinBUnderflowInt | kWinCUnderflowInt;
    dc[kDcIntEnable] = dc[kDcIntEnable] | kWinBUnderflowInt | kWinCUnderflowInt;
}

void planeWriteWindowRegisters(unsigned window, const switch_display::Overlay &p, bool live)
{
    using namespace native_planes;
    const SwitchMMIOWords win = dc + planeWindowBase(window);
    const SwitchMMIOWords buf = dc + planeBufferBase(window);
    if (!p.on) {
        win[kOptions] = 0;
        return;
    }
    const Image &m = p.image;
    // CSC is live rather than shadowed; an incompatible active matrix is converted by VIC before admission.
    if (m.yuv && !live &&
        (planeCsc[window][0] != static_cast<int>(m.matrix) || planeCsc[window][1] != (m.fullRange ? 1 : 0)))
        planeWriteCsc(window, m);
    if (!planeFiltersWritten[window] && !live)
        planeStartWindowRegisters(window);
    win[kColorDepth] = m.colorDepth;
    win[kByteSwap] = m.byteSwap;
    win[kPosition] = m.position;
    win[kSize] = m.size;
    win[kPrescaled] = m.prescaled;
    win[kHInitialDda] = m.hInitial;
    win[kVInitialDda] = m.vInitial;
    win[kDdaIncrement] = m.dda;
    win[kLineStride] = m.lineStride;
    win[kBufStride] = 0;
    win[kUvBufStride] = 0;
    const uint64_t base = static_cast<uint64_t>(planeSlotIova(p.slot)) - planeSlots[p.slot].low;
    buf[kStartAddr] = static_cast<uint32_t>(base + m.start[0]);
    buf[kStartAddrHi] = 0;
    buf[kStartAddrU] = m.yuv ? static_cast<uint32_t>(base + m.start[1]) : 0;
    buf[kStartAddrHiU] = 0;
    buf[kHOffset] = m.hOffset;
    buf[kVOffset] = m.vOffset;
    buf[kSurfaceKind] = m.surfaceKind;
    buf[kCdeControl] = 0;
    win[kBlendLayer] = m.yuv ? kBlendBypass | p.depth : kBlendPremultiplied | p.depth;
    win[kBlendMatch] = m.yuv ? 0 : kBlendOver;
    win[kBlendNomatch] = m.yuv ? 0 : kBlendOver;
    win[kOptions] = m.options;
}

// Window A blends over window B (premultiplied, in front) while B shows something; opaque as hekate left it otherwise.
void planeBlendA(bool over, uint32_t &update, uint32_t &request)
{
    if (over == planeBlending)
        return;
    const SwitchMMIOWords a = dc + kWinABase;
    if (over) {
        a[native_planes::kBlendLayer] = native_planes::kBlendPremultiplied;
        a[native_planes::kBlendMatch] = native_planes::kBlendOver;
        a[native_planes::kBlendNomatch] = native_planes::kBlendOver;
    } else {
        for (unsigned i = 0; i < 3; ++i)
            a[native_planes::kBlendLayer + i] = planeBlendSaved[i];
    }
    planeBlending = over;
    update |= kGeneralUpdate | kWinAUpdate;
    request |= kGeneralActReq | kWinAActReq;
}

// A set waits only for an overlay latch that is coming (its frame end restages it); with none coming, waiting would
// hold every later swap's retirement for good, so the overlays that keep it from going out are dropped instead.
bool planeHoldForLatch(const PlaneSet &p)
{
    if (!planeLatch.requested())
        return false;
    planeHeld = p;
    planeHeldSet = true;
    return true;
}

void planeDropOverlays(PlaneSet &p, unsigned windows)
{
    for (unsigned window = 0; window != 2; ++window)
        if ((windows & (1u << window)) && p.window[window].on) {
            p.window[window] = {};
            if (stats)
                ++stPlaneDropped;
        }
}

// Window B's set p into its assembly registers, with the request bits for the caller's one STATE_CONTROL write; or, while
// which of the last two sets the boundary took is open, held for the frame end that settles it (newest wins).
bool planeStage(PlaneSet p, uint32_t &update, uint32_t &request)
{
    if (planesAbandoned)
        return false;
    if (planeRecovering) {
        holdPlaneRecovery(p);
        return false;
    }
    const switch_display::WindowState windows[2] = {planeWindowState(0), planeWindowState(1)};
    auto assignment = switch_display::assign(p.window, windows);
    if (assignment.convert) {
        planeTarget = p;
        if (planeHoldForLatch(p))
            return false;
        planeDropOverlays(p, assignment.convert);
        assignment = switch_display::assign(p.window, windows);
    }
    if (assignment.window[0] != 0) {
        const auto first = p.window[0];
        p.window[0] = p.window[1];
        p.window[1] = first;
    }
    planeTarget = p;
    if (!planeLatch.mayWrite()) {
        planeHeld = p;
        planeHeldSet = true;
        return false;
    }
    if (!planeReserveBandwidth(p)) {
        if (planeHoldForLatch(p))
            return false;
        planeDropUntilAdmitted(p);
        planeTarget = p;
    }
    PlaneSet commit = p;
    if (!commit.pendingSeq)
        commit.pendingSeq = commit.seq;
    // Publishing the new commit must not make its first CSC initialization appear live.
    const bool live[2] = {planeOnScreen(0), planeOnScreen(1)};
    if (planeLatch.write(commit) < 0) {
        planeHeld = p;
        planeHeldSet = true;
        return false;
    }
    for (unsigned window = 0; window != 2; ++window)
        planeWriteWindowRegisters(window, p.window[window], live[window]);
    planeBlendA(p.on(), update, request);
    update |= kGeneralUpdate | kWinBUpdate | kWinCUpdate;
    request |= kGeneralActReq | kWinBActReq | kWinCActReq;
    planeRequestedAt = frameEnds;
    armFlipDeadline();
    if (stats)
        ++stPlaneFlips;
    return true;
}

// A change of window B alone, from the next frame boundary (no window A frame carries it).
void planeFlipOnly(const PlaneSet &p)
{
    uint32_t update = 0;
    uint32_t request = 0;
    if (!planeStage(p, update, request))
        return;
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
}

