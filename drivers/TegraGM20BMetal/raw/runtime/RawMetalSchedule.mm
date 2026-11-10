// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_scheduled_wait.h"

void wakeSubmit()
{
    std::lock_guard<std::mutex> l(gRM->schedule);
    ++gRM->scheduleGeneration;
    gRM->waitingChanged.notify_one();
}

void enqueueBuffer(RMCommandBuffer *cb)
{
    std::lock_guard<std::mutex> l(gRM->schedule);
    if (cb->_enqueued)
        return;
    gRM->waiting.reserve((__bridge void *)cb->_queue, cb);
    cb->_enqueued = true;
    cb->_before = (__bridge_transfer RMCommandBuffer *)cb->_queue->_last.exchange((__bridge_retained void *)cb);
    std::lock_guard<std::mutex> b(cb->_lock);
    if (cb->_status == MTLCommandBufferStatusNotEnqueued)
        cb->_status = MTLCommandBufferStatusEnqueued;
}

void commitBuffer(RMCommandBuffer *cb)
{
    raw::followers().committing();
    planSegments(cb);
    RawState &r = *gRM;
    r.committed.fetch_add(1);
    if (r.raw->purgedArenas())
        restoreArenas(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (waitsForEvent(cb)) {
        std::lock_guard<std::mutex> h(gHoldersLock);
        gHolders.insert(cb);
    }
#endif
    armEventWaits(cb);
    {
        std::lock_guard<std::mutex> l(r.schedule);
        r.waiting.commit((__bridge void *)cb->_queue, cb);
        ++r.scheduleGeneration;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        r.stats.deferred++;
#endif
    }
    r.waitingChanged.notify_one();
}

// Return pending only for this queue head. Other queues remain runnable while its pipelines compile. A compile it waits for
// runs at once on a thread that follows the committers (raw_follow.h), ahead of the workers' queue. A compile either
// completes or fails: no deadline turns a slow one into an error.
static int pipelineReadiness(RMCommandBuffer *cb, const RMSegment &range)
{
    if (!native_bridge_metal_render_pipeline_ready)
        return 1;  // older host bridge: replay retains its synchronous contract
    bool pending = false;
    std::vector<void *> pipelines;
    cb->_plan.forEachRenderPipeline(range.from, range.to, [&](void *object) {
        pipelines.push_back(object);
    });
    for (void *object : pipelines) {
        id state = twinOf((__bridge id)object);
        const bool first = cb->_pipelineObserved.insert(object).second;
        void (^observer)(void) = nil;
        if (first)
            observer = ^{
                wakeSubmit();
            };
        const int status = first && native_bridge_metal_render_pipeline_urge
                               ? native_bridge_metal_render_pipeline_urge(state, observer, followingRootQueue())
                               : native_bridge_metal_render_pipeline_ready(state, observer);
        if (status < 0)
            return -1;
        if (!status)
            pending = true;
    }
    return pending ? 0 : 1;
}

// What a head eventReadiness found blocked waits for: its own earlier work, a value a submitted command buffer promised, or a
// shared event value nothing reached yet
static raw::HeadWait eventWait(RMCommandBuffer *cb)
{
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_segmentPending)
            return raw::HeadWait::signal;
    }
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    if (c.op != OpWaitEvent)
        return raw::HeadWait::signal;
    return promisedEvent(c.obj, c.c) ? raw::HeadWait::promised : raw::HeadWait::event;
}

static void park(std::vector<void *> &parked, RMCommandBuffer *cb, raw::HeadWait wait)
{
    if (raw::holds(wait))
        parked.push_back((__bridge void *)cb->_queue);
}

// waitUntilScheduled of the command buffers a hold took returns; one a release gave back waits for its scheduling again
static void markHeld(std::vector<RMCommandBuffer *> &buffers, bool held)
{
    for (RMCommandBuffer *cb : buffers) {
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_held = held;
        }
        if (held)
            cb->_changed.notify_all();
    }
    buffers.clear();
}

void submitLoop()
{
    RawState &r = *gRM;
    std::vector<RMCommandBuffer *> candidates, marked;
    std::vector<void *> parked;  // queues whose head waits on a shared event
    for (;;) {
        @autoreleasepool {
            uint64_t observed;
            {
                std::lock_guard<std::mutex> l(r.schedule);
                r.waiting.heads(candidates);
                observed = r.scheduleGeneration;
            }
            auto deadline = raw::EventDeadline::Time::max();
            RMCommandBuffer *cb = nil;
            NSError *error = nil;
            // Only this thread advances queue heads. Registration, Objective-C calls, raw fence reads and readiness
            // checks run without the scheduler lock; commit and wakeups remain bounded even when a client callback runs.
            for (RMCommandBuffer *candidate : candidates) {
                {
                    std::lock_guard<std::mutex> l(candidate->_lock);
                    error = candidate->_segError;
                }
                if (error) {
                    cb = candidate;
                    break;
                }
                const auto state = eventReadiness(candidate, deadline);
                if (state == raw::EventReadiness::blocked) {
                    park(parked, candidate, eventWait(candidate));
                    continue;
                }
                if (state == raw::EventReadiness::expired) {
                    error = [NSError
                        errorWithDomain:MTLCommandBufferErrorDomain
                                   code:MTLCommandBufferErrorTimeout
                               userInfo:@{NSLocalizedDescriptionKey : @"A command buffer event was not signaled"}];
                    cb = candidate;
                    break;
                }
                if (candidate->_segNext < candidate->_segs.size()) {
                    const RMSegment &range = candidate->_segs[candidate->_segNext];
                    if (!range.event) {
                        if (range.raw) {
                            std::lock_guard<std::mutex> m(r.m2vLock);
                            if (!m2vClear()) {
                                park(parked, candidate, raw::HeadWait::order);
                                continue;
                            }
                        } else {
                            const int ready = pipelineReadiness(candidate, range);
                            if (ready < 0) {
                                error = [NSError
                                    errorWithDomain:MTLCommandBufferErrorDomain
                                               code:MTLCommandBufferErrorInternal
                                           userInfo:@{
                                               NSLocalizedDescriptionKey : @"A render pipeline could not be compiled"
                                           }];
                                cb = candidate;
                                break;
                            }
                            if (!ready) {
                                park(parked, candidate, raw::HeadWait::compile);
                                continue;
                            }
                            if (!r.gpuOrder && !r.raw->done(r.raw->submitted())) {
                                park(parked, candidate, raw::HeadWait::order);
                                continue;
                            }
                        }
                    }
                }
                cb = candidate;
                break;
            }
            candidates.clear();
            if (!parked.empty()) {
                {
                    std::lock_guard<std::mutex> l(r.schedule);
                    for (void *queue : parked)
                        r.waiting.hold(queue, marked);
                }
                parked.clear();
                markHeld(marked, true);
            }
            if (!cb) {
                std::unique_lock<std::mutex> l(r.schedule);
                auto changed = [&] {
                    return r.scheduleGeneration != observed;
                };
                if (deadline == raw::EventDeadline::Time::max())
                    r.waitingChanged.wait(l, changed);
                else
                    r.waitingChanged.wait_until(l, deadline, changed);
                continue;
            }
            bool submitted = true;
            if (error) {
                // Already submitted ranges remain retained until every completion has been accounted for.
                finish(cb, error);
            } else {
                std::lock_guard<std::mutex> encode(r.submit);
                submitted = submitLocked(cb);
            }
            {
                std::lock_guard<std::mutex> l(r.schedule);
                r.waiting.release((__bridge void *)cb->_queue, marked);
                if (submitted)
                    r.waiting.retire((__bridge void *)cb->_queue, cb);
                else
                    r.waiting.yield((__bridge void *)cb->_queue);
            }
            markHeld(marked, false);
        }
    }
}
