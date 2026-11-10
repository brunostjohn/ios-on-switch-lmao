// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_stamps.h"

// The textures and buffers a command buffer wrote (render targets, copy destinations, and for m2v anything a compute encoder
// bound): their CPU lines are invalidated before the CPU next reads them.
// A recorded object can be m2v's own (a facade command buffer's copy that went to m2v keeps it): only the raw path's carry
// the flag
static void markDirty(void *o)
{
    id object = (__bridge id)o;
    if (RMBuffer *buffer = cpuBufferOfResource(object)) {
        // No-copy clients may keep their original pointer forever. Publish GPU writes before completion or its event,
        // without requiring a new -contents call. The submission flushed dirty CPU lines before any GPU write.
        if (!buffer->_raw.wc && buffer->_raw.cpu) {
#if defined(__aarch64__)
            for (uintptr_t at = uintptr_t(buffer->_raw.cpu) & ~uintptr_t(63);
                 at < uintptr_t(buffer->_raw.cpu) + buffer->_raw.length; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
            __asm__ volatile("dsb sy" ::: "memory");
#endif
        }
        buffer->_gpuDirty = false;
    }
    if ([object isKindOfClass:[RMTexture class]])
        ((RMTexture *)object)->_gpuDirty = true;
}

void forEachWritten(RMCommandBuffer *cb, size_t from, size_t to, void (*written)(void *))
{
    cb->_plan.forEachWritten(from, to, written);
}

void markWrittenRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    forEachWritten(cb, from, to, markDirty);
}

void retireRawRange(RMCommandBuffer *cb, size_t from, size_t to)
{
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        ++cb->_segmentPending;
    }
    {
        std::lock_guard<std::mutex> l(gRM->doneLock);
        gRM->inflight.push_back({cb, cb->_seqno, from, to});
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

void segmentCompleted(RMCommandBuffer *cb, NSError *error, size_t from, size_t to)
{
    markWrittenRange(cb, from, to);
    bool ready;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (error && !cb->_segError)
            cb->_segError = error;
        --cb->_segmentPending;
        ready = cb->_finishReady && !cb->_segmentPending;
    }
    wakeSubmit();
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
    gRM->stats.finished.fetch_add(1, std::memory_order_relaxed);
    unhold(cb);
    if (error)
        forgetSignals();
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
            gRM->stats.scheduledByFinish++;
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
                retired = r.inflight.front();
            }
            RMCommandBuffer *cb = retired.buffer;
            // false: the kernel killed the channel with it in flight (a GPU fault or hang); the next command buffer gets a new one
            const bool ok = r.raw->wait(retired.seqno);
            if (!ok && r.raw->lostError() == raw::Device::kWaitRefused) {
                os_log_error(
                    rawLog(),
                    "raw: command buffer %u failed: the kernel kept refusing to wait for it on a channel it called "
                    "healthy, so the channel was taken as killed",
                    retired.seqno);
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
                             retired.from, retired.to);
        }
    }
}
