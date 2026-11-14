// SPDX-License-Identifier: BSD-2-Clause-Patent
// The deadlines a lost DC A interrupt or a lost GPU completion panics at. Included in the class body (and in the host
// harness).

// Under the gate, a request just written: checkFlipDeadline looks at it kFlipDeadlineMS from now, or settles it at once while the
// scanout is stopped.
void armFlipDeadline()
{
    if (!flipTimer)
        return;
    settleArmed = scanoutStopped;
    flipTimer->setTimeoutMS(scanoutStopped ? 0 : kFlipDeadlineMS);
}

// A latch with nothing left out: nothing for the deadline to check.
void flipDeadlineIdle()
{
    if (ring.pending == kNoBuffer && !probeRequest && !planeLatch.requested() && !planeHeldSet &&
        !planeRecoveryWaits() && flipTimer) {
        flipTimer->cancelTimeout();
        settleArmed = false;
    }
}

// An offer out: its completion is looked for kGpuCopyDeadlineMS from now.
void armGpuDeadline()
{
    gpuDeadlines = 0;
    gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
}

void cancelGpuDeadline()
{
    gpuDeadline->cancelTimeout();
}
