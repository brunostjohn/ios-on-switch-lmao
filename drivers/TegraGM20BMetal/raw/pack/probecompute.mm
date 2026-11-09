// SPDX-License-Identifier: MIT
// raw GM20B path, pack generator: every corpus compute dispatch shape (the journal's D lines: a compute state and the
// threadgroup size it was dispatched with) made through the shipped frontend + m2v + NVK host builds over the mock GM20B, and
// dispatched once with tagged inputs, so mkpack.py can read off each kernel's record from nvdec's K lines
// (pack/nvdec_compute.cpp, NVDEC_KERNELS): NVK's QMD, the program, and where each Metal argument lands in set 0.
//
// Tags: every buffer slot is setBytes of 4096 bytes whose first word is 0xC0DE0000 | slot (the range m2v gives a slot below
// 4096 is the function's window on it); the line's tex= textures, each (16 + slot) x (8 + slot) in its format; its smp=
// samplers as they are (the key holds them: the record takes their headers as its own); threadgroup memory as its tgmem=
// field names it (<index>:<length>,...). Three command buffers per shape, their grids (3 + 3i + v)x5x2 threadgroups for the
// i-th line (its records name the dispatches by them); the second binds the same tags from buffers at another address, the
// third binds each of those buffers probe_bind.h's probeComputeCode bytes past its tag, which m2v's descriptor still starts
// at, so the root push word holding the slot's SlotBindOffset residual is the one set to that code. stdout: "KPROBE <i>
// tg=<x,y,z> grid=<x,y,z> key=<raw_compute_key.h key>" before each one is committed, "KREFUSED <i> <why>" for a shape not
// made.
// Libraries: lib=<path> under PROBE_LIB_ROOT (the crawl's root), lib=data:<SHA-256> as <PROBE_DATA_LIBS>/<SHA-256>.metallib.
// usage: probecompute <nvk dylib> <cache dir> <corpus.log>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "kernel_shapes.h"
#include "probe_bind.h"

extern "C" void pushcount_report(const char *what);

namespace {

constexpr uint32_t kSlots = 31, kTagBytes = 4096, kBufferOffset = 256;

std::vector<uint32_t> tag(uint32_t slot)
{
    std::vector<uint32_t> t(kTagBytes / 4);
    t[0] = 0xC0DE0000u | slot;
    for (uint32_t k = 1; k < t.size(); k++)
        t[k] = 0xA0000000u | slot << 16 | k;
    return t;
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: probecompute <nvk dylib> <cache dir> <corpus.log>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        // the second dispatch's buffers lie elsewhere: each slot's tagged bytes 256 bytes into a buffer of its own
        NSMutableArray<id<MTLBuffer>> *buffers = [NSMutableArray array];
        for (uint32_t slot = 0; slot < kSlots; slot++) {
            id<MTLBuffer> b = [dev newBufferWithLength:kBufferOffset + kTagBytes options:MTLResourceStorageModeShared];
            memcpy(static_cast<uint8_t *>(b.contents) + kBufferOffset, tag(slot).data(), kTagBytes);
            [buffers addObject:b];
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[3]) encoding:NSUTF8StringEncoding error:nil];
        std::map<std::string, bool> seen;
        unsigned index = 0, made = 0, refused = 0, failed = 0;
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"D ok=1 "])
                continue;
            const std::string line = l.UTF8String;
            probe::Shape s;
            std::string why;
            const unsigned i = index++;
            if (!probe::parse(line, s, why)) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            raw::AirHash air;
            id<MTLFunction> f = probe::function(dev, s, air);
            if (!f) {
                printf("KREFUSED %u no function %s in %s\n", i, s.function.c_str(), s.library.c_str());
                refused++;
                continue;
            }
            id<MTLComputePipelineState> ps = probe::state(dev, s, f, why);
            if (!ps) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            const std::string key = probe::key(s, air);
            if (seen[key])
                continue;
            seen[key] = true;
            NSArray<id<MTLTexture>> *bound = probe::textures(dev, s.textures, why);
            if (!bound) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            // the second with the buffers elsewhere, so kernels.py keeps only what their addresses do not move; the third
            // off 16 bytes, so it names their residual words
            for (uint32_t v = 0; v < 3; v++) {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                for (uint32_t slot = 0; slot < kSlots; slot++) {
                    if (v)
                        [enc setBuffer:buffers[slot]
                                offset:kBufferOffset + (v == 2 ? raw::probeComputeCode(slot) : 0)
                               atIndex:slot];
                    else
                        [enc setBytes:tag(slot).data() length:kTagBytes atIndex:slot];
                }
                for (NSUInteger t = 0; t < bound.count; t++)
                    [enc setTexture:bound[t] atIndex:s.textures[t].slot];
                for (const raw::BoundSampler &b : s.samplers)
                    [enc setSamplerState:probe::sampler(dev, b) atIndex:b.slot];
                std::istringstream memory(s.threadgroupMemory);
                std::string pair;
                while (std::getline(memory, pair, ','))
                    [enc setThreadgroupMemoryLength:strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10)
                                            atIndex:strtoul(pair.c_str(), nullptr, 10)];
                const uint32_t grid[3] = {3 + 3 * i + v, 5, 2};
                [enc dispatchThreadgroups:MTLSizeMake(grid[0], grid[1], grid[2])
                    threadsPerThreadgroup:MTLSizeMake(s.threads[0], s.threads[1], s.threads[2])];
                [enc endEncoding];
                if (!v)
                    printf("KPROBE %u tg=%u,%u,%u grid=%u,%u,%u key=%s\n", i, s.threads[0], s.threads[1], s.threads[2],
                           grid[0], grid[1], grid[2], key.c_str());
                fflush(stdout);
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) {
                    failed++;
                    printf("KFAILED %u %s\n", i, cb.error.localizedDescription.UTF8String);
                } else if (v == 2) {
                    made++;
                }
            }
        }
        fprintf(stderr, "probecompute: %u shapes dispatched, %u refused, %u failed\n", made, refused, failed);
        pushcount_report("probecompute");
    }
    return 0;
}
