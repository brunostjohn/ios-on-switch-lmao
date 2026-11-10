// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <string.h>
#include "SwitchVICJobs.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
IOLock *gLock;
SwitchVICQueue gQueue;
TegraVICBuffer gConfigs;
uint32_t gSyncpt;
bool gSuspended, gFailed;
uint64_t gCompleted;
constexpr uint32_t kThiMethod0 = 0x40;
constexpr uint64_t kQueueUs = 2000000, kMaxBoundUs = 30000000, kAcquireUs = 2000000;
constexpr uint32_t kBatchMax = 3, kDependencies = 32;

int tokenStatus(const switch_gpu_backing_token *token)
{
    if (token->engine != SWITCH_GPU_ENGINE_VIC || token->queue != gSyncpt || token->reset_generation != 1)
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

int tokenWait(const switch_gpu_backing_token *token)
{
    const int status = tokenStatus(token);
    if (status)
        return status;
    const int error = waitSequence(token->value);
    return error ? error : 1;
}

int tokenRetain(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC && token->queue == gSyncpt && token->reset_generation == 1 ? 0 : -22;
}

void tokenRelease(const switch_gpu_backing_token *) {}

uint32_t bound(const TegraVICJob &job, bool acquire)
{
    const uint64_t us = kQueueUs + job.pixels / 8;
    return static_cast<uint32_t>((us < kMaxBoundUs ? us : kMaxBoundUs) + (acquire ? kAcquireUs : 0));
}

void retireThread(void *, wait_result_t)
{
    for (;;) {
        IOLockLock(gLock);
        while (!gQueue.count)
            IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
        const SwitchVICQueue::Job job = gQueue.jobs[gQueue.head];
        IOLockUnlock(gLock);
        // This thread is the only threshold waiter on VIC's syncpoint. Other waiters sleep on completed below.
        const int status = tegra_host1x_syncpt_wait(gSyncpt, static_cast<uint32_t>(job.fence), job.boundUs);
        if (status) {
            char engine[256], host[200];
            tegra_vic_describe(engine, sizeof(engine));
            tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
            panic("TegraVIC: fence %llu failed (%d) after %llu us; %s; %s", job.fence, status,
                  switch_now_us() - job.submittedUs, engine, host);
        }
        IOLockLock(gLock);
        gQueue.retire(static_cast<uint32_t>(job.fence));
        __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
        IOLockWakeup(gLock, &gQueue.count, false);
        IOLockWakeup(gLock, &gQueue.completed, false);
        IOLockUnlock(gLock);
        if (job.completion)
            job.completion(job.context, 0);
    }
}
}  // namespace

bool switch_vic_jobs_start(const TegraVICBuffer &configs, uint32_t syncpt, uint32_t completed)
{
    if (configs.size < kSwitchVICConfigBytes || !(gLock = IOLockAlloc()))
        return false;
    gConfigs = configs;
    gSyncpt = syncpt;
    gQueue.reset(completed);
    __atomic_store_n(&gCompleted, completed, __ATOMIC_RELEASE);
    const switch_gpu_backing_engine operations = {tokenStatus, tokenWait, tokenRetain, tokenRelease, 64};
    if (switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &operations))
        return false;
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(retireThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLockFree(gLock);
        gLock = nullptr;
        return false;
    }
    thread_deallocate(thread);
    return true;
}

extern "C" int switch_vic_submit_after(const TegraVICJob *job, const TegraVICAcquire *acquire,
                                        void (*completion)(void *, int), void *context, uint32_t *fence)
{
    return switch_vic_submit_batch(job, 1, acquire, nullptr, 0, completion, context, fence);
}

int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount,
                            void (*completion)(void *, int), void *context, uint32_t *fence)
{
    if (!gLock)
        return -19;
    if (!jobs || !count || count > kBatchMax || (useCount && !uses))
        return -22;
    for (uint32_t j = 0; j < count; ++j) {
        if (jobs[j].count > TEGRA_VIC_MAX_METHODS)
            return -22;
        uint32_t configs = 0;
        for (uint32_t i = 0; i < jobs[j].count; ++i)
            configs += jobs[j].method[i] == TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET;
        if (configs != 1)
            return -22;
    }
retry:
    IOLockLock(gLock);
    while ((gSuspended || gQueue.count + count > SwitchVICQueue::kSlots) && !gFailed)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    if (gFailed || !gQueue.canPublish(count)) {
        IOLockUnlock(gLock);
        return -19;
    }
    switch_gpu_backing_transaction transaction = {};
    switch_gpu_backing_token dependencies[kDependencies] = {};
    uint32_t needed = 0;
    if (useCount) {
        const switch_gpu_backing_token token = {gQueue.submitted + count, 1,
                                                SWITCH_GPU_ENGINE_VIC, gSyncpt};
        int error = switch_gpu_backing_begin(uses, useCount, &token, &transaction);
        if (!error) {
            useCount = transaction.count;
            error = switch_gpu_backing_dependencies(&transaction, dependencies, kDependencies, &needed);
        }
        if (error) {
            if (transaction.locked)
                switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error != -28)
                return error;
            for (uint32_t u = 0; u < useCount; ++u) {
                const int waited = switch_gpu_backing_wait(uses[u].backing, uses[u].access);
                if (waited < 0)
                    return waited;
            }
            goto retry;
        }
        for (uint32_t i = 0; i < needed; ++i) {
            const int ready = switch_gpu_backing_token_status(&dependencies[i]);
            if (ready == 1) {
                dependencies[i] = {};
                continue;
            }
            if (!ready && (dependencies[i].engine == SWITCH_GPU_ENGINE_VIC ||
                           dependencies[i].engine == SWITCH_GPU_ENGINE_NVDEC))
                continue;
            switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (ready < 0)
                return ready;
            for (uint32_t u = 0; u < useCount; ++u) {
                const int waited = switch_gpu_backing_wait(uses[u].backing, uses[u].access);
                if (waited < 0)
                    return waited;
            }
            goto retry;
        }
    }
    uint32_t words[kBatchMax * (2 + 3 * TEGRA_VIC_MAX_METHODS) + 2 + 2 * kDependencies], n = 0;
    if (acquire)
        n += tegra_host1x_op_wait_syncpt(words, acquire->syncpt, acquire->threshold);
    for (uint32_t i = 0; i < needed; ++i) {
        if (dependencies[i].engine == SWITCH_GPU_ENGINE_NVDEC)
            n += tegra_host1x_op_wait_syncpt(words + n, dependencies[i].queue,
                                            static_cast<uint32_t>(dependencies[i].value));
    }
    for (uint32_t j = 0; j < count; ++j) {
        const auto &job = jobs[j];
        const uint64_t offset = static_cast<uint64_t>(gQueue.configurationSlot(j)) * kSwitchVICConfigStride;
        memcpy(gConfigs.va + offset, job.config, TEGRA_VIC_CONFIG_BYTES);
        switch_dcache_clean(gConfigs.va + offset, TEGRA_VIC_CONFIG_BYTES);
        words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_VIC, 0, 0);
        for (uint32_t i = 0; i < job.count; ++i) {
            words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
            words[n++] = job.method[i] / 4;
            words[n++] = job.method[i] == TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET
                             ? static_cast<uint32_t>((gConfigs.pa + offset) >> 8) : job.value[i];
        }
        words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    }
    const uint64_t started = switch_now_us();
    const int status = tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_VIC, words, n);
    if (!status) {
        for (uint32_t j = 0; j < count; ++j)
            gQueue.publish({0, bound(jobs[j], acquire != nullptr || needed != 0), started,
                            j + 1 == count ? completion : nullptr, context});
        if (useCount)
            switch_gpu_backing_commit(&transaction);
        if (fence)
            *fence = static_cast<uint32_t>(gQueue.submitted);
        IOLockWakeup(gLock, &gQueue.count, false);
    } else if (useCount) {
        switch_gpu_backing_abort(&transaction);
    }
    IOLockUnlock(gLock);
    return status;
}

extern "C" int switch_vic_wait(uint32_t fence)
{
    if (!gLock)
        return -19;
    uint64_t sequence = 0;
    IOLockLock(gLock);
    const bool valid = gQueue.legacySequence(fence, &sequence);
    IOLockUnlock(gLock);
    return valid ? waitSequence(sequence) : -22;
}

void switch_vic_jobs_suspend()
{
    IOLockLock(gLock);
    gSuspended = true;
    while (gQueue.count)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    IOLockUnlock(gLock);
}

void switch_vic_jobs_resume(bool ready, uint32_t completed)
{
    IOLockLock(gLock);
    const bool resumed = ready && gQueue.rebase(completed);
    __atomic_store_n(&gFailed, !resumed, __ATOMIC_RELEASE);
    __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gLock, &gQueue.count, false);
    IOLockWakeup(gLock, &gQueue.completed, false);
    IOLockUnlock(gLock);
}
