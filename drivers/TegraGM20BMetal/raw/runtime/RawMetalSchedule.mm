// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalEventJobs.h"
#include "raw_scheduled_wait.h"
#include "TegraGM20BJobABI.h"

void wakeSubmit(void *queue, uint32_t dependencies)
{
    std::lock_guard<std::mutex> l(gRM->schedule);
    if (queue)
        gRM->waiting.wake(queue);
    gRM->waiting.wakeDependencies(dependencies);
    gRM->waitingChanged.notify_one();
}

void enqueueBuffer(RMCommandBuffer *cb)
{
    std::lock_guard<std::mutex> l(gRM->schedule);
    if (cb->_enqueued)
        return;
    cb->_reservation = gRM->waiting.reserve((__bridge void *)cb->_queue, cb);
    cb->_enqueued = true;
    cb->_before = (__bridge_transfer RMCommandBuffer *)cb->_queue->_last.exchange((__bridge_retained void *)cb);
    std::lock_guard<std::mutex> b(cb->_lock);
    if (cb->_status == MTLCommandBufferStatusNotEnqueued)
        cb->_status = MTLCommandBufferStatusEnqueued;
}

// The imports of the IOSurfaces it writes. Only while the submit thread cannot take it: under the schedule lock, or on
// the submit thread.
static std::vector<uint64_t> writtenSurfaces(RMCommandBuffer *cb)
{
    std::vector<uint64_t> handles;
    cb->_plan.forEachWritten(0, cb->_plan.size(), [&](void *object) {
        id resource = (__bridge id)object;
        if (![resource isKindOfClass:[RMTexture class]])
            return;
        RMTexture *texture = (RMTexture *)resource;
        while (texture->_kind == RMTextureView && texture->_parent)
            texture = texture->_parent;
        const raw::Import *import = texture->_raw.import;
        if (texture->_surface && import && import->bo.handle &&
            std::find(handles.begin(), handles.end(), import->bo.handle) == handles.end())
            handles.push_back(import->bo.handle);
    });
    return handles;
}

// Before its waitUntilScheduled may return held (raw_scheduled_wait.h): its writes of IOSurfaces are promised, so a
// present or another process's job that uses one waits for its job on the GPU, as for a scheduled command buffer on
// Apple's GPU. Its last raw range's last job keeps the promises; one made once that range had gone is given back.
static void promiseWrites(RMCommandBuffer *cb, const std::vector<uint64_t> &handles)
{
    if (handles.empty())
        return;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_promised.load(std::memory_order_relaxed) || cb->_promisesTaken)
            return;
        cb->_promised.store(true, std::memory_order_release);
    }
    std::vector<uint32_t> made;
    for (size_t at = 0; at < handles.size() && made.size() < TegraGM20BMaxJobPromises; at += TegraGM20BMaxPromiseUses) {
        const std::vector<uint64_t> part(handles.begin() + ptrdiff_t(at),
                                         handles.begin() +
                                             ptrdiff_t(std::min(handles.size(), at + TegraGM20BMaxPromiseUses)));
        uint32_t id = 0;
        if (gRM->raw->promise(part, id))
            made.push_back(id);
        else
            once(@"the kernel took no writer promise for surfaces of a command buffer held on a shared event: a present "
                 @"may show them before it is submitted");
    }
    bool late;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        late = cb->_promisesTaken;
        if (!late)
            cb->_promises = made;
    }
    if (late)
        for (uint32_t id : made)
            gRM->raw->cancelPromise(id);
}

// finish(): what its last raw range did not carry to the kernel, after all its work completed
void cancelPromises(RMCommandBuffer *cb)
{
    if (!cb->_promised.load(std::memory_order_acquire))
        return;
    std::vector<uint32_t> left;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        cb->_promisesTaken = true;
        left.swap(cb->_promises);
    }
    for (uint32_t id : left)
        gRM->raw->cancelPromise(id);
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
    bool held = false;
    std::vector<uint64_t> written;
    {
        std::lock_guard<std::mutex> l(r.schedule);
        r.waiting.commit((__bridge void *)cb->_queue, cb, cb->_reservation, &held);
        if (held)
            written = writtenSurfaces(cb);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        RAW_DIAGNOSTIC(r.stats.deferred++);
#endif
    }
    if (held) {
        promiseWrites(cb, written);
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_held = true;
        }
        cb->_changed.notify_all();
    }
    r.waitingChanged.notify_one();
}

// Return pending only for this queue head. Other queues remain runnable while its pipelines compile. A compile it waits for
// is urged on its queue's urge queue (one at a time per Metal queue, at the committers' priority, ahead of the workers' queue).
// A compile either completes or fails: no deadline turns a slow one into an error.
static int pipelineReadiness(RMCommandBuffer *cb, const RMSegment &range)
{
    if (!native_bridge_metal_render_pipeline_ready)
        return 1;  // older host bridge: replay retains its synchronous contract
    bool pending = false;
    for (void *object : cb->_plan.renderPipelines(range.from, range.to)) {
        id state = twinOf((__bridge id)object);
        const bool first = cb->_pipelineObserved.insert(object).second;
        void (^observer)(void) = nil;
        if (first) {
            void *queue = (__bridge void *)cb->_queue;
            observer = ^{
                wakeSubmit(queue);
            };
        }
        const int status = first && native_bridge_metal_render_pipeline_urge
                               ? native_bridge_metal_render_pipeline_urge(state, observer, cb->_queue->_urges)
                               : native_bridge_metal_render_pipeline_ready(state, observer);
        if (status < 0)
            return -1;
        if (!status)
            pending = true;
    }
    return pending ? 0 : 1;
}

// A native variant this queue head waits for, made on the raw compiler's two bounded workers: refused, the range goes to m2v at
// once. No deadline.
static int variantReadiness(RMCommandBuffer *cb, const RMSegment &range)
{
    using Variant = raw::Variant<raw::Pipeline>;
    bool pending = false;
    for (void *object : cb->_plan.renderVariants(range.from, range.to)) {
        auto *variant = static_cast<Variant *>(object);
        std::function<void()> observer;
        if (cb->_variantObserved.insert(object).second) {
            void *queue = (__bridge void *)cb->_queue;
            observer = [queue] {
                wakeSubmit(queue);
            };
        }
        const auto state = variant->observe(std::move(observer));
        if (state == Variant::State::Refused)
            return -1;
        if (state == Variant::State::Pending)
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
        if (cb->_segmentPending || cb->_resourceReady)
            return raw::HeadWait::signal;
    }
    if (cb->_segNext >= cb->_segs.size() || !cb->_segs[cb->_segNext].event)
        return raw::HeadWait::signal;
    const Cmd c = cb->_plan.legacy(cb->_segs[cb->_segNext].from);
    if (c.op != OpWaitEvent)
        return raw::HeadWait::signal;
    return promisedEvent(c.obj, c.c) ? raw::HeadWait::promised : raw::HeadWait::event;
}

// waitUntilScheduled of the command buffers a hold took returns; one a release gave back waits for its scheduling again
static void markHeld(std::vector<RMCommandBuffer *> &buffers, bool held)
{
    for (RMCommandBuffer *cb : buffers) {
        if (held)
            promiseWrites(cb, writtenSurfaces(cb));
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            cb->_held = held;
        }
        if (held)
            cb->_changed.notify_all();
    }
    buffers.clear();
}

// The submit thread has no more raw work in hand: the quiet ranges it left in flight wait for their own interrupt. Only
// this thread sets the promise (retireRawRange), so it reads it without the lock.
static void closePromise()
{
    if (!gRM->submitPromised)
        return;
    std::lock_guard<std::mutex> d(gRM->doneLock);
    gRM->submitPromised = false;
    gRM->doneChanged.notify_one();
}

void submitLoop()
{
    RawState &r = *gRM;
    std::vector<RMCommandBuffer *> marked;
    for (;;) {
        @autoreleasepool {
            raw::SubmitQueue<void *, RMCommandBuffer *>::Ticket ticket;
            bool followed = false;
            {
                std::unique_lock<std::mutex> l(r.schedule);
                for (;;) {
                    r.waiting.expire(raw::EventDeadline::Clock::now());
                    ticket = r.waiting.next();
                    if (ticket) {
                        followed = r.waiting.ready() || r.waiting.behind(ticket);
                        break;
                    }
                    closePromise();
                    const auto due = r.waiting.deadline();
                    const auto ready = [&] {
                        return r.waiting.ready();
                    };
                    if (due == raw::EventDeadline::Time::max())
                        r.waitingChanged.wait(l, ready);
                    else
                        r.waitingChanged.wait_until(l, due, ready);
                }
            }
            RMCommandBuffer *cb = ticket.value;
            auto deadline = raw::EventDeadline::Time::max();
            uint32_t dependencies = 0;
            bool blocked = false;
            raw::HeadWait wait = raw::HeadWait::signal;
            NSError *error;
            {
                std::lock_guard<std::mutex> l(cb->_lock);
                error = cb->_segError;
            }
            if (!error) {
                blocked = eventReadiness(cb, deadline) == raw::EventReadiness::blocked;
                if (blocked)
                    wait = eventWait(cb);
                {
                    std::lock_guard<std::mutex> lock(cb->_lock);
                    error = cb->_segError;
                }
            }
            if (!error && !blocked && cb->_segNext < cb->_segs.size()) {
                RMSegment &range = cb->_segs[cb->_segNext];
                cb->_plan.prepareRange(range.from, range.to);
                bool loopRetiring = false;
                if (range.loopTo) {
                    std::lock_guard<std::mutex> lock(cb->_lock);
                    loopRetiring = cb->_segmentPending != 0;
                }
                if (loopRetiring) {
                    blocked = true;
                    dependencies = RMRawProgress;
                } else if (!range.event && range.raw) {
                    int ready = variantReadiness(cb, range);
                    if (ready > 0)
                        ready = computeReadiness(cb, range);
                    if (ready < 0) {
                        range.raw = false;
                        blocked = eventReadiness(cb, deadline) == raw::EventReadiness::blocked;
                        if (blocked)
                            wait = eventWait(cb);
                        std::lock_guard<std::mutex> lock(cb->_lock);
                        error = cb->_segError;
                    } else if (!ready) {
                        blocked = true;
                        wait = raw::HeadWait::compile;
                    }
                }
                if (!error && !blocked && ((!range.event && range.raw) || signalOrderedOnGpu(cb, range))) {
                    std::lock_guard<std::mutex> m(r.m2vLock);
                    if (!m2vClear()) {
                        blocked = true;
                        dependencies = RMCompatibilityProgress;
                    }
                } else if (!error && !blocked && !range.event) {
                    const int ready = pipelineReadiness(cb, range);
                    if (ready < 0)
                        error = [NSError
                            errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:@{NSLocalizedDescriptionKey : @"A render pipeline could not be compiled"}];
                    else if (!ready)
                        blocked = true;
                    else {
                        bool gpuOrder, rawRetiring;
                        {
                            std::lock_guard<std::mutex> m(r.m2vLock);
                            gpuOrder = r.gpuOrder;
                        }
                        {
                            std::lock_guard<std::mutex> d(r.doneLock);
                            rawRetiring = !r.inflight.empty();
                        }
                        // completionLoop's retirements, which wake RMRawProgress: the device's own jobs (a reopen's context)
                        // have no retirement, and a seqno the device submitted may be another channel's than its fence
                        if (!gpuOrder && rawRetiring) {
                            blocked = true;
                            dependencies = RMRawProgress;
                        }
                    }
                }
            }
            if (blocked) {
                {
                    std::lock_guard<std::mutex> l(r.schedule);
                    r.waiting.park(ticket, dependencies, deadline);
                    if (raw::holds(wait))
                        r.waiting.hold(ticket.key, marked);
                }
                markHeld(marked, true);
                continue;
            }
            bool submitted = true;
            if (error)
                finish(cb, error);
            else {
                std::lock_guard<std::recursive_mutex> s(submitLock());
                submitted = submitRange(cb, followed);
            }
            {
                std::lock_guard<std::mutex> l(r.schedule);
                r.waiting.release(ticket.key, marked);
                if (submitted)
                    r.waiting.retire(ticket);
                else
                    r.waiting.yield(ticket);
            }
            markHeld(marked, false);
        }
    }
}
