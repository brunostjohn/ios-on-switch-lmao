// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchExternalScreen.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <mach/mach_time.h>
#include "SwitchDPAppleTiming.h"
#include "SwitchDisplayPortService.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"
#include "../NativeMobileDisplay/SwitchSurfaceUse.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
extern "C" int tegra_gm20b_surface_layout(uint64_t, struct tgm_layout *);

OSDefineMetaClassAndStructors(SwitchExternalScreen, IOMobileFramebufferLegacy);

namespace {
constexpr uint32_t kBgra = 0x42475241;

OSDictionary *element(uint32_t type, uint64_t id, const void *bytes, unsigned size)
{
    auto *dictionary = OSDictionary::withCapacity(5);
    auto *data = OSData::withBytes(bytes, size);
    if (!dictionary || !data) {
        OSSafeReleaseNULL(dictionary);
        OSSafeReleaseNULL(data);
        return nullptr;
    }
    auto *number = OSNumber::withNumber(id, 64);
    bool ok = number && dictionary->setObject("ID", number);
    OSSafeReleaseNULL(number);
    number = OSNumber::withNumber(type, 32);
    ok = ok && number && dictionary->setObject("ElementType", number) && dictionary->setObject("ElementData", data) &&
         dictionary->setObject("IsVirtual", kOSBooleanFalse);
    OSSafeReleaseNULL(number);
    data->release();
    if (!ok)
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}
}

bool SwitchExternalScreen::begin(SwitchDisplayPort *port, IOWorkLoop *workLoop)
{
    transport = port;
    loop = workLoop;
    completion = IOInterruptEventSource::interruptEventSource(this, &completionFired);
    readySource = IOInterruptEventSource::interruptEventSource(this, &readyFired);
    if (!completion || !readySource || loop->addEventSource(completion) != kIOReturnSuccess ||
        loop->addEventSource(readySource) != kIOReturnSuccess)
        return false;
    auto *node = IORegistryEntry::fromPath("/arm-io/display-external", gIODTPlane);
    auto *nub = OSTypeAlloc(IOPlatformDevice);
    auto *platform = IOService::getPlatform();
    const bool ready = node && nub && platform && nub->init(node, gIODTPlane) && nub->attach(platform) &&
                       nub->start(platform);
    OSSafeReleaseNULL(node);
    if (!ready) {
        OSSafeReleaseNULL(nub);
        return false;
    }
    setName("SwitchExternalDisplay");
    setProperty("external", kOSBooleanTrue);
    setProperty("IOSurfaceRowBytesAlignment", 64, 32);
    if (!attach(nub) || !IOMobileFramebufferLegacy::start(nub))
        return false;
    registerService();
    return true;
}

void SwitchExternalScreen::publishModes()
{
    auto *timings = OSArray::withCapacity(modes.count);
    auto *preferred = OSArray::withCapacity(1);
    auto *colors = OSArray::withCapacity(1);
    const auto colorData = switch_dp_apple_rgb8();
    auto *color = element(1, 1, &colorData, sizeof(colorData));
    bool ok = timings && preferred && colors && color && colors->setObject(color);
    for (unsigned i = 0; ok && i < modes.count; ++i) {
        if (!switch_dp_mode_allowed(&modes.modes[i], available))
            continue;
        const auto timingData = switch_dp_apple_timing(&modes.modes[i]);
        auto *timing = element(0, i + 1, &timingData, sizeof(timingData));
        ok = timing && timing->setObject("ColorModes", colors) &&
             timing->setObject("IsPreferred", modes.modes[i].preferred ? kOSBooleanTrue : kOSBooleanFalse) &&
             timings->setObject(timing);
        if (ok && modes.modes[i].preferred)
            ok = preferred->setObject(timing);
        OSSafeReleaseNULL(timing);
    }
    auto *wire = OSDictionary::withCapacity(1);
    auto *name = OSString::withCString("DP");
    ok = ok && timings->getCount() && wire && name && wire->setObject("Downstream", name);
    if (ok) {
        setProperty("TimingElements", timings);
        setProperty("PreferredTimingElements", preferred);
        setProperty("ColorElements", colors);
        setProperty("Transport", wire);
    } else {
        IOLog("SwitchDisplayPort: no publishable external display timings\n");
    }
    OSSafeReleaseNULL(name);
    OSSafeReleaseNULL(wire);
    OSSafeReleaseNULL(color);
    OSSafeReleaseNULL(colors);
    OSSafeReleaseNULL(preferred);
    OSSafeReleaseNULL(timings);
}

void SwitchExternalScreen::publish(const struct switch_dp_mode_list &list, uint64_t payload)
{
    modes = list;
    available = payload;
    selectedMode = -1;
    connected = true;
    publishModes();
    IOMobileFramebufferLegacy::hotPlugNotify(1);
}

void SwitchExternalScreen::linkUp(const struct switch_dp_mode &timing, TegraDisplayB *controller)
{
    mode = timing;
    dcb = controller;
    setDisplayWidth(mode.hactive);
    setDisplayHeight(mode.vactive);
    const uint64_t ns = uint64_t(switch_dp_mode_htotal(&mode)) * switch_dp_mode_vtotal(&mode) * 1000000u /
                        mode.pixel_khz;
    nanoseconds_to_absolutetime(ns, &interval);
    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
        power = {1, 0, 0, 1, 1, {}};
    setDisplayPowerState(&power, false);
    kickSwapQueue();
}

void SwitchExternalScreen::linkDown()
{
    dcb = nullptr;
    ++linkGeneration;
    switch_gpu_ready_cancel(&ready);
    latched = false;
    if (front >= 0) {
        releaseRead(static_cast<unsigned>(front));
        front = -1;
    }
    if (!gpuPending) {
        if (drawing >= 0)
            releaseRead(static_cast<unsigned>(drawing));
        drawing = -1;
        retire(pending, false);
        pending = nullptr;
    }
    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
        power = {1, 0, 0, 0, 1, {}};
    setDisplayPowerState(&power, false);
}

void SwitchExternalScreen::detach()
{
    for (unsigned i = 0; i < kSlots; ++i)
        if (!gpuPending || static_cast<int>(i) != drawing)
            releaseSlot(i);
    connected = false;
    selectedMode = -1;
    device = 0;
    removeProperty("TimingElements");
    removeProperty("PreferredTimingElements");
    removeProperty("ColorElements");
    removeProperty("Transport");
    IOMobileFramebufferLegacy::hotPlugNotify(0);
}

IOReturn SwitchExternalScreen::getDotPitch(uint32_t *pitch)
{
    if (!pitch)
        return kIOReturnBadArgument;
    *pitch = 0;
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::getDigitalOutState(uint32_t *state)
{
    if (!state)
        return kIOReturnBadArgument;
    *state = connected ? 1 : 0;
    return kIOReturnSuccess;
}

IOReturn SwitchExternalScreen::setDisplayDevice(uint32_t requested)
{
    if (requested > 2)
        return kIOReturnBadArgument;
    device = requested;
    return transport->selectMode(selectedMode, requested == 2);
}

IOReturn SwitchExternalScreen::setDigitalOutMode(uint32_t color, uint32_t timing)
{
    if (color != 1 || timing == 0 || timing > modes.count ||
        !switch_dp_mode_allowed(&modes.modes[timing - 1], available))
        return kIOReturnUnsupported;
    selectedMode = static_cast<int>(timing - 1);
    return transport->selectMode(selectedMode, device == 2);
}

bool SwitchExternalScreen::validateSwap(const OriginalLegacySwapRequest *request)
{
    if (!request || selectedMode < 0 || !connected)
        return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(request);
    uint32_t updated, disabled, background;
    memcpy(&updated, bytes + swapUpdated, sizeof(updated));
    memcpy(&disabled, bytes + swapReady, sizeof(disabled));
    memcpy(&background, bytes + swapBackground, sizeof(background));
    if (native_legacy_blank_state(updated, disabled, background))
        return true;
    if (!native_legacy_single_layer_state(updated, disabled, background))
        return false;
    OSObject *surface = nullptr;
    memcpy(&surface, bytes + swapSurfaces, sizeof(surface));
    uint32_t transform = 0;
    memcpy(&transform, bytes + swapTransforms, sizeof(transform));
    int32_t source[4], destination[4];
    memcpy(source, bytes + swapSourceRects, sizeof(source));
    memcpy(destination, bytes + swapDestinationRects, sizeof(destination));
    const auto &selected = modes.modes[selectedMode];
    if (!surface || !surface->metaCast("IOSurface") || transform || native_legacy_surface_planes(surface) ||
        native_legacy_surface_format(surface) != kBgra || native_legacy_surface_width(surface) != selected.hactive ||
        native_legacy_surface_height(surface) != selected.vactive || source[0] || source[1] || destination[0] ||
        destination[1] || source[2] != selected.hactive || source[3] != selected.vactive ||
        destination[2] != selected.hactive || destination[3] != selected.vactive)
        return false;
    const uint64_t stride = native_legacy_surface_stride(surface);
    auto *memory = native_legacy_surface_descriptor(surface);
    return memory && stride <= 0xffff && (stride & 63) == 0 &&
           native_legacy_surface_bounds(selected.hactive, selected.vactive, stride,
                                         native_legacy_surface_allocation(surface), memory->getLength());
}

IOReturn SwitchExternalScreen::collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t)
{
    if (count)
        *count = 0;
    return kIOReturnUnsupported;
}

IOReturn SwitchExternalScreen::supportedFrameInfo(uint32_t, uint32_t *count, char *, uint32_t *)
{
    if (count)
        *count = 0;
    return kIOReturnUnsupported;
}

void SwitchExternalScreen::retryWhenReady()
{
    if (!ready && switch_gpu_ready_observe(readyQuery, readyDone, this, &ready))
        IOLog("SwitchDisplayPort: cannot observe a blocked external swap\n");
}

int SwitchExternalScreen::readyQuery(void *owner)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    return self->loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *screen = static_cast<SwitchExternalScreen *>(owner);
            if (!screen->dcb || !screen->pendingSwapRequest())
                return 1;
            screen->kickSwapQueue();
            return screen->pending ? 1 : 0;
        }, self);
}

void SwitchExternalScreen::readyDone(void *owner, int)
{
    static_cast<SwitchExternalScreen *>(owner)->readySource->interruptOccurred(nullptr, nullptr, 0);
}

void SwitchExternalScreen::readyFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    switch_gpu_ready_cancel(&self->ready);
    self->kickSwapQueue();
}
