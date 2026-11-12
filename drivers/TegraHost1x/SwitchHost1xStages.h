// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine job's retirement with no thread that sleeps per job. The job's syncpoint fence (host1x's interrupt)
// and, when its stream waits for a GPU producer, that producer's bridge (the bridge's retirement) finish in either
// order; then the job's physical result is published. That and the job's semantic completion (the events' retirement)
// finish in either order and end the job.
//
// A job whose fence failed, or that cancel() failed, ends with kCancelled whatever its GPU producer reports.
//
// Engine supplies, each called once per job, without the engine's lock and never inside start:
//   static void fault(uint32_t slot, int status, int producers);  the fence failed: its bound outlived, or (before
//                                                                  strict) its interrupt lost; on host1x's work loop,
//                                                                  so it takes no lock and never sleeps
//   static void physical(uint32_t slot, int result);              result: 1, the GPU producer's failure or kCancelled
//   static void ended(uint32_t slot, int result);                 result: physical()'s, for the engine to join with
//                                                                  its semantic outcome
#pragma once
#include <stdint.h>
#include "SwitchHost1xFence.h"
#include "../TegraGM20B/SwitchGpuBridge.h"

struct SwitchHost1xStage {
    switch_host1x_fence fence;
    uint32_t hardware, final, boundUs;
    int physical;
    bool ended;
};

template <class Engine, uint32_t Slots> class SwitchHost1xStages {
public:
    static constexpr int kCancelled = -5;
    inline static SwitchHost1xStage stages[Slots] = {};

    // Under the engine's lock, once the job's slot is published and its batch committed. A job with a bridge has its
    // bound started by the bridge's completion, so a GPU producer that is slow, or killed by the GPU's recovery, fails
    // only its own consumer (D8); it cannot end before bridged() is called.
    static int start(uint32_t slot, uint32_t syncpt, uint32_t threshold, bool bridge, bool semantic, uint32_t boundUs)
    {
        auto &stage = stages[slot];
        switch_host1x_fence_init(&stage.fence, syncpt, threshold, signaled);
        stage.hardware = bridge ? 2 : 1;
        stage.final = semantic ? 2 : 1;
        stage.boundUs = boundUs;
        stage.physical = 1;
        stage.ended = false;
        return switch_host1x_fence_add(&stage.fence, bridge ? 0 : boundUs);
    }

    // After the engine's lock: the bridge's lock is held across a GPU submission's wait for channel room.
    static int notify(uint32_t slot, const switch_gpu_bridge *bridge)
    {
        return switch_gpu_bridge_notify(bridge, bridged, &stages[slot]);
    }

    static void semantic(uint32_t slot)
    {
        finalStep(stages[slot]);
    }

    // Under the engine's lock, once the engine has stopped (its channel torn down, the engine reset or powered off): a
    // job whose fence the syncpoint has not reached fails. True: the engine records the failure before it releases
    // the fence, which another engine's stream may be waiting on.
    static bool cancel(uint32_t slot, uint32_t reached)
    {
        auto &stage = stages[slot];
        if (static_cast<int32_t>(reached - stage.fence.threshold) >= 0)
            return false;
        __atomic_store_n(&stage.physical, kCancelled, __ATOMIC_RELEASE);
        return true;
    }

private:
    static uint32_t slotOf(const SwitchHost1xStage &stage)
    {
        return static_cast<uint32_t>(&stage - stages);
    }

    static void signaled(switch_host1x_fence *fence, int status)
    {
        auto &stage = *reinterpret_cast<SwitchHost1xStage *>(fence);
        if (status) {
            const int producers = __atomic_exchange_n(&stage.physical, kCancelled, __ATOMIC_ACQ_REL);
            Engine::fault(slotOf(stage), status, producers);
        }
        hardwareStep(stage);
    }

    static void bridged(void *context, int status)
    {
        auto &stage = *static_cast<SwitchHost1xStage *>(context);
        if (status < 0 && __atomic_load_n(&stage.physical, __ATOMIC_ACQUIRE) != kCancelled)
            __atomic_store_n(&stage.physical, status, __ATOMIC_RELEASE);
        switch_host1x_fence_watch(&stage.fence, stage.boundUs);
        hardwareStep(stage);
    }

    static void hardwareStep(SwitchHost1xStage &stage)
    {
        if (__atomic_sub_fetch(&stage.hardware, 1u, __ATOMIC_ACQ_REL))
            return;
        Engine::physical(slotOf(stage), stage.physical);
        finalStep(stage);
    }

    static void finalStep(SwitchHost1xStage &stage)
    {
        if (__atomic_sub_fetch(&stage.final, 1u, __ATOMIC_ACQ_REL))
            return;
        Engine::ended(slotOf(stage), stage.physical);
    }
};
