// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

static inline CFTypeRef switch_video_color_tag(CMFormatDescriptionRef format, CFStringRef key, int code, int kind)
{
    CFTypeRef tag = CMFormatDescriptionGetExtension(format, key);
    if (tag) return CFRetain(tag);
    if (code == 2) return NULL;
    CFStringRef mapped = kind == 0 ? (code == 5 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4 :
                        CVYCbCrMatrixGetStringForIntegerCodePoint(code)) :
                        kind == 1 ? CVColorPrimariesGetStringForIntegerCodePoint(code) :
                                    CVTransferFunctionGetStringForIntegerCodePoint(code);
    if (mapped) return CFRetain(mapped);
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%s#%d"),
        kind == 0 ? "YCbCrMatrix" : kind == 1 ? "ColorPrimaries" : "TransferFunction", code);
}

static inline CFTypeRef switch_video_chroma_tag(CMFormatDescriptionRef format, CFStringRef key,
                                                bool present, uint32_t code)
{
    CFTypeRef tag = CMFormatDescriptionGetExtension(format, key);
    if (tag) return CFRetain(tag);
    if (!present) return NULL;
    CFStringRef names[] = {kCVImageBufferChromaLocation_Left, kCVImageBufferChromaLocation_Center,
        kCVImageBufferChromaLocation_TopLeft, kCVImageBufferChromaLocation_Top,
        kCVImageBufferChromaLocation_BottomLeft, kCVImageBufferChromaLocation_Bottom};
    return code < 6 ? CFRetain(names[code]) :
        CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("ChromaLocation#%u"), code);
}
