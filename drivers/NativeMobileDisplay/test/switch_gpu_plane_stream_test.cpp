// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchGpuPlaneStream.h: a GPU-rendered window B/C set is armed at its offer, the copy thread submits the bridge, and
// back under the gate the display stream takes the bridge; a stale arm or a refused stream completes through the
// bridge, counted.
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "../../TegraGM20B/SwitchGpuBacking.h"

namespace {
constexpr int kIOReturnAborted = -14, kIOReturnSuccess = 0;
using IOReturn = int;

struct switch_gpu_bridge {
    uint32_t releases = 0, notifies = 0;
    void (*completed)(void *, int) = nullptr;
};

switch_gpu_bridge theBridge;
std::vector<switch_gpu_backing_token> bridged;
int submitError, busy, roomError, notifyError;
unsigned roomWaits, completions;
int completedStatus;

int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count, switch_gpu_bridge **bridge)
{
    if (busy-- > 0)
        return -16;
    if (submitError)
        return submitError;
    bridged.assign(tokens, tokens + count);
    *bridge = &theBridge;
    return 0;
}

int switch_gpu_bridge_wait_room()
{
    ++roomWaits;
    return roomError;
}

int switch_gpu_bridge_notify(const switch_gpu_bridge *bridge, void (*completed)(void *, int), void *)
{
    auto *b = const_cast<switch_gpu_bridge *>(bridge);
    ++b->notifies;
    b->completed = completed;
    return notifyError;
}

void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    ++(*bridge)->releases;
    *bridge = nullptr;
}

void native_legacy_gpu_complete(void *, int status)
{
    ++completions;
    completedStatus = status;
}

struct SwitchCompletionFlip {
    static constexpr uint8_t kIdle = 0, kArmed = 1;
};

struct SwitchHost1xDisplayStream {
    static constexpr uint32_t kProducers = 4;
};

struct OSObject {};

struct NativeLegacyDisplay : OSObject {
    struct PlaneSet {
        switch_gpu_backing_token producers[3];
        unsigned count;
    };

    static constexpr unsigned kPlaneDependencies = 3;
    bool stats = true, copyStopping = false, armRefused = false;
    unsigned arms = 0, streams = 0;
    int streamError = 0;
    switch_gpu_bridge *streamBridge = nullptr;
    std::vector<switch_gpu_backing_token> streamed;
    uint32_t stPlaneGpuStreamed = 0, stPlaneGpuByCpu = 0, stPlaneGpuStreamRefused = 0;
    PlaneSet planeFencing = {};

    struct Flip {
        uint32_t current = 0;
        uint8_t armed = SwitchCompletionFlip::kIdle;

        uint32_t generation() const
        {
            return current;
        }

        uint8_t state() const
        {
            return armed;
        }
    } mediaFlip;

    unsigned planeDependencies(const PlaneSet &p, switch_gpu_backing_token tokens[3]) const
    {
        for (unsigned i = 0; i < p.count; ++i)
            tokens[i] = p.producers[i];
        return p.count;
    }

    bool armMediaFlip(const PlaneSet &)
    {
        ++arms;
        if (armRefused)
            return false;
        ++mediaFlip.current;
        mediaFlip.armed = SwitchCompletionFlip::kArmed;
        return true;
    }

    int submitMediaHostFlip(const switch_gpu_backing_token *producers, unsigned count, switch_gpu_bridge *bridge)
    {
        ++streams;
        if (streamError)
            return streamError;
        streamed.assign(producers, producers + count);
        streamBridge = bridge;
        return 0;
    }

    struct WorkLoop {
        IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *), OSObject *target, void *arg0,
                           void *arg1)
        {
            return action(target, arg0, arg1, nullptr, nullptr);
        }
    } loop;

    WorkLoop *getWorkLoop()
    {
        return &loop;
    }

#include "../SwitchGpuPlaneStream.h"
};

constexpr switch_gpu_backing_token kRender = {12, 1, SWITCH_GPU_ENGINE_GM20B, 3};
constexpr switch_gpu_backing_token kPromise = {13, 1, SWITCH_GPU_ENGINE_PROMISE, 0};
constexpr switch_gpu_backing_token kConvert = {0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};

class GpuPlaneStream : public testing::Test {
protected:
    NativeLegacyDisplay display;

    void SetUp() override
    {
        theBridge = {};
        bridged.clear();
        submitError = busy = roomError = notifyError = 0;
        roomWaits = completions = 0;
        completedStatus = 1;
        NativeLegacyDisplay::planeBridgeArmed = 0;
    }

    void offer(std::initializer_list<switch_gpu_backing_token> producers)
    {
        display.planeFencing = {};
        for (const auto &token : producers)
            display.planeFencing.producers[display.planeFencing.count++] = token;
        display.planeBridgeArm(display.planeFencing);
    }

    void copyThread()
    {
        switch_gpu_backing_token producers[3] = {};
        const unsigned count = display.planeDependencies(display.planeFencing, producers);
        display.planeBridgeWork(producers, count);
    }
};

TEST_F(GpuPlaneStream, AGpuSetIsArmedAndItsStreamTakesTheBridgeWithNoCpuCompletion)
{
    offer({kRender});
    EXPECT_EQ(NativeLegacyDisplay::planeBridgeArmed, 1u);
    copyThread();
    ASSERT_EQ(bridged.size(), 1u);
    EXPECT_EQ(bridged[0].value, kRender.value);
    EXPECT_EQ(display.streams, 1u);
    EXPECT_EQ(display.streamBridge, &theBridge);
    EXPECT_TRUE(display.streamed.empty());
    EXPECT_EQ(completions, 0u);
    EXPECT_EQ(theBridge.releases, 0u) << "the stream owns it";
    EXPECT_EQ(theBridge.notifies, 0u);
    EXPECT_EQ(display.stPlaneGpuStreamed, 1u);
}

// The bridge takes the GPU's producers; the stream WAITs on the bridge and on VIC's syncpoint itself.
TEST_F(GpuPlaneStream, AMixedSetSplitsItsProducersBetweenTheBridgeAndTheStream)
{
    offer({kConvert, kRender, kPromise});
    copyThread();
    ASSERT_EQ(bridged.size(), 2u);
    EXPECT_EQ(bridged[0].engine, uint32_t(SWITCH_GPU_ENGINE_GM20B));
    EXPECT_EQ(bridged[1].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
    ASSERT_EQ(display.streamed.size(), 1u);
    EXPECT_EQ(display.streamed[0].value, kConvert.value);
}

TEST_F(GpuPlaneStream, ABridgeOutOfRoomIsWaitedForOnTheCopyThread)
{
    busy = 2;
    offer({kRender});
    copyThread();
    EXPECT_EQ(roomWaits, 2u);
    EXPECT_EQ(display.streams, 1u);
}

// A gate writer of windows B and C ran while the copy thread submitted the bridge: the arm is gone, and the set completes
// through the bridge, as the bridge fence would have.
TEST_F(GpuPlaneStream, AStaleArmCompletesThroughTheBridge)
{
    offer({kRender});
    display.mediaFlip.armed = SwitchCompletionFlip::kIdle;
    copyThread();
    EXPECT_EQ(display.streams, 0u);
    EXPECT_EQ(theBridge.notifies, 1u);
    EXPECT_EQ(theBridge.completed, native_legacy_gpu_complete);
    EXPECT_EQ(theBridge.releases, 1u);
    EXPECT_EQ(completions, 0u);
    EXPECT_EQ(display.stPlaneGpuStreamRefused, 1u);
}

TEST_F(GpuPlaneStream, ARefusedStreamCompletesThroughTheBridge)
{
    display.streamError = -16;
    offer({kRender});
    copyThread();
    EXPECT_EQ(display.streams, 1u);
    EXPECT_EQ(theBridge.notifies, 1u);
    EXPECT_EQ(theBridge.releases, 1u);
    EXPECT_EQ(display.stPlaneGpuStreamRefused, 1u);
    EXPECT_EQ(display.stPlaneGpuStreamed, 0u);
}

TEST_F(GpuPlaneStream, ABridgeThatCannotBeSubmittedFailsTheSet)
{
    submitError = -5;
    offer({kRender});
    copyThread();
    EXPECT_EQ(display.streams, 0u);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, -5);
}

TEST_F(GpuPlaneStream, AStoppingDisplayLetsTheBridgeGoAndAbortsTheSet)
{
    offer({kRender});
    display.copyStopping = true;
    copyThread();
    EXPECT_EQ(display.streams, 0u);
    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completedStatus, kIOReturnAborted);
}

// A geometry change (the arm refused) keeps the bridge fence, counted as written by the CPU; a set with no GPU producer
// is not this route's.
TEST_F(GpuPlaneStream, AnUnarmedGpuSetKeepsTheBridgeFence)
{
    display.armRefused = true;
    offer({kRender});
    EXPECT_EQ(NativeLegacyDisplay::planeBridgeArmed, 0u);
    EXPECT_EQ(display.stPlaneGpuByCpu, 1u);
    display.armRefused = false;
    offer({kConvert});
    EXPECT_EQ(NativeLegacyDisplay::planeBridgeArmed, 0u);
    EXPECT_EQ(display.arms, 1u);
}
}  // namespace
