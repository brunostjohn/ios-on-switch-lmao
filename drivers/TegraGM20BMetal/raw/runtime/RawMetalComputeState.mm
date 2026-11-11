// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalComputeState.h"
#include "NativeBridgeMetalFrontend.h"
#include "raw_compute_key.h"
#include "raw_metallib.h"

@implementation RMComputeState

+ (Class)twinClass
{
    return NSClassFromString(@"NBComputePipeline");
}

- (Class)rmTwinClass
{
    return [[self class] twinClass];
}

- (id)rmTwin
{
    std::call_once(_twinOnce, [self] {
        if (!_compatibility) {
            NSError *error = nil;
            _compatibility = [_fallback newComputePipelineStateWithDescriptor:_direct
                                                                      options:MTLPipelineOptionNone
                                                                   reflection:nil
                                                                        error:&error];
            if (!_compatibility)
                once([NSString stringWithFormat:@"a deferred compute pipeline could not be created: %@", error]);
        }
    });
    return _compatibility;
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    (void)selector;
    return [self rmTwin];
}

- (BOOL)respondsToSelector:(SEL)selector
{
    return [super respondsToSelector:selector] || [[[self class] twinClass] instancesRespondToSelector:selector];
}

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    return protocol == @protocol(MTLComputePipelineState) || [super conformsToProtocol:protocol];
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return [_metadata maxTotalThreadsPerThreadgroup];
}

- (NSUInteger)threadExecutionWidth
{
    return [_metadata threadExecutionWidth];
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return [_metadata staticThreadgroupMemoryLength];
}

@end

static bool airOf(id<MTLFunction> function, raw::AirHash &out)
{
    const void *bytes = nullptr;
    size_t length = 0;
    std::map<std::string, raw::AirHash> hashes;
    if (!native_bridge_metal_function_metallib(twinOf(function), &bytes, &length) ||
        !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
        return false;
    const auto f = hashes.find(native_bridge_metal_function_library_name(twinOf(function)).UTF8String ?: "");
    if (f == hashes.end())
        return false;
    out = f->second;
    return true;
}

// The archives a creation under MTLPipelineOptionFailOnBinaryArchiveMiss lists, as Metal's answer: false (and Metal's error)
// where every one is Apple's, none declares the state and the pack holds no kernel of it, never compiled. Where one declares
// it or the pack holds it, m2v's state goes without them (it reads an Apple archive as holding nothing); m2v answers for its
// own.
static bool archivesAnswer(RMComputeState *s, RMFunction *f, MTLPipelineOption &options, NSError **error)
{
    if (!f)
        return true;
    NSString *name = native_bridge_metal_function_library_name(s->_direct.computeFunction) ?: f.name;
    const std::string key =
        raw::computeArchiveKey(name.UTF8String ?: "", s->_constants, s->_maxThreads, s->_multipleOfWidth);
    const RMArchiveAnswer answer =
        rmArchiveAnswer(s->_direct.binaryArchives, options, raw::Pack::fnv(key.data(), key.size()));
    const bool held =
        answer == RMArchiveAnswer::Missed && s->_keyed &&
        rmPackHoldsKernels(raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth));
    if (answer == RMArchiveAnswer::Missed && !held) {
        if (error)
            *error = rmArchiveMissError(f.name, "compute");
        return false;
    }
    if (answer == RMArchiveAnswer::Declared || held) {
        options &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
        s->_direct.binaryArchives = nil;
    }
    return true;
}

id rmComputeState(id device, id<MTLDevice> fallback, MTLComputePipelineDescriptor *descriptor,
                  MTLPipelineOption options, MTLAutoreleasedComputePipelineReflection *reflection, NSError **error)
{
    id<MTLFunction> function = descriptor.computeFunction;
    RMComputeState *s = [RMComputeState new];
    s->_device = device;
    s->_fallback = fallback;
    RMFunction *f = [(id)function isKindOfClass:[RMFunction class]] ? (RMFunction *)function : nil;
    // linked functions are no part of the key: such a state's dispatches stay m2v's
    s->_keyed = f && !descriptor.linkedFunctions.functions.count &&
                !descriptor.linkedFunctions.privateFunctions.count && airOf(function, s->_air);
    s->_constants = f ? f->_constants : "";
    s->_maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
    s->_multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    s->_direct = [descriptor copy];
    s->_direct.computeFunction = twinOf(function);
    if (!archivesAnswer(s, f, options, error))
        return nil;
    id nativeReflection = nil;
    NSError *nativeError = nil;
    if (s->_keyed)
        s->_metadata = native_bridge_metal_compute_prepare(s->_direct, options,
                                                           reflection ? &nativeReflection : nullptr, &nativeError);
    if (!s->_metadata) {
        s->_compatibility = [fallback newComputePipelineStateWithDescriptor:s->_direct
                                                                    options:options
                                                                 reflection:reflection
                                                                      error:error];
        s->_metadata = s->_compatibility;
        if (!s->_metadata)
            return nil;
    } else {
        if (reflection)
            *reflection = nativeReflection;
        if (error)
            *error = nil;
    }
    s->_threadLimit = uint32_t(std::min<NSUInteger>(1024, [s->_metadata maxTotalThreadsPerThreadgroup]));
    if (s->_maxThreads)
        s->_threadLimit = std::min(s->_threadLimit, s->_maxThreads);
    s.label = descriptor.label;
    return s;
}
