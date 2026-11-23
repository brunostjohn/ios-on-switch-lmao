// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchExternalScreen.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSNumber.h>
#include <mach/mach_time.h>
#include "SwitchDPProperties.h"
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

uint64_t periodTicks(uint64_t period)
{
    const uint64_t ns = (period >> 32) * 1000000000u + ((uint64_t(uint32_t(period)) * 1000000000u) >> 32);
    uint64_t ticks;
    nanoseconds_to_absolutetime(ns, &ticks);
    return ticks;
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
    publishScanoutLimits();
    if (!attach(nub) || !IOMobileFramebufferLegacy::start(nub))
        return false;
    registerService();
    return true;
}

namespace {
struct Limit { const char *key; uint64_t value; };
bool publishLimits(IOService *screen, const char *name, const Limit *limits, unsigned count, unsigned bits)
{
    auto *dictionary = OSDictionary::withCapacity(count);
    bool ok = dictionary != nullptr;
    for (unsigned i = 0; ok && i < count; ++i) {
        auto *number = OSNumber::withNumber(limits[i].value, bits);
        ok = number && dictionary->setObject(limits[i].key, number);
        OSSafeReleaseNULL(number);
    }
    if (ok)
        ok = screen->setProperty(name, dictionary);
    OSSafeReleaseNULL(dictionary);
    return ok;
}
}

void SwitchExternalScreen::publishScanoutLimits()
{
    const Limit stride[] = {{"StrideLinearHorizontal", 64}};
    publishLimits(this, "DisplayPipeStrideRequirements", stride, 1, 32);
    const Limit base[] = {{"PlaneBaseAlignmentLinear", 4096}};
    publishLimits(this, "DisplayPipePlaneBaseAlignment", base, 1, 64);
    const Limit layers[] = {{"MaxNumLayers", 1}};
    publishLimits(this, "IOMFBNumLayers", layers, 1, 32);
    const Limit scaling[] = {{"RGBLayer_MinScaleFraction", 1}, {"RGBLayer_MaxScale", 1},
                             {"YUVLayer_MinScaleFraction", 1}, {"YUVLayer_MaxScale", 1}};
    publishLimits(this, "IOMFBScalingLimits", scaling, 4, 32);
    setProperty("IOMFBYUVPackedUnSupported", true);
}

bool SwitchExternalScreen::publishSourceLimits(uint32_t clockHz)
{
    const Limit source[] = {{"MaxSrcRectWidth", SWITCH_DP_MAX_WIDTH},
                            {"MaxSrcRectTotal", uint64_t{SWITCH_DP_MAX_WIDTH} * SWITCH_DP_MAX_HEIGHT},
                            {"MaxSrcBufferWidth", SWITCH_DP_MAX_WIDTH}, {"MaxSrcBufferHeight", SWITCH_DP_MAX_HEIGHT},
                            {"MaxVideoSrcDownscalingWidth", SWITCH_DP_MAX_WIDTH}, {"VideoClock", clockHz},
                            {"PixelClock", clockHz}, {"IOMFBCompressionSupport", 0}};
    return publishLimits(this, "IOMFBMaxSrcPixels", source, 8, 64);
}

bool SwitchExternalScreen::publishModes(OSString *uuid)
{
    auto *timings = OSArray::withCapacity(modes.count);
    auto *preferred = OSArray::withCapacity(1);
    auto *colors = OSArray::withCapacity(1);
    auto *color = switch_dp_color_element();
    bool ok = timings && preferred && colors && color && colors->setObject(color);
    unsigned best = 0;
    for (unsigned i = 1; i < modes.count; ++i)
        if (switch_dp_prefer(&modes.modes[i], &modes.modes[best]))
            best = i;
    uint32_t clockKHz = 0;
    for (unsigned i = 0; ok && i < modes.count; ++i) {
        modes.modes[i].preferred = i == best;
        auto *timing = switch_dp_timing_element(&modes.modes[i], i + 1, colors);
        ok = timing && timings->setObject(timing);
        if (ok && i == best)
            ok = preferred->setObject(timing);
        if (clockKHz < modes.modes[i].pixel_khz)
            clockKHz = modes.modes[i].pixel_khz;
        OSSafeReleaseNULL(timing);
    }
    auto *wire = OSDictionary::withCapacity(1);
    auto *name = OSString::withCString("DP");
    ok = ok && timings->getCount() && wire && name && wire->setObject("Downstream", name);
    ok = ok && publishSourceLimits(clockKHz * 1000u) && setProperty("TimingElements", timings) &&
         setProperty("PreferredTimingElements", preferred) && setProperty("ColorElements", colors) &&
         setProperty("Transport", wire) && setProperty("IOMFBUUID", uuid);
    if (!ok)
        IOLog("SwitchDisplayPort: no publishable external display timings\n");
    OSSafeReleaseNULL(name);
    OSSafeReleaseNULL(wire);
    OSSafeReleaseNULL(color);
    OSSafeReleaseNULL(colors);
    OSSafeReleaseNULL(preferred);
    OSSafeReleaseNULL(timings);
    return ok;
}

void SwitchExternalScreen::publishActiveGeometry()
{
    uint64_t geometry = 0;
    if (connected && selectedMode >= 0 && static_cast<unsigned>(selectedMode) < modes.count) {
        const auto &selected = modes.modes[selectedMode];
        geometry = (static_cast<uint64_t>(selected.hactive) << 32) | selected.vactive;
    }
    __atomic_store_n(&activeGeometry, geometry, __ATOMIC_RELEASE);
}

void SwitchExternalScreen::drainSwapQueue()
{
    if (!gpuPending) {
        while (retire(pendingSwapRequest(), false)) {
        }
    }
}

bool SwitchExternalScreen::publish(const struct switch_dp_mode_list &list, const uint8_t *edid, unsigned bytes)
{
    selectedMode = -1;
    publishActiveGeometry();
    auto *uuid = switch_dp_edid_uuid(edid);
    modes = list;
    const bool published = uuid && publishModes(uuid) && setProperty("EDID", const_cast<uint8_t *>(edid), bytes);
    OSSafeReleaseNULL(uuid);
    if (!published) {
        connected = false;
        publishActiveGeometry();
        clearSinkProperties();
        return false;
    }
    connected = true;
    publishActiveGeometry();
    IOMobileFramebufferLegacy::hotPlugNotify(1);
    return true;
}

void SwitchExternalScreen::linkUp(const struct switch_dp_mode &timing, TegraDisplayB *controller)
{
    if (mode.hactive != timing.hactive || mode.vactive != timing.vactive) {
        for (unsigned i = 0; i < kSlots; ++i)
            if (slots[i].surface && static_cast<int>(i) != drawing)
                releaseSlot(i);
    }
    mode = timing;
    dcb = controller;
    setDisplayWidth(mode.hactive);
    setDisplayHeight(mode.vactive);
    refreshPeriod = switch_dp_refresh_period(&mode);
    interval = periodTicks(refreshPeriod);
    if (!native_legacy_set_display_refresh_properties(this))
        IOLog("SwitchDisplayPort: could not publish the selected refresh interval\n");
    struct { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; }
        power = {1, 0, 0, 1, 1, {}};
    setDisplayPowerState(&power, false);
    kickSwapQueue();
}

void SwitchExternalScreen::linkDown()
{
    if (!dcb)
        return;
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
    const bool wasConnected = connected;
    connected = false;
    selectedMode = -1;
    device = 0;
    publishActiveGeometry();
    clearSinkProperties();
    drainSwapQueue();
    if (wasConnected)
        IOMobileFramebufferLegacy::hotPlugNotify(0);
}

void SwitchExternalScreen::modeFailed()
{
    selectedMode = -1;
    publishActiveGeometry();
    drainSwapQueue();
}

void SwitchExternalScreen::clearSinkProperties()
{
    removeProperty("IOMFBDisplayRefresh");
    removeProperty("TimingElements");
    removeProperty("PreferredTimingElements");
    removeProperty("ColorElements");
    removeProperty("Transport");
    removeProperty("IOMFBUUID");
    removeProperty("EDID");
    removeProperty("IOMFBMaxSrcPixels");
}

bool SwitchExternalScreen::getRefreshAttributes(uint64_t *periods, uint32_t internal)
{
    if (!dcb)
        return IOMobileFramebufferLegacy::getRefreshAttributes(periods, internal);
    constexpr uint64_t externalLongest = (1ull << 32) / 24;
    periods[0] = refreshPeriod;
    periods[1] = interval;
    periods[2] = internal || refreshPeriod >= externalLongest ? refreshPeriod : externalLongest;
    periods[3] = periods[2] == refreshPeriod ? interval : periodTicks(externalLongest);
    periods[4] = periods[5] = 0;
    return true;
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
    return selectOutput(false, requested);
}

IOReturn SwitchExternalScreen::setDigitalOutMode(uint32_t color, uint32_t timing)
{
    if (color != 1 || !timing)
        return kIOReturnUnsupported;
    return selectOutput(true, timing - 1);
}

IOReturn SwitchExternalScreen::selectOutput(bool timing, uint32_t value)
{
    struct Selection { bool timing; uint32_t value; } selection = {timing, value};
    return loop->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<SwitchExternalScreen *>(owner);
        const auto &s = *static_cast<Selection *>(argument);
        if (s.timing && (!self->connected || s.value >= self->modes.count))
            return kIOReturnUnsupported;
        const int index = s.timing ? static_cast<int>(s.value) : self->selectedMode;
        const uint32_t device = s.timing ? self->device : s.value;
        if (device == 2 && !self->connected)
            return kIOReturnNotReady;
        const auto result = index < 0 && device == 2 ? kIOReturnSuccess : self->transport->selectMode(index, device == 2);
        if (result == kIOReturnSuccess) {
            self->selectedMode = index;
            self->device = device;
            self->publishActiveGeometry();
            if (device != 2)
                self->drainSwapQueue();
        }
        return result;
    }, this, &selection);
}

bool SwitchExternalScreen::validateSwap(const OriginalLegacySwapRequest *request)
{
    const uint64_t geometry = __atomic_load_n(&activeGeometry, __ATOMIC_ACQUIRE);
    if (!request || !geometry)
        return false;
    const uint32_t width = static_cast<uint32_t>(geometry >> 32);
    const uint32_t height = static_cast<uint32_t>(geometry);
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
    if (!surface || !surface->metaCast("IOSurface") || transform || native_legacy_surface_planes(surface) ||
        native_legacy_surface_format(surface) != kBgra || native_legacy_surface_width(surface) != width ||
        native_legacy_surface_height(surface) != height || source[0] || source[1] || destination[0] ||
        destination[1] || source[2] != static_cast<int32_t>(width) || source[3] != static_cast<int32_t>(height) ||
        destination[2] != static_cast<int32_t>(width) || destination[3] != static_cast<int32_t>(height))
        return false;
    const uint64_t stride = native_legacy_surface_stride(surface);
    auto *memory = native_legacy_surface_descriptor(surface);
    return memory && stride <= 0xffff && (stride & 63) == 0 &&
           native_legacy_surface_bounds(width, height, stride,
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
    if (!ready && switch_gpu_ready_observe(readyQuery, [](void *, int) {}, this, &ready))
        dropFrame(pendingSwapRequest());
}

int SwitchExternalScreen::readyQuery(void *owner)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    return self->loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *screen = static_cast<SwitchExternalScreen *>(owner);
            const auto *request = screen->pendingSwapRequest();
            if (screen->dcb && request) {
                screen->kickSwapQueue();
                if (!screen->pending && screen->pendingSwapRequest() == request)
                    return 0;
            }
            // Disarm on the display loop. A delayed GPU callback could otherwise cancel a new attachment's watch.
            switch_gpu_ready_cancel(&screen->ready);
            return 1;
        }, self);
}

void SwitchExternalScreen::readyFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchExternalScreen *>(owner);
    self->kickSwapQueue();
}
