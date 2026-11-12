// SPDX-License-Identifier: MIT
// What a Core Animation shaped command buffer costs through the raw facade (RawMetal.mm), recording, encode, retire and
// completion together: HW Test gpu.path.encode_cost's command buffers (6 passes of QuartzCore's untextured fill, each
// draw's vertices, transform and zero uniforms by setBytes with a scissor of its own, at 8 and 16 draws a pass) over the
// m2v device the render server's facade wraps, on the mock GPU. Per command buffer: the facade's encode profile
// (switchRawEncodeProfile, as the HW Test reads it, over a held batch after a held batch at the same draws), then on
// every thread the Objective-C messages, locks and allocations the facade's image made (facadecount.c, preloaded) and
// the CPU time of the recording thread and of the others, with each command buffer released once it completes
// (facade) and with the batch held until all complete (held). A line FAIL where the profile's or the held window's
// count grows with the draws (the released window's vary with how far completion trails recording: a command buffer
// whose predecessor's completion has not run hands its own to it).
// usage: DYLD_INSERT_LIBRARIES=<libfacadecount.dylib> facadecost <libvulkan_nouveau.dylib> <cache dir>
//        <QuartzCore default.metallib> <pack.bin> [buffers]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "../runtime/RawMetal.h"

struct facadecount {
    uint64_t sends, locks, mallocs, frees;
};

using SitesFn = size_t (*)(unsigned kind, uintptr_t *out, size_t pairs);

@interface NSObject (SwitchRawEncodeProfile)
- (NSDictionary<NSString *, NSNumber *> *)switchRawEncodeProfile;  // RawMetalEncodeCounters.mm (debug builds)
@end

// Signals when the object it is associated with is destroyed, after that object's own ivars are released.
@interface FacadeCostGone : NSObject
@property (nonatomic, strong) dispatch_semaphore_t gone;
@end

@implementation FacadeCostGone

- (void)dealloc
{
    dispatch_semaphore_signal(_gone);
}

@end

namespace {

// PBGRAXm_A2Xhf's functions and their constants, as drivers/TegraGM20BMetal/precompile/corpus.log records them
const char *kFillVertex = "fixed_vert_lph_spc fc_generic:53:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 "
                          "fc_vertex_layout:49:00";
const char *kFillFragment =
    "fixed_frag_lph_cph fc_attachment_count:49:02 fc_blend_function:49:3a fc_clamp_color:53:00 fc_color_inputs0:49:00 "
    "fc_color_inputs1:49:00 fc_color_opaque0:53:00 fc_color_opaque1:53:00 fc_color_opcodes0:52:00000000 "
    "fc_color_opcodes1:52:00000000 fc_coord_function0:49:00 fc_coord_function1:49:00 fc_debug_xr:53:00 "
    "fc_dest_count:49:00 fc_dest_index:49:00 fc_destination_function:49:00 fc_extended_range:53:00 fc_filter0:49:00 "
    "fc_filter1:49:00 fc_framebuffer_fetch:53:01 fc_generic:53:00 fc_has_asg_77_weights:53:00 "
    "fc_has_attachment_0:53:00 fc_has_attachment_1:53:00 fc_has_attachment_2:53:00 fc_has_color_cube0:53:00 "
    "fc_has_color_cube1:53:00 fc_has_color_params:53:00 fc_has_color_trc0:53:00 fc_has_color_trc1:53:00 "
    "fc_has_edr_factor:53:00 fc_has_gamma_luts:53:00 fc_has_hdr_scale:53:00 fc_has_image_0A:53:00 "
    "fc_has_image_0B:53:00 fc_has_image_0C:53:00 fc_has_image_1A:53:00 fc_has_image_1B:53:00 fc_has_image_1C:53:00 "
    "fc_has_lod_bias:53:00 fc_has_lut_tex:53:00 fc_has_noise_scale:53:00 fc_has_noise_tex:53:00 "
    "fc_has_premultiply:53:00 fc_has_ycbcr_matrix:53:00 fc_image_count:49:00 fc_image_function0:49:00 "
    "fc_image_function1:49:00 fc_memoryless_index0:49:00 fc_memoryless_index1:49:00 fc_noise_width:49:00 "
    "fc_plane_count0:49:00 fc_plane_count1:49:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 "
    "fc_texture_function:49:00 fc_unbounded_range:53:00 fc_vertex_layout:49:00";

struct Vertex {
    float position[4], texcoord0[2], texcoord1[2];
    __fp16 color[4], pad[4];
};

static_assert(sizeof(Vertex) == 48);

std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> out;
    for (size_t at = 0, end; at < s.size(); at = end + 1) {
        end = s.find(' ', at);
        if (end == std::string::npos)
            end = s.size();
        if (end > at)
            out.push_back(s.substr(at, end - at));
    }
    return out;
}

id<MTLFunction> function(id<MTLLibrary> library, const std::string &part)
{
    const std::vector<std::string> w = words(part);
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    for (size_t i = 1; i < w.size(); i++) {
        const size_t a = w[i].rfind(':'), b = w[i].rfind(':', a - 1);
        const std::string hex = w[i].substr(a + 1);
        uint8_t bytes[16] = {};
        for (size_t k = 0; k + 1 < hex.size() && k / 2 < sizeof bytes; k += 2)
            bytes[k / 2] = uint8_t(strtoul(hex.substr(k, 2).c_str(), nullptr, 16));
        [values setConstantValue:bytes
                            type:MTLDataType(atoi(w[i].substr(b + 1, a - b - 1).c_str()))
                        withName:@(w[i].substr(0, b).c_str())];
    }
    NSError *error = nil;
    id<MTLFunction> made = [library newFunctionWithName:@(w[0].c_str()) constantValues:values error:&error];
    if (!made)
        fprintf(stderr, "%s: %s\n", w[0].c_str(), error.localizedDescription.UTF8String);
    return made;
}

id<MTLRenderPipelineState> fillState(id<MTLDevice> device, id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.label = @"com.apple.coreanimation.PBGRAXm_A2Xhf";
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const MTLVertexFormat formats[4] = {MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
                                        MTLVertexFormatHalf4};
    const unsigned offsets[4] = {0, 16, 24, 32};
    for (unsigned a = 0; a < 4; a++) {
        vd.attributes[a].format = formats[a];
        vd.attributes[a].offset = offsets[a];
        vd.attributes[a].bufferIndex = 1;
    }
    vd.layouts[1].stride = sizeof(Vertex);
    d.vertexDescriptor = vd;
    d.vertexFunction = function(library, kFillVertex);
    d.fragmentFunction = function(library, kFillFragment);
    NSError *error = nil;
    id<MTLRenderPipelineState> state =
        d.vertexFunction && d.fragmentFunction ? [device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
    if (!state)
        fprintf(stderr, "PBGRAXm_A2Xhf: %s\n", error.localizedDescription.UTF8String);
    return state;
}

uint64_t threadNs(thread_act_t thread)
{
    thread_basic_info_data_t t{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&t, &n);
    return (uint64_t(t.user_time.seconds) * 1000000 + t.user_time.microseconds +
            uint64_t(t.system_time.seconds) * 1000000 + t.system_time.microseconds) *
           1000;
}

// CPU time of this thread and of every other thread of the task
void cpuNs(uint64_t &mine, uint64_t &others)
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    task_threads(mach_task_self(), &threads, &count);
    const mach_port_t self = mach_thread_self();
    mine = others = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        (threads[i] == self ? mine : others) += threadNs(threads[i]);
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
}

// FACADECOST_SITES=<n>: the n facade call sites that counted most of each kind in the last arm, by symbol and offset
void printSites(SitesFn sites, unsigned buffers)
{
    const char *n = getenv("FACADECOST_SITES");
    if (!sites || !n)
        return;
    static const char *kinds[] = {"", "message", "lock", "allocation"};
    std::vector<uintptr_t> pairs(2 * 4096);
    for (unsigned kind = 1; kind <= 3; kind++) {
        const size_t found = sites(kind, pairs.data(), 4096);
        for (size_t i = 0; i < found && i < size_t(atoi(n)); i++) {
            Dl_info info{};
            dladdr(reinterpret_cast<void *>(pairs[2 * i]), &info);
            printf("site %s %.2f/cb %s+0x%lx (0x%lx)\n", kinds[kind], double(pairs[2 * i + 1]) / buffers,
                   info.dli_sname ? info.dli_sname : "?", (unsigned long)(pairs[2 * i] - uintptr_t(info.dli_saddr)),
                   (unsigned long)(pairs[2 * i] - uintptr_t(info.dli_fbase)));
        }
    }
}

NSDictionary<NSString *, NSNumber *> *profile(id<MTLDevice> device)
{
    return [(id)device respondsToSelector:@selector(switchRawEncodeProfile)] ? [(id)device switchRawEncodeProfile]
                                                                             : @{};
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: facadecost <libvulkan_nouveau.dylib> <cache dir> <QuartzCore default.metallib> <pack.bin> "
                "[buffers]\n");
        return 2;
    }
    const auto image = reinterpret_cast<void (*)(const void *)>(dlsym(RTLD_DEFAULT, "facadecount_image"));
    const auto start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "facadecount_start"));
    const auto stop = reinterpret_cast<void (*)(facadecount *)>(dlsym(RTLD_DEFAULT, "facadecount_stop"));
    const auto sites = reinterpret_cast<SitesFn>(dlsym(RTLD_DEFAULT, "facadecount_sites"));
    if (!image || !start || !stop) {
        fprintf(stderr, "facadecount.dylib is not preloaded (DYLD_INSERT_LIBRARIES)\n");
        return 2;
    }
    image(reinterpret_cast<const void *>(&raw_metal_device_open));
    const unsigned buffers = argc > 5 ? unsigned(atoi(argv[5])) : 24;
    int failures = 0;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        NSString *why = nil;
        id<MTLDevice> device = m2v ? raw_metal_device_open(argv[4], 0, m2v, &why) : nil;
        if (!device) {
            fprintf(stderr, "no device: %s\n", m2v ? why.UTF8String : "m2v");
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        id<MTLRenderPipelineState> fill = library ? fillState(device, library) : nil;
        if (!fill)
            return 1;
        constexpr unsigned kSize = 256, kPasses = 6;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:kSize
                                                                                     height:kSize
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeMemoryless;
        id<MTLTexture> scratch = [device newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *passes[kPasses];
        for (unsigned p = 0; p < kPasses; p++) {
            passes[p] = [MTLRenderPassDescriptor new];
            passes[p].colorAttachments[0].texture = target;
            passes[p].colorAttachments[0].loadAction = p ? MTLLoadActionLoad : MTLLoadActionClear;
            passes[p].colorAttachments[0].storeAction = MTLStoreActionStore;
            passes[p].colorAttachments[1].texture = scratch;
            passes[p].colorAttachments[1].loadAction = MTLLoadActionClear;
            passes[p].colorAttachments[1].storeAction = MTLStoreActionDontCare;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        static const uint8_t zeros[256] = {};
        auto encode = [&](unsigned draws) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            for (unsigned p = 0; p < kPasses; p++) {
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:passes[p]];
                [e setRenderPipelineState:fill];
                for (unsigned d = 0; d < draws; d++) {
                    const float x = float(d % 8) / 4 - 1, y = float(p) / 3 - 1;
                    const __fp16 c[4] = {__fp16(float(d % 4) / 4), __fp16(float(p) / 8), 0.5, 1};
                    const Vertex v[4] = {{{x, y, 0, 1}, {0, 1}, {0, 1}, {c[0], c[1], c[2], c[3]}, {}},
                                         {{x + 0.25f, y, 0, 1}, {1, 1}, {1, 1}, {c[0], c[1], c[2], c[3]}, {}},
                                         {{x, y + 0.3f, 0, 1}, {0, 0}, {0, 0}, {c[0], c[1], c[2], c[3]}, {}},
                                         {{x + 0.25f, y + 0.3f, 0, 1}, {1, 0}, {1, 0}, {c[0], c[1], c[2], c[3]}, {}}};
                    [e setVertexBytes:v length:sizeof v atIndex:1];
                    [e setVertexBytes:identity length:sizeof identity atIndex:2];
                    [e setVertexBytes:zeros length:sizeof zeros atIndex:0];
                    [e setVertexBytes:zeros length:sizeof zeros atIndex:3];
                    for (unsigned s = 0; s < 4; s++)
                        [e setFragmentBytes:zeros length:sizeof zeros atIndex:s];
                    [e setScissorRect:(MTLScissorRect){(d % 8) * 32, p * 32, 32, 64}];
                    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                }
                [e endEncoding];
            }
            [cb commit];
            return cb;
        };
        // A batch ends when its last command buffer is destroyed, not when it completes: the completion thread
        // releases it (and frees what it owned) after waitUntilCompleted returns, which would land a varying part of
        // one command buffer's frees in the next window.
        // A held batch keeps every command buffer until all complete, as an app that commits a batch before it waits
        // (and HW Test gpu.path.encode_cost) does: each records while the others are still alive.
        static char goneKey;
        auto batch = [&](unsigned draws, unsigned count, bool hold) {
            id<MTLCommandBuffer> last = nil;
            NSMutableArray<id<MTLCommandBuffer>> *held = [NSMutableArray arrayWithCapacity:count];
            for (unsigned i = 0; i < count; i++)
                @autoreleasepool {
                    last = encode(draws);
                    if (hold)
                        [held addObject:last];
                }
            bool completed = true;
            for (id<MTLCommandBuffer> b in held) {
                [b waitUntilCompleted];
                completed &= b.status == MTLCommandBufferStatusCompleted;
            }
            [held removeAllObjects];
            [last waitUntilCompleted];
            completed &= last.status == MTLCommandBufferStatusCompleted;
            dispatch_semaphore_t gone = dispatch_semaphore_create(0);
            @autoreleasepool {
                FacadeCostGone *sentinel = [FacadeCostGone new];
                sentinel.gone = gone;
                objc_setAssociatedObject(last, &goneKey, sentinel, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                last = nil;
            }
            if (dispatch_semaphore_wait(gone, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
                fprintf(stderr, "the last command buffer outlived its completion by 5 s\n");
                return false;
            }
            return completed;
        };
        const unsigned arms[2] = {8, 16};
        std::map<std::string, double> counted[2];
        for (unsigned a = 0; a < 2; a++) {
            if (!batch(arms[a], 8, false)) {
                fprintf(stderr, "a warm-up command buffer failed\n");
                return 1;
            }
            failures += !batch(arms[a], buffers, true);
            NSDictionary<NSString *, NSNumber *> *before = profile(device);
            failures += !batch(arms[a], buffers, true);
            NSDictionary<NSString *, NSNumber *> *after = profile(device);
            const double encodes = after[@"encodes"].doubleValue - before[@"encodes"].doubleValue;
            const double mallocEncodes = after[@"malloc_encodes"].doubleValue - before[@"malloc_encodes"].doubleValue;
            auto per = [&](NSString *key, double over) {
                return over > 0 ? (after[key].doubleValue - before[key].doubleValue) / over : 0;
            };
            printf("facadecost d%u profile: encodes %.0f/%u wall_us %.1f retire_us %.1f commit_us %.1f locks %.2f "
                   "sends %.2f mallocs %.2f frees %.2f uses %.1f use_adds %.1f job_uses %.1f descriptor_uploads %.1f\n",
                   arms[a], encodes, buffers, per(@"wall_us", encodes), per(@"retire_us", encodes),
                   per(@"commit_us", encodes), per(@"locks", encodes), per(@"sends", encodes),
                   per(@"mallocs", mallocEncodes), per(@"frees", mallocEncodes), per(@"uses", encodes),
                   per(@"use_adds", encodes), per(@"exec_uses", encodes), per(@"descriptor_uploads", encodes));
            counted[a]["encodes"] = encodes / buffers;
            for (NSString *key : {@"locks", @"sends", @"use_adds", @"geometry_vertices"})
                counted[a][std::string("profile ") + key.UTF8String] = per(key, encodes);
            counted[a]["profile mallocs"] = per(@"mallocs", mallocEncodes);
            counted[a]["profile frees"] = per(@"frees", mallocEncodes);

            // a held window twice, judged by the fewer of each count: a queue or pool that grows to a new depth once
            // counts in only one of them
            for (const bool hold : {false, true, true}) {
                facadecount c{};
                uint64_t mine0, others0, mine1, others1;
                cpuNs(mine0, others0);
                start();
                failures += !batch(arms[a], buffers, hold);
                stop(&c);
                cpuNs(mine1, others1);
                const char *shape = hold ? "held" : "facade";
                printf(
                    "facadecost d%u %s: sends %.2f locks %.2f mallocs %.2f frees %.2f | cpu us recording thread %.1f "
                    "others %.1f\n",
                    arms[a], shape, double(c.sends) / buffers, double(c.locks) / buffers, double(c.mallocs) / buffers,
                    double(c.frees) / buffers, double(mine1 - mine0) / 1e3 / buffers,
                    double(others1 - others0) / 1e3 / buffers);
                if (!hold)
                    continue;
                const std::pair<const char *, uint64_t> counts[] = {{"held sends", c.sends},
                                                                    {"held locks", c.locks},
                                                                    {"held mallocs", c.mallocs},
                                                                    {"held frees", c.frees}};
                for (const auto &[name, count] : counts) {
                    const double per = double(count) / buffers;
                    counted[a][name] = counted[a].count(name) ? std::min(counted[a][name], per) : per;
                }
            }
        }
        printSites(sites, buffers);
        for (const auto &[name, value] : counted[0]) {
            const double grown = counted[1][name] - value;
            if (name == "encodes" ? value < 1 : grown >= 0.5) {
                printf("FAIL: %s %.2f at %u draws a pass, %.2f at %u\n", name.c_str(), value, arms[0], counted[1][name],
                       arms[1]);
                failures++;
            }
        }
    }
    printf("facadecost: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
