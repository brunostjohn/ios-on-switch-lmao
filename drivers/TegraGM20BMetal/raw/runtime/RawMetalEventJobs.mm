// SPDX-License-Identifier: MIT
#import "RawMetalEventJobs.h"
#import "RawMetalInternal.h"
#include "raw_event_job.h"
#include "raw_order.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static std::atomic<uint64_t> gSignalJobs{0};

uint64_t eventJobsSubmitted()
{
    return gSignalJobs.load(std::memory_order_relaxed);
}
#endif

static raw::EventRangeView viewOf(RMCommandBuffer *cb, size_t at)
{
    const RMSegment &range = cb->_segs[at];
    raw::EventRangeView view;
    view.event = range.event;
    view.carried = range.gpuSignal;
    if (range.event) {
        const Cmd c = cb->_plan.legacy(range.from);
        view.signal = c.op == OpSignalEvent;
        view.leasable = view.signal && sharedEventLease((__bridge id)c.obj) != nullptr;
    }
    return view;
}

bool signalOrderedOnGpu(RMCommandBuffer *cb, const RMSegment &range)
{
    if (!range.event || range.gpuSignal)
        return false;
    const Cmd c = cb->_plan.legacy(range.from);
    if (c.op != OpSignalEvent || !sharedEventLease((__bridge id)c.obj))
        return false;
    std::lock_guard<std::mutex> m(gRM->m2vLock);
    return gRM->gpuOrder;
}

static void eventJobFailure(RMCommandBuffer *cb, int result)
{
    std::lock_guard<std::mutex> lock(cb->_lock);
    if (!cb->_segError)
        cb->_segError = [NSError
            errorWithDomain:MTLCommandBufferErrorDomain
                       code:MTLCommandBufferErrorInternal
                   userInfo:@{
                       NSLocalizedDescriptionKey : [NSString
                           stringWithFormat:@"The kernel refused an event job (0x%x): a producer failed or is lost",
                                            result]
                   }];
}

RMEventJob submitEventJobs(RMCommandBuffer *cb)
{
    RawState &r = *gRM;
    const size_t head = cb->_segNext;
    std::vector<raw::EventRangeView> views(head);
    for (size_t at = head; at < cb->_segs.size() && cb->_segs[at].event; at++)
        views.push_back(viewOf(cb, at));
    const std::vector<size_t> carried = raw::signalJobRanges(views, head);
    if (carried.empty())
        return RMEventJob::None;
    std::vector<std::shared_ptr<raw::SharedEventLease>> leases;
    std::vector<TegraGM20BAbiEventUse> signals;
    for (size_t at : carried) {
        const Cmd c = cb->_plan.legacy(cb->_segs[at].from);
        leases.push_back(sharedEventLease((__bridge id)c.obj));
        signals.push_back({leases.back()->handle(), c.c});
    }
    if (r.gpuOrder && r.m2vSinceRaw) {
        uint32_t acquires = 0;
        const int kr = r.raw->orderAfterOthers(acquires);
        switch (raw::orderAnswer(kr)) {
        case raw::OrderAnswer::ordered:
            break;
        case raw::OrderAnswer::refused:
            orderRefused("event signal after m2v", kr);
            return RMEventJob::Retry;
        case raw::OrderAnswer::noChannel:
            eventJobFailure(cb, kr);
            return RMEventJob::Retry;
        }
        r.m2vSinceRaw = false;
        RAW_DIAGNOSTIC(r.stats.acquires += acquires);
    }
    if (validateNativeEventWaits(cb, r.raw->channelGeneration()) != raw::EventDependencyState::ready)
        return RMEventJob::Retry;
    uint32_t seqno = 0;
    for (size_t from = 0; from < signals.size(); from += TegraGM20BMaxJobEvents) {
        const uint32_t count = uint32_t(std::min<size_t>(signals.size() - from, TegraGM20BMaxJobEvents));
        int result = KERN_SUCCESS;
        if (!r.raw->eventJob({nullptr, signals.data() + from, 0, count}, seqno, &result)) {
            eventJobFailure(cb, result);
            return RMEventJob::Retry;
        }
    }
    cb->_sharedSignalRanges = carried;
    publishSharedEvents(cb);
    cb->_seqno = seqno;
    cb->_channelGeneration = r.raw->channelGeneration();  // the job's: eventJob makes a channel the kernel killed again
    r.rawSinceM2v = true;
    cb->_rangeQuiet = false;
    retireRawRange(cb, cb->_segs[head].from, cb->_segs[head].to);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gSignalJobs.fetch_add(1, std::memory_order_relaxed);
#endif
    return RMEventJob::Submitted;
}
