// SPDX-License-Identifier: BSD-2-Clause-Patent
    uint64_t oldestPendingFrame() const
    {
        uint64_t first = 0;
        const auto include = [&first](uint64_t sequence) {
            if (sequence && (!first || sequence < first))
                first = sequence;
        };
        const int entries[] = {ring.pending, ring.superseded, ring.held};
        for (const int entry : entries)
            if (entry != kNoBuffer)
                include(entryPendingSeq[entry] ? entryPendingSeq[entry] : entrySeq[entry]);
        const auto plane = [this, &include](const PlaneSet &set) {
            if (!planesAbandoned)
                include(set.pendingSeq ? set.pendingSeq : set.seq);
        };
        if (planeRecovering)
            plane(planeRecoveryTarget);
        if (planeLatch.requested())
            plane(planeSetOf(planeLatch.armed));
        if (planeLatch.superseded >= 0)
            plane(planeSetOf(planeLatch.superseded));
        if (planeHeldSet)
            plane(planeHeld);
        if (planeFencePending)
            plane(planeFencing);
        return first;
    }

    // A swap that waited for a frame end (a busy overlay source, a scanned slot) goes again at one where nothing else
    // will kick: no window A flip or overlay set out, no console scroll, probe or overlay recovery in progress.
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void kickAtIdleFrameEnd()
    {
        if (!kickDeferred || ring.pending != kNoBuffer || planeLatch.requested() || tapeSettle || probeRequest ||
            probe.active() || planeProbe.active() || planeRecovering || hwSuspended || scanoutStopped)
            return;
        kickDeferred = false;
        kickSwapQueue();
    }

    // At a latch (latchRetire): every taken swap at the head up to the one seen on screen (take sequence seq) retires, in queue
    // order; the one still in flight, or a newer one asked for, stays. shownAt: when the screen began showing it (mach time).
    // NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
    void retireThrough(uint64_t seq, uint64_t shownAt)
    {
        if (latchRetire) {
            scanoutRetirement.observe(seq, shownAt);
            const auto ready = scanoutRetirement.take(oldestPendingFrame());
            if (!ready.sequence)
                return;
            seq = ready.sequence;
            shownAt = ready.shownAt;
        }
        frameShown(seq, shownAt);
        if (!latchRetire || !seq)
            return;
        bool retired = false;
        for (const OriginalLegacySwapRequest *head = pendingSwapRequest(); head; head = pendingSwapRequest()) {
            const uint64_t at = seqOf(head);
            if (!at || at > seq || (gpuScanout && head == gpuRequest))
                break;
            if (!retireSwap(head, true))
                break;
            retired = true;
        }
        if (retired)
            kickSwapQueue();
    }

