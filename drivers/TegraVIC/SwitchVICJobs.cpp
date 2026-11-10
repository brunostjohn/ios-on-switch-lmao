// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <string.h>
#include "SwitchVICJobs.h"
#include "SwitchVICDependencies.h"
#include "SwitchVICEvents.h"
#include "SwitchVICSemantics.h"
#include "../TegraHost1x/SwitchHost1xResults.h"
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
constexpr uint32_t kBatchMax = 3;
uint32_t
    gWords[kBatchMax * (2 + 3 * TEGRA_VIC_MAX_METHODS) + SWITCH_HOST1X_WAIT_WORDS * (2 + SWITCH_GPU_BRIDGE_MAX_TOKENS)];

using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;

int tokenStatus(const switch_gpu_backing_token *token)
{
    return Results::status(token);
}

int waitSequence(uint64_t sequence)
{
    return Results::waitLegacy(sequence);
}

int tokenWait(const switch_gpu_backing_token *token)
{
    return Results::wait(token);
}

int tokenRetain(const switch_gpu_backing_token *token)
{
    return Results::retain(token);
}

void tokenRelease(const switch_gpu_backing_token *token)
{
    Results::release(token);
}

uint32_t bound(const TegraVICJob &job, bool acquire)
{
    const uint64_t us = kQueueUs + job.pixels / 8;
    return static_cast<uint32_t>((us < kMaxBoundUs ? us : kMaxBoundUs) + (acquire ? kAcquireUs : 0));
}

int completeJob(SwitchVICQueue::Job &job)
{
    int result = job.bridge ? switch_gpu_bridge_wait(job.bridge) : 1;
    switch_gpu_bridge_release(&job.bridge);
    Results::physical(job.outcome, result);
    switch_vic_semantics_physical(0);
    if (job.semantics) {
        const int semantic = switch_gpu_event_waits_wait(job.semantics, 0);
        if (semantic < 0)
            result = semantic;
        switch_vic_semantics_retire(job.semantics);
        switch_gpu_event_waits_release(&job.semantics);
    }
    Results::complete(&job.outcome, result);
    return result < 0 ? result : 0;
}

void retireThread(void *, wait_result_t)
{
    for (;;) {
        IOLockLock(gLock);
        while (!gQueue.count)
            IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
        SwitchVICQueue::Job job = gQueue.jobs[gQueue.head];
        const switch_gpu_bridge *bridge = gQueue.batchEnd().bridge;
        IOLockUnlock(gLock);
        // The bound is VIC's own work: a GPU producer the stream waits for, slow or killed (its recovery advances the
        // fence the bridge acquires), is waited for first, on the bridge's completion.
        const int producers = bridge ? switch_gpu_bridge_wait(bridge) : 1;
        // This thread is the only threshold waiter on VIC's syncpoint. Other waiters sleep on completed below.
        const int status = tegra_host1x_syncpt_wait(gSyncpt, static_cast<uint32_t>(job.fence), job.boundUs);
        if (status) {
            char engine[256], host[200];
            tegra_vic_describe(engine, sizeof(engine));
            tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
            panic("TegraVIC: fence %llu failed (%d) after %llu us, GPU producers %d; %s; %s", job.fence, status,
                  switch_now_us() - job.submittedUs, producers, engine, host);
        }
        const int result = completeJob(job);
        IOLockLock(gLock);
        gQueue.retire(static_cast<uint32_t>(job.fence));
        __atomic_store_n(&gCompleted, gQueue.completed, __ATOMIC_RELEASE);
        IOLockWakeup(gLock, &gQueue.count, false);
        IOLockWakeup(gLock, &gQueue.completed, false);
        IOLockUnlock(gLock);
        switch_gpu_backing_completed();
        if (job.completion)
            job.completion(job.context, result < 0 ? result : 0);
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
    if (switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &operations) || !switch_vic_semantics_start(syncpt))
        return false;
    switch_vic_semantics_physical(completed);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(retireThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLockFree(gLock);
        gLock = nullptr;
        return false;
    }
    thread_deallocate(thread);
    return true;
}

int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, void (*completion)(void *, int),
                            void *context, uint32_t *fence)
{
    return switch_vic_submit_owned(jobs, count, acquire, uses, useCount, nullptr, 0, completion, context, fence);
}

int switch_vic_submit_owned(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount, switch_gpu_cpu_lease *const *leases,
                            uint32_t leaseCount, void (*completion)(void *, int), void *context, uint32_t *fence)
{
    const SwitchVICOwnership ownership = {uses, useCount, nullptr, 0, leases, leaseCount};
    return switch_vic_submit_checked(jobs, count, acquire, ownership, completion, context, fence);
}

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *fence, SwitchVICEvents *events)
{
    auto *uses = ownership.uses;
    uint32_t useCount = ownership.useCount;
    auto *const *leases = ownership.leases;
    const uint32_t leaseCount = ownership.leaseCount;
    if (!gLock)
        return -19;
    if (!jobs || !count || count > kBatchMax || (useCount && !uses) ||
        (ownership.guardCount && (!ownership.guards || !useCount)) || (leaseCount && (!leases || !useCount)))
        return -22;
    // A job's output is a recorded write: it moves the allocation's host epoch, so the GPU's next reader invalidates its
    // L2 before reading what VIC wrote.
    bool writes = false;
    for (uint32_t i = 0; i < useCount; ++i)
        writes |= (uses[i].access & SWITCH_GPU_BACKING_WRITE) != 0;
    if (!writes)
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
    SwitchVICDependencies dependencies;

    Results::Reservation outcome;

    struct Batch {
        switch_gpu_event_waits *value = nullptr;

        ~Batch()
        {
            switch_gpu_event_waits_abort(&value);
        }
    } batch;

    batch.value = switch_vic_events_batch(events);
    switch_gpu_event_waits_retain(batch.value);
    if (!batch.value)
        if (const int error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value))
            return error;
    const switch_gpu_backing_token *eventTokens = nullptr;
    const uint32_t eventCount = switch_vic_events_tokens(events, &eventTokens);
retry:
    if (const int error = outcome.renew())
        return error;
    IOLockLock(gLock);
    while ((gSuspended || gQueue.count + count > SwitchVICQueue::kSlots) && !gFailed)
        IOLockSleep(gLock, &gQueue.count, THREAD_UNINT);
    if (gFailed || !gQueue.canPublish(count)) {
        IOLockUnlock(gLock);
        return -19;
    }
    switch_gpu_backing_transaction transaction = {};
    uint32_t needed = 0;
    switch_gpu_backing_token token = {};
    if (const int error = Results::prepare(outcome.value, gQueue.submitted + 1, gQueue.submitted + count, &token)) {
        IOLockUnlock(gLock);
        return error;
    }
    int error = 0;
    if (useCount) {
        error = leaseCount
                    ? switch_gpu_backing_begin_conversion_checked(uses, useCount, leases, leaseCount, ownership.guards,
                                                                  ownership.guardCount, &token, &transaction)
                    : switch_gpu_backing_begin_checked(uses, useCount, ownership.guards, ownership.guardCount, &token,
                                                       &transaction);
        if (!error) {
            useCount = transaction.count;
            error = switch_gpu_backing_dependencies(&transaction, dependencies.tokens,
                                                    dependencies.capacity - eventCount, &needed);
        }
        if (error) {
            if (transaction.locked)
                switch_gpu_backing_abort(&transaction);
            IOLockUnlock(gLock);
            if (error != -28)
                return error;
            if (const int grown = dependencies.grow(needed + eventCount))
                return grown;
            goto retry;
        }
    }
    for (uint32_t i = 0; i < eventCount; ++i)
        dependencies.tokens[needed++] = eventTokens[i];
    bool capture = false;
    for (uint32_t i = 0; i < needed && !error; ++i) {
        const int covered = switch_gpu_event_waits_covers_token(batch.value, &dependencies.tokens[i]);
        capture |= covered == 0;
        if (covered < 0)
            error = covered;
    }
    if (error) {
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        return error;
    }
    if (capture) {
        error = dependencies.holdAll(needed);
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        if (error)
            return error;
        switch_gpu_event_waits *inferred = nullptr;
        error = switch_gpu_event_waits_prepare_tokens(dependencies.tokens, needed, &inferred);
        dependencies.releaseTokens();
        if (!error && !batch.value)
            error = switch_gpu_event_waits_prepare(nullptr, 0, &batch.value);
        if (!error && inferred)
            error = switch_gpu_event_waits_merge_uncommitted(batch.value, inferred);
        switch_gpu_event_waits_abort(&inferred);
        if (error)
            return error;
        goto retry;
    }
    bool bridgeRequired = false;
    for (uint32_t i = 0; i < needed; ++i) {
        auto &dependency = dependencies.tokens[i];
        const int ready = switch_gpu_backing_token_external_status(&dependency);
        if (ready == 1) {
            dependency = {};
            continue;
        }
        if (!ready && (dependency.engine == SWITCH_GPU_ENGINE_VIC || dependency.engine == SWITCH_GPU_ENGINE_NVDEC))
            continue;
        if (!ready && dependency.engine == SWITCH_GPU_ENGINE_GM20B) {
            const int covered = dependencies.covered(dependency);
            if (covered >= 0) {
                bridgeRequired |= covered == 0;
                continue;
            }
            error = covered;
        } else {
            error = ready < 0 ? ready : -95;
        }
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        return error;
    }
    if (bridgeRequired) {
        error = dependencies.holdGpu(needed);
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        if (error)
            return error;
        if (const int submitted = dependencies.submitBridge())
            return submitted;
        goto retry;
    }
    SwitchVICSemantic *semantic = nullptr;
    error = switch_vic_semantics_bind(batch.value, &token, &semantic);
    if (!error)
        error = switch_vic_events_bind(events, &token);
    if (error) {
        switch_gpu_backing_abort(&transaction);
        IOLockUnlock(gLock);
        return error;
    }
    uint32_t *words = gWords;
    uint32_t n = 0, bridgeSyncpoint = 0, bridgeThreshold = 0;
    if (dependencies.acquire(&bridgeSyncpoint, &bridgeThreshold))
        n += tegra_host1x_op_wait_syncpt(words + n, bridgeSyncpoint, bridgeThreshold);
    if (acquire)
        n += tegra_host1x_op_wait_syncpt(words + n, acquire->syncpt, acquire->threshold);
    for (uint32_t i = 0; i < needed; ++i) {
        if (dependencies.tokens[i].engine == SWITCH_GPU_ENGINE_NVDEC)
            n += tegra_host1x_op_wait_syncpt(words + n, dependencies.tokens[i].queue,
                                             static_cast<uint32_t>(dependencies.tokens[i].value));
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
                             ? static_cast<uint32_t>((gConfigs.pa + offset) >> 8)
                             : job.value[i];
        }
        words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    }
    const uint64_t started = switch_now_us();
    const int status = tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_VIC, words, n);
    if (!status) {
        for (uint32_t j = 0; j < count; ++j)
            gQueue.publish({0, bound(jobs[j], acquire != nullptr || needed != 0), started,
                            j + 1 == count ? completion : nullptr, context,
                            j + 1 == count ? dependencies.takeBridge() : nullptr,
                            j + 1 == count ? batch.value : nullptr, j + 1 == count ? outcome.take() : nullptr,
                            count - 1 - j});
        if (batch.value)
            switch_gpu_event_waits_retain(batch.value);
        if (useCount)
            switch_gpu_backing_commit(&transaction);
        switch_vic_semantics_commit(semantic);
        switch_gpu_event_waits_commit(&batch.value);
        switch_vic_events_commit(events);
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
    switch_vic_semantics_physical(gQueue.completed);
    switch_gpu_backing_completed();
}
