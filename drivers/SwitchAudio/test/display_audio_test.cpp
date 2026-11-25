// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../hal/SwitchDisplayAudio.h"
#include "../hal/SwitchAudioObjects.h"

namespace {
SwitchAudioControl *gKernelControl;
uint32_t gMaps, gStarts, gStops;
int32_t gMappedRing[2];
kern_return_t gMapResult;
}

extern "C" kern_return_t IOConnectMapMemory64(io_connect_t, uint32_t type, task_t, mach_vm_address_t *address,
                                              mach_vm_size_t *size, IOOptionBits)
{
    EXPECT_EQ(type, static_cast<uint32_t>(SwitchAudioMemoryDisplayOutput));
    ++gMaps;
    *address = reinterpret_cast<mach_vm_address_t>(gMappedRing);
    *size = sizeof(gMappedRing);
    return gMapResult;
}

extern "C" kern_return_t IOConnectCallScalarMethod(mach_port_t, uint32_t selector, const uint64_t *, uint32_t,
                                                   uint64_t *, uint32_t *)
{
    if (selector == SwitchAudioStartDisplayOutput) {
        ++gStarts;
        gKernelControl->state = SwitchAudioOutputRunning;
    } else if (selector == SwitchAudioStopDisplayOutput) {
        ++gStops;
        gKernelControl->state = 0;
    } else {
        ADD_FAILURE() << "unexpected selector " << selector;
        return kIOReturnBadArgument;
    }
    return KERN_SUCCESS;
}

namespace {
struct Notification {
    AudioObjectID object;
    AudioObjectPropertySelector selector;
};

std::vector<Notification> gNotifications;

OSStatus changed(AudioServerPlugInHostRef, AudioObjectID object, UInt32 count,
                 const AudioObjectPropertyAddress *addresses)
{
    for (UInt32 i = 0; i < count; ++i)
        gNotifications.push_back({object, addresses[i].mSelector});
    return 0;
}

AudioServerPlugInHostInterface gCallbacks = {changed, nullptr, nullptr, nullptr, nullptr};

class DisplayAudio : public testing::Test {
protected:
    SwitchAudioControl control{};

    void SetUp() override
    {
        gNotifications.clear();
        gMaps = gStarts = gStops = 0;
        gMapResult = KERN_SUCCESS;
        gKernelControl = &control;
        switch_audio_display_attach(&gCallbacks, &control, nullptr);
    }

    void TearDown() override
    {
        switch_audio_display_attach(nullptr, nullptr, nullptr);
    }

    template <class T>
    T property(AudioObjectID object, AudioObjectPropertySelector selector,
               AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        T value{};
        UInt32 bytes = 0;
        const AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
        EXPECT_EQ(switch_audio_display_get(object, &address, sizeof(value), &bytes, &value), 0);
        EXPECT_EQ(bytes, sizeof(value));
        return value;
    }
};

TEST_F(DisplayAudio, PublishesRealSinkAvailabilityAndDeviceListChanges)
{
    EXPECT_FALSE(switch_audio_display_available());
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyDeviceIsAlive), 0u);
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    EXPECT_TRUE(switch_audio_display_available());
    ASSERT_EQ(gNotifications.size(), 3u);
    EXPECT_EQ(gNotifications[0].object, kDisplay);
    EXPECT_EQ(gNotifications[0].selector, kAudioDevicePropertyDeviceIsAlive);
    EXPECT_EQ(gNotifications[1].object, kPlugIn);
    EXPECT_EQ(gNotifications[1].selector, kAudioPlugInPropertyDeviceList);
    EXPECT_EQ(gNotifications[2].selector, kAudioObjectPropertyOwnedObjects);
    switch_audio_display_changed();
    EXPECT_EQ(gNotifications.size(), 3u);
    control.jack = 0;
    switch_audio_display_changed();
    EXPECT_FALSE(switch_audio_display_available());
    EXPECT_EQ(gNotifications.size(), 6u);
    EXPECT_EQ(switch_audio_display_start(0), kAudioHardwareNotRunningError);
}

TEST_F(DisplayAudio, HasDisplayPortTransportAndAnIndependent48kStereoClock)
{
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyTransportType), kAudioDeviceTransportTypeDisplayPort);
    EXPECT_EQ(property<UInt32>(kDisplayOut, kAudioStreamPropertyTerminalType), kAudioStreamTerminalTypeDisplayPort);
    EXPECT_NE(property<UInt32>(kDisplay, kAudioDevicePropertyClockDomain), SWITCH_AUDIO_MAGIC);
    const auto format = property<AudioStreamBasicDescription>(kDisplayOut, kAudioStreamPropertyPhysicalFormat);
    EXPECT_EQ(format.mSampleRate, 48000);
    EXPECT_EQ(format.mChannelsPerFrame, 2u);
    EXPECT_EQ(format.mFormatID, kAudioFormatLinearPCM);
    EXPECT_EQ(format.mBitsPerChannel, 32u);
    EXPECT_NE(format.mFormatFlags & kAudioFormatFlagIsFloat, 0u);
}

TEST_F(DisplayAudio, RejectsUnsupportedRatesAndHasNoInputStream)
{
    const Float64 unsupported = 96000;
    const AudioObjectPropertyAddress rate = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    EXPECT_EQ(switch_audio_display_set(kDisplay, &rate, sizeof(unsupported), &unsupported),
              kAudioDeviceUnsupportedFormatError);
    UInt32 bytes = 100, value = 0;
    const AudioObjectPropertyAddress input = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                                              kAudioObjectPropertyElementMain};
    EXPECT_EQ(switch_audio_display_get(kDisplay, &input, sizeof(value), &bytes, &value), 0);
    EXPECT_EQ(bytes, 0u);
    EXPECT_EQ(property<AudioObjectID>(kDisplay, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput),
              static_cast<AudioObjectID>(kDisplayOut));
}

TEST_F(DisplayAudio, PlaybackClearingPeriodDoesNotIncreaseTheAudioServerWriteLead)
{
    control.period_frames = 16384;
    control.lead_frames = 960;
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertyLatency), 960u);
    EXPECT_EQ(property<UInt32>(kDisplay, kAudioDevicePropertySafetyOffset), 1920u);
}

TEST_F(DisplayAudio, MapsPlaybackOnlyWhenIOStartsAndReusesItAcrossStarts)
{
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    EXPECT_EQ(gMaps, 0u);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 1u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
    EXPECT_EQ(gStops, 0u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
    EXPECT_EQ(gStops, 1u);
    EXPECT_EQ(switch_audio_display_start(1), 0);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 2u);
    EXPECT_EQ(switch_audio_display_stop(1), 0);
}

TEST_F(DisplayAudio, MappingFailureDoesNotStartDMA)
{
    control.jack = SwitchAudioJackHeadphones;
    switch_audio_display_changed();
    gMapResult = kIOReturnNoMemory;
    EXPECT_EQ(switch_audio_display_start(1), kAudioHardwareNotRunningError);
    EXPECT_EQ(gMaps, 1u);
    EXPECT_EQ(gStarts, 0u);
    EXPECT_EQ(control.state, 0u);
}
}
