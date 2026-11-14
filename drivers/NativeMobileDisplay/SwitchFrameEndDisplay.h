// SPDX-License-Identifier: BSD-2-Clause-Patent
// DC A's frame end between its primary filter and the gate: INT_MASK's FRAME_END routing, the filter, and the status the
// frame-end action takes. Included in the class body (and in the host harness).

void writeIntMask(uint32_t clear, uint32_t set)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    dc[kDcIntMask] = (dc[kDcIntMask] & ~clear) | set;
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// Any context. A stopped or powered-down scanout gives no frame end; setScanout unmasks it at the restart. INT_ENABLE keeps
// FRAME_END latching while it is masked, so the latched one is stale (a frame end long ago, not now) and goes first.
void unmaskFrameEnd()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    if (intMaskLive && !scanoutStopped && !hwSuspended && !(dc[kDcIntMask] & kFrameEndInt)) {
        dc[kDcIntStatus] = kFrameEndInt;
        dc[kDcIntMask] = dc[kDcIntMask] | kFrameEndInt;
    }
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// The kick's first step: every display job starts there and some wait for a frame end.
void kickUnmaskFrameEnd()
{
    unmaskFrameEnd();
}

void maskIdleFrameEnd()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    if (!__atomic_load_n(&gFrameEndHolds, __ATOMIC_ACQUIRE))
        dc[kDcIntMask] = dc[kDcIntMask] & ~kFrameEndInt;
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// Primary interrupt context. The action is scheduled with signalInterrupt() and the filter returns false: returning
// true on this level line makes IOFilterInterruptEventSource::disableInterruptOccurred mask it until the work loop has
// run the action, and TegraPanel's DSI steps and the console tape need every frame end however long the gate is held.
// The filter clears the DC's status and reads it back first, which is what signalInterrupt() requires.
template <class Self> static bool vblankFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    Self self = static_cast<Self>(owner);
    const uint32_t status = self->dc[kDcIntStatus] & (kVblankInt | kFrameEndInt | kVpulse2Int);
    if (!status)
        return false;
    self->dc[kDcIntStatus] = status;
    (void)self->dc[kDcIntStatus];  // read back: the line is low before ArmCpu's EOI
    if (gPanicRestore.panicking)
        return false;  // window A is the panic's (restorePitchOnPanic): no flip, no DSI command
    if (status & kVblankInt) {
        self->vblankTime = mach_absolute_time();
        self->vblankContinuous = mach_continuous_time();
    }
    if (status & kVpulse2Int) {  // r17-touch: the vsync phase's line
        self->vpulseTime = mach_absolute_time();
        self->vpulseContinuous = mach_continuous_time();
    }
    // The panel's DSI command steps, timed from the frame end as hekate does (the line is already low; the last step takes ~620 us).
    // A console waiting to scroll (waitTapeFrame) wakes at every frame end while it draws on the tape.
    if (status & kFrameEndInt) {
        if (__atomic_load_n(&self->scanoutQuiescing, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&self->scanoutStopFrames, 1ULL, __ATOMIC_RELEASE);
            thread_wakeup(reinterpret_cast<event_t>(&self->scanoutStopFrames));
        }
        tegra_panel_frame_end_filter();
        self->color->frameEndFilter();
        if (self->tapeAttached) {
            __atomic_fetch_add(&self->tapeFrames, 1ULL, __ATOMIC_RELEASE);
            thread_wakeup((event_t)&self->tapeFrames);
        }
        // r6-lows: the frame end's time (the present stamp); debug kernels also the DC A period and a frame end the
        // action has not taken yet.
        const uint64_t now = mach_absolute_time();
        const uint64_t last = self->feLast;
        __atomic_store_n(&self->feStamp, now, __ATOMIC_RELEASE);
        self->feLast = now;
        if (self->stats) {
            // Only consecutive frame ends (a stopped scanout or SC7 in between is no period).
            if (last && now > last && (!self->vsyncInterval || now - last < 2 * self->vsyncInterval)) {
                const uint64_t period = now - last;
                __atomic_fetch_add(&self->fePeriodSum, period, __ATOMIC_RELAXED);
                __atomic_fetch_add(&self->fePeriodN, 1u, __ATOMIC_RELAXED);
                if (period < __atomic_load_n(&self->fePeriodMin, __ATOMIC_RELAXED))
                    __atomic_store_n(&self->fePeriodMin, period, __ATOMIC_RELAXED);
                if (period > __atomic_load_n(&self->fePeriodMax, __ATOMIC_RELAXED))
                    __atomic_store_n(&self->fePeriodMax, period, __ATOMIC_RELAXED);
            }
            if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kFrameEndInt)
                __atomic_fetch_add(&self->stFeFolded, 1u, __ATOMIC_RELAXED);
        }
    }
    // Two V_BLANKs before the action runs (the gate busy, e.g. with a copy) fold into one notification; the stats count both.
    if ((status & kVblankInt) && self->stats && self->vsyncArmed) {
        __atomic_fetch_add(&self->stVblanks, 1u, __ATOMIC_RELAXED);
        if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kVblankInt)
            __atomic_fetch_add(&self->stVsyncFolded, 1u, __ATOMIC_RELAXED);
    }
    __atomic_fetch_or(&self->dcPending, status, __ATOMIC_RELEASE);
    source->signalInterrupt();
    return false;
}

// The frame-end action's first step: what the filter took since the last action.
uint32_t takeFrameEndStatus()
{
    return __atomic_exchange_n(&dcPending, 0u, __ATOMIC_ACQUIRE);
}
