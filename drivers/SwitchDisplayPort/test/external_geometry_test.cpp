// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SwitchExternalGeometry.h"

#include <gtest/gtest.h>

namespace {

using namespace switch_external;

LayerDesc makeLayer(uint32_t surfW, uint32_t surfH, uint32_t stride, Rect src, Rect dst,
                    uint32_t format = kFourccBgra, uint32_t planes = 0, uint32_t transform = 0)
{
    LayerDesc l = {};
    l.format = format;
    l.planes = planes;
    l.transform = transform;
    l.surfW = surfW;
    l.surfH = surfH;
    l.stride = stride;
    l.allocation = static_cast<uint64_t>(stride) * surfH;
    l.descriptorBytes = l.allocation;
    l.src = src;
    l.dst = dst;
    return l;
}

SurfaceLayout pitchLayout(uint32_t surfW, uint32_t surfH, uint32_t stride, uint32_t depth = kDepthB8G8R8A8)
{
    SurfaceLayout l = {};
    l.surfW = surfW;
    l.surfH = surfH;
    l.stride = stride;
    l.colorDepth = depth;
    l.mappedBytes = static_cast<uint64_t>(stride) * surfH;
    return l;
}

SurfaceLayout blockLayout(uint32_t surfW, uint32_t surfH, uint8_t yLog2, uint32_t depth = kDepthB8G8R8A8)
{
    const uint32_t rowBytes = (surfW * 4u + 63u) & ~63u;
    const uint32_t blockRows = 8u << yLog2;
    const uint32_t rows = (surfH + blockRows - 1u) / blockRows * blockRows;
    const uint64_t blockBytes = static_cast<uint64_t>(rowBytes) * rows;
    SurfaceLayout l = {};
    l.surfW = surfW;
    l.surfH = surfH;
    l.stride = rowBytes;
    l.colorDepth = depth;
    l.mappedBytes = blockBytes;
    l.blockLinear = true;
    l.yLog2 = yLog2;
    l.rowBytes = rowBytes;
    l.blockBytes = blockBytes;
    return l;
}

TEST(ExternalGeometry, FullScreen1080p)
{
    const Rect r = {0, 0, 1920, 1080};
    LayerDesc layers[kLayers] = {};
    layers[0] = makeLayer(1920, 1080, 7680, r, r);
    const bool has[kLayers] = {true, false, false, false};

    const auto check = validateSwap(0x80000001u, 0x80000000u, kOpaqueBlack, has, layers);
    ASSERT_TRUE(check.valid);
    EXPECT_FALSE(check.blank);
    EXPECT_EQ(check.activeLayer, 0);

    const auto frame = switch_external_frame(pitchLayout(1920, 1080, 7680), r, r, 1920, 1080);
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.visible);
    EXPECT_EQ(frame.startOffset, 0u);
    EXPECT_EQ(frame.stride, 7680u);
    EXPECT_EQ(frame.kind, 0u);
    EXPECT_EQ(frame.colorDepth, kDepthB8G8R8A8);
    EXPECT_EQ(frame.position, 0u);
    EXPECT_EQ(frame.size, (1080u << 16) | 1920u);
    EXPECT_EQ(frame.prescaled, (1080u << 16) | (1920u * 4u));
    EXPECT_EQ(frame.hOffset, 0u);
    EXPECT_EQ(frame.vOffset, 0u);
}

TEST(ExternalGeometry, Pillarboxed608x1080)
{
    const Rect src = {0, 0, 608, 1080};
    const Rect dst = {656, 0, 608, 1080};
    LayerDesc layers[kLayers] = {};
    layers[0] = makeLayer(608, 1080, 2432, src, dst);
    const bool has[kLayers] = {true, false, false, false};

    const auto check = validateSwap(1u, 0u, 0u, has, layers);
    ASSERT_TRUE(check.valid);
    EXPECT_EQ(check.activeLayer, 0);

    const auto frame = switch_external_frame(pitchLayout(608, 1080, 2432), src, dst, 1920, 1080);
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.visible);
    EXPECT_EQ(frame.startOffset, 0u);
    EXPECT_EQ(frame.stride, 2432u);
    EXPECT_EQ(frame.position, 656u);
    EXPECT_EQ(frame.size, (1080u << 16) | 608u);
    EXPECT_EQ(frame.prescaled, (1080u << 16) | (608u * 4u));
    EXPECT_EQ(frame.hOffset, 0u);
    EXPECT_EQ(frame.vOffset, 0u);
}

TEST(ExternalGeometry, Letterboxed1920x720)
{
    const Rect src = {0, 0, 1920, 720};
    const Rect dst = {0, 180, 1920, 720};
    const auto frame = switch_external_frame(pitchLayout(1920, 720, 7680), src, dst, 1920, 1080);
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.visible);
    EXPECT_EQ(frame.position, 180u << 16);
    EXPECT_EQ(frame.size, (720u << 16) | 1920u);
    EXPECT_EQ(frame.prescaled, (720u << 16) | (1920u * 4u));
}

TEST(ExternalGeometry, PaddedSurfaceAndSubRectSource)
{
    const Rect src = {16, 20, 608, 1000};
    const Rect dst = {656, 40, 608, 1000};
    LayerDesc layers[kLayers] = {};
    layers[0] = makeLayer(640, 1080, 2560, src, dst);
    const bool has[kLayers] = {true, false, false, false};

    const auto check = validateSwap(1u, 0u, 0u, has, layers);
    ASSERT_TRUE(check.valid);

    const auto frame = switch_external_frame(pitchLayout(640, 1080, 2560), src, dst, 1920, 1080);
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.visible);
    EXPECT_EQ(frame.startOffset, 20u * 2560u);
    EXPECT_EQ(frame.hOffset, 16u * 4u);
    EXPECT_EQ(frame.vOffset, 0u);
    EXPECT_EQ(frame.position, (40u << 16) | 656u);
    EXPECT_EQ(frame.size, (1000u << 16) | 608u);
    EXPECT_EQ(frame.prescaled, (1000u << 16) | (608u * 4u));
}

TEST(ExternalGeometry, ClippedOffScreenEdgesAndWhollyOffScreen)
{
    const Rect src = {10, 12, 600, 500};
    const Rect dst = {-100, -50, 600, 500};
    const auto clipped = switch_external_frame(pitchLayout(640, 600, 2560), src, dst, 1920, 1080);
    ASSERT_TRUE(clipped.valid);
    EXPECT_TRUE(clipped.visible);
    EXPECT_EQ(clipped.position, 0u);
    EXPECT_EQ(clipped.size, (450u << 16) | 500u);
    EXPECT_EQ(clipped.prescaled, (450u << 16) | (500u * 4u));
    EXPECT_EQ(clipped.hOffset, (10u + 100u) * 4u);
    EXPECT_EQ(clipped.startOffset, (12u + 50u) * 2560u);

    const Rect rightClipDst = {1800, 1000, 600, 500};
    const auto rightClip = switch_external_frame(pitchLayout(640, 600, 2560), src, rightClipDst, 1920, 1080);
    ASSERT_TRUE(rightClip.valid);
    EXPECT_TRUE(rightClip.visible);
    EXPECT_EQ(rightClip.position, (1000u << 16) | 1800u);
    EXPECT_EQ(rightClip.size, (80u << 16) | 120u);
    EXPECT_EQ(rightClip.hOffset, 10u * 4u);
    EXPECT_EQ(rightClip.startOffset, 12u * 2560u);

    const Rect offDst = {2000, 0, 600, 500};
    const auto hidden = switch_external_frame(pitchLayout(640, 600, 2560), src, offDst, 1920, 1080);
    ASSERT_TRUE(hidden.valid);
    EXPECT_FALSE(hidden.visible);
}

TEST(ExternalGeometry, BlockLinearOffsetsAndKind)
{
    const auto layout = blockLayout(608, 1080, 4);
    const Rect src = {12, 150, 500, 800};
    const Rect dst = {700, 140, 500, 800};
    const auto frame = switch_external_frame(layout, src, dst, 1920, 1080);
    ASSERT_TRUE(frame.valid);
    EXPECT_TRUE(frame.visible);
    EXPECT_EQ(frame.kind, 2u | (4u << 4));
    EXPECT_EQ(frame.stride, 2432u);
    EXPECT_EQ(frame.startOffset, 1u * 2432u * 128u);
    EXPECT_EQ(frame.vOffset, 22u);
    EXPECT_EQ(frame.hOffset, 12u * 4u);
    EXPECT_EQ(frame.position, (140u << 16) | 700u);
    EXPECT_EQ(frame.size, (800u << 16) | 500u);
}

TEST(ExternalGeometry, BlockLinearNon16PxClonedWidths)
{
    struct Case {
        uint32_t w, h, modeW, modeH;
    };
    const Case cases[] = {
        {405, 720, 1280, 720},
        {506, 900, 1600, 900},
        {605, 1080, 1920, 1080},
    };
    for (const auto &c : cases) {
        const auto layout = blockLayout(c.w, c.h, 4);
        EXPECT_NE((c.w * 4u) % 64u, 0u);
        EXPECT_EQ(layout.rowBytes % 64u, 0u);
        const Rect src = {0, 0, static_cast<int32_t>(c.w), static_cast<int32_t>(c.h)};
        const Rect dst = {static_cast<int32_t>((c.modeW - c.w) / 2u), 0, static_cast<int32_t>(c.w),
                          static_cast<int32_t>(c.h)};
        const auto frame = switch_external_frame(layout, src, dst, c.modeW, c.modeH);
        ASSERT_TRUE(frame.valid) << c.w << "x" << c.h;
        EXPECT_TRUE(frame.visible);
        EXPECT_EQ(frame.stride, layout.rowBytes);
        EXPECT_EQ(frame.size, (c.h << 16) | c.w);
        EXPECT_EQ(frame.prescaled, (c.h << 16) | (c.w * 4u));
        EXPECT_EQ(frame.kind, 2u | (4u << 4));
    }
}

TEST(ExternalGeometry, FormatsAndLayerIndices)
{
    EXPECT_EQ(colorDepthOf(kFourccBgra, 0), kDepthB8G8R8A8);
    EXPECT_EQ(colorDepthOf(kFourccBgraComp, 0), kDepthB8G8R8A8);
    EXPECT_EQ(colorDepthOf(kFourccBgraComp, 1), kDepthB8G8R8A8);
    EXPECT_EQ(colorDepthOf(kFourccRgba, 0), kDepthR8G8B8A8);
    EXPECT_EQ(colorDepthOf(kFourccBgra, 1), 0u);
    EXPECT_EQ(colorDepthOf(kFourccBgraComp, 2), 0u);
    EXPECT_EQ(colorDepthOf(kFourccRgba, 1), 0u);
    EXPECT_EQ(colorDepthOf(0x34323076u, 2), 0u);

    const Rect r = {0, 0, 608, 1080};
    for (unsigned idx = 0; idx < kLayers; ++idx) {
        LayerDesc layers[kLayers] = {};
        bool has[kLayers] = {};
        has[idx] = true;
        const uint32_t fmt = idx == 1 ? kFourccBgraComp : idx == 2 ? kFourccRgba : kFourccBgra;
        const uint32_t planes = idx == 1 ? 1u : 0u;
        layers[idx] = makeLayer(608, 1080, 2432, r, r, fmt, planes);
        const auto check = validateSwap(1u << idx, 0u, 0u, has, layers);
        ASSERT_TRUE(check.valid) << idx;
        EXPECT_EQ(check.activeLayer, static_cast<int>(idx));
    }
}

TEST(ExternalGeometry, BlankAndBackgroundOnlyAndDisableSwaps)
{
    LayerDesc layers[kLayers] = {};
    const bool none[kLayers] = {};

    auto check = validateSwap(1u, 1u, 0u, none, layers);
    EXPECT_TRUE(check.valid);
    EXPECT_TRUE(check.blank);

    check = validateSwap(0x80000001u, 0x80000001u, kOpaqueBlack, none, layers);
    EXPECT_TRUE(check.valid);
    EXPECT_TRUE(check.blank);

    check = validateSwap(kBackgroundBit, kBackgroundBit, kOpaqueBlack, none, layers);
    EXPECT_TRUE(check.valid);
    EXPECT_TRUE(check.blank);

    check = validateSwap(1u << 2, 1u << 2, 0u, none, layers);
    EXPECT_TRUE(check.valid);
    EXPECT_TRUE(check.blank);
}

TEST(ExternalGeometry, RejectsInvalidSwaps)
{
    const Rect r = {0, 0, 608, 1080};
    LayerDesc layers[kLayers] = {};
    layers[0] = makeLayer(608, 1080, 2432, r, r);
    layers[1] = makeLayer(608, 1080, 2432, r, r);

    const bool two[kLayers] = {true, true, false, false};
    EXPECT_EQ(validateSwap(3u, 0u, 0u, two, layers).reason, kRejectMultiLayer);

    const bool one[kLayers] = {true, false, false, false};
    layers[0] = makeLayer(608, 1080, 2432, r, r, kFourccBgra, 0, 1);
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectTransform);

    layers[0] = makeLayer(608, 1080, 2432, r, {0, 0, 1216, 1080});
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectScale);

    layers[0] = makeLayer(608, 1080, 2432, r, r, 0x34323076u, 2);
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectFormat);

    layers[0] = makeLayer(608, 1080, 2436, r, r);
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectStride);
    layers[0] = makeLayer(608, 1080, 0x10000u, r, r);
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectStride);

    layers[0] = makeLayer(608, 1080, 2432, {1, 0, 608, 1080}, r);
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectSourceRect);
    layers[0] = makeLayer(608, 1080, 2432, r, {8000, 0, 608, 1080});
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectDestRect);
    layers[0] = makeLayer(608, 1080, 2432, r, r);
    layers[0].allocation -= 4096;
    EXPECT_EQ(validateSwap(1u, 0u, 0u, one, layers).reason, kRejectBounds);

    layers[0] = makeLayer(608, 1080, 2432, r, r);
    EXPECT_EQ(validateSwap(0x10u, 0u, 0u, one, layers).reason, kRejectLayerState);
    EXPECT_EQ(validateSwap(1u, 2u, 0u, one, layers).reason, kRejectLayerState);
    EXPECT_EQ(validateSwap(1u, 0u, 0xff112233u, one, layers).reason, kRejectLayerState);
    EXPECT_EQ(validateSwap(0x80000001u, 0x80000000u, 0u, one, layers).reason, kRejectLayerState);
}

}  // namespace
