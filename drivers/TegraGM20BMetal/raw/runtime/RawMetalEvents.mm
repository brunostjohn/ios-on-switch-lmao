// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_promised_signals.h"

@implementation RMCommandBuffer (Events)

// `milliseconds`: a wait's own limit (encodeWaitForEvent:value:timeout:), 0 for none
- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal timeout:(uint32_t)milliseconds
{
    // the m2v device's events, shared or plain (NBEvent answers a shared event's value, set and notifications): the raw path
    // signals them from the CPU once its work completes, and holds its work until a wait is covered (waitCovered); an event
    // without those (another device's) is m2v's alone
    if (![(id)event respondsToSelector:@selector(signaledValue)] ||
        ![(id)event respondsToSelector:@selector(setSignaledValue:)] ||
        ![(id)event respondsToSelector:@selector(notifyListener:atValue:block:)]) {
        [self takeWholeReason:WhyEvent why:@"an event the raw path cannot read, set or be notified of"];
        hit(self, PredEvent, signal);
    }
    [self retain:event];
    Cmd m = cmd(signal ? OpSignalEvent : OpWaitEvent);
    m.obj = (__bridge void *)event;
    m.c = value;
    m.a = milliseconds;
    [self record:m];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true timeout:0];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false timeout:0];
}

- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true timeout:0];
}

// The agents a signal is for are Apple's GPU scheduler's: a signal is every agent's here
- (void)encodeSignalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value agentMask:(uint64_t)mask
{
    (void)mask;
    [self event:event value:value signal:true timeout:0];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value timeout:(uint32_t)milliseconds
{
    [self event:event value:value signal:false timeout:milliseconds];
}

@end

// The committed command buffers that wait for a shared event, until scheduled.
std::mutex gHoldersLock;
std::set<RMCommandBuffer *> gHolders;

bool waitsForEvent(RMCommandBuffer *cb)
{
    for (const RMPlanEvent &event : cb->_plan.events())
        if (!event.signal)
            return true;
    return false;
}

void unhold(RMCommandBuffer *cb)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    std::lock_guard<std::mutex> l(gHoldersLock);
    gHolders.erase(cb);
#else
    (void)cb;
#endif
}

uint64_t signalled(NSMapTable *map, void *event)
{
    std::lock_guard<std::mutex> l(gRM->eventValues);
    NSNumber *n = [map objectForKey:(__bridge id)event];
    return n ? n.unsignedLongLongValue : 0;
}

// Only completed execution ranges publish CPU event values. A later wait cannot observe a signal from an unexecuted
// suffix, and a failed/cancelled range never manufactures a successful signal.
void forgetSignals()
{
    std::lock_guard<std::mutex> l(gRM->eventValues);
    [gRM->rawSignals removeAllObjects];
    [gRM->m2vSignals removeAllObjects];
}

static raw::RangeKind rangeKind(RMCommandBuffer *cb, size_t at)
{
    const RMSegment &range = cb->_segs[at];
    if (!range.event)
        return range.raw ? raw::RangeKind::native : raw::RangeKind::compatibility;
    return cb->_plan.operation(range.from).op == OpSignalEvent ? raw::RangeKind::signal : raw::RangeKind::wait;
}

// The raw range at _segNext was just submitted: the signals right after it are promised, to be published as it completes
// (raw_promised_signals.h), so they no longer hold its queue's head until then
std::vector<RMEventSignal> publishRawEventSignals(RMCommandBuffer *cb)
{
    std::vector<RMEventSignal> signals;
    const uint32_t generation = gRM->raw->channelGeneration();
    if (!cb->_rawGeneration)
        cb->_rawGeneration = generation;
    const size_t next = cb->_segNext + 1;
    if (next >= cb->_segs.size() || rangeKind(cb, next) != raw::RangeKind::signal)
        return signals;
    bool failed;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        failed = cb->_segError != nil;
    }
    const size_t end = raw::promisedSignalsEnd(
        [cb](size_t at) {
            return rangeKind(cb, at);
        },
        cb->_segs.size(), cb->_segNext, cb->_rawGeneration == generation, failed);
    std::vector<raw::EventTimeline::Key> wake;
    std::lock_guard<std::mutex> l(gRM->eventValues);
    for (size_t at = next; at < end; ++at) {
        RMSegment &range = cb->_segs[at];
        const Cmd event = cb->_plan.legacy(range.from);
        auto promise =
            gRM->events.publish(reinterpret_cast<uintptr_t>(event.obj), event.c, {cb->_seqno, generation}, wake);
        if (!promise)
            break;
        signals.push_back({event.obj, std::move(promise)});
        range.promised = true;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    gRM->stats.signalsPublished += signals.size();
#endif
    return signals;
}

// The raw range the signals followed completed: on success each value is written before its promise leaves the
// registry, so a wait never finds it neither promised nor reached
void retireEventSignals(const std::vector<RMEventSignal> &signals, bool success)
{
    for (const RMEventSignal &signal : signals) {
        if (success) {
            id<MTLSharedEvent> event = (__bridge id)signal.event;
            if (event.signaledValue < signal.promise->value)
                event.signaledValue = signal.promise->value;
        }
        std::lock_guard<std::mutex> l(gRM->eventValues);
        gRM->events.retire(reinterpret_cast<uintptr_t>(signal.event), signal.promise, success);
    }
}

bool promisedEvent(void *event, uint64_t value)
{
    std::lock_guard<std::mutex> l(gRM->eventValues);
    return gRM->events.promised(reinterpret_cast<uintptr_t>(event), value);
}

void armEventWaits(RMCommandBuffer *cb)
{
    cb->_eventWaits.clear();
    cb->_eventStart = {};
    cb->_eventArmed = 0;
}

raw::EventReadiness eventReadiness(RMCommandBuffer *cb, raw::EventDeadline::Time &nextWake)
{
    using Readiness = raw::EventReadiness;
    if (cb->_segNext >= cb->_segs.size())
        return Readiness::ready;
    const RMSegment &range = cb->_segs[cb->_segNext];
    if (!range.event)
        return Readiness::ready;
    const Cmd c = cb->_plan.legacy(range.from);
    if (c.op == OpSignalEvent) {
        if (range.promised)
            return Readiness::ready;
        std::lock_guard<std::mutex> l(cb->_lock);
        return cb->_segmentPending ? Readiness::blocked : Readiness::ready;
    }
    id event = (__bridge id)c.obj;
    if ([event signaledValue] >= c.c)
        return Readiness::ready;
    {
        std::lock_guard<std::mutex> l(cb->_lock);
        if (cb->_segmentPending)
            return Readiness::
                blocked;  // timeout belongs to execution at this position, not time spent on earlier GPU work
    }
    if (cb->_eventArmed != range.from + 1) {
        cb->_eventArmed = range.from + 1;
        cb->_eventStart = raw::EventDeadline::Clock::now();
        cb->_eventWaits.push_back(range.from);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (promisedEvent(c.obj, c.c))
            gRM->stats.waitsOnPromise++;
#endif
        // Registration is done outside the scheduler mutex: an already satisfied event may deliver its notification
        // immediately. The callback does not retain a command buffer or scan unrelated dependency lists.
        [event notifyListener:gRM->listener
                      atValue:c.c
                        block:^(id<MTLSharedEvent>, uint64_t) {
                            wakeSubmit();
                        }];
    }
    const raw::EventDeadline deadline(cb->_eventStart, c.a);
    const Readiness state = deadline.state([event signaledValue] >= c.c, raw::EventDeadline::Clock::now());
    if (state == Readiness::blocked)
        nextWake = std::min(nextWake, deadline.until());
    return state;
}

bool expiredEventWait(RMCommandBuffer *cb, size_t index)
{
    const Cmd c = cb->_plan.legacy(index);
    return c.a && cb->_eventArmed == index + 1 &&
           raw::EventDeadline::Clock::now() >= raw::EventDeadline(cb->_eventStart, c.a).until();
}
