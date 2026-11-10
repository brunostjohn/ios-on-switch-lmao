// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "TegraVIC.h"
#include "TegraVICPresent.h"
#include "TegraVICSurface.h"
#include "SwitchVICPicturePlan.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace {
bool gPresentReady;

struct Present {
    TegraVICSurface surface = {};
    switch_gpu_backing *destination = nullptr;
    SwitchVICMapping *sourceMap = nullptr, *destinationMaps[2] = {};
    TegraVICJob job = {};
};

void release(Present *work)
{
    switch_vic_mapping_release(work->sourceMap);
    for (auto *mapping : work->destinationMaps)
        switch_vic_mapping_release(mapping);
    if (work->destination)
        switch_gpu_backing_release(work->destination);
    tegra_vic_surface_release(&work->surface);
    IODelete(work, Present, 1);
}

int place(const TegraVICPicture *picture, task_t task, Present *work, const char **why)
{
    if (!picture || !picture->backing || tegra_vic_surface_lookup(picture->surfaceID, task, &work->surface)) {
        *why = "picture backing or surface ID not found";
        return -22;
    }
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    SwitchVICPlacement placement = {};
    *why = switch_vic_picture_bytes(picture, &work->surface.desc, &bytes, &desc);
    if (!*why)
        *why = switch_vic_picture_place(&bytes, &desc, &placement);
    if (*why)
        return -95;
    work->destination = switch_vic_surface_backing(&work->surface);
    if (!work->destination) {
        *why = "the destination could not be retained";
        return -12;
    }
    work->sourceMap = switch_vic_mapping_acquire(picture->backing, false);
    for (uint32_t plane = 0; plane < 2; ++plane)
        work->destinationMaps[plane] = switch_vic_mapping_acquire_range(
            work->destination, true, static_cast<uint32_t>(placement.band[plane].start >> 12), placement.pages[plane]);
    if (!work->sourceMap || !work->destinationMaps[0] || !work->destinationMaps[1]) {
        *why = "the picture mappings could not be retained";
        return -12;
    }
    TegraVICImage source = {}, destination = {};
    TegraVICOp operation = {};
    switch_vic_picture_describe(&bytes, placement, switch_vic_mapping_address(work->sourceMap),
                               0, &source, &destination, &operation);
    for (uint32_t plane = 0; plane < 2; ++plane)
        destination.base[plane] = switch_vic_mapping_address(work->destinationMaps[plane]) +
                                  (placement.band[plane].start & 4095);
    *why = tegra_vic_build(&source, &destination, &operation, tegra_vic_traits(),
                           tegra_vic_config_address(), tegra_vic_filter_address(), &work->job);
    if (*why)
        return -95;
    switch_gpu_backing_use uses[] = {{picture->backing, SWITCH_GPU_BACKING_READ, 0},
                                      {work->destination, SWITCH_GPU_BACKING_WRITE, 0}};
    uint32_t fence = 0;
    int error = switch_vic_submit_batch(&work->job, 1, nullptr, uses, 2, nullptr, nullptr, &fence);
    if (!error)
        error = switch_vic_wait(fence);
    if (!error)
        tegra_vic_surface_written(&work->surface);
    else
        *why = "the queued picture transfer failed";
    return error;
}
}  // namespace

extern "C" bool tegra_vic_present_ready(void)
{
    const auto *traits = tegra_vic_traits();
    return __atomic_load_n(&gPresentReady, __ATOMIC_ACQUIRE) && tegra_vic_ready() && traits && traits->blockLinearIn;
}

extern "C" void tegra_vic_present_start(void)
{
    if (tegra_vic_traits()->blockLinearIn)
        __atomic_store_n(&gPresentReady, true, __ATOMIC_RELEASE);
}

extern "C" int tegra_vic_present(const TegraVICPicture *picture, task_t task, const char **why)
{
    if (!why)
        return -22;
    *why = nullptr;
    if (!tegra_vic_present_ready()) {
        *why = "VIC is not published";
        return -19;
    }
    auto *work = IONewZero(Present, 1);
    if (!work)
        return -12;
    const int error = place(picture, task, work, why);
    release(work);
    if (error && *why)
        switch_refusal("TegraVIC", *why, "decoded picture not placed by VIC, the decoder copies it on the CPU");
    return error;
}
