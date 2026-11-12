// SPDX-License-Identifier: MIT
// What the device has no form of, answered where Metal lets a caller ask: the capability queries (raw_device_answers.h),
// the creations that may fail (nil, with an error where Metal gives one), and the calls a caller reaches only past a NO.
#import "RawMetalInternal.h"

const char *rmUnsupportedRenderState(MTLRenderPipelineDescriptor *d)
{
    if (d.vertexFunction.patchType != MTLPatchTypeNone)
        return "a post-tessellation vertex function: supportsTessellation answers NO, maxTessellationFactor 0";
    if (d.maxVertexAmplificationCount > 1)
        return "a vertex amplification count above 1: supportsVertexAmplificationCount: answers NO for it";
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format != MTLVertexFormatInvalid && a.bufferIndex < 31 &&
                v.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a dynamic attribute stride: supportsDynamicAttributeStride answers NO";
        }
    return nullptr;
}

const char *rmUnsupportedComputeState(MTLComputePipelineDescriptor *d)
{
    if (MTLStageInputOutputDescriptor *in = d.stageInputDescriptor)
        for (NSUInteger i = 0; i < 31; i++) {
            MTLAttributeDescriptor *a = in.attributes[i];
            if (a.format != MTLAttributeFormatInvalid && a.bufferIndex < 31 &&
                in.layouts[a.bufferIndex].stride == MTLBufferLayoutStrideDynamic)
                return "a dynamic attribute stride: supportsDynamicAttributeStride answers NO";
        }
    return nullptr;
}

id rmRefuseUnsupported(const char *why, NSError **error)
{
    SWITCH_REFUSE_DETAIL("raw", "state.unsupported", uintptr_t(why), "state refused at creation: %s", why);
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorUnsupported
                                 userInfo:@{NSLocalizedDescriptionKey : @(why)}];
    return nil;
}

@interface RMRenderEncoder (Amplification)
@end
@implementation RMRenderEncoder (Amplification)

// Count 1 is Metal's default, one view. No state of this device takes more (rmUnsupportedRenderState), so a larger count is
// the caller's error: one line, and its draws render the one view.
- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)mappings
{
    (void)mappings;
    if (count > 1)
        SWITCH_REFUSE_DETAIL("raw", "call.amplification", count,
                             "setVertexAmplificationCount:%lu: supportsVertexAmplificationCount: answers NO above 1, "
                             "its draws render one view",
                             (unsigned long)count);
}

@end
