// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

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
        r.waiting.commit((__bridge void *)cb->_queue, cb, cb->_reservation);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        RAW_DIAGNOSTIC(r.stats.deferred++);
#endif
    }
    r.waitingChanged.notify_one();
}

// Return pending only for this queue head. Other queues remain runnable while its pipelines compile.
static int pipelineReadiness(RMCommandBuffer *cb, const RMSegment &range, raw::EventDeadline::Time &deadline)
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
        const int status = native_bridge_metal_render_pipeline_ready(state, observer);
        if (status < 0)
            return -1;
        if (!status) {
            const auto now = std::chrono::steady_clock::now();
            const auto [start, inserted] = cb->_pipelineStarted.try_emplace(object, now);
            const auto due = start->second + std::chrono::seconds(60);
            if (now >= due)
                return -1;
            deadline = std::min(deadline, due);
            pending = true;
        }
    }
    return pending ? 0 : 1;
}

static int variantReadiness(RMCommandBuffer *cb, const RMSegment &range, raw::EventDeadline::Time &deadline)
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
        if (state == Variant::State::Pending) {
            const auto now = std::chrono::steady_clock::now();
            const auto [start, inserted] = cb->_variantStarted.try_emplace(object, now);
            const auto due = start->second + std::chrono::seconds(60);
            if (now >= due)
                return -2;
            deadline = std::min(deadline, due);
            pending = true;
        }
    }
    return pending ? 0 : 1;
}

void submitLoop()
{
    RawState &r = *gRM;
    for (;;) {
        @autoreleasepool {
            raw::SubmitQueue<void *, RMCommandBuffer *>::Ticket ticket;
            {
                std::unique_lock<std::mutex> l(r.schedule);
                for (;;) {
                    r.waiting.expire(raw::EventDeadline::Clock::now());
                    ticket = r.waiting.next();
                    if (ticket)
                        break;
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
            NSError *error;
            {
                std::lock_guard<std::mutex> l(cb->_lock);
                error = cb->_segError;
            }
            if (!error) {
                const auto state = eventReadiness(cb, deadline);
                blocked = state == raw::EventReadiness::blocked;
                if (state == raw::EventReadiness::expired)
                    error = [NSError
                        errorWithDomain:MTLCommandBufferErrorDomain
                                   code:MTLCommandBufferErrorTimeout
                               userInfo:@{NSLocalizedDescriptionKey : @"A command buffer event was not signaled"}];
                if (!error) {
                    std::lock_guard<std::mutex> lock(cb->_lock);
                    error = cb->_segError;
                }
            }
            if (!error && !blocked && cb->_segNext < cb->_segs.size()) {
                RMSegment &range = cb->_segs[cb->_segNext];
                cb->_plan.prepareRange(range.from, range.to);
                if (!range.event && range.raw) {
                    int ready = variantReadiness(cb, range, deadline);
                    if (ready > 0)
                        ready = computeReadiness(cb, range, deadline);
                    if (ready == -1) {
                        range.raw = false;
                        blocked = eventReadiness(cb, deadline) == raw::EventReadiness::blocked;
                        std::lock_guard<std::mutex> lock(cb->_lock);
                        error = cb->_segError;
                    } else if (ready == -2)
                        error = [NSError
                            errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorTimeout
                                   userInfo:@{NSLocalizedDescriptionKey : @"A native shader job did not compile"}];
                    else if (!ready)
                        blocked = true;
                }
                if (!error && !blocked && !range.event && range.raw) {
                    std::lock_guard<std::mutex> m(r.m2vLock);
                    if (!m2vClear()) {
                        blocked = true;
                        dependencies = RMCompatibilityProgress;
                    }
                } else if (!error && !blocked && !range.event) {
                    const int ready = pipelineReadiness(cb, range, deadline);
                    if (ready < 0)
                        error = [NSError
                            errorWithDomain:MTLCommandBufferErrorDomain
                                       code:MTLCommandBufferErrorInternal
                                   userInfo:@{NSLocalizedDescriptionKey : @"A render pipeline could not be compiled"}];
                    else if (!ready)
                        blocked = true;
                    else {
                        bool gpuOrder;
                        {
                            std::lock_guard<std::mutex> m(r.m2vLock);
                            gpuOrder = r.gpuOrder;
                        }
                        if (!gpuOrder && !r.raw->done(r.raw->submitted())) {
                            blocked = true;
                            dependencies = RMRawProgress;
                        }
                    }
                }
            }
            if (blocked) {
                std::lock_guard<std::mutex> l(r.schedule);
                r.waiting.park(ticket, dependencies, deadline);
                continue;
            }
            bool submitted = true;
            if (error)
                finish(cb, error);
            else {
                submitted = submitRange(cb);
            }
            {
                std::lock_guard<std::mutex> l(r.schedule);
                if (submitted)
                    r.waiting.retire(ticket);
                else
                    r.waiting.yield(ticket);
            }
        }
    }
}
