// SPDX-License-Identifier: MIT
// The device's counter sets, sample buffers and clock, as the identity's AGX driver answers them (raw_counters.h).
#import "RawMetalCounters.h"

static NSString *text(std::string_view s)
{
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

@interface RMCounter : NSObject <MTLCounter>
@end
@implementation RMCounter

- (NSString *)name
{
    return MTLCommonCounterTimestamp;
}

- (NSString *)description
{
    static NSString *const description = text(raw::counters::kCounterDescription);
    return description;
}

@end

@interface RMCounterSet : NSObject <MTLCounterSet>
@end
@implementation RMCounterSet

- (NSString *)name
{
    return MTLCommonCounterSetTimestamp;
}

- (NSArray<id<MTLCounter>> *)counters
{
    static NSArray<id<MTLCounter>> *const counters = @[ [RMCounter new] ];
    return counters;
}

- (NSString *)description
{
    static NSString *const description = text(raw::counters::kSetDescription);
    return description;
}

@end

@implementation RMCounterSampleBuffer

- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}

- (NSUInteger)sampleCount
{
    return _sampleCount;
}

// The driver's raw copy of the samples, here each record placed by the device's anchor
- (NSData *)resolveCounterRange:(NSRange)range
{
    if (!raw::counters::resolvable(range.location, range.length, _sampleCount))
        return nil;
    const auto *records = static_cast<const raw::counters::Record *>([(id<MTLBuffer>)_storage contents]);
    if (!records)
        return nil;
    const raw::counters::Anchor anchor = rmCounterAnchor();
    NSMutableData *data = [NSMutableData dataWithLength:range.length * raw::counters::kSampleBytes];
    auto *out = static_cast<uint64_t *>(data.mutableBytes);
    for (NSUInteger i = 0; i < range.length; i++) {
        raw::counters::Record r;
        memcpy(&r, records + range.location + i, sizeof r);
        out[i] = raw::counters::resolved(r, anchor);
    }
    return data;
}

@end

static std::atomic<bool> gCounterBuffersMade{false};

bool rmCounterBuffersMade()
{
    return gCounterBuffersMade.load(std::memory_order_relaxed);
}

uint64_t rmHostNanoseconds()
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

static raw::counters::Reading readPtimer()
{
    raw::counters::Reading r = {};
    r.before = rmHostNanoseconds();
    gRM->raw->ptimer(r.units);
    r.after = rmHostNanoseconds();
    return r;
}

raw::counters::Anchor rmCounterAnchor()
{
    static std::mutex lock;
    static raw::counters::Anchor held;
    std::lock_guard<std::mutex> hold(lock);
    const raw::counters::Reading now = readPtimer();
    if (raw::counters::agrees(held, now) || (held.valid && raw::counters::poweredDown(now)))
        return held;
    const raw::counters::Reading readings[3] = {now, readPtimer(), readPtimer()};
    held = raw::counters::anchorOf(raw::counters::narrowest(readings, 3));
    if (!held.valid)
        SWITCH_REFUSE("raw", "counter.anchor",
                      "the kernel gave no PTIMER reading: counter samples resolve to the error value");
    return held;
}

static id failed(NSError **error, MTLCounterSampleBufferError code, const std::string &message)
{
    if (error)
        *error = [NSError errorWithDomain:MTLCounterErrorDomain
                                     code:code
                                 userInfo:@{
                                     NSLocalizedDescriptionKey : @(message.c_str())
                                 }];
    return nil;
}

@interface RMDevice (Counters)
@end
@implementation RMDevice (Counters)

- (NSArray<id<MTLCounterSet>> *)counterSets
{
    static NSArray<id<MTLCounterSet>> *const sets = @[ [RMCounterSet new] ];
    return sets;
}

// -[AGXA1xFamilyDevice counterSampleBufferLimits]: the same constant on both identities
- (NSDictionary *)counterSampleBufferLimits
{
    return @{
        @"maxBufferSize" : @(raw::counters::kMaxBufferBytes),
        @"maxDeviceBufferNum" : @32,
        @"maxGlobalBufferNum" : @1024,
    };
}

- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)d
                                                             error:(NSError **)error
{
    id<MTLCounterSet> set = d.counterSet;
    NSArray<id<MTLCounter>> *counters = set.counters;
    NSString *setName = set.name, *counterName = counters.count ? counters[0].name : nil;
    const raw::counters::Check check = raw::counters::checkDescriptor(
        counters.count, setName.UTF8String ?: "", counterName.UTF8String ?: "", uint64_t(d.sampleCount));
    if (check != raw::counters::Check::Ok) {
        NSString *found = check == raw::counters::Check::SetName ? setName : counterName;
        return failed(
            error, MTLCounterSampleBufferErrorInvalid,
            raw::counters::checkMessage(check, found ? found.UTF8String : "(null)", uint64_t(d.sampleCount) << 3));
    }
    const NSUInteger bytes = d.sampleCount * raw::counters::kRecordBytes;
    RMBuffer *storage = (RMBuffer *)[(id<MTLDevice>)self newBufferWithLength:bytes
                                                                     options:MTLResourceStorageModeShared];
    if (![(id)storage isKindOfClass:[RMBuffer class]] || !storage->_raw.cpu)
        return failed(error, MTLCounterSampleBufferErrorOutOfMemory, "Cannot allocate sample buffer");
    memset(storage->_raw.cpu, 0, bytes);
    if (!storage->_raw.wc)
        cacheLines(storage->_raw.cpu, bytes, false);
    RMCounterSampleBuffer *b = [RMCounterSampleBuffer new];
    b->_device = self;
    b->_storage = storage;
    b->_sampleCount = d.sampleCount;
    b.label = d.label;
    gCounterBuffersMade.store(true, std::memory_order_relaxed);
    // A private buffer's samples are resolved by a blit alone: its kernel compiled ahead (a shared one's at the first)
    static std::once_flag warm;
    if (d.storageMode == MTLStorageModePrivate)
        std::call_once(warm, [] {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                rmCounterResolveState();
            });
        });
    if (error)
        *error = nil;
    return b;
}

// The driver's answer: one reading of the CPU's clock for both, the GPU's samples being in its domain
- (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp
{
    const uint64_t ns = rmHostNanoseconds();
    if (cpuTimestamp)
        *cpuTimestamp = ns;
    if (gpuTimestamp)
        *gpuTimestamp = ns;
}

@end
