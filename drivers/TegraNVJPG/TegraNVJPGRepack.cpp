// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraNVJPGRepack.h. The buffer is registered as a GPU backing, so VIC maps and orders it as any engine buffer; NVJPG
// records nothing in it, and VIC's job waits for NVJPG's syncpoint instead.
#include <IOKit/IOLib.h>
#include "TegraNVJPGRepack.h"
#include "TegraNVJPGMapping.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraGM20B/SwitchGpuSharedEvent.h"
#include "../TegraGM20B/TegraGM20BCopy.h"
#include "../TegraVIC/SwitchVICDecodeOutput.h"
#include "../TegraVIC/SwitchVICSurfaceAccess.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

namespace {
// Grown to the largest decode yet and kept: decodes run one at a time on the engine thread, and each returns only once
// VIC has read the buffer, so the next may write it.
switch_dma_pages *gPages;
switch_gpu_backing *gBacking;
// Only what IOLockSleep needs to sleep on VIC's completion: the engine thread and that completion's thread call are its
// only takers, so nothing else contends for it.
IOLock *gWaitLock;

void releasePages(void *owner)
{
    auto *pages = static_cast<switch_dma_pages *>(owner);
    switch_dma_pages_free(pages);
    IODelete(pages, switch_dma_pages, 1);
}

int grow(uint64_t bytes)
{
    if (gBacking && gPages->size >= bytes)
        return 0;
    auto *pages = IONewZero(switch_dma_pages, 1);
    if (!pages)
        return -12;
    if (!switch_dma_pages_alloc(pages, static_cast<size_t>(bytes))) {
        IODelete(pages, switch_dma_pages, 1);
        return -12;
    }
    auto *backing = switch_gpu_backing_register(pages->pages, pages->count, 0, pages, releasePages);
    if (!backing) {
        releasePages(pages);
        return -12;
    }
    if (gBacking)
        switch_gpu_backing_release(gBacking);
    gPages = pages;
    gBacking = backing;
    return 0;
}

struct Wait {
    bool queued, done;
    uint32_t pending;
    int status;
    const char *why;
};

struct Queue {
    const TegraNVJPGPlan *plan;
    const TegraNVJPGDecodeRequest *request;
    const TegraVICSurface *destination;
    SwitchVICSurfaceAccess *access;
    Wait *wait;
};

void repacked(void *context, int status)
{
    auto *wait = static_cast<Wait *>(context);
    IOLockLock(gWaitLock);
    wait->status = status;
    wait->done = true;
    IOLockWakeup(gWaitLock, wait, false);
    IOLockUnlock(gWaitLock);
}

void ceCompleted(void *context, int status)
{
    auto *wait = static_cast<Wait *>(context);
    if (status) {
        int expected = 0;
        __atomic_compare_exchange_n(&wait->status, &expected, status, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    if (__atomic_sub_fetch(&wait->pending, 1u, __ATOMIC_ACQ_REL))
        return;
    IOLockLock(gWaitLock);
    wait->done = true;
    IOLockWakeup(gWaitLock, wait, false);
    IOLockUnlock(gWaitLock);
}

void queueRepack(void *context, uint32_t syncpt, uint32_t fence)
{
    const auto *q = static_cast<const Queue *>(context);
    const TegraNVJPGPlan *p = q->plan;
    const TegraVICAcquire after = {syncpt, fence};
    SwitchVICDecodeOutput output = {};
    output.source = gBacking;
    for (uint32_t i = 0; i < 3; ++i) {
        output.planeOffset[i] = p->planeStart[i];
        output.pitch[i] = p->target.pitch[i];
        output.rows[i] = p->extent.rows[i];
    }
    output.chroma444 = p->target.format == TEGRA_NVJPG_OUT_NV24 ? 1 : 0;
    output.width = q->request->out_width;
    output.height = q->request->out_height;
    output.x = q->request->x_offset;
    output.y = q->request->y_offset;
    output.after = &after;
    q->wait->queued = switch_vic_decode_output(q->destination, &output, repacked, q->wait, &q->wait->why) == 0;
}

void queueCeRepack(void *context, uint32_t syncpt, uint32_t fence)
{
    const auto *q = static_cast<const Queue *>(context);
    const TegraNVJPGPlan *p = q->plan;
    const tegra_gm20b_ce_wait after = {syncpt, fence};
    const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, ceCompleted, q->wait};
    switch_gpu_cpu_lease *lease = q->access->owned;
    const switch_gpu_copy_ownership ownership = {lease ? &lease : nullptr, lease ? 1u : 0u, &q->access->guard, 1};
    const switch_gpu_event_submission none = {};
    __atomic_store_n(&q->wait->pending, 1u, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < p->planes; ++i) {
        const tegra_gm20b_ce_plane src = {gPages->memory, p->planeStart[i], p->target.pitch[i]};
        const tegra_gm20b_ce_plane dst = {q->destination->memory, q->destination->baseOffset + p->copyDstStart[i],
                                          p->copyDstPitch[i]};
        __atomic_add_fetch(&q->wait->pending, 1u, __ATOMIC_RELAXED);
        const int error = tegra_gm20b_ce_copy_plane_checked(
            &src, &dst, p->copyRowBytes[i], p->copyRows[i], &after, &done, gBacking, p->planeStart[i],
            q->access->backing, q->access->offset + p->copyDstStart[i], &ownership, &none);
        if (error) {
            int expected = 0;
            __atomic_compare_exchange_n(&q->wait->status, &expected, error, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            __atomic_sub_fetch(&q->wait->pending, 1u, __ATOMIC_ACQ_REL);
            if (!q->wait->queued)
                q->wait->why = "copy engine refused the re-pack";
            break;
        }
        q->wait->queued = true;
    }
    switch_vic_surface_unreserve(q->access);
    if (q->wait->queued && !__atomic_sub_fetch(&q->wait->pending, 1u, __ATOMIC_ACQ_REL)) {
        IOLockLock(gWaitLock);
        q->wait->done = true;
        IOLockWakeup(gWaitLock, q->wait, false);
        IOLockUnlock(gWaitLock);
    }
}
}  // namespace

int tegra_nvjpg_repack(TegraNVJPGJob *engine, const TegraNVJPGPlan *plan, const TegraNVJPGDecodeRequest *request,
                       const TegraVICSurface *destination, switch_gpu_backing *backing, uint64_t bitstream,
                       nvjpg_dec_status *status, uint64_t *busyUs, const char **why)
{
    *why = nullptr;
    if (!gWaitLock && !(gWaitLock = IOLockAlloc()))
        return -12;
    const bool ce = plan->repack == TEGRA_NVJPG_REPACK_CE;
    SwitchVICSurfaceAccess access = {};
    if (ce) {
        if (!destination->memory || !plan->planes || plan->planes > 2) {
            *why = "destination memory unavailable for copy-engine re-pack";
            return -22;
        }
        for (uint32_t i = 0; i < plan->planes; ++i) {
            if ((destination->baseOffset + plan->copyDstStart[i]) & 15) {
                *why = "destination plane offset not 16-byte aligned";
                return -95;
            }
        }
        if (const int error = switch_vic_surface_access(destination, true, &access, false, backing)) {
            *why = "the destination's write could not be acquired";
            return error;
        }
    }
    if (const int error = grow(plan->repackBytes)) {
        switch_vic_surface_release_access(&access);
        *why = ce ? "no memory for the planes Copy Engine re-packs" : "no memory for the planes VIC re-packs";
        return error;
    }
    const SwitchNVJPGMapping *mapping =
        switch_nvjpg_mapping(switch_nvjpg_backing(gBacking), gPages->count, plan->writeEnd, why);
    if (!mapping) {
        switch_vic_surface_release_access(&access);
        return -28;
    }
    engine->bitstream = bitstream;
    for (uint32_t i = 0; i < plan->planes; ++i)
        engine->plane[i] = mapping->address + plan->planeStart[i];
    Wait wait = {};
    Queue q = {plan, request, destination, &access, &wait};
    const TegraNVJPGFollow follow = {ce ? queueCeRepack : queueRepack, &q};
    tegra_nvjpg_lock();
    const int ret = tegra_nvjpg_run(engine, status, busyUs, &follow);
    tegra_nvjpg_unlock();
    if (wait.queued) {
        IOLockLock(gWaitLock);
        while (!wait.done)
            IOLockSleep(gWaitLock, &wait, THREAD_UNINT);
        IOLockUnlock(gWaitLock);
        switch_vic_surface_release_access(&access);
        if (wait.status)
            IOLog("TegraNVJPG: %s re-pack into the destination failed (%d); this decode fails\n",
                  ce ? "Copy Engine's" : "VIC's", wait.status);
        return ret ? ret : wait.status;
    }
    switch_vic_surface_release_access(&access);
    if (wait.why) {
        IOLog("TegraNVJPG: %s refused the re-pack into the destination (%s); this decode fails\n",
              ce ? "Copy Engine" : "VIC", wait.why);
        return ret ? ret : -5;
    }
    return ret;
}
