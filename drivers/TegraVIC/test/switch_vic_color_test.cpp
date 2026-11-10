// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include "../TegraVICRequest.h"
#include "../SwitchVICPicturePlan.h"

namespace {
TegraVICSurfaceDesc surface(bool yuv, uint32_t width = 1920, uint32_t height = 1080)
{
    TegraVICSurfaceDesc d = {};
    d.fourcc = yuv ? TEGRA_VIC_FOURCC('4', '2', '0', 'v') : TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    d.width = width; d.height = height; d.planes = yuv ? 2 : 0;
    d.planeBytesPerRow[0] = ((width * (yuv ? 1 : 4) + 255) & ~255u);
    d.planeHeight[0] = height;
    if (yuv) {
        d.planeBytesPerRow[1] = d.planeBytesPerRow[0];
        d.planeOffset[1] = d.planeBytesPerRow[0] * height;
        d.planeHeight[1] = height / 2;
    }
    d.length = tegra_vic_surface_extent(&d);
    return d;
}

TEST(SwitchVICColor, ExplicitMatrixOverridesResolution)
{
    for (uint32_t width : {256u, 1920u}) {
        auto d = surface(true, width, 128);
        for (const char *tag : {"ITU_R_601_4", "ITU_R_709_2", "ITU_R_2020"}) {
            d.colorMatrix = switch_vic_matrix_tag(tag);
            TegraVICImage image = {};
            ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
            EXPECT_EQ(image.standard, d.colorMatrix - 1);
        }
    }
}

TEST(SwitchVICColor, UntaggedDefaultDoesNotInventHDMetadata)
{
    for (uint32_t width : {256u, 1920u}) {
        auto d = surface(true, width, 128);
        d.colorMatrix = switch_vic_matrix_tag(nullptr);
        TegraVICImage image = {};
        ASSERT_EQ(tegra_vic_image_from(&d, 0x10000000, &image), nullptr);
        EXPECT_EQ(image.standard, TEGRA_VIC_YCC_601);
        EXPECT_EQ(d.colorMatrix, SWITCH_VIC_MATRIX_UNTAGGED);
    }
}

TEST(SwitchVICColor, TaggedMatricesAndRangesReachPackedConfiguration)
{
    for (uint32_t matrix : {SWITCH_VIC_MATRIX_601, SWITCH_VIC_MATRIX_709}) {
        for (bool full : {false, true}) {
            for (bool fromYuv : {false, true}) {
                auto yuv = surface(true), rgb = surface(false);
                yuv.colorMatrix = matrix;
                if (full)
                    yuv.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'f');
                TegraVICImage src = {}, dst = {};
                ASSERT_EQ(tegra_vic_image_from(fromYuv ? &yuv : &rgb, 0x10000000, &src), nullptr);
                ASSERT_EQ(tegra_vic_image_from(fromYuv ? &rgb : &yuv, 0x20000000, &dst), nullptr);
                TegraVICOp op = {};
                op.srcW = uint64_t(yuv.width) << 16; op.srcH = uint64_t(yuv.height) << 16;
                op.dstW = yuv.width; op.dstH = yuv.height;
                TegraVICJob job = {};
                ASSERT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
                TegraVICMatrix expected = {};
                if (fromYuv)
                    tegra_vic_matrix_ycc_to_rgb(matrix - 1, full, &kTegraVICTraits.in, &expected);
                else
                    tegra_vic_matrix_rgb_to_ycc(matrix - 1, full, &kTegraVICTraits.out, &expected);
                uint8_t packed[32] = {};
                tegra_vic_matrix_put(packed, 0, &expected);
                const auto offset = fromYuv ? TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX : TEGRA_VIC_OUT_MATRIX;
                EXPECT_EQ(memcmp(job.config + offset, packed, sizeof(packed)), 0);
            }
        }
    }
}

TEST(SwitchVICColor, UnsupportedConversionRefusesBut2020ByteCopyRemainsValid)
{
    for (bool full : {false, true}) {
        auto yuv = surface(true), rgb = surface(false);
        yuv.colorMatrix = SWITCH_VIC_MATRIX_2020;
        if (full)
            yuv.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'f');
        TegraVICImage src = {}, dst = {};
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x10000000, &src), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x20000000, &dst), nullptr);
        TegraVICOp op = {};
        op.srcW = uint64_t(yuv.width) << 16; op.srcH = uint64_t(yuv.height) << 16;
        op.dstW = yuv.width; op.dstH = yuv.height;
        TegraVICJob job = {};
        EXPECT_EQ(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&rgb, 0x20000000, &dst), nullptr);
        EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&rgb, 0x10000000, &src), nullptr);
        ASSERT_EQ(tegra_vic_image_from(&yuv, 0x20000000, &dst), nullptr);
        EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        op.fill = true;
        EXPECT_NE(tegra_vic_build(&src, &dst, &op, &kTegraVICTraits, 0x30000000, 0x30001000, &job), nullptr);
        yuv.colorMatrix = switch_vic_matrix_tag("SMPTE_240M_1995");
        EXPECT_NE(tegra_vic_image_from(&yuv, 0x10000000, &src), nullptr);
    }
}

TEST(SwitchVICColor, P010ByteViewPreserves2020Tag)
{
    auto d = surface(true);
    d.fourcc = TEGRA_VIC_FOURCC('x', '4', '2', '0');
    d.colorMatrix = SWITCH_VIC_MATRIX_2020;
    TegraVICPicture picture = {}, bytes = {};
    picture.sampleBytes = 2; picture.width = d.width;
    TegraVICSurfaceDesc desc = {};
    ASSERT_EQ(switch_vic_picture_bytes(&picture, &d, &bytes, &desc), nullptr);
    EXPECT_EQ(desc.colorMatrix, SWITCH_VIC_MATRIX_2020);
}
}  // namespace
