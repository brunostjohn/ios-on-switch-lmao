// SPDX-License-Identifier: MIT
#import "RawMetalEventJobs.h"
#import "RawMetalInternal.h"
#import "SwitchSharedEvent.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.*: an object this device made itself answers its class and whether m2v's twin of it was ever
// made; an m2v object does not answer
@implementation RMObject (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @(_twin != nil)};
}

@end

@implementation SwitchSharedEvent (NativeProbe)

- (NSDictionary<NSString *, id> *)switchRawObject
{
    return @{@"class" : NSStringFromClass([self class]), @"twin" : @NO};
}

@end

// The signal jobs of no work made since the device opened, and the signals whose value the CPU set after the work before them
// (an event the kernel does not hold)
@implementation RMDevice (EventProbe)

- (NSDictionary<NSString *, NSNumber *> *)switchRawEvents
{
    return @{@"signal_jobs" : @(eventJobsSubmitted()), @"cpu_signals" : @(gRM->stats.signalsAfterWork.load())};
}

@end
#endif
