// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include "SwitchGpuBridge.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BSyncpt.h"
#include "../TegraHost1x/TegraHost1x.h"

struct switch_gpu_bridge {
    switch_gpu_backing_token tokens[SWITCH_GPU_BRIDGE_MAX_TOKENS];
    uint32_t count, refs, syncpoint, threshold, sequence;
    bool active;
    int result;
    void (*completed)(void *, int);
    void *context;
};

namespace {
constexpr uint32_t kSlots = 8;
constexpr uint64_t kPage = 4096, kPushVa = 1ULL << 20;
struct BridgeQueue {
    IOLock *lock;
    thread_call_t retirement;
    tgm_vm *vm;
    tgm_chan *channel;
    tgm_mem *push, *fence;
    uint32_t *words;
    volatile uint32_t *fenceCpu;
    uint64_t fenceGpu, fencePage, pushPa, pages[kSlots];
    uint32_t waitingSources;
    switch_gpu_bridge jobs[kSlots];
} queue;

void releaseLocked(switch_gpu_bridge &job)
{
    if (--job.refs)
        return;
    for (uint32_t i = 0; i < job.count; ++i)
        switch_gpu_backing_token_release(&job.tokens[i]);
    job.count = 0;
    IOLockWakeup(queue.lock, &queue.jobs, false);
}

int producers(const switch_gpu_bridge &job)
{
    bool pending = false;
    for (uint32_t i = 0; i < job.count; ++i) {
        const int result = switch_gpu_backing_token_status(&job.tokens[i]);
        if (result < 0)
            return result;
        pending |= result == 0;
    }
    return pending ? 0 : 1;
}

void retire(thread_call_param_t, thread_call_param_t)
{
    struct Completion { switch_gpu_bridge *job; void (*fn)(void *, int); void *context; int result; };
    Completion callbacks[kSlots];
    uint32_t count = 0;
    IOLockLock(queue.lock);
    __atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_RELEASE);
    bool pendingSources = false;
    int failed = 0;
    const uint32_t completed = queue.channel ? tgm_chan_settled(queue.channel, &failed) : 0;
    for (auto &job : queue.jobs) {
        if (!job.active || (!failed && int32_t(completed - job.sequence) < 0))
            continue;
        const int source = producers(job);
        if (!failed && !source) {
            pendingSources = true;
            continue;
        }
        job.result = failed ? -5 : source == 1 ? 1 : source < 0 ? source : -5;
	if (failed) {
		const int released = tegra_host1x_syncpt_release_failed(
			job.syncpoint, job.threshold, TGM_SYNCPT_INCRS);
		// Whatever waits on the old syncpoint fails or times out on its own engine; this slot no longer shares it.
		if (released) {
			IOLog("SwitchGpuBridge: syncpoint %u of a failed job stays short of %u "
			      "(%d); its slot takes "
			      "another\n",
			      job.syncpoint, job.threshold, released);
			job.syncpoint = 0;
		}
	}
	if (job.result == 1)
            for (uint32_t i = 0; i < job.count; ++i)
                if (job.tokens[i].engine == SWITCH_GPU_ENGINE_GM20B)
                    (void)switch_gpu_timeline_promote(&job.tokens[i]);
        job.active = false;
        IOLockWakeup(queue.lock, &job, false);
        releaseLocked(job);
    }
    for (auto &job : queue.jobs) {
        if (!job.active && job.completed) {
            callbacks[count++] = {&job, job.completed, job.context, job.result};
            job.completed = nullptr;
        }
    }
    __atomic_store_n(&queue.waitingSources, pendingSources, __ATOMIC_RELEASE);
    IOLockUnlock(queue.lock);
    for (uint32_t i = 0; i < count; ++i) {
        const auto &callback = callbacks[i];
        callback.fn(callback.context, callback.result == 1 ? 0 : callback.result);
        IOLockLock(queue.lock);
        releaseLocked(*callback.job);
        IOLockUnlock(queue.lock);
    }
}

void completed(void *)
{
    thread_call_enter(queue.retirement);
}

bool initialize()
{
    if (__atomic_load_n(&queue.lock, __ATOMIC_ACQUIRE))
        return true;
    thread_call_t retirement = thread_call_allocate_with_options(retire, nullptr,
        THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    IOLock *lock = retirement ? IOLockAlloc() : nullptr;
    if (!lock) {
        if (retirement)
            thread_call_free(retirement);
        return false;
    }
    thread_call_t noCall = nullptr;
    if (!__atomic_compare_exchange_n(&queue.retirement, &noCall, retirement, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        thread_call_free(retirement);
    IOLock *noLock = nullptr;
    if (!__atomic_compare_exchange_n(&queue.lock, &noLock, lock, false,
            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        IOLockFree(lock);
    return true;
}

void discardSpace()
{
	tgm_vm_del(&queue.vm);
	tgm_mem_del(&queue.push);
	tgm_mem_del(&queue.fence);
	if (queue.words)
		tgm_pool_free(queue.words, kPage * kSlots, queue.pushPa);
	queue.words = nullptr;
}

// A syncpoint stays with its slot across a failed open, as host1x hands none back.
bool openSpace()
{
    uint32_t fenceOffset = 0;
    queue.words = static_cast<uint32_t *>(tgm_pool_alloc(kPage * kSlots, 0, &queue.pushPa));
    if (!queue.words ||
        tgm_fence_kernel(TGM_FENCE_KERNEL_BRIDGE, &queue.fenceCpu, &queue.fencePage, &fenceOffset) ||
        tgm_vm_new(&queue.vm))
        return false;
    tgm_vm_claim(queue.vm, &queue);
    for (uint32_t i = 0; i < kSlots; ++i) {
	    queue.pages[i] = queue.pushPa + i * kPage;
	    if (!queue.jobs[i].syncpoint && tegra_host1x_syncpt_alloc(&queue.jobs[i].syncpoint))
		    return false;
    }
    if (tgm_mem_new(queue.vm, queue.pages, kSlots, &queue.push) ||
        tgm_vm_map_ordered(queue.vm, kPushVa, kPage * kSlots, queue.push, 0, 0, 1, nullptr) ||
        tgm_mem_new(queue.vm, &queue.fencePage, 1, &queue.fence) ||
        tgm_vm_kmap(queue.vm, queue.fence, kPage, &queue.fenceGpu))
        return false;
    queue.fenceGpu += fenceOffset;
    return true;
}

int channel()
{
    if (queue.channel && tgm_chan_error(queue.channel)) {
        for (const auto &job : queue.jobs)
            if (job.refs)
                return -16;
        tgm_chan_del(&queue.channel);
    }
    if (!queue.vm && !openSpace()) {
	    discardSpace();
	    return -12;
    }
    if (!queue.channel) {
        const int result = tgm_chan_new_copy(queue.vm, &queue, queue.fenceCpu, queue.fenceGpu,
                                        "host1x-bridge", 0, &queue.channel);
        if (result)
            return result;
        tgm_chan_on_fence(queue.channel, completed, nullptr);
    }
    return 0;
}
}

extern "C" int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count,
                                          switch_gpu_bridge **out)
{
    if (!out || !tokens || !count || count > SWITCH_GPU_BRIDGE_MAX_TOKENS)
        return -22;
    *out = nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if ((tokens[i].engine != SWITCH_GPU_ENGINE_GM20B && tokens[i].engine != SWITCH_GPU_ENGINE_VIC &&
             tokens[i].engine != SWITCH_GPU_ENGINE_NVDEC && tokens[i].engine != SWITCH_GPU_ENGINE_DISPLAY) ||
            !tokens[i].reset_generation ||
            (tokens[i].engine == SWITCH_GPU_ENGINE_GM20B && tokens[i].value > UINT32_MAX))
            return -22;
    if (!initialize())
        return -12;
    IOLockLock(queue.lock);
    int error = channel();
    uint32_t slot = 0;
    while (slot < kSlots && queue.jobs[slot].refs)
        ++slot;
    if (!error && slot < kSlots && !queue.jobs[slot].syncpoint &&
	tegra_host1x_syncpt_alloc(&queue.jobs[slot].syncpoint))
	    error = -12;
    if (error || slot == kSlots) {
        IOLockUnlock(queue.lock);
        return error ? error : -16;
    }
    auto &job = queue.jobs[slot];
    job.refs = 1;
    job.result = 0;
    for (uint32_t i = 0; i < count; ++i) {
        error = switch_gpu_backing_token_retain(&tokens[i]);
        if (error)
            break;
        job.tokens[job.count++] = tokens[i];
    }
    if (!error) {
        const int source = producers(job);
        if (source < 0)
            error = source;
    }
    if (!error) {
        job.threshold = tegra_host1x_syncpt_read(job.syncpoint) + TGM_SYNCPT_INCRS;
        const tgm_push push = {kPushVa + slot * kPage,
            tgm_syncpt_incr(queue.words + slot * (kPage / 4), job.syncpoint), 0, 0, 0};
        switch_gpu_chan_submit_enter(queue.channel);
        for (;;) {
            error = switch_gpu_chan_exec(queue.channel, &push, 1, job.tokens, job.count,
                                         &job.sequence, nullptr);
            if (error != -11)
                break;
            error = switch_gpu_chan_wait_submission_room(queue.channel, &push, 1, job.count, nullptr);
            if (error)
                break;
        }
        switch_gpu_chan_submit_leave();
    }
    if (error) {
        releaseLocked(job);
    } else {
        job.active = true;
        ++job.refs;
        *out = &job;
    }
    IOLockUnlock(queue.lock);
    return error;
}

extern "C" void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *bridge, uint32_t *syncpoint, uint32_t *threshold)
{
    *syncpoint = bridge->syncpoint;
    *threshold = bridge->threshold;
}

extern "C" int switch_gpu_bridge_covers(const switch_gpu_bridge *bridge, const switch_gpu_backing_token *token)
{
    if (!bridge || !token)
        return -22;
    const int source = switch_gpu_backing_token_external_status(token);
    if (source)
        return source;
    for (uint32_t i = 0; i < bridge->count; ++i) {
        const auto &held = bridge->tokens[i];
        if (token->engine == held.engine && token->queue == held.queue &&
            token->reset_generation == held.reset_generation &&
            (held.engine == SWITCH_GPU_ENGINE_GM20B ? int32_t(uint32_t(held.value - token->value)) >= 0
                                                   : held.value >= token->value))
            return 1;
    }
    return 0;
}

extern "C" void switch_gpu_bridge_completed(void)
{
    if (__atomic_load_n(&queue.waitingSources, __ATOMIC_ACQUIRE))
        thread_call_enter(queue.retirement);
}

extern "C" int switch_gpu_bridge_status(const switch_gpu_bridge *bridge)
{
    if (!bridge)
        return -22;
    IOLockLock(queue.lock);
    int result = bridge->result;
    if (!result) {
        result = producers(*bridge);
        if (result == 1 && int32_t(tegra_host1x_syncpt_read(bridge->syncpoint) - bridge->threshold) < 0)
            result = 0;
    }
    IOLockUnlock(queue.lock);
    return result;
}

extern "C" int switch_gpu_bridge_wait(const switch_gpu_bridge *bridge)
{
    if (!bridge)
        return -22;
    IOLockLock(queue.lock);
    while (bridge->active)
        IOLockSleep(queue.lock, const_cast<switch_gpu_bridge *>(bridge), THREAD_UNINT);
    const int result = bridge->result;
    IOLockUnlock(queue.lock);
    return result;
}

extern "C" void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    if (!bridge || !*bridge)
        return;
    IOLockLock(queue.lock);
    releaseLocked(**bridge);
    *bridge = nullptr;
    IOLockUnlock(queue.lock);
}

extern "C" int switch_gpu_bridge_wait_room(void)
{
    if (!initialize())
        return -12;
    IOLockLock(queue.lock);
    for (;;) {
        uint32_t held = 0;
        for (const auto &job : queue.jobs)
            held += job.refs != 0;
        const bool failed = queue.channel && tgm_chan_error(queue.channel);
        if (failed ? held == 0 : held < kSlots)
            break;
        IOLockSleep(queue.lock, &queue.jobs, THREAD_UNINT);
    }
    IOLockUnlock(queue.lock);
    return 0;
}

extern "C" int switch_gpu_bridge_fence(const switch_gpu_backing_token *tokens, uint32_t count,
        void (*callback)(void *, int), void *context)
{
    if (!callback)
        return -22;
    switch_gpu_bridge *bridge = nullptr;
    const int error = switch_gpu_bridge_submit(tokens, count, &bridge);
    if (error)
        return error;
    IOLockLock(queue.lock);
    bridge->completed = callback;
    bridge->context = context;
    ++bridge->refs;
    thread_call_enter(queue.retirement);
    IOLockUnlock(queue.lock);
    switch_gpu_bridge_release(&bridge);
    return 0;
}

extern "C" int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *token)
{
    switch_gpu_bridge *bridge = nullptr;
    int result;
    for (;;) {
        result = switch_gpu_bridge_submit(token, 1, &bridge);
        if (result != -16)
            break;
        result = switch_gpu_bridge_wait_room();
        if (result)
            return result;
    }
    if (result)
        return result;
    result = switch_gpu_bridge_wait(bridge);
    switch_gpu_bridge_release(&bridge);
    return result;
}
