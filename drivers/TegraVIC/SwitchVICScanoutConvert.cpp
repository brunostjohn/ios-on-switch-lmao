// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICScanoutConvert.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"

namespace {
struct Conversion {
    uint32_t references;
    SwitchVICMapping *mapping[2];
    TegraVICJob job;
};

void release(Conversion *work)
{
    if (__atomic_sub_fetch(&work->references, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *mapping : work->mapping)
        switch_vic_mapping_release(mapping);
    IODelete(work, Conversion, 1);
}

void completed(void *context, int)
{
    release(static_cast<Conversion *>(context));
}
}

int switch_vic_scanout_convert(const SwitchNativeScanout *source, const TegraVICImage *image,
                               switch_gpu_backing *destination, uint32_t width, uint32_t height, uint32_t pitch,
                               const TegraVICOp *operation, SwitchNativeScanout *output)
{
    if (!output || output == source)
        return -22;
    *output = {};
    if (!source || !source->lease || !source->view.native || !image || !image->format || !destination ||
        !operation || !image->format->planes || image->format->planes > 2 || !width || !height || width > 16384 || height > 16384 ||
        pitch < uint64_t(width) * 4 || (pitch & 255) || source->view.native == destination || source->view.logical == destination)
        return -22;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return -19;
    switch_gpu_backing_info target = {};
    switch_gpu_backing_describe(destination, &target);
    const uint64_t bytes = uint64_t(pitch) * height;
    if (bytes > (uint64_t(target.page_count) << 12))
        return -22;
    const uint64_t sourceBytes = uint64_t(source->allocation.page_count) << 12;
    for (uint32_t p = 0; p < image->format->planes; ++p)
        if (image->base[p] > sourceBytes || !image->span[p] || image->span[p] > sourceBytes - image->base[p])
            return -22;
    auto *work = IONewZero(Conversion, 1);
    if (!work)
        return -12;
    work->references = 1;
    work->mapping[0] = switch_vic_mapping_acquire(source->view.native, false);
    work->mapping[1] = switch_vic_mapping_acquire(destination, true);
    int error = !work->mapping[0] || !work->mapping[1] ? -12 : 0;
    if (!error) {
        TegraVICImage input = *image, out = {};
        const uint64_t base = switch_vic_mapping_address(work->mapping[0]);
        for (uint32_t p = 0; p < image->format->planes; ++p) {
            if (input.base[p] > UINT64_MAX - base)
                error = -22;
            input.base[p] += base;
        }
        out.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
        out.transfer = input.transfer;
        out.primaries = input.primaries;
        out.width = width;
        out.height = height;
        out.pitch[0] = pitch;
        out.rows[0] = height;
        out.span[0] = bytes;
        out.base[0] = switch_vic_mapping_address(work->mapping[1]);
        if (!error && tegra_vic_build(&input, &out, operation, tegra_vic_traits(), tegra_vic_config_address(),
                                     tegra_vic_filter_address(), &work->job))
            error = -95;
    }
    if (!error) {
        switch_gpu_backing_use uses[3] = {{source->view.native, SWITCH_GPU_BACKING_READ, 0},
                                         {destination, SWITCH_GPU_BACKING_WRITE, 0}, {}};
        uint32_t count = 2, guardCount = 0;
        switch_gpu_backing_guard guards[2] = {};
        if (source->view.logical) {
            uses[count++] = {source->view.logical, SWITCH_GPU_BACKING_READ, 0};
            if (source->view.guard.generation)
                guards[guardCount++] = source->view.guard;
            if (source->view.representation.generation)
                guards[guardCount++] = source->view.representation;
        }
        const SwitchVICOwnership ownership = {uses, count, guards, guardCount, nullptr, 0};
        __atomic_add_fetch(&work->references, 1, __ATOMIC_RELAXED);
        error = switch_vic_submit_checked(&work->job, 1, nullptr, ownership, completed, work, nullptr);
        if (error)
            release(work);
    }
    if (!error)
        error = switch_gpu_backing_export_read(destination, &output->view.producer, &output->lease);
    if (!error) {
        switch_gpu_backing_retain(destination);
        output->view.native = destination;
        output->view.layout.format = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
        output->view.layout.plane_count = 1;
        output->view.layout.transfer = image->transfer;
        output->view.layout.primaries = image->primaries;
        auto &plane = output->view.layout.planes[0];
        plane.width = width;
        plane.height = height;
        plane.pitch = pitch;
        plane.bytes = bytes;
        plane.bytes_per_element = 4;
        plane.layout = SWITCH_GPU_BACKING_PITCH;
        output->view.crop = {0, 0, width, height};
        switch_gpu_backing_describe(destination, &output->allocation);
    }
    release(work);
    return error;
}
