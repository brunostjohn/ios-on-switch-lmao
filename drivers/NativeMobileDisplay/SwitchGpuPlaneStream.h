// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window B/C set the GPU renders flips by host1x stream too. The gate arms its flip at the offer, as
// SwitchMediaPlaneFence.h arms a video set's. The copy thread submits the GPU-to-host1x bridge over the set's GPU
// producers, which may sleep for room. Back under the gate, the display stream WAITs on the bridge's syncpoint (and on
// any VIC or NVDEC producer's), so DC A takes the set when the GPU is done, with no CPU thread between. Included in the
// class body after SwitchMediaCompletionFlipDisplay.h.
//
// Ownership: planeBridgeArmed is the gate's, written before the hand-off to the copy thread, which reads it. The bridge
// handle is the copy thread's until planeBridgeSubmitted takes it under the gate. That hands it to the stream, or, when
// the arm went stale or the stream is refused, to the bridge's own completion (switch_gpu_bridge_notify), counted in the
// stats line. A producer that hangs on the stream route is the overlay fence deadline's (gpuDeadlineCheck).

static inline uint32_t planeBridgeArmed = 0;  // the flip generation armed for the bridged set; 0: the bridge fence

static bool gpuProducer(const switch_gpu_backing_token &token)
{
    return token.engine == SWITCH_GPU_ENGINE_GM20B || token.engine == SWITCH_GPU_ENGINE_PROMISE;
}

static bool host1xProducer(const switch_gpu_backing_token &token)
{
    return token.engine == SWITCH_GPU_ENGINE_VIC || token.engine == SWITCH_GPU_ENGINE_NVDEC;
}

// Under the gate, at the offer of a set planeMediaFence left to the copy thread: its flip armed for a stream when its
// producers include the GPU's and the rest are at most a stream's VIC and NVDEC ones.
void planeBridgeArm(const PlaneSet &p)
{
    planeBridgeArmed = 0;
    switch_gpu_backing_token producers[kPlaneDependencies] = {};
    const unsigned count = planeDependencies(p, producers);
    unsigned host1x = 0;
    bool gpu = false;
    for (unsigned i = 0; i < count; ++i) {
        if (host1xProducer(producers[i]))
            ++host1x;
        else if (gpuProducer(producers[i]))
            gpu = true;
        else
            return;
    }
    if (!gpu)
        return;
    if (host1x > SwitchHost1xDisplayStream::kProducers || !armMediaFlip(p)) {
        if (stats)
            ++stPlaneGpuByCpu;
        return;
    }
    planeBridgeArmed = mediaFlip.generation();
}

// The copy thread: the bridge over every producer of the set (VIC and NVDEC ones too, as the bridge fence took them,
// so a completion through the bridge waits for and sees them all), then the gate's stream.
void planeBridgeWork(const switch_gpu_backing_token *producers, unsigned count)
{
    switch_gpu_bridge *bridge = nullptr;
    int error = kIOReturnAborted;
    while (!__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        error = switch_gpu_bridge_submit(producers, count, &bridge);
        if (error != -16)
            break;
        error = switch_gpu_bridge_wait_room();
        if (error)
            break;
        error = kIOReturnAborted;
    }
    getWorkLoop()->runAction(
        [](OSObject *owner, void *handle, void *status, void *, void *) -> IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->planeBridgeSubmitted(
                static_cast<switch_gpu_bridge *>(handle), static_cast<int>(reinterpret_cast<intptr_t>(status)));
            return kIOReturnSuccess;
        },
        this, bridge, reinterpret_cast<void *>(static_cast<intptr_t>(error)));
}

// Under the gate: the stream over the bridge while the set's flip is still the one armed; else the bridge's own
// completion, whose finishPlaneFence withdraws what is armed and writes the set from the CPU.
void planeBridgeSubmitted(switch_gpu_bridge *bridge, int error)
{
    if (!error && __atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) {
        switch_gpu_bridge_release(&bridge);
        error = kIOReturnAborted;
    }
    if (error) {
        native_legacy_gpu_complete(this, error);
        return;
    }
    if (planeBridgeArmed == mediaFlip.generation() && mediaFlip.state() == SwitchCompletionFlip::kArmed) {
        switch_gpu_backing_token producers[kPlaneDependencies] = {}, host1x[kPlaneDependencies] = {};
        const unsigned count = planeDependencies(planeFencing, producers);
        unsigned n = 0;
        for (unsigned i = 0; i < count; ++i)
            if (host1xProducer(producers[i]))
                host1x[n++] = producers[i];
        if (!submitMediaHostFlip(host1x, n, bridge)) {
            if (stats)
                ++stPlaneGpuStreamed;
            return;
        }
    }
    if (stats)
        ++stPlaneGpuStreamRefused;
    static bool told = false;
    if (!told) {
        told = true;
        IOLog(
            "SwitchDisplay: a GPU-rendered window B/C set completes through its bridge: its stream was refused or its "
            "flip withdrawn while the bridge was submitted (once a boot)\n");
    }
    const int notified = switch_gpu_bridge_notify(bridge, native_legacy_gpu_complete, this);
    switch_gpu_bridge_release(&bridge);
    if (notified)
        native_legacy_gpu_complete(this, notified);
}
