// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>

#include "SwitchDPModes.h"
#include "../NativeMobileDisplay/NativeLegacyRequest.h"

namespace switch_external {

constexpr unsigned kLayers = 4;
constexpr uint32_t kKnownMask = 0x8000000fu;
constexpr uint32_t kLayerBits = 0xfu;
constexpr uint32_t kBackgroundBit = 1u << 31;
constexpr uint32_t kOpaqueBlack = 0xff000000u;

constexpr uint32_t kFourccBgra = 0x42475241u;
constexpr uint32_t kFourccBgraComp = 0x26424741u;
constexpr uint32_t kFourccRgba = 0x52474241u;

constexpr uint32_t kDepthB8G8R8A8 = 12u;
constexpr uint32_t kDepthR8G8B8A8 = 13u;

enum RejectReason : uint32_t {
    kAccepted = 0,
    kRejectLayerState = 1,
    kRejectMultiLayer = 2,
    kRejectFormat = 3,
    kRejectTransform = 4,
    kRejectSourceRect = 5,
    kRejectDestRect = 6,
    kRejectScale = 7,
    kRejectStride = 8,
    kRejectBounds = 9,
};

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

struct LayerDesc {
    uint32_t format = 0;
    uint32_t planes = 0;
    uint32_t transform = 0;
    uint32_t surfW = 0;
    uint32_t surfH = 0;
    uint64_t stride = 0;
    uint64_t allocation = 0;
    uint64_t descriptorBytes = 0;
    Rect src = {};
    Rect dst = {};
};

struct SwapCheck {
    bool valid = false;
    bool blank = false;
    int activeLayer = -1;
    RejectReason reason = kAccepted;
};

struct SurfaceLayout {
    uint32_t surfW = 0;
    uint32_t surfH = 0;
    uint32_t stride = 0;
    uint32_t colorDepth = kDepthB8G8R8A8;
    uint64_t mappedBytes = 0;
    bool blockLinear = false;
    uint8_t yLog2 = 0;
    uint32_t rowBytes = 0;
    uint64_t blockBytes = 0;
};

struct ExternalFrame {
    bool valid = false;
    bool visible = false;
    uint32_t startOffset = 0;
    uint32_t stride = 0;
    uint32_t kind = 0;
    uint32_t colorDepth = kDepthB8G8R8A8;
    uint32_t position = 0;
    uint32_t size = 0;
    uint32_t prescaled = 0;
    uint32_t hOffset = 0;
    uint32_t vOffset = 0;
};

inline uint32_t colorDepthOf(uint32_t format, uint32_t planes)
{
    if ((format == kFourccBgra && planes == 0u) || (format == kFourccBgraComp && planes <= 1u))
        return kDepthB8G8R8A8;
    if (format == kFourccRgba && planes == 0u)
        return kDepthR8G8B8A8;
    return 0u;
}

inline RejectReason validateLayer(const LayerDesc &layer)
{
    if (colorDepthOf(layer.format, layer.planes) == 0u)
        return kRejectFormat;
    if (layer.transform != 0u)
        return kRejectTransform;
    if (layer.surfW == 0u || layer.surfH == 0u || layer.surfW > SWITCH_DP_MAX_WIDTH ||
        layer.surfH > SWITCH_DP_MAX_HEIGHT)
        return kRejectSourceRect;
    if (layer.src.x < 0 || layer.src.y < 0 || layer.src.w <= 0 || layer.src.h <= 0 ||
        static_cast<uint64_t>(layer.src.x) + static_cast<uint64_t>(layer.src.w) > layer.surfW ||
        static_cast<uint64_t>(layer.src.y) + static_cast<uint64_t>(layer.src.h) > layer.surfH)
        return kRejectSourceRect;
    if (layer.dst.w <= 0 || layer.dst.h <= 0 || layer.dst.x < -8191 || layer.dst.y < -8191 ||
        static_cast<int64_t>(layer.dst.x) + layer.dst.w > 8191 ||
        static_cast<int64_t>(layer.dst.y) + layer.dst.h > 8191)
        return kRejectDestRect;
    if (layer.dst.w != layer.src.w || layer.dst.h != layer.src.h)
        return kRejectScale;
    if (layer.stride > 0xffffu || (layer.stride & 63u) != 0u)
        return kRejectStride;
    if (!native_legacy_surface_bounds(layer.surfW, layer.surfH, layer.stride, layer.allocation,
                                      layer.descriptorBytes))
        return kRejectBounds;
    return kAccepted;
}

inline SwapCheck validateSwap(uint32_t updated, uint32_t disabled, uint32_t background,
                              const bool hasSurface[kLayers], const LayerDesc layers[kLayers])
{
    SwapCheck result = {};
    if (((updated | disabled) & ~kKnownMask) != 0u || (disabled & ~updated) != 0u || updated == 0u) {
        result.reason = kRejectLayerState;
        return result;
    }
    if (background != 0u && background != kOpaqueBlack) {
        result.reason = kRejectLayerState;
        return result;
    }
    if ((updated & kBackgroundBit) != 0u && background != kOpaqueBlack) {
        result.reason = kRejectLayerState;
        return result;
    }
    int active = -1;
    unsigned count = 0;
    for (unsigned i = 0; i < kLayers; ++i) {
        if ((updated & (1u << i)) == 0u)
            continue;
        if (hasSurface[i]) {
            ++count;
            active = static_cast<int>(i);
        } else if ((disabled & (1u << i)) == 0u) {
            result.reason = kRejectLayerState;
            return result;
        }
    }
    if (count == 0u) {
        result.valid = true;
        result.blank = true;
        return result;
    }
    if (count > 1u) {
        result.reason = kRejectMultiLayer;
        return result;
    }
    result.activeLayer = active;
    result.reason = validateLayer(layers[active]);
    result.valid = result.reason == kAccepted;
    return result;
}

inline ExternalFrame switch_external_frame(const SurfaceLayout &layout, const Rect &src, const Rect &dst,
                                           uint32_t modeW, uint32_t modeH)
{
    ExternalFrame out = {};
    if (modeW == 0u || modeH == 0u ||
        (layout.colorDepth != kDepthB8G8R8A8 && layout.colorDepth != kDepthR8G8B8A8) ||
        layout.surfW == 0u || layout.surfH == 0u || layout.surfW > SWITCH_DP_MAX_WIDTH ||
        layout.surfH > SWITCH_DP_MAX_HEIGHT)
        return out;
    if (src.x < 0 || src.y < 0 || src.w <= 0 || src.h <= 0 ||
        static_cast<uint64_t>(src.x) + static_cast<uint64_t>(src.w) > layout.surfW ||
        static_cast<uint64_t>(src.y) + static_cast<uint64_t>(src.h) > layout.surfH)
        return out;
    if (dst.w <= 0 || dst.h <= 0 || dst.w != src.w || dst.h != src.h || dst.x < -8191 || dst.y < -8191 ||
        static_cast<int64_t>(dst.x) + dst.w > 8191 || static_cast<int64_t>(dst.y) + dst.h > 8191)
        return out;

    const int64_t d0x = dst.x < 0 ? 0 : static_cast<int64_t>(dst.x);
    const int64_t d0y = dst.y < 0 ? 0 : static_cast<int64_t>(dst.y);
    const int64_t d1x = static_cast<int64_t>(dst.x) + dst.w > static_cast<int64_t>(modeW)
                            ? static_cast<int64_t>(modeW)
                            : static_cast<int64_t>(dst.x) + dst.w;
    const int64_t d1y = static_cast<int64_t>(dst.y) + dst.h > static_cast<int64_t>(modeH)
                            ? static_cast<int64_t>(modeH)
                            : static_cast<int64_t>(dst.y) + dst.h;
    if (d1x <= d0x || d1y <= d0y) {
        out.valid = true;
        out.visible = false;
        return out;
    }

    const uint32_t posX = static_cast<uint32_t>(d0x);
    const uint32_t posY = static_cast<uint32_t>(d0y);
    const uint32_t outW = static_cast<uint32_t>(d1x - d0x);
    const uint32_t outH = static_cast<uint32_t>(d1y - d0y);
    const uint32_t srcX = static_cast<uint32_t>(static_cast<int64_t>(src.x) + (d0x - dst.x));
    const uint32_t srcY = static_cast<uint32_t>(static_cast<int64_t>(src.y) + (d0y - dst.y));

    out.position = (posY << 16) | posX;
    out.size = (outH << 16) | outW;
    out.prescaled = (outH << 16) | (outW * 4u);
    out.hOffset = srcX * 4u;
    out.colorDepth = layout.colorDepth;

    uint64_t startOffset = 0;
    uint64_t readEnd = 0;
    if (layout.blockLinear) {
        if (layout.yLog2 > 5u || layout.rowBytes < layout.surfW * 4u || (layout.rowBytes & 63u) != 0u ||
            layout.rowBytes > 0xffffu || layout.blockBytes > layout.mappedBytes)
            return {};
        const uint64_t blockRows = 8ULL << layout.yLog2;
        const uint64_t blockRowBytes = static_cast<uint64_t>(layout.rowBytes) * blockRows;
        const uint64_t firstBlockRow = srcY / blockRows;
        const uint64_t endBlockRow = (static_cast<uint64_t>(srcY) + outH + blockRows - 1) / blockRows;
        startOffset = firstBlockRow * blockRowBytes;
        readEnd = endBlockRow * blockRowBytes;
        out.vOffset = static_cast<uint32_t>(srcY - firstBlockRow * blockRows);
        out.stride = layout.rowBytes;
        out.kind = 2u | (static_cast<uint32_t>(layout.yLog2) << 4);
    } else {
        if (layout.stride < layout.surfW * 4u || (layout.stride & 63u) != 0u || layout.stride > 0xffffu)
            return {};
        startOffset = static_cast<uint64_t>(srcY) * layout.stride;
        readEnd = startOffset + static_cast<uint64_t>(outH - 1u) * layout.stride + out.hOffset +
                  static_cast<uint64_t>(outW) * 4u;
        out.vOffset = 0u;
        out.stride = layout.stride;
        out.kind = 0u;
    }

    if (startOffset > UINT32_MAX || readEnd > layout.mappedBytes)
        return {};
    out.startOffset = static_cast<uint32_t>(startOffset);
    out.valid = true;
    out.visible = true;
    return out;
}

}  // namespace switch_external
