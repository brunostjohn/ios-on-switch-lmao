// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchVICEncodeInput.h. The source is read as switch_vic_transform reads one (a decoder's native picture, else the
// surface's pitch rows under a checked read), the destination is the engine's buffer as a recorded write, and the job
// waits on the hardware for the engine's last read of that buffer before it overwrites it.
#include <IOKit/IOLib.h>
#include "SwitchVICEncodeInput.h"
#include "SwitchVICJobs.h"
#include "SwitchVICMapping.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchVICTransform.h"
#include "SwitchNativeVICImage.h"

namespace {
struct EncodeInput {
    uint32_t refs = 1;
    TegraVICSurface surface = {};
    SwitchVICSurfaceAccess access;
    SwitchNativeSurfaceView native = {};
    SwitchVICMapping *mappings[2] = {};
    TegraVICJob job = {};
};

void release(EncodeInput *work)
{
    if (__atomic_sub_fetch(&work->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mappings)
        switch_vic_mapping_release(mapping);
    switch_vic_surface_release_access(&work->access);
    switch_native_surface_release(&work->native);
    tegra_vic_surface_release(&work->surface);
    IODelete(work, EncodeInput, 1);
}

void completed(void *context, int status)
{
    if (status) {
        static volatile int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            IOLog("TegraVIC: an encoder's input picture failed (%d); the encoder codes what its buffer held\n", status);
    }
    release(static_cast<EncodeInput *>(context));
}

const char *destinationImage(const SwitchVICEncodeInput *in, uint64_t address, const TegraVICImage &source,
                             TegraVICImage *out)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(in->destination, &info);
    const uint64_t bytes = uint64_t(info.page_count) << 12;
    TegraVICImage image = {};
    image.format = tegra_vic_format(in->fourcc);
    if (!image.format || image.format->planes != 2 || !image.format->subY || image.format->kind == TEGRA_VIC_KIND_RGB)
        return "encoder input format";
    if (in->lumaOffset >= bytes || in->chromaOffset >= bytes)
        return "encoder input outside its buffer";
    image.width = in->width;
    image.height = in->height;
    const uint64_t offsets[2] = {in->lumaOffset, in->chromaOffset};
    const uint32_t rows[2] = {in->lumaRows, in->chromaRows};
    for (uint32_t plane = 0; plane < 2; ++plane) {
        image.pitch[plane] = in->pitch;
        image.rows[plane] = rows[plane];
        image.base[plane] = address + offsets[plane];
        image.span[plane] = bytes - offsets[plane];
    }
    image.blockLinear = in->blockLinear;
    image.blockHeightLog2 = in->blockHeightLog2;
    // A Y'CbCr source keeps its matrix, so the job is the channel pass-through; an RGB one takes the encoder's.
    const bool ycc = source.format->kind != TEGRA_VIC_KIND_RGB;
    image.standard = ycc ? source.standard : in->matrix;
    image.chromaSite = source.chromaSite;
    image.transfer = source.transfer;
    image.primaries = source.primaries;
    *out = image;
    return nullptr;
}

int place(EncodeInput *work, uint32_t surfaceID, task_t task, const SwitchVICEncodeInput *in, uint32_t *fence,
          const char **why)
{
    if (tegra_vic_surface_lookup(surfaceID, task, &work->surface)) {
        *why = "surface ID not found in the encoding task";
        return -22;
    }
    const int native = switch_native_surface_acquire(&work->surface, &work->native, false);
    int error = native;
    if (native == -2 || native == -95 || native == SWITCH_GPU_BACKING_STALE)
        error = switch_vic_surface_access(&work->surface, false, &work->access);
    if (error) {
        *why = "the surface's read could not be acquired";
        return error;
    }
    auto *input = work->native.owner ? work->native.native : work->access.backing;
    const uint64_t extent = tegra_vic_surface_extent(&work->surface.desc);
    if (!work->native.owner &&
        (extent > UINT64_MAX - work->access.offset || !switch_vic_pitch_backing(input, work->access.offset + extent))) {
        *why = "a source surface that is not pitch linear";
        return -95;
    }
    work->mappings[0] = switch_vic_mapping_acquire(input, false);
    work->mappings[1] = switch_vic_mapping_acquire(in->destination, true);
    if (!work->mappings[0] || !work->mappings[1]) {
        *why = "VIC mappings";
        return -12;
    }
    TegraVICImage src = {}, dst = {};
    const uint64_t address = switch_vic_mapping_address(work->mappings[0]);
    *why = work->native.owner ? switch_native_vic_image(work->native, address, &src)
                              : tegra_vic_image_from(&work->surface.desc, address + work->access.offset, &src);
    if (!*why)
        *why = destinationImage(in, switch_vic_mapping_address(work->mappings[1]), src, &dst);
    TegraVICOp op = {};
    op.srcX = uint64_t(in->x) << 16;
    op.srcY = uint64_t(in->y) << 16;
    op.srcW = uint64_t(in->w) << 16;
    op.srcH = uint64_t(in->h) << 16;
    op.dstW = in->w;
    op.dstH = in->h;
    op.nearest = 1;
    if (!*why && work->native.owner && !switch_native_vic_crop(work->native, &op))
        *why = "native source crop";
    if (!*why)
        *why = tegra_vic_build(&src, &dst, &op, tegra_vic_traits(), tegra_vic_config_address(),
                               tegra_vic_filter_address(), &work->job);
    if (*why)
        return -95;
    switch_gpu_backing_use uses[3] = {
        {in->destination, SWITCH_GPU_BACKING_WRITE, 0},
        {input, SWITCH_GPU_BACKING_READ, 0},
        {work->native.logical, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0}};
    switch_gpu_backing_guard guards[2] = {};
    uint32_t guardCount = 0;
    if (work->native.owner) {
        guards[guardCount++] = work->native.guard;
        guards[guardCount++] = work->native.representation;
    } else {
        guards[guardCount++] = work->access.guard;
    }
    switch_gpu_cpu_lease *lease = work->access.owned;
    const SwitchVICOwnership ownership = {uses,       work->native.owner ? 3u : 2u, guards,
                                          guardCount, lease ? &lease : nullptr,     lease ? 1u : 0u};
    __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
    error = switch_vic_submit_checked(&work->job, 1, in->after, ownership, completed, work, fence);
    switch_vic_surface_unreserve(&work->access);
    if (error) {
        release(work);
        *why = "VIC refused the job";
    }
    return error;
}
}  // namespace

bool switch_vic_encode_input_ready(bool blockLinear)
{
    const auto *traits = tegra_vic_traits();
    return tegra_vic_ready() && traits && (!blockLinear || traits->blockLinearOut);
}

int switch_vic_encode_input(uint32_t surfaceID, task_t task, const SwitchVICEncodeInput *input, uint32_t *fence,
                            const char **why)
{
    *why = nullptr;
    if (!input || !input->destination || !fence) {
        *why = "request";
        return -22;
    }
    if (!switch_vic_encode_input_ready(input->blockLinear)) {
        *why = "VIC is not up or has not proven block-linear output";
        return -19;
    }
    auto *work = IONewZero(EncodeInput, 1);
    if (!work) {
        *why = "memory";
        return -12;
    }
    work->refs = 1;
    const int error = place(work, surfaceID, task, input, fence, why);
    release(work);
    return error;
}
