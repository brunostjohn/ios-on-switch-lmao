// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalBinaryArchive.h"
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

// The descriptor's, as Metal's state answers it, without making m2v's twin
- (BOOL)supportIndirectCommandBuffers
{
    return _direct.supportIndirectCommandBuffers;
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

// The archives a creation under MTLPipelineOptionFailOnBinaryArchiveMiss lists (this device's, RawMetalBinaryArchive.mm), as
// Metal's answer: false (and Metal's error) where none holds the state and the pack holds no kernel of it, never compiled.
// m2v's state goes without the archives or the option either way: it reads none of this device's archives.
// A stage input descriptor's set attributes and their layouts, as a key names them (a dynamic stride as UINT32_MAX, as
// RawPipelineKey.h's vertex layouts): "" for none, so a kernel without one keys as before
static std::string stageInText(MTLStageInputOutputDescriptor *in)
{
    std::string text;
    char b[96];
    for (NSUInteger i = 0; in && i < 31; i++) {
        MTLAttributeDescriptor *a = in.attributes[i];
        if (a.format == MTLAttributeFormatInvalid)
            continue;
        MTLBufferLayoutDescriptor *l = in.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
        const unsigned long stride = l.stride == MTLBufferLayoutStrideDynamic ? UINT32_MAX : l.stride;
        snprintf(b, sizeof b, " a%lu=%lu:%lu:%lu:%lu:%lu:%lu", (unsigned long)i, (unsigned long)a.format,
                 (unsigned long)a.offset, (unsigned long)a.bufferIndex, stride, (unsigned long)l.stepFunction,
                 (unsigned long)l.stepRate);
        text += b;
    }
    if (text.empty())
        return text;
    snprintf(b, sizeof b, " i=%lu:%lu", (unsigned long)in.indexType, (unsigned long)in.indexBufferIndex);
    return " | in" + text + b;
}

static bool archivesAnswer(RMComputeState *s, RMFunction *f, MTLPipelineOption &options, NSError **error)
{
    raw::ArchiveAnswer answer = raw::ArchiveAnswer::NotAsked;
    if (f) {
        NSString *name = native_bridge_metal_function_library_name(s->_direct.computeFunction) ?: f.name;
        const std::string key =
            raw::computeArchiveKey(name.UTF8String ?: "", s->_constants, s->_maxThreads, s->_multipleOfWidth);
        answer = rmArchiveAnswer(s->_direct.binaryArchives, options, raw::Pack::fnv(key.data(), key.size()));
    }
    options &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    s->_direct.binaryArchives = nil;
    if (answer != raw::ArchiveAnswer::Missed ||
        (s->_keyed &&
         rmPackHoldsKernels(raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth))))
        return true;
    if (error)
        *error = rmArchiveMissError(f.name, "compute");
    return false;
}

id rmComputeState(id device, id<MTLDevice> fallback, MTLComputePipelineDescriptor *descriptor,
                  MTLPipelineOption options, MTLAutoreleasedComputePipelineReflection *reflection, NSError **error)
{
    if (const char *why = rmUnsupportedComputeState(descriptor))
        return rmRefuseUnsupported(why, error);
    id<MTLFunction> function = descriptor.computeFunction;
    RMComputeState *s = [RMComputeState new];
    s->_device = device;
    s->_fallback = fallback;
    RMFunction *f = [(id)function isKindOfClass:[RMFunction class]] ? (RMFunction *)function : nil;
    // linked functions are no part of the key: such a state's dispatches stay m2v's
    s->_keyed = f && !descriptor.linkedFunctions.functions.count &&
                !descriptor.linkedFunctions.privateFunctions.count && airOf(function, s->_air);
    s->_constants = (f ? f->_constants : "") + stageInText(descriptor.stageInputDescriptor);
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
