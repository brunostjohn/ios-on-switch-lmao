// SPDX-License-Identifier: BSD-2-Clause-Patent
// How a picture reaches NVENC's input buffer. NVENC 5.0 reads 16x16 tiles or block linear, never the pitch rows an
// IOSurface holds (TegraNVENC/nvidia/SOURCE), so with TEGRA_NVENC_FEATURE_SURFACE_INPUT the session's input buffer is
// BL16 (block_height 16, a code the firmware's table names outright, 0x8643-0x8664) and every submit names the
// picture's IOSurface: the kernel has VIC write the region into the buffer and NVENC's job waits for VIC's on the
// hardware. The CPU touches no sample. The CPU copy (16-byte runs into the buffer's layout) is left for what VIC cannot
// read: a picture without an IOSurface, rows off the grid VIC reads (VideoToolbox is asked for buffers on it; a
// client's own pool may not be), a surface the kernel cannot take now (that picture), or one it refuses outright (the
// rest of the session). One line each.
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOReturn.h>
#include "AVEEncoder.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

enum {
    kVICPlaneAlignment = 256,  // VIC addresses planes in 256-byte units
};

uint32_t ave_input_layout(const Encoder *e)
{
    return e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT ? TEGRA_NVENC_LAYOUT_BL16 : e->info.input_layout;
}

static void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, number);
    CFRelease(number);
}

CFDictionaryRef ave_copy_input_attributes(const Encoder *e, int32_t width, int32_t height)
{
    const int32_t formats[] = {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                               kCVPixelFormatType_420YpCbCr8BiPlanarFullRange};
    CFNumberRef numbers[2];
    for (int i = 0; i < 2; i++)
        numbers[i] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[i]);
    CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, (const void **)numbers, 2, &kCFTypeArrayCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, list);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    if (width && height) {
        setNumber(attributes, kCVPixelBufferWidthKey, width);
        setNumber(attributes, kCVPixelBufferHeightKey, height);
    }
    if (e->info.features & TEGRA_NVENC_FEATURE_SURFACE_INPUT) {
        setNumber(attributes, kCVPixelBufferBytesPerRowAlignmentKey, (int32_t)e->info.surface_row_align);
        setNumber(attributes, kCVPixelBufferPlaneAlignmentKey, kVICPlaneAlignment);
    }
    CFRelease(surface);
    CFRelease(list);
    for (int i = 0; i < 2; i++)
        CFRelease(numbers[i]);
    return attributes;
}

static uint32_t codedWidth(const Encoder *e)
{
    return e->codec == kCMVideoCodecType_HEVC ? hevcenc_coded_width(&e->hevc) : h264enc_width_mbs(&e->config) * 16;
}

static uint32_t codedHeight(const Encoder *e)
{
    return e->codec == kCMVideoCodecType_HEVC ? hevcenc_coded_height(&e->hevc) : h264enc_height_mbs(&e->config) * 16;
}

// Why VIC cannot read the picture, or NULL: rows off the grid the kernel states, which VideoToolbox's own buffers keep
// to and a client's own pool may not. A plane off the grid is the kernel's to refuse (tegra_vic_build).
static const char *unreadable(const Encoder *e, CVPixelBufferRef image)
{
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(image);
    if (!surface)
        return "a picture without an IOSurface";
    for (size_t p = 0; p < 2; p++) {
        if (!e->info.surface_row_align || IOSurfaceGetBytesPerRowOfPlane(surface, p) % e->info.surface_row_align)
            return "rows VIC does not read as they are";
    }
    return NULL;
}

static bool copyRegion(Encoder *e, CVPixelBufferRef image, const AVERegion *r, TegraNVENCSubmit *submit)
{
    if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly))
        return false;
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    const uint8_t *luma = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 0) + r->y * lumaStride + r->x;
    const uint8_t *chroma =
        (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, 1) + r->y / 2 * chromaStride + r->x;
    h264enc_put_region(e->config.input_layout, &e->layout.input, e->inputVA, luma, lumaStride, chroma, chromaStride,
                       r->width, r->height, codedWidth(e), codedHeight(e));
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    submit->source = (TegraNVENCSource){0};
    return true;
}

// The fill functions' first clean range is the whole input buffer, for the copy; VIC's input is no CPU write.
static void dropInputClean(const Encoder *e, TegraNVENCSubmit *submit)
{
    if (submit->clean_count && submit->clean[0].handle == e->handles.input) {
        submit->clean_count--;
        memmove(&submit->clean[0], &submit->clean[1], submit->clean_count * sizeof(submit->clean[0]));
    }
}

static uint32_t vicMatrix(uint8_t h273)
{
    return h273 == 9 ? 2 : h273 == 1 ? 1 : 0;
}

bool ave_input_put(Encoder *e, CVPixelBufferRef image, const AVERegion *r, TegraNVENCSubmit *submit)
{
    const char *why = e->surfaceInput ? unreadable(e, image) : NULL;
    if (!e->surfaceInput || why) {
        if (why)
            switch_refusal("AVE", why, "%s: this picture's input is copied by the CPU", why);
        return copyRegion(e, image, r, submit);
    }
    dropInputClean(e, submit);
    submit->source = (TegraNVENCSource){
        .surface_id = IOSurfaceGetID(CVPixelBufferGetIOSurface(image)),
        .x = r->x,
        .y = r->y,
        .width = r->width,
        .height = r->height,
        .format = e->config.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                       : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        .matrix = vicMatrix(e->config.matrix),
    };
    return true;
}

int ave_input_submit(Encoder *e, CVPixelBufferRef image, const AVERegion *r, TegraNVENCSubmit *submit, uint32_t *fence)
{
    int status = nvenc_submit(e->client, submit, fence);
    if (submit->source.surface_id && (status == kIOReturnUnsupported || status == kIOReturnNotReady)) {
        if (status == kIOReturnUnsupported) {
            e->surfaceInput = false;
            os_log_error(gLog,
                         "AVE: TegraNVENC refused %{public}s's IOSurface input (its line says why); this session's "
                         "pictures are copied by the CPU",
                         getprogname());
        } else {
            switch_refusal("AVE", "a surface VIC could not take now", "this picture's input is copied by the CPU");
        }
        submit->clean[submit->clean_count++] = (TegraNVENCRange){e->handles.input, 0, e->layout.input.bytes, 0};
        status = copyRegion(e, image, r, submit) ? nvenc_submit(e->client, submit, fence) : kIOReturnError;
    }
    if (!status) {
        if (submit->source.surface_id)
            e->inputVIC++;
        else
            e->inputCopies++;
    }
    return status;
}
