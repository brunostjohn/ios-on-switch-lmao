// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchDisplayAudio.h"
#include "SwitchAudioObjects.h"
#include "SwitchAudioConvert.h"
#include "../../../kits/SwitchDMA/SwitchDcache.h"
#include <mach/mach_time.h>
#include <stddef.h>
#include <string.h>

static AudioServerPlugInHostRef gHost;
static SwitchAudioControl *gControl;
static void *gOutput;
static UInt32 gUsers;
static uint32_t gAvailable;
static uint64_t gSeed;

Boolean switch_audio_display_object(AudioObjectID object)
{
    return object == kDisplay || object == kDisplayOut;
}

void switch_audio_display_attach(AudioServerPlugInHostRef host, SwitchAudioControl *control, void *output)
{
    gHost = host;
    gOutput = output;
    gControl = control;
    __atomic_store_n(&gAvailable, control && __atomic_load_n(&control->jack, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

Boolean switch_audio_display_available(void)
{
    return __atomic_load_n(&gAvailable, __ATOMIC_ACQUIRE) != 0;
}

void switch_audio_display_changed(void)
{
    const uint32_t available = gControl && __atomic_load_n(&gControl->jack, __ATOMIC_ACQUIRE);
    if (__atomic_exchange_n(&gAvailable, available, __ATOMIC_ACQ_REL) == available || !gHost)
        return;
    const AudioObjectPropertyAddress device = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
    const AudioObjectPropertyAddress plugin[] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
    gHost->PropertiesChanged(gHost, kDisplay, 1, &device);
    gHost->PropertiesChanged(gHost, kPlugIn, 2, plugin);
}

static AudioStreamBasicDescription format(void)
{
    return (AudioStreamBasicDescription){48000,
                                         kAudioFormatLinearPCM,
                                         kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                                             kAudioFormatFlagIsPacked,
                                         8,
                                         1,
                                         8,
                                         2,
                                         32,
                                         0};
}

#define REPLY(type, value)                             \
    do {                                               \
        if (capacity < sizeof(type))                   \
            return kAudioHardwareBadPropertySizeError; \
        *(type *)data = (value);                       \
        *bytes = sizeof(type);                         \
        return 0;                                      \
    } while (0)

OSStatus switch_audio_display_get(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 capacity,
                                  UInt32 *bytes, void *data)
{
    const Boolean device = object == kDisplay, input = address->mScope == kAudioObjectPropertyScopeInput;
    const AudioObjectPropertySelector selector = address->mSelector;
    *bytes = 0;
    switch (selector) {
    case kAudioObjectPropertyBaseClass:
        REPLY(AudioClassID, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
        REPLY(AudioClassID, device ? kAudioDeviceClassID : kAudioStreamClassID);
    case kAudioObjectPropertyOwner:
        REPLY(AudioObjectID, device ? kPlugIn : kDisplay);
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyModelName:
        REPLY(CFStringRef, CFSTR("DisplayPort"));
    case kAudioObjectPropertyManufacturer:
        REPLY(CFStringRef, CFSTR("Switch"));
    case kAudioObjectPropertyCustomPropertyInfoList:
    case 'taps':
        return 0;
    case kAudioObjectPropertyOwnedObjects:
        if (!device || input)
            return 0;
        REPLY(AudioObjectID, kDisplayOut);
    default:
        break;
    }
    if (!device) {
        switch (selector) {
        case kAudioStreamPropertyIsActive:
            REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection:
            REPLY(UInt32, 0);
        case kAudioStreamPropertyTerminalType:
            REPLY(UInt32, kAudioStreamTerminalTypeDisplayPort);
        case kAudioStreamPropertyStartingChannel:
            REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency:
            REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            REPLY(AudioStreamBasicDescription, format());
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            REPLY(AudioStreamRangedDescription, ((AudioStreamRangedDescription){format(), {48000, 48000}}));
        default:
            return kAudioHardwareUnknownPropertyError;
        }
    }
    switch (selector) {
    case kAudioDevicePropertyDeviceUID:
        REPLY(CFStringRef, CFSTR(SWITCH_AUDIO_DISPLAY_UID));
    case kAudioDevicePropertyModelUID:
        REPLY(CFStringRef, CFSTR("SwitchDisplayPortAudio"));
    case kAudioObjectPropertySerialNumber:
        REPLY(CFStringRef, CFSTR("0001"));
    case kAudioObjectPropertyFirmwareVersion:
        REPLY(CFStringRef, CFSTR("1.0"));
    case kAudioDevicePropertyConfigurationApplication:
        REPLY(CFStringRef, CFSTR("com.apple.Preferences"));
    case kAudioObjectPropertyElementName:
        REPLY(CFStringRef, CFSTR(""));
    case kAudioDevicePropertyTransportType:
        REPLY(UInt32, kAudioDeviceTransportTypeDisplayPort);
    case kAudioDevicePropertyClockDomain:
        REPLY(UInt32, SWITCH_AUDIO_MAGIC + 1);
    case kAudioDevicePropertyRelatedDevices:
        REPLY(AudioObjectID, kDisplay);
    case kAudioDevicePropertyDeviceIsAlive:
        REPLY(UInt32, switch_audio_display_available());
    case kAudioDevicePropertyDeviceIsRunning:
        REPLY(UInt32, gControl && (gControl->state & SwitchAudioOutputRunning));
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        REPLY(UInt32, !input);
    case kAudioDevicePropertyClockIsStable:
        REPLY(UInt32, 1);
    case kAudioDevicePropertyClockAlgorithm:
        REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
    case kAudioDevicePropertyIsHidden:
    case kAudioObjectPropertyIdentify:
    case 'accs':
    case 'auto':
        REPLY(UInt32, 0);
    case kAudioDevicePropertyLatency:
        REPLY(UInt32, gControl ? gControl->lead_frames : 0);
    case kAudioDevicePropertySafetyOffset:
        REPLY(UInt32, gControl ? gControl->lead_frames + 2 * SWITCH_AUDIO_PERIOD_FRAMES : 0);
    case kAudioDevicePropertyZeroTimeStampPeriod:
        REPLY(UInt32, SWITCH_AUDIO_RING_FRAMES);
    case kAudioDevicePropertyNominalSampleRate:
        REPLY(Float64, 48000);
    case kAudioDevicePropertyAvailableNominalSampleRates:
        REPLY(AudioValueRange, ((AudioValueRange){48000, 48000}));
    case kAudioDevicePropertyStreams:
        if (input)
            return 0;
        REPLY(AudioObjectID, kDisplayOut);
    case kAudioObjectPropertyControlList:
        return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        if (capacity < 2 * sizeof(UInt32))
            return kAudioHardwareBadPropertySizeError;
        ((UInt32 *)data)[0] = 1;
        ((UInt32 *)data)[1] = 2;
        *bytes = 2 * sizeof(UInt32);
        return 0;
    case kAudioDevicePropertyPreferredChannelLayout: {
        const UInt32 length = (UInt32)offsetof(AudioChannelLayout, mChannelDescriptions);
        if (capacity < length)
            return kAudioHardwareBadPropertySizeError;
        memset(data, 0, length);
        ((AudioChannelLayout *)data)->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        *bytes = length;
        return 0;
    }
    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

Boolean switch_audio_display_settable(AudioObjectPropertySelector selector)
{
    return selector == kAudioStreamPropertyVirtualFormat || selector == kAudioStreamPropertyPhysicalFormat ||
           selector == kAudioStreamPropertyIsActive || selector == kAudioDevicePropertyNominalSampleRate;
}

OSStatus switch_audio_display_set(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 bytes,
                                  const void *data)
{
    if (object == kDisplay && address->mSelector == kAudioDevicePropertyNominalSampleRate)
        return bytes < sizeof(Float64)           ? kAudioHardwareBadPropertySizeError
               : *(const Float64 *)data == 48000 ? 0
                                                 : kAudioDeviceUnsupportedFormatError;
    if (object == kDisplayOut) {
        if (address->mSelector == kAudioStreamPropertyIsActive)
            return bytes >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
        if (address->mSelector == kAudioStreamPropertyVirtualFormat ||
            address->mSelector == kAudioStreamPropertyPhysicalFormat) {
            const AudioStreamBasicDescription wanted = format();
            if (bytes < sizeof(wanted))
                return kAudioDeviceUnsupportedFormatError;
            const AudioStreamBasicDescription *offered = data;
            return offered->mSampleRate == wanted.mSampleRate && offered->mFormatID == wanted.mFormatID &&
                           offered->mFormatFlags == wanted.mFormatFlags &&
                           offered->mBytesPerPacket == wanted.mBytesPerPacket &&
                           offered->mFramesPerPacket == wanted.mFramesPerPacket &&
                           offered->mBytesPerFrame == wanted.mBytesPerFrame &&
                           offered->mChannelsPerFrame == wanted.mChannelsPerFrame &&
                           offered->mBitsPerChannel == wanted.mBitsPerChannel
                       ? 0
                       : kAudioDeviceUnsupportedFormatError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

OSStatus switch_audio_display_start(io_connect_t connection)
{
    if (!switch_audio_display_available() || !gControl)
        return kAudioHardwareNotRunningError;
    if (!gOutput) {
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(connection, SwitchAudioMemoryDisplayOutput, mach_task_self(), &address, &size,
                                 kIOMapAnywhere) != kIOReturnSuccess)
            return kAudioHardwareNotRunningError;
        gOutput = (void *)(uintptr_t)address;
    }
    if (!(gControl->state & SwitchAudioOutputRunning)) {
        if (IOConnectCallScalarMethod(connection, SwitchAudioStartDisplayOutput, NULL, 0, NULL, NULL))
            return kAudioHardwareNotRunningError;
        ++gSeed;
    }
    ++gUsers;
    return 0;
}

OSStatus switch_audio_display_stop(io_connect_t connection)
{
    if (gUsers && --gUsers == 0)
        (void)IOConnectCallScalarMethod(connection, SwitchAudioStopDisplayOutput, NULL, 0, NULL, NULL);
    return 0;
}

OSStatus switch_audio_display_timestamp(Float64 *sample, UInt64 *host, UInt64 *seed)
{
    if (!gControl || !(gControl->state & SwitchAudioOutputRunning))
        return kAudioHardwareNotRunningError;
    const Float64 ticks = (Float64)gControl->ticks_per_frame_q32 / 4294967296.0;
    const uint64_t now = mach_absolute_time(), anchor = gControl->anchor_host;
    const uint64_t periods =
        now > anchor ? (uint64_t)((Float64)(now - anchor) / (ticks * SWITCH_AUDIO_RING_FRAMES)) : 0;
    *sample = (Float64)(periods * SWITCH_AUDIO_RING_FRAMES);
    *host = anchor + (uint64_t)((Float64)(periods * SWITCH_AUDIO_RING_FRAMES) * ticks);
    *seed = gSeed;
    return 0;
}

void switch_audio_display_mix(UInt32 frames, const AudioServerPlugInIOCycleInfo *info, const float *samples)
{
    if (!gControl || !gOutput || !switch_audio_display_available())
        return;
    const uint64_t first = (uint64_t)info->mOutputTime.mSampleTime;
    const uint32_t total = frames < SWITCH_AUDIO_RING_FRAMES ? frames : SWITCH_AUDIO_RING_FRAMES;
    const uint32_t at = (uint32_t)(first % SWITCH_AUDIO_RING_FRAMES);
    const uint32_t run = total < SWITCH_AUDIO_RING_FRAMES - at ? total : SWITCH_AUDIO_RING_FRAMES - at;
    const uint32_t width = gControl->sample_bits == 16 ? 2 : 4;
    if (width == 4) {
        int32_t *ring = gOutput;
        switch_audio_write_s32(ring + at * 2, samples, run * 2);
        if (run < total)
            switch_audio_write_s32(ring, samples + run * 2, (total - run) * 2);
    } else {
        int16_t *ring = gOutput;
        switch_audio_write_s16(ring + at * 2, samples, run * 2);
        if (run < total)
            switch_audio_write_s16(ring, samples + run * 2, (total - run) * 2);
    }
    switch_dcache_clean_lines((uint8_t *)gOutput + at * 2 * width, run * 2 * width);
    if (run < total)
        switch_dcache_clean_lines(gOutput, (total - run) * 2 * width);
    switch_dcache_sync();
    if (first + total > gControl->out_written)
        gControl->out_written = first + total;
}
