// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's flip written by its producer's completion (SwitchCompletionFlip.h): for a direct frame, a GPU copy and a
// VIC copy, native_legacy_gpu_complete writes window A's START_ADDR, layout and request on whatever thread delivers it
// (the GM20B interrupt pass for a watched present, a present job's completion work item, host1x's fence callback), so
// the request reaches DC A with no display work-loop hop. Everything else stays on the gate: the completion's own pass
// puts the flip in the ring exactly where presentEntry would have written it, and a frame end that comes first runs
// that pass before it reads the latch, so the latch, the retirement and the present stamps see the flip at the
// boundary that took it.
//
// The completion writes window A through its own aperture (mainline drm/tegra dc.c tegra_plane_offset, window A's
// plane->offset 0xa00: DC_WIN 0x700 + i at 0xb80 + i, DC_WINBUF 0x800 + i at 0xbc0 + i), never WINDOW_HEADER or
// STATE_ACCESS, which the gate switches for its reads of the active copy. Its WIN_A_UPDATE promotes all of window
// A's assembly copy, so a gate write that puts something else there (a recovery or held flip, window A's blend over
// an overlay, the palette bit, the stop) first withdraws an armed flip under the lock; that frame then flips from the
// gate as before. Included in the class body; writeCompletionFlip runs on the completion's thread, the rest under the
// gate.

static inline IOSimpleLock *completionFlipLock = nullptr;
SwitchCompletionFlip completionFlip;
static constexpr uint32_t kWinbufABase = 0xbc0;
static constexpr uint32_t kWinAStart = kWinbufABase, kWinAKind = kWinbufABase + (kWinbufSurfaceKind - kWinbufStartAddr),
                          kWinAHOffset = kWinbufABase + (kWinbufHOffset - kWinbufStartAddr),
                          kWinAStride = kWinABase + (kWinLineStride - kWinOptions);
#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kFlipTotals = 4;
static inline uint32_t flipTotals[kFlipTotals] = {};  // completion, gate, lead us, withdrawn
static inline struct sysctl_oid flipTotalOids[kFlipTotals] = {};
#endif

void startCompletionFlip()
{
    IOSimpleLock *lock = IOSimpleLockAlloc();
    if (!lock)
        panic("SwitchDisplay: no lock for window A's completion flips");
    __atomic_store_n(&completionFlipLock, lock, __ATOMIC_RELEASE);
    flipTotalsRegister();
}

bool completionFlipIdle() const
{
    return __atomic_load_n(&completionFlip.state, __ATOMIC_RELAXED) == SwitchCompletionFlip::kIdle;
}

bool completionFlipOut() const
{
    return !completionFlipIdle();
}

// Armed only when presentEntry would write this frame at once whatever latches meanwhile: no hold, no window B change
// riding on it, nothing else owning window A. A recovery between here and the completion withdraws it (flipFromGate).
bool completionFlipMay(int entry)
{
    if (!rotated || !rotationLatched || !doubleBuffer || (entry >= kSlot0 && !directAttached) || hwSuspended ||
        gPanicRestore.panicking || planeNextSet || probeRequest || probe.active() || columnScanBusy())
        return false;
    if (ring.held != kNoBuffer || ring.superseded != kNoBuffer)
        return false;
    return ring.pending == kNoBuffer || (ring.replace && sameLayout(entry, ring.pending));
}

// Before the offer: its completion may run on another CPU before the offer returns. True when armed.
bool armCompletionFlip(int entry)
{
    if (!completionFlipLock)
        startCompletionFlip();
    if (!completionFlipMay(entry))
        return false;
    if (completionFlipOut())
        panic("SwitchDisplay: an offer for entry %d while entry %d's completion flip is out (state %u)", entry,
              completionFlip.entry, static_cast<unsigned>(completionFlip.state));
    uint32_t kind = 0, stride = 0, hOffset = 0;
    layoutOf(entry, kind, stride, hOffset);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    completionFlip.arm(entry, surfaceAddress(entry));
    completionFlip.armLayout(kind, stride, hOffset);
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    return true;
}

int offerWithCompletionFlip(const TegraScanoutCopy &copy, int entry)
{
    const bool armed = armCompletionFlip(entry);
    const int offered = tegra_gm20b_scanout_copy(&copy);
    if (offered && armed)
        withdrawCompletionFlip();
    return offered;
}

// The completion's thread, after gpuDonePending is set: a failed frame withdraws the flip, a done one is requested.
void writeCompletionFlip(int status)
{
    IOSimpleLock *lock = __atomic_load_n(&completionFlipLock, __ATOMIC_ACQUIRE);
    if (!lock)
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (gPanicRestore.panicking)
        completionFlip.withdraw();
    const bool write = completionFlip.claim(status);
    if (write) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (completionFlip.layout) {
            dc[kWinAKind] = completionFlip.kind;
            dc[kWinAStride] = completionFlip.stride;
            dc[kWinAHOffset] = completionFlip.hOffset;
        }
        dc[kWinAStart] = completionFlip.start;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        completionFlip.requestedAt = __atomic_load_n(&frameEnds, __ATOMIC_RELAXED);
        completionFlip.writtenAt = mach_absolute_time();
    }
    [[maybe_unused]] const uint32_t start = completionFlip.start;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (write && SWITCH_FRAME_TIMELINE)
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FLIP), start);
}

void withdrawCompletionFlip()
{
    if (completionFlipIdle())
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const bool armed = completionFlip.state == SwitchCompletionFlip::kArmed;
    completionFlip.withdraw();
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (armed && SWITCH_FRAME_TIMELINE)
        noteFlipTotal(3, 1);
}

// writeFlip's first step: the gate writes window A itself.
void flipFromGate()
{
    withdrawCompletionFlip();
    if (SWITCH_FRAME_TIMELINE)
        noteFlipTotal(1, 1);
}

// The completion's own pass, run now (gpuDoneFired's), when its flip is already written: a frame end or SC7 that
// comes first must see the flip in the ring.
void runCompletionPass()
{
    if (!__atomic_exchange_n(&gpuDonePending, false, __ATOMIC_ACQUIRE))
        panic("SwitchDisplay: window A was flipped to %#x by its producer's completion, which signalled nothing",
              completionFlip.start);
    finishGpuSwap(__atomic_load_n(&gpuStatus, __ATOMIC_RELAXED));
}

bool completionFlipWritten()
{
    if (completionFlipIdle())
        return false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const bool written = completionFlip.state == SwitchCompletionFlip::kWritten;
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    return written;
}

// vblankTick's first step.
void takeCompletionFlip()
{
    if (completionFlipWritten())
        runCompletionPass();
}

// SC7, before the controller's state is saved: a written flip goes in the ring, an armed one is withdrawn, since no
// completion may write the controller once it is unclocked. The pass may kick the next offer, armed in turn.
void suspendCompletionFlip()
{
    while (!completionFlipIdle()) {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
        const bool armed = completionFlip.state == SwitchCompletionFlip::kArmed;
        completionFlip.withdraw();
        const bool written = completionFlip.state == SwitchCompletionFlip::kWritten;
        IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
        if (armed && SWITCH_FRAME_TIMELINE)
            noteFlipTotal(3, 1);
        if (written)
            runCompletionPass();
    }
}

// checkFlip's read of what the boundary took: STATE_CONTROL and window A's active START_ADDR, with no completion
// write between them and the ring's view (one written before is put in the ring first).
struct LatchState {
    uint32_t control, start;
};

LatchState latchState()
{
    for (;;) {
        if (completionFlipIdle())
            return {dc[kDcStateControl], windowActive(kWinbufStartAddr)};
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
        if (completionFlip.state != SwitchCompletionFlip::kWritten) {
            const LatchState seen = {dc[kDcStateControl], windowActive(kWinbufStartAddr)};
            IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
            return seen;
        }
        IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
        runCompletionPass();
    }
}

// presentEntry's first step: true when the completion already wrote entry i, which then enters the ring as
// presentEntry's own write would have.
bool presentedByCompletion(int i)
{
    if (!completionFlipLock)
        startCompletionFlip();
    if (completionFlipIdle())
        return false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const SwitchCompletionFlip flip = completionFlip;
    const bool written = completionFlip.present(i);
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (flip.state == SwitchCompletionFlip::kWritten && !written)
        panic("SwitchDisplay: window A flips to entry %d while entry %d's completion flip is not in the ring", i,
              flip.entry);
    if (!written) {
        if (flip.state == SwitchCompletionFlip::kArmed && SWITCH_FRAME_TIMELINE)
            noteFlipTotal(3, 1);
        return false;
    }
    completionFlipIntoRing(flip);
    return true;
}

void completionFlipIntoRing(const SwitchCompletionFlip &flip)
{
    const int i = flip.entry;
    const bool mayReplace = ring.pending == kNoBuffer || sameLayout(i, ring.pending);
    if (ring.ready(i, mayReplace) == TegraScanoutRing::kHold)
        panic("SwitchDisplay: window A was flipped to %#x by its producer's completion, but the ring holds it "
              "(front %d pending %d replace %d)",
              flip.start, ring.front, ring.pending, ring.replace);
    entrySeq[i] = entryPendingSeq[i] = seqOf(flipRequest);
    entryPlaneSet[i] = false;
    markFlip(i);
    marks[i].ready = flip.writtenAt;
    flipRequested = flip.requestedAt;
    armFlipDeadline();
    if (SWITCH_FRAME_TIMELINE)
        noteCompletionFlip(flip);
}

#if SWITCH_FRAME_TIMELINE
void noteCompletionFlip(const SwitchCompletionFlip &flip)
{
    const uint64_t now = mach_absolute_time();
    const uint64_t lead = now > flip.writtenAt ? Span::ticksToUs(now - flip.writtenAt) : 0;
    noteFlipTotal(0, 1);
    noteFlipTotal(2, static_cast<uint32_t>(lead));
    if (flipTotals[0] == 1)
        IOLog("SwitchDisplay: first window A flip written by its producer's completion (entry %d), %llu us before "
              "the display's work loop took it\n",
              flip.entry, static_cast<unsigned long long>(lead));
}

static void noteFlipTotal(unsigned which, uint32_t n)
{
    __atomic_fetch_add(&flipTotals[which], n, __ATOMIC_RELAXED);
}

static int flipTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const int value =
        static_cast<int>(__atomic_load_n(&flipTotals[static_cast<unsigned>(which) % kFlipTotals], __ATOMIC_RELAXED));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void flipTotalsRegister()
{
    static const char *const names[kFlipTotals] = {"switch_flips_completion", "switch_flips_gate",
                                                   "switch_flips_lead_us", "switch_flips_withdrawn"};
    for (unsigned i = 0; i < kFlipTotals; ++i) {
        switch_sysctl_oid(&flipTotalOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), flipTotalSysctl, "I",
                          "window A since boot: flips its producers' completions wrote, flips the display's gate "
                          "wrote, microseconds the completions' writes came before the gate took them, completion "
                          "flips withdrawn");
        flipTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&flipTotalOids[i]);
    }
}
#else
void noteCompletionFlip(const SwitchCompletionFlip &)
{}

static void noteFlipTotal(unsigned, uint32_t)
{}

static void flipTotalsRegister()
{}
#endif
