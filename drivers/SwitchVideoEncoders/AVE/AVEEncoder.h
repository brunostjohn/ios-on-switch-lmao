// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVE's instance state and the session steps its entry points share.
#ifndef AVE_ENCODER_H
#define AVE_ENCODER_H
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include "H264NVENC.h"
#include "HEVCNVENC.h"
#include "NVENCClient.h"
#include "RateControl.h"
#include "VTEncoderSPI.h"

typedef struct {
    pthread_mutex_t lock;
    VTVideoEncoderSession session;
    NVENCClient *client;
    TegraNVENCInfo info;
    CMVideoDimensions size;
    CMVideoCodecType codec;  // 'avc1', or 'hvc1': every picture an IDR (HEVCNVENC.h has no P pictures)
    bool configured, failed;
    HEVCEncConfig hevc;
    H264EncConfig config;
    H264EncLayout layout;
    H264EncHandles handles;
    uint8_t *inputVA, *bitstreamVA, *auxVA;
    uint32_t buffers[sizeof(H264EncHandles) / sizeof(uint32_t)];  // one per handle
    unsigned bufferCount;
    CMVideoFormatDescriptionRef format;
    uint8_t *avcc;  // AVCC scratch for one picture
    size_t avccCap;
    int32_t maxKeyInterval, averageBitRate, constantBitRate;
    double maxKeyDuration, quality, expectedFrameRate, limitBytes, limitSeconds;
    bool allowTemporal, realTime;
    CFStringRef profileLevel, entropy, primaries, transfer, matrix;
    bool haveRef, keyPending;
    uint32_t frameNum, idrPicId, pictureIndex, pocCount, sinceKey;
    CMTime lastKeyTime;
    RateControl rc;
    uint32_t frames, keyFrames, errors;
    uint64_t bytes, copyTicks, encodeTicks;
} Encoder;

extern os_log_t gLog;

// The TegraNVENC client for a session of e->size and e->codec, or kVTCouldNotFindVideoEncoderErr with the refusal
// logged once (a size NVENC cannot code, the process refused, no H.265 on this kernel).
OSStatus ave_open_engine(Encoder *e);
// The stream the first picture fixes (full range or not and its colour tags): buffers, parameter sets, the format
// description every sample carries, the rate control.
OSStatus ave_configure(Encoder *e, CVPixelBufferRef image);

#endif
