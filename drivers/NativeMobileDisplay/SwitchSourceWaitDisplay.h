// SPDX-License-Identifier: BSD-2-Clause-Patent
struct SourceWait {
    const OriginalLegacySwapRequest *request;
    OSObject *surface;
    SwitchNativeScanout read;
    int status;
};
SourceWait *sourceWait = nullptr;
bool sourceWaitQueued = false;

void releaseSourceWait()
{
    if (!sourceWait || sourceWaitQueued)
        return;
    switch_native_scanout_release(&sourceWait->read);
    OSSafeReleaseNULL(sourceWait->surface);
    sourceWait->request = nullptr;
}

bool deferSource(const OriginalLegacySwapRequest *request, OSObject *surface)
{
    if (!copySource || !surface || copyStopping) {
        native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
        return true;
    }
    if (!sourceWait)
        sourceWait = IONewZero(SourceWait, 1);
    if (!sourceWait) {
        native_legacy_transfer_observed(kIOReturnNoMemory, request, 0);
        return true;
    }
    releaseSourceWait();
    sourceWait->request = request;
    sourceWait->surface = surface;
    surface->retain();
    sourceWaitQueued = transferred = true;
    copySource->interruptOccurred(nullptr, nullptr, 0);
    return true;
}

bool holdCopySource(OSObject *surface)
{
    if (!surface)
        return false;
    if (sourceWait && sourceWait->read.lease && sourceWait->surface == surface)
        return true;
    if (!sourceWait)
        sourceWait = IONewZero(SourceWait, 1);
    if (!sourceWait)
        return false;
    releaseSourceWait();
    sourceWait->surface = surface;
    surface->retain();
    return true;
}

// 0 once the producer's writes are in DRAM: external_wait answers 1 for that, 0 while it is still pending.
int waitCopySource()
{
    const int error = sourceWait->read.lease ? 0 : switch_native_scanout_pitch_wait(sourceWait->surface, &sourceWait->read);
    if (error)
        return error;
    const auto &producer = sourceWait->read.view.producer;
    if (!producer.engine)
        return 0;
    const int visible = switch_gpu_backing_token_external_wait(&producer);
    return visible == 1 ? 0 : visible < 0 ? visible : -11;
}

void sourceWaitFinished()
{
    sourceWaitQueued = false;
    transferred = false;
    if (!copyStopping && (latchRetire ? firstUntaken() : pendingSwapRequest()) == sourceWait->request) {
        if (!sourceWait->status) {
            kickSwapQueue();
        } else if (sourceWait->status == TEGRA_SCANOUT_SOURCE_LOST) {
            noteTaken(sourceWait->request, mach_absolute_time());
            planeNextSet = false;
            dropLostFrame(sourceWait->request, kNoBuffer);
        } else {
            native_legacy_transfer_observed(kIOReturnNotReady, sourceWait->request, 0);
        }
    }
    // A CPU fallback still reads this allocation after the retry leaves the family gate.
    if (!copyQueued)
        releaseSourceWait();
}

void sourceWaitWork()
{
    sourceWait->status = __atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)
                             ? kIOReturnAborted
                             : switch_native_scanout_pitch_wait(sourceWait->surface, &sourceWait->read);
    getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->sourceWaitFinished();
            return kIOReturnSuccess;
        }, this);
}
