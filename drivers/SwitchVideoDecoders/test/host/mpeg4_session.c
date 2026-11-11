// SPDX-License-Identifier: BSD-2-Clause-Patent
// avd_host's 'mp4v' sessions: a clip of test/MPEG4Clip.h described as an MP4 file describes it (an 'esds' atom whose
// DecoderSpecificInfo is the clip's VOS, VO and VOL headers), every VOP a sample, through VideoToolbox.
#include "mpeg4_session.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>
#include "../MPEG4Clip.h"

typedef struct {
    int out, errors;
} Count;

static void output(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                   CMTime pts, CMTime duration)
{
    (void)frameRefcon, (void)flags, (void)pts, (void)duration;
    Count *c = refcon;
    if (status || !image)
        c->errors++;
    else
        c->out++;
}

static size_t descriptor(uint8_t *at, uint8_t tag, size_t length)
{
    const uint8_t header[5] = {tag, 0x80 | (uint8_t)(length >> 21 & 0x7f), 0x80 | (uint8_t)(length >> 14 & 0x7f),
                               0x80 | (uint8_t)(length >> 7 & 0x7f), (uint8_t)(length & 0x7f)};
    memcpy(at, header, 5);
    return 5;
}

// ISO/IEC 14496-1 ES_Descriptor for MPEG-4 Visual (objectTypeIndication 0x20), every length in the four-byte form.
static CFDataRef esds(const uint8_t *dsi, size_t size)
{
    uint8_t e[512] = {0};
    const size_t config = 13 + 5 + size, es = 3 + 5 + config + 5 + 1;
    size_t n = 4;  // version, flags
    n += descriptor(e + n, 0x03, es);
    n += 3;  // ES_ID, flags
    n += descriptor(e + n, 0x04, config);
    e[n++] = 0x20;
    e[n++] = 0x11;  // visual stream
    n += 11;        // bufferSizeDB, maxBitrate, avgBitrate
    n += descriptor(e + n, 0x05, size);
    memcpy(e + n, dsi, size);
    n += size;
    n += descriptor(e + n, 0x06, 1);
    e[n++] = 0x02;  // SLConfigDescriptor: predefined MP4
    return CFDataCreate(NULL, e, (CFIndex)n);
}

bool mpeg4_session_check(CFDictionaryRef spec, bool simple, bool wantHardware, char *line, size_t size)
{
    const uint8_t *clip = simple ? kMPEG4ClipB : kMPEG4ClipA;
    const size_t *vops = simple ? kMPEG4ClipBVOPs : kMPEG4ClipAVOPs;
    const int count = simple ? 32 : 18;
    CFDataRef atom = esds(clip, simple ? kMPEG4ClipBConfig : kMPEG4ClipAConfig);
    const void *atomKeys[] = {CFSTR("esds")}, *atomValues[] = {atom};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, atomKeys, atomValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    const void *extKeys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *extValues[] = {atoms};
    CFDictionaryRef extensions = CFDictionaryCreate(NULL, extKeys, extValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                    &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_MPEG4Video, simple ? 136 : 176,
                                                     simple ? 72 : 144, extensions, &format);
    CFRelease(extensions);
    CFRelease(atoms);
    CFRelease(atom);
    Count out = {0};
    VTDecompressionOutputCallbackRecord callback = {output, &out};
    VTDecompressionSessionRef session = NULL;
    if (!status)
        status = VTDecompressionSessionCreate(NULL, format, spec, NULL, &callback, &session);
    if (status) {
        snprintf(line, size, "mp4v: session failed %d", (int)status);
        if (format)
            CFRelease(format);
        return false;
    }
    CFBooleanRef hardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hardware);
    int refused = 0;
    for (int i = 0; i < count; i++) {
        const size_t offset = vops[i], bytes = vops[i + 1] - offset;
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, (void *)(clip + offset), bytes, kCFAllocatorNull, NULL, 0,
                                           bytes, 0, &block);
        CMSampleBufferCreateReady(NULL, block, format, 1, 0, NULL, 1, &bytes, &sample);
        VTDecodeInfoFlags info = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, 0, NULL, &info))
            refused++;
        CFRelease(sample);
        CFRelease(block);
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    const bool onHardware = hardware == kCFBooleanTrue;
    if (hardware)
        CFRelease(hardware);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    snprintf(line, size, "mp4v %c: %s decoder, %d of %d out, %d errors, %d refused", simple ? 'B' : 'A',
             onHardware ? "hardware" : "software", out.out, count, out.errors, refused);
    return onHardware == wantHardware && out.out == count && !out.errors && !refused;
}
