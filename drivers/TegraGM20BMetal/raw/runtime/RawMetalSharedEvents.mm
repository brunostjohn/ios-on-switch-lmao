// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "SwitchSharedEvent.h"

@interface NSObject (SwitchSharedEventPort)
- (mach_port_t)eventPort;
- (BOOL)supportsRollback;
@end

@interface SwitchSharedEventBinding : NSObject {
@public
    std::shared_ptr<raw::SharedEventLease> lease;
    std::atomic<uint64_t> submittedValue;
}
@end
@implementation SwitchSharedEventBinding
@end

static SwitchSharedEventBinding *eventBinding(id event)
{
    if (!gRM->sharedEvents || ![event respondsToSelector:@selector(eventPort)] ||
        ([event respondsToSelector:@selector(supportsRollback)] && [event supportsRollback]))
        return nil;
    static char bindingKey;
    SwitchSharedEventBinding *binding = objc_getAssociatedObject(event, &bindingKey);
    if (binding && binding->lease->owner() == gRM->sharedEvents)
        return binding;
    @synchronized(event) {
        binding = objc_getAssociatedObject(event, &bindingKey);
        if (binding && binding->lease->owner() == gRM->sharedEvents)
            return binding;
        auto lease = gRM->sharedEvents->import([event eventPort]);
        if (!lease)
            return nil;
        binding = [SwitchSharedEventBinding new];
        binding->lease = std::move(lease);
        binding->submittedValue.store(0, std::memory_order_relaxed);
        objc_setAssociatedObject(event, &bindingKey, binding, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return binding;
    }
}

std::shared_ptr<raw::SharedEventLease> sharedEventLease(id event)
{
    SwitchSharedEventBinding *binding = eventBinding(event);
    return binding ? binding->lease : nullptr;
}

static void eventFailure(RMCommandBuffer *cb, NSString *message)
{
    std::lock_guard<std::mutex> lock(cb->_lock);
    cb->_segError = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                                   userInfo:@{NSLocalizedDescriptionKey: message}];
}

int sharedEventReadiness(RMCommandBuffer *cb, id event, uint64_t value, bool native)
{
    if (!native)
        return -1;
    SwitchSharedEventBinding *binding = eventBinding(event);
    if (!binding)
        return -1;
    auto lease = binding->lease;
    // This is only a readiness hint. Atomic submit still resolves the authoritative producer and its failure state.
    const int result = binding->submittedValue.load(std::memory_order_acquire) >= value ? KERN_SUCCESS :
                       gRM->sharedEvents->probe(*lease, value);
    if (result == KERN_SUCCESS) {
        cb->_sharedEventWaits.push_back({(__bridge void *)event, std::move(lease), value});
        return 1;
    }
    if (result != kIOReturnNotReady) {
        eventFailure(cb, @"A shared-event producer failed");
        return 1;
    }
    if (!cb->_sharedEventWatch) {
        RMQueue *queue = cb->_queue;
        int armed = KERN_SUCCESS;
        cb->_sharedEventWatch = gRM->sharedEvents->observe(std::move(lease), value, [queue](int) {
            wakeSubmit((__bridge void *)queue);
        }, armed);
        if (!cb->_sharedEventWatch) {
            eventFailure(cb, @"Shared-event producer notification could not be armed");
            return 1;
        }
    }
    return 0;
}

bool sharedWaitsReady(RMCommandBuffer *cb, bool native)
{
    if (native)
        return true;
    for (auto &wait : cb->_sharedEventWaits) {
        if (!wait.completion) {
            RMQueue *queue = cb->_queue;
            int armed = KERN_SUCCESS;
            wait.completion = gRM->sharedEvents->completion(wait.lease, wait.value, [queue] {
                wakeSubmit((__bridge void *)queue);
            }, armed);
            if (!wait.completion) {
                eventFailure(cb, @"Shared-event completion notification could not be armed");
                return true;
            }
        }
        const int result = wait.completion->result();
        if (result == kIOReturnNotReady)
            return false;
        if (result != KERN_SUCCESS) {
            eventFailure(cb, @"A shared-event producer failed before compatibility work");
            return true;
        }
    }
    cb->_sharedEventWaits.clear();
    return true;
}

void prepareSharedEvents(RMCommandBuffer *cb, raw::CommandBuffer *commands)
{
    std::vector<TegraGM20BAbiEventUse> waits, signals;
    waits.reserve(cb->_sharedEventWaits.size());
    for (const auto &wait : cb->_sharedEventWaits)
        waits.push_back({wait.lease->handle(), wait.value});
    cb->_sharedSignalRanges.clear();
    if (cb->_gpuPrefixEligible) {
        for (size_t at = cb->_segNext + 1; at < cb->_segs.size(); ++at) {
            const auto &range = cb->_segs[at];
            if (!range.event)
                break;
            const Cmd event = cb->_plan.legacy(range.from);
            if (event.op != OpSignalEvent)
                break;
            auto lease = sharedEventLease((__bridge id)event.obj);
            if (!lease)
                continue;
            signals.push_back({lease->handle(), event.c});
            cb->_sharedSignalRanges.push_back(at);
        }
    }
    commands->sharedEvents(std::move(waits), std::move(signals));
}

void publishSharedEvents(RMCommandBuffer *cb)
{
    for (size_t at : cb->_sharedSignalRanges) {
        cb->_segs[at].gpuSignal = true;
        const Cmd event = cb->_plan.legacy(cb->_segs[at].from);
        SwitchSharedEventBinding *binding = eventBinding((__bridge id)event.obj);
        if (binding) {
            uint64_t old = binding->submittedValue.load(std::memory_order_relaxed);
            while (old < event.c && !binding->submittedValue.compare_exchange_weak(
                old, event.c, std::memory_order_release, std::memory_order_relaxed)) {}
        }
    }
    cb->_sharedSignalRanges.clear();
    cb->_sharedEventWaits.clear();
}
