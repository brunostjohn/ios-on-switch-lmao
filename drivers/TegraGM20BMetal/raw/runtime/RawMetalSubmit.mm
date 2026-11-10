// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_execution_plan.h"
#include "raw_order.h"

// ---- commit: order between the two drivers ----
// With the kernel's Order (TegraGM20BABI.h method 14), the two drivers' command buffers are ordered on the GPU: before a raw
// submission that follows m2v work, one Order(AfterTask) makes its first entry a semaphore acquire on what NVK's channels had
// submitted; before an m2v commit that follows raw work, one Order(BeforeTask) makes NVK's channels' next submissions start with
// an acquire on the raw channel's last seqno. Neither thread waits for the GPU. The one CPU handoff left: a raw command buffer is
// submitted once the m2v ones before it are scheduled (NVK's Exec made, so the AfterTask acquire covers them) - a wait for the
// m2v runtime's own submission of work the GPU is already running, not for that work. What the GPU order cannot cover is
// ordered on the CPU, each with a one-shot line naming it: a kernel without Order, an Order the kernel refuses, a shared event
// signalled outside this device's submitted work.

// What a raw command buffer waits for before it is submitted: GPU order, the m2v command buffers before it scheduled; CPU
// order, completed. m2vLock held.
bool m2vClear()
{
    return gRM->gpuOrder ? gRM->m2vUnscheduled == 0 : gRM->m2vPending == 0;
}

// The kernel refused an Order: the drivers are ordered on the CPU until the kernel makes the raw channel again (one line).
void orderRefused(const char *what, int kr)
{
    once([NSString stringWithFormat:@"CPU-ordered: the kernel refused Order (%s, 0x%x)", what, kr]);
    std::lock_guard<std::mutex> m(gRM->m2vLock);
    gRM->gpuOrder = false;
    gRM->orderRefusedAt = gRM->raw->stats.reopens.load(std::memory_order_relaxed);
}

// A raw submission went through on a channel the kernel made since it refused an Order: GPU order again
static void orderReturned()
{
    RawState &r = *gRM;
    if (!raw::orderReturns(r.gpuOrder, r.raw->canOrder(), r.raw->stats.reopens.load(std::memory_order_relaxed),
                           r.orderRefusedAt))
        return;
    std::lock_guard<std::mutex> m(r.m2vLock);
    r.gpuOrder = true;
}

static void countDriver(int driver)
{
    RawState &r = *gRM;
    if (r.lastDriver && r.lastDriver != driver)
        r.stats.switches++;
    r.lastDriver = driver;
}

// Submits `cb` now: the caller holds gRM->submit and everything committed before it is submitted. false: not yet (a raw
// range whose Order the kernel refused just now waits for the m2v work before it to complete, one with no raw channel goes
// to m2v; the caller keeps it first).
// Diagnostics: each predicate `cb` hit, once (and once more when it was the only one)
static void countPreds(RMCommandBuffer *cb)
{
    const uint64_t m = cb->_preds;
    if (!m)
        return;
    const bool only = !(m & (m - 1));
    for (uint64_t left = m; left; left &= left - 1) {
        const int p = __builtin_ctzll(left);
        gPredCbs[p].fetch_add(1, std::memory_order_relaxed);
        if (only)
            gPredOnly[p].fetch_add(1, std::memory_order_relaxed);
    }
}

#if SWITCH_FRAME_TIMELINE
// The m2v replay's parts for the rates line: the wait for an m2v command buffer (its queue's slots), the twins made, and the
// replay's encode end to its commit's return (the Order call, a CPU-order wait, m2v's commit)
static void noteReplay(const switch_frame::Stamps &f, uint64_t returned)
{
    using namespace switch_frame;
    Stats &s = gRM->stats;
    const uint64_t perMs = ticksPerMs();
    auto us = [perMs](uint64_t from, uint64_t to) {
        return to > from ? (to - from) * 1000 / perMs : 0;
    };
    s.replaySlotUs += us(f.get(SlotStart), f.get(SlotEnd));
    s.replayTwinUs += f.get(Twins) * 1000 / perMs;
    s.replayCommitUs += us(f.get(ReplayEnd), returned);
    const uint64_t total = us(f.get(ReplayStart), returned);
    uint64_t seen = s.replayMaxUs.load(std::memory_order_relaxed);
    while (total > seen && !s.replayMaxUs.compare_exchange_weak(seen, total, std::memory_order_relaxed)) {
    }
}
#endif

// Every submission range ends at an encoder boundary. Events are separate operations, so the scheduler can run the
// prefix, publish its signal, and then block its suffix without preventing another queue from producing that suffix's event.
void planSegments(RMCommandBuffer *cb)
{
    cb->_segs.clear();
    cb->_segNext = 0;
    [cb closeEncoder];
    raw::ExecutionPlan plan;
    const size_t count = cb->_plan.size();
    size_t begin = 0;
    bool open = false;
    bool native = cb->_reason == WhyRaw;
    for (size_t at = 0; at < count; ++at) {
        const Cmd c = cb->_plan.legacy(at);
        if (c.op == OpRender || c.op == OpCompute || c.op == OpBlit) {
            begin = at;
            open = true;
            native = !cb->_whole && !c.index;
        } else if (c.op == OpEnd && open) {
            plan.append(begin, at + 1, native ? raw::ExecutionKind::native : raw::ExecutionKind::compatibility);
            open = false;
        } else if (c.op == OpSignalEvent || c.op == OpWaitEvent) {
            if (open) {
                cb->_segError = [NSError
                    errorWithDomain:MTLCommandBufferErrorDomain
                               code:MTLCommandBufferErrorInternal
                           userInfo:@{NSLocalizedDescriptionKey : @"An event was encoded inside an active encoder"}];
                return;
            }
            id event = (__bridge id)c.obj;
            const bool observable = [event respondsToSelector:@selector(signaledValue)] &&
                                    [event respondsToSelector:@selector(setSignaledValue:)] &&
                                    [event respondsToSelector:@selector(notifyListener:atValue:block:)];
            plan.append(at, at + 1,
                        observable ? (c.op == OpSignalEvent ? raw::ExecutionKind::signal : raw::ExecutionKind::wait)
                                   : raw::ExecutionKind::compatibility);
        } else if (!open) {
            // Opaque command-buffer operations retain their original position and explicit backend boundary.
            plan.append(at, at + 1, raw::ExecutionKind::compatibility);
        }
    }
    if (open) {
        cb->_segError =
            [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                code:MTLCommandBufferErrorInternal
                            userInfo:@{NSLocalizedDescriptionKey : @"An encoder did not end before commit"}];
        return;
    }
    for (const auto &range : plan.ranges()) {
        cb->_plan.prepareRange(range.from, range.to);
        cb->_segs.push_back({uint32_t(range.from), uint32_t(range.to), range.kind == raw::ExecutionKind::native,
                             range.kind == raw::ExecutionKind::wait || range.kind == raw::ExecutionKind::signal});
    }
}

// Whether only signals follow range `at`: they are published once the work before them completes, so the command buffer is
// scheduled with its last work, not with its completion
static bool onlySignalsAfter(RMCommandBuffer *cb, size_t at)
{
    for (size_t i = at + 1; i < cb->_segs.size(); ++i)
        if (!cb->_segs[i].event || cb->_plan.operation(cb->_segs[i].from).op != OpSignalEvent)
            return false;
    return true;
}

// The raw device's encoding arenas have one CPU owner. This lock is separate from the scheduler; no event or CPU GPU-fence
// wait happens here. A single visit emits one legal range and yields, retaining the Metal queue's reservation.
bool submitLocked(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    NSError *priorError;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        priorError = cb->_segError;
    }
    if (priorError) {
        finish(cb, priorError);
        return true;
    }
    if (cb->_segNext == cb->_segs.size()) {
        finish(cb, nil);
        return true;
    }
    RMSegment &range = cb->_segs[cb->_segNext];
    const bool last = cb->_segNext + 1 == cb->_segs.size();
    const bool lastWork = onlySignalsAfter(cb, cb->_segNext);
    if (range.event) {
        const Cmd event = cb->_plan.legacy(range.from);
        if (event.op == OpSignalEvent) {
            // eventReadiness required all earlier submitted ranges to retire successfully before reaching this position.
            id<MTLSharedEvent> target = (__bridge id)event.obj;
            if (target.signaledValue < event.c)
                target.signaledValue = event.c;
        }
        cb->_eventStart = {};
        cb->_eventArmed = 0;
    } else if (range.raw) {
        if (r.gpuOrder && r.m2vSinceRaw) {
            uint32_t waits = 0;
            const int kr = r.raw->orderAfterOthers(waits);
            const raw::OrderAnswer answer = raw::orderAnswer(kr);
            if (answer == raw::OrderAnswer::noChannel) {
                // A channel the next encode made again would run this range unordered after the m2v work before it
                range.raw = false;
                cb->_reason = WhyEncode;
                cb->_why = @"the raw channel is being made again";
                return false;
            }
            if (answer == raw::OrderAnswer::refused) {
                orderRefused("raw after m2v", kr);
                return false;
            }
            r.m2vSinceRaw = false;
            r.stats.acquires += waits;
        }
        const char *why = nullptr;
#if SWITCH_FRAME_TIMELINE
        const uint64_t began = nowUs();
        cb->_frame.mark(switch_frame::EncodeStart);
#endif
        const bool encoded = encodeRaw(cb, range.from, range.to, &why);
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::EncodeEnd);
        r.stats.encodeUs += nowUs() - began;
#endif
        if (!encoded) {
            if (cb->_partial) {
                // raw::CommandBuffer::commit drains its published prefix before reporting a partial failure.
                markWrittenRange(cb, range.from, range.to);
                finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                               code:MTLCommandBufferErrorInternal
                                           userInfo:nil]);
                return true;
            }
            range.raw = false;
            cb->_reason = WhyEncode;
            cb->_why = [NSString stringWithUTF8String:why ?: "?"];
            return false;
        }
        r.stats.raw++;
        r.rawSinceM2v = true;
        if (!r.gpuOrder)
            orderReturned();
        countDriver(1);
        retireRawRange(cb, range.from, range.to);
        if (lastWork)
            scheduled(cb);
    } else {
        if (r.gpuOrder && r.rawSinceM2v) {
            uint32_t waits = 0;
            const int kr = r.raw->orderOthersAfter(waits);
            if (kr != KERN_SUCCESS) {
                orderRefused("m2v after raw", kr);
                return false;
            }
            r.stats.acquires += waits;
        }
#if SWITCH_FRAME_TIMELINE
        const uint64_t began = nowUs();
        cb->_frame.mark(switch_frame::ReplayStart);
        tTwinTicks = 0;
#endif
        id<MTLCommandBuffer> m = r.fallback ? replay(cb, range.from, range.to, true) : nil;
#if SWITCH_FRAME_TIMELINE
        cb->_frame.mark(switch_frame::ReplayEnd);
        cb->_frame.set(switch_frame::Twins, tTwinTicks);
#endif
        if (!m) {
            finish(cb, [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                           code:MTLCommandBufferErrorInternal
                                       userInfo:nil]);
            return true;
        }
        r.rawSinceM2v = false;
        r.m2vSinceRaw = true;
        cb->_m2v = true;
        countDriver(2);
        r.stats.m2v++;
        countPreds(cb);
        const size_t from = range.from, to = range.to;
        {
            std::lock_guard<std::mutex> l(cb->_lock);
            ++cb->_segmentPending;
        }
        __block bool counted = false;
        {
            std::lock_guard<std::mutex> l(r.m2vLock);
            ++r.m2vPending;
            ++r.m2vUnscheduled;
        }
        [m addScheduledHandler:^(id<MTLCommandBuffer>) {
            // before the raw command buffer behind it may be submitted (m2vClear), so its scheduled handlers go first
            if (lastWork)
                scheduled(cb);
            {
                std::lock_guard<std::mutex> l(gRM->m2vLock);
                if (!counted)
                    --gRM->m2vUnscheduled;
                counted = true;
            }
            wakeSubmit();
        }];
        [m addCompletedHandler:^(id<MTLCommandBuffer> done) {
            {
                std::lock_guard<std::mutex> l(gRM->m2vLock);
                if (!counted)
                    --gRM->m2vUnscheduled;
                counted = true;
                --gRM->m2vPending;
            }
            segmentCompleted(cb, done.error, from, to);
        }];
        cb->_submittedM2v = true;
        [m commit];
#if SWITCH_FRAME_TIMELINE
        r.stats.replayUs += nowUs() - began;
        noteReplay(cb->_frame, cb->_frame.mark(switch_frame::Returned));
#endif
    }
    ++cb->_segNext;
    if (last)
        finish(cb, nil);  // joins every submitted range before handlers/resources are released
    return last;
}
