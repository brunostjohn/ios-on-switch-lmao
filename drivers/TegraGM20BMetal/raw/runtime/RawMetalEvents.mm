// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

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
