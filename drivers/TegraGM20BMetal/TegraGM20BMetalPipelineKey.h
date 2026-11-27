// SPDX-License-Identifier: MIT
// Objective-C++ adapters over pipeline_key.h for MTLRenderPipelineDescriptor and MTLFunctionConstantValues.
#pragma once
#import <Metal/Metal.h>
#include <objc/message.h>
#include <cstdio>
#include <string>
#include "pipeline_key.h"

@protocol SwitchMetalConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (SwitchMetalPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

namespace gm20b {

// "#<index>:<MTLDataType>:<hex bytes>" by index in its order, then "<name>:<MTLDataType>:<hex bytes>"; "" for no values
inline std::string constantsText(MTLFunctionConstantValues *values)
{
    std::string text;
    if (!values)
        return text;
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *groups[2] = {[snapshot newIndexedConstantArray], [snapshot newNamedConstantArray]};
    for (int g = 0; g < 2; g++)
        for (id<SwitchMetalConstantValue> v in groups[g]) {
            const size_t n = constantBytes(v.dataType);
            char head[48];
            if (g)
                text += std::string(text.empty() ? "" : " ") + v.name.UTF8String;
            else {
                snprintf(head, sizeof head, "%s#%lu", text.empty() ? "" : " ", (unsigned long)v.index);
                text += head;
            }
            snprintf(head, sizeof head, ":%lu:", (unsigned long)v.dataType);
            text += head;
            for (size_t i = 0; i < n; i++) {
                snprintf(head, sizeof head, "%02x", ((const uint8_t *)v.data)[i]);
                text += head;
            }
        }
    return text;
}

// MTLRenderPipelineDescriptor's textureWriteRoundingMode (Apple SPI; 0 where Metal has none): 0 and 1 store a float
// colour output to a half-float target rounded toward zero, 2 to nearest even
inline uint64_t textureWriteRoundingOf(MTLRenderPipelineDescriptor *d)
{
    static const SEL sel = sel_registerName("textureWriteRoundingMode");
    return [d respondsToSelector:sel] ? reinterpret_cast<NSUInteger (*)(id, SEL)>(objc_msgSend)(d, sel) : 0;
}

// The descriptor's fields a key names (pipeline_fields.h): what a compile request carries of it, so the shader compiler
// keys a request with the same pipelineKey the client keyed its descriptor with
inline PipelineFields fieldsOf(MTLRenderPipelineDescriptor *d)
{
    PipelineFields f;
    f.samples = d.rasterSampleCount;
    f.topology = d.inputPrimitiveTopology;
    f.depthFormat = d.depthAttachmentPixelFormat;
    f.stencilFormat = d.stencilAttachmentPixelFormat;
    f.rasterization = d.rasterizationEnabled ? 1 : 0;
    f.amplification = d.maxVertexAmplificationCount;
    f.textureWriteRounding = textureWriteRoundingOf(d);
    f.patchType = d.vertexFunction.patchType;
    f.alphaToCoverage = d.alphaToCoverageEnabled ? 1 : 0;
    f.tessellation = {d.tessellationPartitionMode,
                      d.tessellationFactorFormat,
                      d.tessellationFactorStepFunction,
                      d.tessellationOutputWindingOrder,
                      d.tessellationControlPointIndexType,
                      d.maxTessellationFactor,
                      d.tessellationFactorScaleEnabled ? 1u : 0u};
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        f.color[i] = {a.pixelFormat,
                      a.writeMask,
                      a.blendingEnabled ? 1u : 0u,
                      a.sourceRGBBlendFactor,
                      a.destinationRGBBlendFactor,
                      a.rgbBlendOperation,
                      a.sourceAlphaBlendFactor,
                      a.destinationAlphaBlendFactor,
                      a.alphaBlendOperation};
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            f.attributes[i] = {a.format, a.offset, a.bufferIndex, l.stride, l.stepFunction, l.stepRate};
        }
    return f;
}

inline std::string pipelineKey(MTLRenderPipelineDescriptor *d, const StageKey *vertex, const StageKey *fragment,
                               const std::string &linked, const std::string &framework)
{
    return pipelineKey(fieldsOf(d), vertex, fragment, linked, framework);
}

// The fields the key names, read alone (fieldsOf reads every one, for a compile request), with the vertex function's
// MTLPatchType `patchType`
inline PipelineFields keyFieldsOf(MTLRenderPipelineDescriptor *d, uint64_t patchType)
{
    PipelineFields f;
    f.samples = d.rasterSampleCount;
    f.topology = d.inputPrimitiveTopology;
    f.depthFormat = d.depthAttachmentPixelFormat;
    f.stencilFormat = d.stencilAttachmentPixelFormat;
    f.rasterization = d.rasterizationEnabled ? 1 : 0;
    f.amplification = d.maxVertexAmplificationCount;
    f.textureWriteRounding = textureWriteRoundingOf(d);
    f.patchType = patchType;
    f.alphaToCoverage = d.alphaToCoverageEnabled ? 1 : 0;
    if (patchType)
        f.tessellation = {d.tessellationPartitionMode,
                          d.tessellationFactorFormat,
                          d.tessellationFactorStepFunction,
                          d.tessellationOutputWindingOrder,
                          d.tessellationControlPointIndexType,
                          d.maxTessellationFactor,
                          d.tessellationFactorScaleEnabled ? 1u : 0u};
    MTLRenderPipelineColorAttachmentDescriptorArray *colors = d.colorAttachments;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = colors[i];
        if (const MTLPixelFormat format = a.pixelFormat)
            f.color[i] = {format,
                          a.writeMask,
                          a.blendingEnabled ? 1u : 0u,
                          a.sourceRGBBlendFactor,
                          a.destinationRGBBlendFactor,
                          a.rgbBlendOperation,
                          a.sourceAlphaBlendFactor,
                          a.destinationAlphaBlendFactor,
                          a.alphaBlendOperation};
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor) {
        MTLVertexAttributeDescriptorArray *attributes = v.attributes;
        MTLVertexBufferLayoutDescriptorArray *layouts = v.layouts;
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = attributes[i];
            const MTLVertexFormat format = a.format;
            if (format == MTLVertexFormatInvalid)
                continue;
            const NSUInteger buffer = a.bufferIndex;
            MTLVertexBufferLayoutDescriptor *l = layouts[buffer < 31 ? buffer : 0];
            f.attributes[i] = {format, a.offset, buffer, l.stride, l.stepFunction, l.stepRate};
        }
    }
    return f;
}

}  // namespace gm20b
