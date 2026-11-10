// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_stamps.h"

static bool markWritten(const raw::RetirementWrites &writes, bool success, const raw::CpuCompletion *cpu = nullptr)
{
    const auto result = success && cpu ? gRM->raw->completeCpu(*cpu) : raw::CpuCompletionResult::Legacy;
    const bool legacy = success && result == raw::CpuCompletionResult::Legacy;
#if defined(__aarch64__)
    bool maintained = false;
#endif
    for (void *object : writes.buffers) {
        RMBuffer *buffer = (__bridge RMBuffer *)object;
        // No-copy clients may keep their original pointer forever. Publish GPU writes before completion or its event,
        // without requiring a new -contents call. The submission flushed dirty CPU lines before any GPU write.
        if (legacy && buffer->_raw.hostVisible && !buffer->_raw.wc && buffer->_raw.cpu) {
#if defined(__aarch64__)
            maintained = true;
            for (uintptr_t at = uintptr_t(buffer->_raw.cpu) & ~uintptr_t(63);
                 at < uintptr_t(buffer->_raw.cpu) + buffer->_raw.length; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
#endif
        }
    }
#if defined(__aarch64__)
    if (maintained)
        __asm__ volatile("dsb sy" ::: "memory");
#endif
    for (void *object : writes.buffers) {
        RMBuffer *buffer = (__bridge RMBuffer *)object;
        buffer->_gpuDirty = !buffer->_raw.wc && !(success &&
            (legacy || result == raw::CpuCompletionResult::Current));
    }
    for (void *object : writes.textures)
        ((__bridge RMTexture *)object)->_gpuDirty = true;
    return result != raw::CpuCompletionResult::Failed;
}

void forEachWritten(RMCommandBuffer *cb, size_t from, size_t to, void (*written)(void *))
{
    cb->_plan.forEachWritten(from, to, written);
}

void markWrittenRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    markWritten(*captureWrittenRange(cb, from, to), false);
}

std::shared_ptr<const raw::RetirementWrites> captureWrittenRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    // Later kernels can still replace plan summaries while this range's completion runs.
    return std::make_shared<const raw::RetirementWrites>(raw::RetirementWrites::capture(
        [&](auto visit) { cb->_plan.forEachWritten(from, to, visit); },
        [](void *value) {
            id object = (__bridge id)value;
            return raw::RetirementWrites::Target{(__bridge void *)cpuBufferOfResource(object),
                [object isKindOfClass:[RMTexture class]] ? value : nullptr};
        }));
}

void retireRawRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    auto writes = captureWrittenRange(cb, from, to);
    std::vector<const raw::Buffer *> cached;
    for (void *object : writes->buffers) {
        const raw::Buffer &buffer = ((__bridge RMBuffer *)object)->_raw;
        if (buffer.hostVisible && !buffer.wc && buffer.cpu)
            cached.push_back(&buffer);
    }
    auto cpu = gRM->raw->captureCpuCompletion(cb->_seqno, cb->_channelGeneration, cached);
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        ++cb->_segmentPending;
    }
    auto signals = publishRawEventSignals(cb);
    {
        std::lock_guard<std::mutex> l(gRM->doneLock);
        gRM->inflight.push_back({cb, cb->_seqno, std::move(writes), std::move(signals), cb->_gpuEventWaits,
                                 cb->_channelGeneration, std::move(cpu)});
    }
    gRM->doneChanged.notify_one();
}

dispatch_queue_t scheduledQueueLocked(RMCommandBuffer *cb)
{
    if (!cb->_scheduledQueue)
        cb->_scheduledQueue = followingSerialQueue("com.brunostjohn.switch.raw.command.handlers");
    return cb->_scheduledQueue;
}

static void dispatchScheduledLocked(RMCommandBuffer *cb, NSArray *handlers)
{
    if (!handlers.count) {
        cb->_scheduledDone = true;
        return;
    }
    dispatch_async(scheduledQueueLocked(cb), ^{
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_scheduledDone = true;
        }
        cb->_changed.notify_all();
    });
}

// A command buffer's handlers stay ordered without holding up handlers for another command buffer it waits for.
void scheduled(RMCommandBuffer *cb)
{
    unhold(cb);
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_scheduledSent)
            return;  // once (eventHeld: at its commit; then its submission's)
        cb->_scheduledSent = true;
        if (cb->_status == MTLCommandBufferStatusCommitted)
            cb->_status = MTLCommandBufferStatusScheduled;
        handlers = cb->_scheduledHandlers;
        cb->_scheduledHandlers = nil;
        cb->_kernelEnd = cb->_gpuStart = hostSeconds();
        // Core Animation's render thread waits for this every frame (waitUntilScheduled): with nothing to run, no hop through
        // the handler queue
        if (!handlers.count)
            cb->_scheduledDone = true;
        else
            dispatchScheduledLocked(cb, handlers);
    }
    if (!handlers.count)
        cb->_changed.notify_all();
}

// Completed: CPU-signalled events, written resources marked, handlers (on the queue's completion queue when it has one), the
// objects released, waiters woken.
// The raw path's empty chunks go back as m2v's blocks do: on memory pressure, and outside the render server once nothing completed
// for NMB_IDLE_RELEASE_MS (each completion moves the deadline)
GpuMemoryRelease &gRawRelease = *new GpuMemoryRelease;

void segmentCompleted(RMCommandBuffer *cb, NSError *error, const raw::RetirementWrites &writes, uint32_t dependencies,
                      const std::vector<RMNativeEventSignal> &signals,
                      const std::vector<raw::EventTimeline::Promise> &waits, const raw::CpuCompletion *cpu)
{
    if (!error && raw::eventDependencies(waits, 0, false) != raw::EventDependencyState::ready)
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil];
    if (!markWritten(writes, !error, cpu) && !error)
        error = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                               userInfo:@{NSLocalizedDescriptionKey : @"CPU-visible GPU results could not be acquired"}];
    retireEventSignals(signals, !error);
    bool ready;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (error && !cb->_segError)
            cb->_segError = error;
        --cb->_segmentPending;
        ready = cb->_finishReady && !cb->_segmentPending;
    }
    wakeSubmit((__bridge void *)cb->_queue, dependencies);
    if (ready)
        finish(cb, nil);
}

void finish(RMCommandBuffer *cb, NSError *error)
{
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_finishing)
            return;
        if (error && !cb->_segError)
            cb->_segError = error;
        cb->_finishReady = true;
        if (cb->_segmentPending)
            return;
        cb->_finishing = true;
        error = cb->_segError;
    }
    RAW_DIAGNOSTIC(gRM->stats.finished.fetch_add(1, std::memory_order_relaxed));
    releaseEventWaits(cb);
    unhold(cb);
    // A completed command buffer is scheduled. Its scheduled handlers scheduled() never took run here, before the completed ones:
    // one whose fence passed before its submitting thread's scheduled() (the completion thread wakes on work it was just handed),
    // one that never reached a GPU. Ones scheduled() already queued run first, the completed ones queued behind them on the
    // handler queue.
    NSArray *handlers;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        NSArray *scheduledHandlers = nil;
        const bool sendScheduled = !cb->_scheduledSent;
        if (sendScheduled) {
            scheduledHandlers = cb->_scheduledHandlers;
            cb->_scheduledHandlers = nil;
            cb->_kernelEnd = hostSeconds();
#if SWITCH_FRAME_TIMELINE
            RAW_DIAGNOSTIC(gRM->stats.scheduledByFinish++);
#endif
        }
        cb->_status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
        cb->_error = error;
        // the GPU's own time, its end where this thread saw it complete: the duration exact, both late by that latency
        uint64_t ns = 0;
        const bool stamped = !error && !cb->_m2v && gRM->raw->gpuNanoseconds(cb->_stamp, ns);
        const raw::CommandBufferTimes t =
            raw::orderedTimes(cb->_kernelStart, cb->_kernelEnd, hostSeconds(), stamped, ns);
        cb->_kernelStart = t.kernelStart;
        cb->_kernelEnd = t.kernelEnd;
        cb->_gpuStart = t.gpuStart;
        cb->_gpuEnd = t.gpuEnd;
        handlers = cb->_completedHandlers;
        cb->_completedHandlers = nil;
        cb->_completedSent = true;
        cb->_scheduledSent = true;  // scheduled() runs no more
        if (sendScheduled)
            dispatchScheduledLocked(cb, scheduledHandlers);
    }
    cb->_changed.notify_all();
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler h in handlers)
            h((id<MTLCommandBuffer>)cb);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_completedDone = true;
            cb->_scheduledDone = true;
        }
        cb->_changed.notify_all();
        cb->_owners.clear();  // the resources may die now: no submission uses them any more (its capacity is recycled)
        cb->_computeJobs.clear();
        cb->_sharedEventWaits.clear();
        cb->_resourceReady.reset();
        if (gRM->committed.fetch_sub(1, std::memory_order_acq_rel) == 1)
            gRawRelease.drained();
#if SWITCH_FRAME_TIMELINE
        switch_frame::finished(cb->_frame);
#endif
    };
    // A handler may submit and wait for another command buffer. Neither GPU worker nor the serial scheduled-handler queue
    // can run it inline without preventing that work from completing.
    bool inlineCompletion;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        dispatch_queue_t q = cb->_queue ? cb->_queue->_completionQueue : nil;
        inlineCompletion = !q && !cb->_scheduledQueue && !handlers.count;
        if (!inlineCompletion) {
            if (!q)
                q = followingRootQueue();
            if (cb->_scheduledQueue)
                dispatch_async(cb->_scheduledQueue, ^{
                    dispatch_async(q, run);
                });
            else
                dispatch_async(q, run);
        }
    }
    if (inlineCompletion)
        run();
}

// The completion thread: sleeps in the kernel on each raw submission's fence (the interrupt), in order.
void completionLoop()
{
    RawState &r = *gRM;
    for (;;) {
        @autoreleasepool {
            RMRawRetirement retired;
            {
                std::unique_lock<std::mutex> l(r.doneLock);
                r.doneChanged.wait(l, [&] {
                    return !r.inflight.empty();
                });
                retired = std::move(r.inflight.front());
            }
            RMCommandBuffer *cb = retired.buffer;
            // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
            const bool ok = r.raw->wait(retired.seqno, retired.generation);
            if (!ok && r.raw->lostError() == raw::Device::kWaitRefused) {
                os_log_error(
                    rawLog(),
                    "raw: command buffer %u failed: the kernel kept refusing to wait for it on a channel it called "
                    "healthy, so the channel was taken as killed",
                    retired.seqno);
            } else if (!ok && r.raw->lostError() == raw::Device::kDependencyFailed) {
                os_log_error(rawLog(), "raw: command buffer %u failed an upstream dependency", retired.seqno);
            } else if (!ok) {
                const uint64_t va = r.raw->lostFaultVa();
                os_log_error(
                    rawLog(),
                    "raw: command buffer %u failed: the kernel killed its channel (error %u, fault VA 0x%llx%s%s)",
                    retired.seqno, r.raw->lostError(), (unsigned long long)va, va ? ": " : "",
                    va ? r.raw->vaOwner(va).c_str() : "");
            }
#if SWITCH_FRAME_TIMELINE
            cb->_frame.mark(switch_frame::Fence);
            switch_frame::trace(switch_frame::kFence, switch_frame::kPoint, cb->_frame.seq);
#endif
            {
                std::lock_guard<std::mutex> l(r.doneLock);
                r.inflight.pop_front();
            }
            segmentCompleted(cb,
                             ok ? nil
                                : [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                                      code:MTLCommandBufferErrorInternal
                                                  userInfo:nil],
                             *retired.writes, RMRawProgress, retired.signals, retired.waits, &retired.cpu);
        }
    }
}
