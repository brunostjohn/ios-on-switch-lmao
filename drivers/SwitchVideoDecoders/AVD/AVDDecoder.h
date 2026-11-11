// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's instance state, shared by its frame sessions (AVDDecoder.c) and tile sessions (AVDTiles.c).
#ifndef AVD_DECODER_H
#define AVD_DECODER_H
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "AVDFrameTiming.h"
#include "H264NVDEC.h"
#include "HEVCNVDEC.h"
#include "MPEG2NVDEC.h"
#include "MPEG4NVDEC.h"
#include "NVDECClient.h"
#include "VP9NVDEC.h"
#include "VTDecoderSPI.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"

// The tile being decoded: its frame ID, the canvas it goes into and where.
typedef struct {
    int64_t frame;
    CVPixelBufferRef canvas;
    uint32_t x, y;
    bool answered;
} AVDTile;

typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    VTTileDecoderSession tileSession;  // a tile session (grid images) instead of a frame session
    AVDTile tile;
    NVDECClient *client;
    FourCharCode codec;  // 'avc1', 'vp09', 'hvc1', 'mp4v', 'mp2v' or 'mp1v'
    H264Decoder *h264;
    VP9Decoder *vp9;
    HEVCDecoder *hevc;
    MPEG4Decoder *mpeg4;
    MPEG2Decoder *mpeg2;
    VTVideoDecoderRef software;  // the session runs on Apple's software decoder (AVDDecoder.c)
    CFDataRef avcC;              // or vpcC, hvcC, esds
    unsigned lengthSize;
    OSType pixelFormat;
    CFTypeRef matrix, primaries, transfer, chromaTop, chromaBottom;
    uint32_t width, height, frames, errors;
    uint64_t decodeTicks, copyTicks;  // copyTicks: placing pictures in pixel buffers (VIC's or the CPU's copy)
    bool present;
    uint32_t presented, exported, copied;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    AVDFrameTiming timing;
#endif
} Decoder;

typedef struct {
    CVPixelBufferRef image;
    VTVideoDecoderFrame frame;
    OSStatus status;
} Output;

extern os_log_t gAVDLog;

// The factory every entry registers (AVDRegister.c).
OSStatus avd_create_instance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out);

void avd_set_number(CFMutableDictionaryRef dict, CFStringRef key, int32_t value);
CFDataRef avd_copy_atom(CMVideoFormatDescriptionRef format, CFStringRef name);
int avd_load_hvcc(Decoder *d, CFDataRef hvcC);
const char *avd_codec_name(FourCharCode codec);
// The session's NV12 pool at d->width x d->height ('x420'/'xf20' when tenBit), rows aligned for VIC.
void avd_set_attributes(Decoder *d, VTVideoDecoderSession session, bool fullRange, bool tenBit, OSStatus *status);
// The session on Apple's software decoder for d->codec, `why` logged once; NVDEC's state must be gone already.
OSStatus avd_start_software(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format,
                            const char *why);
// AVDMPEG.c: an 'mp4v' session, an 'mp2v' or 'mp1v' session, and a picture's field attachments (AVD_FIELDS_*).
OSStatus avd_start_mpeg4(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format);
OSStatus avd_start_mpeg2(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format);
Boolean avd_mpeg2_accepts(const Decoder *d, CMVideoFormatDescriptionRef format);
void avd_tag_fields(CVPixelBufferRef image, uint8_t fields);
// The sample's bytes, contiguous: *data points into the sample or at *copy (the caller frees it).
bool avd_sample_bytes(CMSampleBufferRef sample, const uint8_t **data, size_t *size, uint8_t **copy);
// The picture into `image` with its top-left at (x, y), by VIC, else by the CPU; clipped to the image.
void avd_place_picture(Decoder *d, const H264Picture *picture, CVPixelBufferRef image, uint32_t x, uint32_t y);

// AVDTiles.c: the decoder class's version 3 slots.
OSStatus avd_start_tile_session(VTVideoDecoderRef ref, VTTileDecoderSession session,
                                CMVideoFormatDescriptionRef format);
OSStatus avd_decode_tile(VTVideoDecoderRef ref, int64_t frameID, CMSampleBufferRef sample, VTInt32Point tile,
                         VTInt32Size size, CVPixelBufferRef canvas, VTInt32Point offset, VTDecodeFrameFlags flags,
                         VTDecodeInfoFlags *info);
// A decoded tile picture into its canvas, and its answer (both on the decoding thread, inside avd_decode_tile).
void *avd_tile_decoded(Decoder *d, const H264Picture *picture);
void avd_tile_emit(Decoder *d, Output *out);

#endif
