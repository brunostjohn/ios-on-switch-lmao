// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder's 'mp4v' sessions. The video object layer comes from the format description's 'esds'
// DecoderSpecificInfo (ISO/IEC 14496-14 MP4VisualSampleEntry, as AVFoundation's readers and FFmpeg's VideoToolbox
// hwaccel describe MPEG-4 Part 2). A stream NVDEC does not take (mpeg4_vol_unsupported: interlaced, GMC, data
// partitioning, ..., or no VOL to decide by) runs on Apple's software decoder, VCPMP4V.videodecoder, inside this
// instance, as H.264 does.
#include "AVDDecoder.h"
#include "SwitchVideoColor.h"

OSStatus avd_start_mpeg4(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    d->avcC = avd_copy_atom(format, CFSTR("esds"));
    MPEG4VOL vol = {0};
    uint8_t objectType = 0;
    const uint8_t *dsi = NULL;
    size_t dsiSize = 0;
    const bool esds =
        d->avcC && mpeg4_esds(CFDataGetBytePtr(d->avcC), (size_t)CFDataGetLength(d->avcC), &objectType, &dsi, &dsiSize);
    const int parsed = esds && dsi ? mpeg4_parse_config(&vol, dsi, dsiSize) : 0;
    const char *why = !esds || objectType != 0x20 ? "no MPEG-4 Visual 'esds' to start from"
                      : parsed < 0                ? "a malformed video object layer"
                      : parsed == 0               ? "no video object layer in the format description"
                                                  : mpeg4_vol_unsupported(&vol);
    if (!why && mpeg4_decoder_configure(d->mpeg4, &vol))
        why = "no NVDEC memory for its buffers (see the AVD alloc line)";
    if (why) {
        mpeg4_decoder_destroy(d->mpeg4);
        nvdec_close(d->client);
        d->mpeg4 = NULL;
        d->client = NULL;
        return avd_start_software(d, session, format, why);
    }
    d->width = vol.width;
    d->height = vol.height;
    const bool vui = vol.colour_description;
    d->matrix =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? vol.matrix : 2, 0, d->height);
    d->primaries = switch_video_color_tag(format, kCMFormatDescriptionExtension_ColorPrimaries,
                                          vui ? vol.colour_primaries : 2, 1, 0);
    d->transfer =
        switch_video_color_tag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? vol.transfer : 2, 2, 0);
    OSStatus status;
    avd_set_attributes(d, session, vol.full_range, false, &status);
    if (!status)
        os_log(
            gAVDLog,
            "AVD: session %ux%u in %{public}s on NVDEC (MPEG-4 Part 2 object type %u, profile and level %#x%{public}s"
            "%{public}s), "
            "pictures out by %{public}s",
            d->width, d->height, getprogname(), vol.object_type, vol.profile_level,
            vol.quarter_sample ? ", quarter sample" : "", vol.quant_type ? ", MPEG quantisation" : "",
            d->present ? "VIC" : "the CPU");
    return status;
}
