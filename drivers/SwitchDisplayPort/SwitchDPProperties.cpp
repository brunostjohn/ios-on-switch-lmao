// SPDX-License-Identifier: BSD-2-Clause-Patent
#if defined(SWITCH_DP_PROPERTIES_TEST)
#include "test/switch_dp_properties_stubs.h"
#else
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#endif
#include "SwitchDPProperties.h"
#include "SwitchDPAppleTiming.h"

namespace {
bool number(OSDictionary *dictionary, const char *key, uint64_t value, unsigned bits = 32)
{
    auto *object = OSNumber::withNumber(value, bits);
    const bool ok = object && dictionary->setObject(key, object);
    OSSafeReleaseNULL(object);
    return ok;
}

OSDictionary *element(uint32_t type, uint32_t id, const void *bytes, unsigned size)
{
    auto *dictionary = OSDictionary::withCapacity(type == 0 ? 24 : 12);
    auto *data = OSData::withBytes(bytes, size);
    const bool ok = dictionary && data && number(dictionary, "ID", id, 64) && number(dictionary, "ElementType", type) &&
                    dictionary->setObject("ElementData", data) && dictionary->setObject("IsVirtual", kOSBooleanFalse);
    OSSafeReleaseNULL(data);
    if (!ok)
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}

OSDictionary *axis(const struct switch_dp_apple_axis &value)
{
    auto *dictionary = OSDictionary::withCapacity(9);
    const uint32_t rate = static_cast<uint32_t>((int64_t{value.precise_sync_rate} + 0x8000) & ~int64_t{0xffff});
    const bool ok = dictionary && number(dictionary, "Total", value.total) &&
                    number(dictionary, "Active", value.active) && number(dictionary, "SyncWidth", value.sync_width) &&
                    number(dictionary, "BackPorch", value.back_porch) &&
                    number(dictionary, "FrontPorch", value.front_porch) && number(dictionary, "SyncRate", rate) &&
                    number(dictionary, "PreciseSyncRate", static_cast<uint32_t>(value.precise_sync_rate)) &&
                    number(dictionary, "SyncPolarity", value.sync_polarity) &&
                    number(dictionary, "PixelRepetition", value.pixel_repetition);
    if (!ok)
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}
}

OSDictionary *switch_dp_timing_element(const struct switch_dp_mode *mode, uint32_t id, OSArray *colors)
{
    const auto timing = switch_dp_apple_timing(mode);
    auto *dictionary = element(0, id, &timing, sizeof(timing));
    auto *horizontal = axis(timing.horizontal);
    auto *vertical = axis(timing.vertical);
    auto *empty = OSArray::withCapacity(0);
    const bool ok =
        dictionary && horizontal && vertical && empty && dictionary->setObject("HorizontalAttributes", horizontal) &&
        dictionary->setObject("VerticalAttributes", vertical) && dictionary->setObject("ColorModes", colors) &&
        dictionary->setObject("UnsafeColorElementIDs", empty) &&
        dictionary->setObject("DSCRequiredColorElementIDs", empty) &&
        dictionary->setObject("IsInterlaced", kOSBooleanFalse) && dictionary->setObject("IsSplit", kOSBooleanFalse) &&
        dictionary->setObject("IsOverscanned", kOSBooleanFalse) &&
        dictionary->setObject("IsPreferred", mode->preferred ? kOSBooleanTrue : kOSBooleanFalse) &&
        dictionary->setObject("IsPromoted", kOSBooleanFalse) &&
        number(dictionary, "ScanInformation", timing.scan_information) &&
        number(dictionary, "ValidPixelEncodings", 1) && number(dictionary, "StandardType", 1) &&
        number(dictionary, "TimingType", 3) && number(dictionary, "AspectRatio", timing.aspect_ratio) &&
        number(dictionary, "PreciseAspectRatio", uint64_t{mode->hactive} * 65536 / mode->vactive);
    OSSafeReleaseNULL(empty);
    OSSafeReleaseNULL(vertical);
    OSSafeReleaseNULL(horizontal);
    if (!ok)
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}

OSDictionary *switch_dp_color_element()
{
    const auto color = switch_dp_apple_rgb8();
    auto *dictionary = element(1, 1, &color, sizeof(color));
    const bool ok = dictionary && number(dictionary, "Depth", color.depth) &&
                    number(dictionary, "PixelEncoding", color.pixel_encoding) &&
                    number(dictionary, "DynamicRange", color.dynamic_range) && number(dictionary, "EOTF", color.eotf) &&
                    number(dictionary, "Colorimetry", color.colorimetry) &&
                    dictionary->setObject("SupportsDSC", kOSBooleanFalse) && number(dictionary, "StandardType", 1);
    if (!ok)
        OSSafeReleaseNULL(dictionary);
    return dictionary;
}
