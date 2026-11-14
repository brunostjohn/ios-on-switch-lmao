// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test's window A aperture check (SwitchWindowAperture.h; display.completion_flip winbuf_alias), debug kernels only:
// included in the class body after DisplayWindowProbeDisplay.h. It runs under the gate with window A's completion flip
// withdrawn, so no writer of window A comes between its stores and their restore.

#if SWITCH_FRAME_TIMELINE
// From TegraVIC's user-client thread.
IOReturn windowApertureCall(SwitchDisplayWindowApertureReply &reply)
{
    if (!getWorkLoop())
        return kIOReturnNotReady;
    reply = {};
    reply.magic = SWITCH_DISPLAY_WINDOW_PROBE_MAGIC;
    reply.words = switch_window_aperture::kAllWords;
    windowProbeOnLoop([&] {
        if (!dc || hwSuspended || gPanicRestore.panicking) {
            reply.error = EAGAIN;
            return;
        }
        if (windowProbeBusy() || columnScanBusy() || landscapeOn() || landscapeHoldsKicks()) {
            reply.error = EBUSY;
            return;
        }
        withdrawCompletionFlip();
        const switch_window_aperture::Result result = switch_window_aperture::check(dc);
        reply.aliased = result.aliased;
        reply.restored = result.restored;
    });
    return kIOReturnSuccess;
}
#endif
