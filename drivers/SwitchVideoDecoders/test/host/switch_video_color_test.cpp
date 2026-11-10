// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <IOSurface/IOSurface.h>
#include <vector>
#include "drivers/SwitchVideoDecoders/AVD/SwitchVideoColor.h"

namespace {
class VideoColor : public testing::Test {
protected:
    std::vector<CFTypeRef> owned;
    CFTypeRef keep(CFTypeRef value) { if (value) owned.push_back(value); return value; }
    CMVideoFormatDescriptionRef format(int height = 64, CFDictionaryRef extensions = nullptr)
    {
        CMVideoFormatDescriptionRef result = nullptr;
        EXPECT_EQ(CMVideoFormatDescriptionCreate(nullptr, kCMVideoCodecType_H264, 64, height, extensions, &result), noErr);
        keep(result);
        return result;
    }
    void TearDown() override { for (auto value : owned) CFRelease(value); }
};

TEST_F(VideoColor, UnspecifiedVuiDoesNotInferExplicitColorFromDimensions)
{
    for (int height : {64, 720, 2160}) {
        auto f = format(height);
        EXPECT_EQ(switch_video_color_tag(f, kCMFormatDescriptionExtension_YCbCrMatrix, 2, 0), nullptr);
        EXPECT_EQ(switch_video_color_tag(f, kCMFormatDescriptionExtension_ColorPrimaries, 2, 1), nullptr);
        EXPECT_EQ(switch_video_color_tag(f, kCMFormatDescriptionExtension_TransferFunction, 2, 2), nullptr);
    }
}

TEST_F(VideoColor, ExplicitFormatTagWinsOverSpsColor)
{
    const void *keys[] = {kCMFormatDescriptionExtension_YCbCrMatrix};
    const void *values[] = {kCVImageBufferYCbCrMatrix_ITU_R_2020};
    auto extensions = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    keep(extensions);
    auto tag = keep(switch_video_color_tag(format(64, extensions), kCMFormatDescriptionExtension_YCbCrMatrix, 1, 0));
    ASSERT_NE(tag, nullptr);
    EXPECT_TRUE(CFEqual(tag, kCVImageBufferYCbCrMatrix_ITU_R_2020));
}

TEST_F(VideoColor, UnsupportedExplicitCodesRemainExplicit)
{
    auto f = format();
    auto matrix = keep(switch_video_color_tag(f, kCMFormatDescriptionExtension_YCbCrMatrix, 7, 0));
    auto identity = keep(switch_video_color_tag(f, kCMFormatDescriptionExtension_YCbCrMatrix, 0, 0));
    auto transfer = keep(switch_video_color_tag(f, kCMFormatDescriptionExtension_TransferFunction, 16, 2));
    ASSERT_NE(matrix, nullptr); ASSERT_NE(identity, nullptr); ASSERT_NE(transfer, nullptr);
    EXPECT_TRUE(CFEqual(matrix, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995));
    EXPECT_TRUE(CFEqual(identity, CFSTR("YCbCrMatrix#0")));
    EXPECT_TRUE(CFEqual(transfer, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ));
    auto standard = keep(switch_video_color_tag(f, kCMFormatDescriptionExtension_YCbCrMatrix, 5, 0));
    EXPECT_TRUE(CFEqual(standard, kCVImageBufferYCbCrMatrix_ITU_R_601_4));
}

TEST_F(VideoColor, ChromaLocationPreservesEverySpsSiteAndUnspecifiedState)
{
    auto f = format();
    EXPECT_EQ(switch_video_chroma_tag(f, kCMFormatDescriptionExtension_ChromaLocationTopField, false, 0), nullptr);
    CFStringRef expected[] = {kCVImageBufferChromaLocation_Left, kCVImageBufferChromaLocation_Center,
        kCVImageBufferChromaLocation_TopLeft, kCVImageBufferChromaLocation_Top,
        kCVImageBufferChromaLocation_BottomLeft, kCVImageBufferChromaLocation_Bottom};
    for (unsigned i = 0; i < 6; ++i) {
        auto tag = keep(switch_video_chroma_tag(f, kCMFormatDescriptionExtension_ChromaLocationTopField, true, i));
        EXPECT_TRUE(CFEqual(tag, expected[i]));
    }
}

TEST_F(VideoColor, CoreVideoPublishesTheExactIOSurfaceMetadataKeys)
{
    auto empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    keep(empty);
    const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {empty};
    auto attrs = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    keep(attrs);
    CVPixelBufferRef image = nullptr;
    ASSERT_EQ(CVPixelBufferCreate(nullptr, 32, 32, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &image), kCVReturnSuccess);
    keep(image);
    CFStringRef cv[] = {kCVImageBufferYCbCrMatrixKey, kCVImageBufferTransferFunctionKey, kCVImageBufferColorPrimariesKey,
                       kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocationBottomFieldKey};
    CFStringRef surface[] = {CFSTR("IOSurfaceYCbCrMatrix"), CFSTR("IOSurfaceTransferFunction"), CFSTR("IOSurfaceColorPrimaries"),
                            CFSTR("IOSurfaceChromaLocationTopField"), CFSTR("IOSurfaceChromaLocationBottomField")};
    CFStringRef tags[] = {kCVImageBufferYCbCrMatrix_ITU_R_2020, kCVImageBufferTransferFunction_ITU_R_2100_HLG,
                         kCVImageBufferColorPrimaries_ITU_R_2020, kCVImageBufferChromaLocation_Left, kCVImageBufferChromaLocation_TopLeft};
    auto io = CVPixelBufferGetIOSurface(image);
    ASSERT_NE(io, nullptr);
    for (unsigned i = 0; i < 5; ++i) {
        CVBufferSetAttachment(image, cv[i], tags[i], kCVAttachmentMode_ShouldPropagate);
        auto value = keep(IOSurfaceCopyValue(io, surface[i]));
        ASSERT_NE(value, nullptr);
        EXPECT_TRUE(CFEqual(value, tags[i]));
    }
}
}
