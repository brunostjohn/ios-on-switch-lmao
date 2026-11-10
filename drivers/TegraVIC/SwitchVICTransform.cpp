// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICTransform.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

namespace {
struct SwitchVICTransform {
    switch_gpu_backing *backings[2] = {};
    SwitchVICMapping *mappings[2] = {};
    TegraVICJob jobs[3] = {};
    void (*completion)(void *, int) = nullptr;
    void *context = nullptr;
};

void release(SwitchVICTransform *work)
{
    for (uint32_t i = 0; i < 2; ++i) {
        switch_vic_mapping_release(work->mappings[i]);
        if (work->backings[i])
            switch_gpu_backing_release(work->backings[i]);
    }
    IODelete(work, SwitchVICTransform, 1);
}

void completed(void *context, int status)
{
    auto *work = static_cast<SwitchVICTransform *>(context);
    const auto completion = work->completion;
    void *owner = work->context;
    completion(owner, status);
    release(work);
}

bool pitchLayout(switch_gpu_backing *backing, uint64_t extent)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    if (extent > static_cast<uint64_t>(info.page_count) * 4096)
        return false;
    tgm_layout legacy = {};
    if (!info.layout.plane_count && info.page_count && tegra_gm20b_surface_layout(info.pages[0], &legacy) == 1)
        return false;
    for (uint32_t p = 0; p < info.layout.plane_count; ++p) {
        if (info.layout.planes[p].layout != SWITCH_GPU_BACKING_PITCH)
            return false;
    }
    return true;
}
}  // namespace

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination,
                         const TegraVICPlan *plan, uint32_t chromaSite,
                         void (*completion)(void *, int), void *context)
{
    if (!completion || !plan->count || plan->count > 3)
        return -22;
    auto *work = IONewZero(SwitchVICTransform, 1);
    if (!work)
        return -12;
    work->completion = completion;
    work->context = context;
    work->backings[1] = switch_vic_surface_backing(destination);
    if (plan->readsSource)
        work->backings[0] = switch_vic_surface_backing(source);
    if (!work->backings[1] || (plan->readsSource && !work->backings[0])) {
        release(work);
        return -12;
    }
    if (!pitchLayout(work->backings[1], tegra_vic_surface_extent(&destination->desc)) ||
        (plan->readsSource && !pitchLayout(work->backings[0], tegra_vic_surface_extent(&source->desc)))) {
        release(work);
        return 0;
    }
    work->mappings[1] = switch_vic_mapping_acquire(work->backings[1], true);
    if (plan->readsSource)
        work->mappings[0] = switch_vic_mapping_acquire(work->backings[0], false);
    if (!work->mappings[1] || (plan->readsSource && !work->mappings[0])) {
        release(work);
        return -12;
    }
    TegraVICImage src = {}, dst = {};
    const char *why = tegra_vic_image_from(&destination->desc, switch_vic_mapping_address(work->mappings[1]), &dst);
    dst.chromaSite = chromaSite;
    if (!why && plan->readsSource)
        why = tegra_vic_image_from(&source->desc, switch_vic_mapping_address(work->mappings[0]), &src);
    for (uint32_t i = 0; !why && i < plan->count; ++i) {
        const auto &op = plan->op[i];
        why = tegra_vic_build(op.fill ? nullptr : &src, &dst, &op, tegra_vic_traits(),
                              tegra_vic_config_address(), tegra_vic_filter_address(), &work->jobs[i]);
    }
    if (why) {
        release(work);
        return -22;
    }
    switch_gpu_backing_use uses[2] = {{work->backings[1], SWITCH_GPU_BACKING_WRITE, 0},
                                       {work->backings[0], SWITCH_GPU_BACKING_READ, 0}};
    const int error = switch_vic_submit_batch(work->jobs, plan->count, nullptr, uses, plan->readsSource ? 2 : 1,
                                             completed, work, nullptr);
    if (error) {
        release(work);
        return error;
    }
    return 1;
}
