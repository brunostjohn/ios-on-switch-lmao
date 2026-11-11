// SPDX-License-Identifier: BSD-2-Clause-Patent
// See MPEGFrames.h.
#include "MPEGFrames.h"
#include <string.h>
#include "../../TegraNVDEC/nvidia/clc5b0.h"
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"

#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

void mpeg_frames_release(MPEGFrames *f)
{
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (f->handle[i])
            f->b.free(f->b.ctx, f->handle[i]);
        f->handle[i] = 0;
        f->va[i] = NULL;
    }
    if (f->input)
        f->b.free(f->b.ctx, f->input);
    f->input = 0;
    f->input_va = NULL;
    f->past = f->future = -1;
}

int mpeg_frames_configure(MPEGFrames *f, uint32_t width_mbs, uint32_t height_mbs)
{
    mpeg_frames_release(f);
    f->width_mbs = width_mbs;
    f->height_mbs = height_mbs;
    f->pitch = ALIGN(width_mbs * 16, 64);
    f->rows = ALIGN(height_mbs * 16, 32);
    f->luma_bytes = f->pitch * f->rows;
    f->bytes = f->luma_bytes + f->luma_bytes / 2;
    void *va;
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (f->b.alloc(f->b.ctx, f->bytes, &f->handle[i], &va)) {
            f->handle[i] = 0;
            mpeg_frames_release(f);
            return -3;
        }
        f->va[i] = va;
    }
    f->input_bytes = ALIGN(MPEG_FRAMES_BITS_OFFSET + (uint64_t)width_mbs * height_mbs * 192 + 0x10000, 0x10000);
    if (f->b.alloc(f->b.ctx, f->input_bytes, &f->input, &va)) {
        f->input = 0;
        mpeg_frames_release(f);
        return -3;
    }
    f->input_va = va;
    return 0;
}

int mpeg_frames_take(MPEGFrames *f)
{
    for (int i = 0; i < MPEG_FRAMES_SURFACES; i++) {
        if (i == f->past || i == f->future)
            continue;
        if (f->b.recycle) {
            void *address = f->va[i];
            if (f->b.recycle(f->b.ctx, f->handle[i], f->bytes, &address))
                return -1;
            f->va[i] = address;
        }
        return i;
    }
    return -1;
}

void mpeg_frames_reference(MPEGFrames *f, int index)
{
    f->past = f->future;
    f->future = (int8_t)index;
}

static int growInput(MPEGFrames *f, uint64_t needed)
{
    uint64_t bytes = f->input_bytes;
    while (bytes < needed)
        bytes *= 2;
    if (bytes > (32u << 20))
        return -1;
    uint32_t handle;
    void *va;
    if (f->b.alloc(f->b.ctx, bytes, &handle, &va))
        return -1;
    memcpy(va, f->input_va, MPEG_FRAMES_BITS_OFFSET + f->bits_len);
    f->b.free(f->b.ctx, f->input);
    f->input = handle;
    f->input_va = va;
    f->input_bytes = bytes;
    return 0;
}

int mpeg_frames_append(MPEGFrames *f, const uint8_t *data, size_t size, bool reset)
{
    if (reset)
        f->bits_len = 0;
    const uint64_t needed = (uint64_t)MPEG_FRAMES_BITS_OFFSET + f->bits_len + size;
    if (needed > f->input_bytes && growInput(f, needed))
        return -1;
    memcpy(f->input_va + MPEG_FRAMES_BITS_OFFSET + f->bits_len, data, size);
    f->bits_len += (uint32_t)size;
    return 0;
}

void mpeg_frames_entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

void mpeg_frames_bind(MPEGFrames *f, TegraNVDECSubmit *s, int current, int past, int future)
{
    mpeg_frames_entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, f->picture_index++, 0);
    mpeg_frames_entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, f->input, MPEG_FRAMES_BITS_OFFSET);
    mpeg_frames_entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, f->input, 0);
    const int slots[3] = {current, past, future};
    for (uint16_t i = 0; i < 3; i++) {
        const uint32_t handle = f->handle[slots[i]];
        mpeg_frames_entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, handle, 0);
        mpeg_frames_entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, handle,
                          f->luma_bytes);
    }
}

int mpeg_frames_run(MPEGFrames *f, TegraNVDECSubmit *s, int current, TegraNVDECRange clean)
{
    mpeg_frames_entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    s->clean[s->clean_count++] = (TegraNVDECRange){f->input, MPEG_FRAMES_BITS_OFFSET, f->bits_len, 0};
    if (clean.length)
        s->clean[s->clean_count++] = clean;
    TegraNVDECWait wait = {.invalidate_count = 1};
    wait.invalidate[0] = (TegraNVDECRange){f->input, 0, sizeof(nvdec_status_s), 0};
    if (!f->b.cpu_reads || f->b.cpu_reads(f->b.ctx))
        wait.invalidate[wait.invalidate_count++] = (TegraNVDECRange){f->handle[current], 0, f->bytes, 0};
    if (f->b.submit(f->b.ctx, s, &wait.fence) || f->b.wait(f->b.ctx, &wait))
        return -2;
    const nvdec_status_s *status = (const nvdec_status_s *)f->input_va;
    return status->error_status && !status->mbs_correctly_decoded ? (int)status->error_status : 0;
}

void mpeg_frames_output(MPEGFrames *f, int index, uint32_t width, uint32_t height, int status, void *token)
{
    const H264Picture picture = {
        .luma = f->va[index],
        .chroma = f->va[index] + f->luma_bytes,
        .pitch = f->pitch,
        .handle = f->handle[index],
        .chroma_offset = f->luma_bytes,
        .rows = f->rows,
        .coded_width = f->width_mbs * 16,
        .coded_height = f->height_mbs * 16,
        .width = width,
        .height = height,
        .status = status,
        .token = token,
    };
    void *item = f->b.decoded(f->b.ctx, &picture);
    if (item)
        f->b.emit(f->b.ctx, item);
}
