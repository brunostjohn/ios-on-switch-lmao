// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU client's completion of an offer and the work-loop pass it signals. Included in the class body (and in the
// host harness).

// Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop. The GPU frame copy's
// completion (any context, even the GPU's interrupt filter) records its status, writes a direct frame's armed window A flip
// (SwitchCompletionFlipDisplay.h) and signals the work loop.
void gpuComplete(int status)
{
    status = completionFlipVerdict(status);
    if (SWITCH_FRAME_TIMELINE) {
        __atomic_store_n(&gpuDoneAt, mach_absolute_time(), __ATOMIC_RELAXED);
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_PRESENT_DONE), gpuSequence, status);
    }
    if (gpuDoneSource) {
        __atomic_store_n(&gpuStatus, status, __ATOMIC_RELAXED);
        __atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);
        writeCompletionFlip(status);
        gpuDoneSource->interruptOccurred(nullptr, nullptr, 0);
        return;
    }

    using Self = decltype(this);

    struct Args {
        Self display;
        int status;
    } args = {this, status};

    IOWorkLoop *loop = getWorkLoop();
    const auto action = [](OSObject *, void *a0, void *, void *, void *) -> IOReturn {
        const Args *a = static_cast<Args *>(a0);
        a->display->finishGpuSwap(a->status);
        return kIOReturnSuccess;
    };
    if (loop) {
        loop->runAction(action, this, &args);
    } else {
        finishGpuSwap(status);
    }
}

template <class Self> static void gpuDoneFired(OSObject *owner, IOInterruptEventSource *, int)
{
    Self self = static_cast<Self>(owner);
    if (!__atomic_exchange_n(&self->gpuDonePending, false, __ATOMIC_ACQUIRE))
        return;
    self->finishGpuSwap(__atomic_load_n(&self->gpuStatus, __ATOMIC_RELAXED));
}
