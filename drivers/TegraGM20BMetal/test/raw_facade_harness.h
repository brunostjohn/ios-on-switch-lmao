// SPDX-License-Identifier: MIT
// The raw facade on the host: its device opened once per process over the kernel model with no m2v device, the frontend and
// the bridge answered by raw_facade_standins.mm. Include it after raw_device_harness.h, in the one unit of a test that holds
// the kernel model.
#pragma once
#import <Metal/Metal.h>
#include <cstring>
#include <vector>
#include "raw_facade_standins.h"
#import "../raw/runtime/RawMetal.h"
#import "../raw/runtime/RawMetalInternal.h"

namespace facade {

inline id<MTLDevice> device()
{
    static id<MTLDevice> opened = [] {
        NSString *why = nil;
        id<MTLDevice> made = raw_metal_device_open(harness::emptyPack().c_str(), 0, nil, &why);
        EXPECT_NE(made, nil) << (why ? why.UTF8String : "");
        return made;
    }();
    return opened;
}

inline uint64_t word(MTLResourceID id)
{
    uint64_t w;
    memcpy(&w, &id, sizeof w);
    return w;
}

inline id<MTLBuffer> shared(NSUInteger length)
{
    return [device() newBufferWithLength:length options:MTLResourceStorageModeShared];
}

// An argument buffer of `fill` bytes, none of which an encoder's word or a test's constant is
inline id<MTLBuffer> filled(NSUInteger length, uint8_t fill = 0xA5)
{
    id<MTLBuffer> b = shared(length);
    memset(b.contents, fill, length);
    return b;
}

inline id<MTLTexture> texture(MTLStorageMode mode = MTLStorageModeShared, id<MTLHeap> heap = nil)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = mode;
    d.usage = MTLTextureUsageShaderRead;
    return heap ? [heap newTextureWithDescriptor:d] : [device() newTextureWithDescriptor:d];
}

inline id<MTLSamplerState> sampler(bool normalized = true)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.supportArgumentBuffers = YES;
    d.normalizedCoordinates = normalized;
    return [device() newSamplerStateWithDescriptor:d];
}

inline id<MTLHeap> heap(MTLStorageMode mode, NSUInteger bytes)
{
    MTLHeapDescriptor *d = [MTLHeapDescriptor new];
    d.size = bytes;
    d.storageMode = mode;
    d.type = MTLHeapTypeAutomatic;
    return [device() newHeapWithDescriptor:d];
}

inline std::vector<uint8_t> bytesOf(id<MTLBuffer> b)
{
    const auto *at = static_cast<const uint8_t *>(b.contents);
    return {at, at + b.length};
}

inline uint64_t at(id<MTLBuffer> b, uint64_t offset)
{
    uint64_t w;
    memcpy(&w, static_cast<const uint8_t *>(b.contents) + offset, sizeof w);
    return w;
}

}  // namespace facade
