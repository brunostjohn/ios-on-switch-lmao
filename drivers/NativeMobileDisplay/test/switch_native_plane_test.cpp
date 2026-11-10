// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../SwitchNativePlane.h"

namespace {
class SwitchNativePlaneTest : public testing::Test {
protected:
    native_planes::Layer layer;
    switch_gpu_backing_layout layout;
    native_planes::Image image;
    void SetUp() override
    {
        using namespace native_planes;
        layer = {};
        layer.kind = kKindNv12Video;
        layer.surface.format = fourcc('4', '2', '0', 'v');
        layer.surface.width = 1280;
        layer.surface.height = 720;
        layer.surface.planes = 2;
        layer.surface.stride[0] = layer.surface.stride[1] = 1280;
        layer.surface.planeHeight[0] = 720;
        layer.surface.planeHeight[1] = 360;
        layer.surface.offset[1] = 1280 * 720;
        layer.src = {0, 0, 1280, 720};
        layer.dst = {0, 437, 720, 405};
        layout = {};
        layout.format = layer.surface.format;
        layout.plane_count = 2;
        layout.color_matrix = 2;
        layout.color_range = 1;
        layout.planes[0] = {0, 1536 * 736, 1536, 736, 1536, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1536 * 736, 1536 * 368, 768, 368, 1536, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    }
    bool admit(uint32_t cropX = 0, uint32_t cropY = 0)
    { return switch_native_plane_image(layer, 720, 1280, layout, cropX, cropY, image); }
};
TEST_F(SwitchNativePlaneTest, NativePlanesUseTheirPaddedOffsetsAndPitch)
{
    ASSERT_TRUE(admit());
    EXPECT_EQ(image.start[0], 0u);
    EXPECT_EQ(image.start[1], 1536u * 736);
    EXPECT_EQ(image.lineStride, (1536u << 16) | 1536u);
    EXPECT_EQ(image.surfaceKind, 0x12u);
    EXPECT_EQ(image.readLow, 0u);
    EXPECT_EQ(image.readHigh, (1536u * (736 + 368) + 4095u) & ~4095u);
    EXPECT_EQ(image.matrix, native_planes::kBt709);
    EXPECT_EQ(image.position, 437u << 16);
    EXPECT_EQ(image.size, (405u << 16) | 720u);
}
TEST_F(SwitchNativePlaneTest, NativeFullRangeAnd2020MetadataRemainExplicit)
{
    layer.kind = native_planes::kKindNv12Full;
    layout.format = layer.surface.format = native_planes::fourcc('4', '2', '0', 'f');
    layout.color_range = 2;
    layout.color_matrix = 3;
    ASSERT_TRUE(admit());
    EXPECT_TRUE(image.fullRange);
    EXPECT_EQ(image.matrix, native_planes::kBt2020);
}
TEST_F(SwitchNativePlaneTest, CroppedOrClippedNativeImagesRequireMaterialization)
{
    EXPECT_FALSE(admit(2, 0));
    EXPECT_FALSE(admit(0, 2));
    layer.src.x = 2; layer.src.w -= 2;
    EXPECT_FALSE(admit());
    layer.src.x = 0; layer.src.w += 2;
    layer.dst.x = -2;
    EXPECT_FALSE(admit());
    layer.dst.x = 1;
    EXPECT_FALSE(admit());
}
TEST_F(SwitchNativePlaneTest, PlaneOverlapAndIncompleteBlocksAreRefused)
{
    layout.planes[1].offset -= 256;
    EXPECT_FALSE(admit());
    layout.planes[1].offset += 256;
    --layout.planes[1].height;
    EXPECT_FALSE(admit());
    ++layout.planes[1].height;
    --layout.planes[1].bytes;
    EXPECT_FALSE(admit());
}
TEST_F(SwitchNativePlaneTest, WrongSampleSizeRangeMatrixAndBlockHeightAreRefused)
{
    layout.planes[1].bytes_per_element = 1;
    EXPECT_FALSE(admit());
    layout.planes[1].bytes_per_element = 2;
    layout.color_range = 2;
    EXPECT_FALSE(admit());
    layout.color_range = 1;
    layout.color_matrix = 4;
    EXPECT_FALSE(admit());
    layout.color_matrix = 1;
    layout.planes[1].block_height_log2 = 2;
    EXPECT_FALSE(admit());
}
TEST_F(SwitchNativePlaneTest, OverflowCannotReduceRequiredMappedExtent)
{
    layout.planes[1].bytes = UINT64_MAX;
    EXPECT_FALSE(admit());
    layout.planes[1].bytes = 1536 * 368;
    layout.planes[1].offset = UINT64_MAX & ~255ULL;
    EXPECT_FALSE(admit());
}
}  // namespace
