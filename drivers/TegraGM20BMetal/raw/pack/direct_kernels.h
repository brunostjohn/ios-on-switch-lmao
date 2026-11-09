// SPDX-License-Identifier: MIT
// A corpus's compute dispatch shapes as the direct compile makes their kernels (runtime/raw_direct_compute.h), read as the
// kernel probe read them (kernel_shapes.h): the pack generator's directpack.mm and the host check test/kernelcheck.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <set>
#include <string>
#include <vector>
#include "bridge/NativeMetalDirect.h"
#include "kernel_shapes.h"

namespace probe {

// What a direct compile is asked for: `dispatch` points into the vectors, so it moves with them
struct KernelDispatch {
    NmbDirectDispatch dispatch = {};
    std::vector<NmbDirectBoundTexture> textures;
    std::vector<NmbDirectBoundSampler> samplers;

    KernelDispatch() = default;
    KernelDispatch(const KernelDispatch &) = delete;
    KernelDispatch &operator=(const KernelDispatch &) = delete;
};

inline NmbSamplerDescriptor samplerDescriptor(const raw::BoundSampler &b)
{
    return {b.minFilter,  b.magFilter,     b.mipFilter, b.sAddress,   b.tAddress, b.rAddress, b.border,
            b.normalized, b.maxAnisotropy, b.compare,   b.lodAverage, b.lodMin,   b.lodMax};
}

// `s`'s dispatch at `threads` threads a threadgroup, its textures `bound` (textures()' of its own)
inline void dispatchOf(const Shape &s, const uint32_t threads[3], NSArray<id<MTLTexture>> *bound, KernelDispatch &out)
{
    memcpy(out.dispatch.threads, threads, sizeof out.dispatch.threads);
    std::istringstream memory(s.threadgroupMemory);
    std::string pair;
    while (std::getline(memory, pair, ',')) {
        const unsigned long index = strtoul(pair.c_str(), nullptr, 10);
        if (index < 31)
            out.dispatch.memory[index] = uint32_t(strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10));
    }
    for (size_t i = 0; i < s.textures.size(); i++) {
        const raw::BoundTexture &b = s.textures[i];
        out.textures.push_back(
            {b.slot, b.pixelFormat, b.textureType, uint64_t(bound[i].usage), b.levels, b.arrayLength, 1});
    }
    for (const raw::BoundSampler &b : s.samplers)
        out.samplers.push_back({b.slot, samplerDescriptor(b)});
    out.dispatch.textures = out.textures.data();
    out.dispatch.textureCount = uint32_t(out.textures.size());
    out.dispatch.samplers = out.samplers.data();
    out.dispatch.samplerCount = uint32_t(out.samplers.size());
}

struct Kernel {
    std::string line, key;
    Shape shape;
    MTLComputePipelineDescriptor *descriptor = nil;
    NSArray<id<MTLTexture>> *textures = nil;
};

// Each dispatch shape of `text`'s D lines the kernel probe made (its state made, its key once): `each(kernel)`, and
// `refused(line, why)` for one it would not make
template <typename Each, typename Refused>
void eachKernel(id<MTLDevice> dev, NSString *text, Each each, Refused refused)
{
    std::set<std::string> seen;
    for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
        if (![l hasPrefix:@"D ok=1 "])
            continue;
        Kernel k;
        k.line = l.UTF8String;
        std::string why;
        raw::AirHash air;
        id<MTLFunction> f = nil;
        if (!parse(k.line, k.shape, why) || !(f = function(dev, k.shape, air)) || !state(dev, k.shape, f, why) ||
            !(k.textures = textures(dev, k.shape.textures, why))) {
            refused(k.line, why.empty() ? "no function " + k.shape.function : why);
            continue;
        }
        k.key = key(k.shape, air);
        if (!seen.insert(k.key).second)
            continue;
        k.descriptor = descriptor(k.shape, f);
        each(k);
    }
}

}  // namespace probe
