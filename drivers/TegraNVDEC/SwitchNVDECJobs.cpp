// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <string.h>
#include "SwitchNVDECJobs.h"
#include "SwitchNVDECQueue.h"
#include "SwitchNVDECAccess.h"
#include "TegraNVDEC.h"
#include "TegraNVDECCheck.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr uint32_t kSetupStride = 1024, kDependencies = 64, kThiMethod0 = 0x40;
constexpr uint32_t kWaitBoundUs = SwitchNVDECQueue::kSlots * 400000;
static_assert(uint32_t(SWITCH_NVDEC_READ) == uint32_t(SWITCH_GPU_BACKING_READ) &&
              uint32_t(SWITCH_NVDEC_WRITE) == uint32_t(SWITCH_GPU_BACKING_WRITE));
IOLock *gLock;
SwitchNVDECQueue gQueue;
switch_dma_buffer gSetup;
uint32_t gSyncpt;
uint64_t gCompleted;
bool gSuspended, gFailed;

struct Submission {
    TegraNVDECBlock blocks[TEGRA_NVDEC_MAX_ENTRIES] = {};
    switch_gpu_backing_use uses[TEGRA_NVDEC_MAX_ENTRIES] = {};
    switch_gpu_backing_token dependencies[kDependencies] = {};
    uint32_t words[2 + 3 * (TEGRA_NVDEC_MAX_ENTRIES + 1) + 2 * kDependencies] = {};
    uint32_t useCount = 0;
};

int status(const switch_gpu_backing_token *token)
{
    if (token->engine != SWITCH_GPU_ENGINE_NVDEC || token->queue != gSyncpt || token->reset_generation != 1)
        return -22;
    if (token->value <= __atomic_load_n(&gCompleted, __ATOMIC_ACQUIRE))
        return 1;
    return __atomic_load_n(&gFailed, __ATOMIC_ACQUIRE) ? -19 : 0;
}

int waitSequence(uint64_t sequence)
{
    IOLockLock(gLock);
    if (sequence > gQueue.submitted) {
        IOLockUnlock(gLock);
        return -22;
    }
    while (sequence > gQueue.completed && !gFailed)
        IOLockSleep(gLock, &gQueue.completed, THREAD_UNINT);
    const int result = sequence <= gQueue.completed ? 0 : -19;
    IOLockUnlock(gLock);
    return result;
}

int wait(const switch_gpu_backing_token *token)
{
    const int ready = status(token);
    if (ready)
        return ready;
    const int error = waitSequence(token->value);
    return error ? error : 1;
}

int retain(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_NVDEC && token->queue == gSyncpt && token->reset_generation == 1
               ? 0 : -22;
}

void release(const switch_gpu_backing_token *) {}

void retireThread(void *, wait_result_t)
{
    for (;;) {
        IOLockLock(gLock);
        while (!gQueue.count)
            IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
        const auto job = gQueue.jobs[gQueue.head];
        IOLockUnlock(gLock);
        const uint32_t fence = static_cast<uint32_t>(job.sequence);
        const int error = tegra_host1x_syncpt_wait(gSyncpt, fence, kWaitBoundUs);
        if (error)
            switch_nvdec_job_fault(fence, error, switch_now_us() - job.submittedUs);
        IOLockLock(gLock);
        gQueue.retire(fence);
        __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
        IOLockWakeup(gLock, &gQueue.count, false);
        IOLockWakeup(gLock, &gQueue.completed, false);
        IOLockUnlock(gLock);
    }
}

int prepare(const TegraNVDECSubmit &submit, TegraNVDECResolve resolve, void *context, Submission &work,
            const char **why)
{
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        const auto &entry = submit.entries[i];
        if (entry.kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        auto &block = work.blocks[i];
        if (!resolve(context, entry.value, &block) || !block.backing || entry.offset > block.size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = block.size - entry.offset;
    }
    *why = tegra_nvdec_check(&submit, avail);
    if (*why)
        return -22;
    for (uint32_t i = 0; i < submit.entry_count; ++i) {
        if (submit.entries[i].kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        const uint32_t access = switch_nvdec_entry_access(submit, i);
        if (!access) {
            *why = "unclassified buffer access";
            return -22;
        }
        work.uses[work.useCount++] = {work.blocks[i].backing, access, 0};
    }
    for (uint32_t i = 0; i < submit.clean_count; ++i) {
        const auto &range = submit.clean[i];
        TegraNVDECBlock block = {};
        if (!resolve(context, range.handle, &block) || !block.backing ||
            static_cast<uint64_t>(range.offset) + range.length > block.size) {
            *why = "clean range";
            return -22;
        }
        if (!range.length)
            continue;
        // The caller already wrote this range. Acquiring it must preserve those dirty CPU lines.
        constexpr uint32_t access = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
        int error = switch_gpu_backing_cpu_acquire(block.backing, range.offset, range.length, access);
        if (error)
            return error;
        error = switch_gpu_backing_cpu_publish(block.backing, range.offset, range.length);
        const int released = switch_gpu_backing_cpu_release(block.backing, range.offset, range.length, access);
        if (error || released)
            return error ? error : released;
    }
    return 0;
}

int waitUses(const Submission &work)
{
    for (uint32_t i = 0; i < work.useCount; ++i) {
        const int error = switch_gpu_backing_wait(work.uses[i].backing, work.uses[i].access);
        if (error)
            return error;
    }
    return 0;
}

int publish(const TegraNVDECSubmit &submit, Submission &work, uint32_t *fence)
{
    for (;;) {
        IOLockLock(gLock);
        while ((gSuspended || gQueue.full()) && !gFailed)
            IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
        if (gFailed || gQueue.next() == 0) {
            IOLockUnlock(gLock);
            return -19;
        }
        const switch_gpu_backing_token token = {gQueue.next(), 1, SWITCH_GPU_ENGINE_NVDEC, gSyncpt};
        switch_gpu_backing_transaction transaction = {};
        uint32_t needed = 0;
        int error = switch_gpu_backing_begin(work.uses, work.useCount, &token, &transaction);
        if (!error) {
            work.useCount = transaction.count;
            error = switch_gpu_backing_dependencies(&transaction, work.dependencies, kDependencies, &needed);
        }
        bool softwareWait = error == -28;
        uint32_t n = 0;
        for (uint32_t i = 0; !error && i < needed; ++i) {
            const auto &dependency = work.dependencies[i];
            const int ready = switch_gpu_backing_token_status(&dependency);
            if (ready == 1)
                continue;
            if (ready < 0) {
                error = ready;
                break;
            }
            if (dependency.engine == SWITCH_GPU_ENGINE_NVDEC)
                continue;
            softwareWait = true;
        }
        if (error || softwareWait) {
            if (transaction.locked)
                switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error && error != -28)
                return error;
            if (const int waited = waitUses(work))
                return waited;
            continue;
        }
        const uint64_t slot = static_cast<uint64_t>(gQueue.tail()) * kSetupStride;
        memcpy(gSetup.va + slot, submit.setup, submit.setup_bytes);
        switch_dcache_clean(gSetup.va + slot, submit.setup_bytes);
        work.words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0);
        const auto method = [&](uint32_t offset, uint32_t value) {
            work.words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
            work.words[n++] = offset / 4;
            work.words[n++] = value;
        };
        for (uint32_t i = 0; i < submit.entry_count; ++i) {
            const auto &entry = submit.entries[i];
            if (i + 1 == submit.entry_count)
                method(NVC5B0_SET_DRV_PIC_SETUP_OFFSET, static_cast<uint32_t>((gSetup.pa + slot) >> 8));
            method(entry.method, entry.kind == TEGRA_NVDEC_ENTRY_RELOC
                                     ? static_cast<uint32_t>((work.blocks[i].iova + entry.offset) >> 8) : entry.value);
        }
        work.words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
        const uint64_t began = switch_now_us();
        error = tegra_host1x_submit(work.words, n);
        if (!error) {
            gQueue.publish(began);
            switch_gpu_backing_commit(&transaction);
            *fence = static_cast<uint32_t>(gQueue.submitted);
            IOLockWakeup(gLock, &gQueue.count, false);
        } else {
            switch_gpu_backing_abort(&transaction);
        }
        IOLockUnlock(gLock);
        return error;
    }
}
}  // namespace

bool switch_nvdec_jobs_start(const switch_dma_buffer &setup, uint32_t syncpt, uint32_t completed)
{
    if (setup.size < SwitchNVDECQueue::kSlots * kSetupStride || !(gLock = IOLockAlloc()))
        return false;
    gSetup = setup;
    gSyncpt = syncpt;
    gQueue.submitted = gQueue.completed = completed;
    __atomic_store_n(&gCompleted, completed, __ATOMIC_RELEASE);
    const switch_gpu_backing_engine operations = {status, wait, retain, release, 64};
    if (switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_NVDEC, &operations)) {
        IOLockFree(gLock);
        gLock = nullptr;
        return false;
    }
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(retireThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLockFree(gLock);
        gLock = nullptr;
        return false;
    }
    thread_deallocate(thread);
    return true;
}

extern "C" int tegra_nvdec_run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context,
                               uint32_t *fence, const char **why)
{
    if (!why)
        return -22;
    *why = nullptr;
    if (!gLock || !tegra_nvdec_ready())
        return -19;
    if (!submit || !resolve || !fence || submit->entry_count > TEGRA_NVDEC_MAX_ENTRIES ||
        submit->clean_count > TEGRA_NVDEC_MAX_RANGES || submit->setup_bytes > kSetupStride) {
        *why = "counts";
        return -22;
    }
    auto *work = IONewZero(Submission, 1);
    if (!work)
        return -12;
    int error = prepare(*submit, resolve, context, *work, why);
    if (!error)
        error = publish(*submit, *work, fence);
    IODelete(work, Submission, 1);
    return error;
}

extern "C" int tegra_nvdec_wait(uint32_t fence)
{
    if (!gLock)
        return -19;
    uint64_t sequence = 0;
    IOLockLock(gLock);
    const bool valid = gQueue.legacySequence(fence, &sequence);
    IOLockUnlock(gLock);
    return valid ? waitSequence(sequence) : -22;
}

void switch_nvdec_jobs_suspend()
{
    IOLockLock(gLock);
    gSuspended = true;
    while (gQueue.count)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    IOLockUnlock(gLock);
}

void switch_nvdec_jobs_resume(bool ready, uint32_t completed)
{
    IOLockLock(gLock);
    if (!gQueue.rebase(completed))
        panic("SwitchNVDEC: syncpoint reset lost its completed timeline");
    __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
    __atomic_store_n(&gFailed, !ready, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gLock, &gQueue.count, false);
    IOLockWakeup(gLock, &gQueue.completed, false);
    IOLockUnlock(gLock);
}
