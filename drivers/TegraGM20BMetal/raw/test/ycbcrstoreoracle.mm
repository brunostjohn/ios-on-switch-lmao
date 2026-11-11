// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_store.h against this Mac's Metal: render passes (clears, float draws, half draws) into two-plane YCbCr
// attachments over CoreVideo surfaces, every byte of both planes against the header's rule: the code formats (540, 541 over
// '420v', '420f', '422v', '422f'; 543, 544 over 'x420', 'xf20', 'x422', 'xf22') and, as a control against m2v's rule, the
// converted 500 and 502. The draws' floats lie beside the half midpoints and the ties of the 1/255, 1/1023 and 1/2048 grids, out
// of range and not numbers.
// Then the raw path's route for the 8-bit code formats on this Mac's GPU: the same floats drawn into the RGBA32Float stand-in,
// TwoPlaneStandIn.metal's two_plane_store into an R8Unorm luma and an RG8Unorm image of each texel's chroma codes, and that image
// halved as the 2D engine halves it (fx::referenceDownsample): its bytes must be Metal's too.
// usage: ycbcrstoreoracle <drivers/NativeMetal/TwoPlaneStandIn.metal>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <random>
#include <vector>

#include "../fx/fx_mip.h"
#include "../runtime/raw_ycbcr_store.h"

namespace {
constexpr uint32_t W = 128, H = 64, N = W * H;

NSString *const kValues =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"struct V { float4 p [[position]]; };\n"
    @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, float(i >> 1) * 2 - 1, 0, 1);"
    @" return v; }\n"
    @"fragment float4 floats(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
    @"  return k[uint(in.p.y) * 128 + uint(in.p.x)]; }\n"
    @"fragment half4 halves(V in [[stage_in]], device const float4 *k [[buffer(0)]]) {\n"
    @"  return half4(k[uint(in.p.y) * 128 + uint(in.p.x)]); }\n";

float bitsOf(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

std::vector<float> valueSet(uint32_t seed)
{
    std::mt19937 rng(seed);
    auto beside = [&](double x, int spread) {
        float f = float(x);
        uint32_t u;
        memcpy(&u, &f, 4);
        return bitsOf(u + uint32_t(int(rng() % uint32_t(2 * spread + 1)) - spread));
    };
    std::vector<float> v(4 * N, 1.0f);
    for (uint32_t i = 0; i < N; i++)
        for (int c = 0; c < 3; c++) {
            const uint32_t k = rng();
            switch (k % 7) {
            case 0: {
                _Float16 a, b;
                const uint16_t h = uint16_t(k >> 8) % 0x3c00, h1 = h + 1;
                memcpy(&a, &h, 2);
                memcpy(&b, &h1, 2);
                v[4 * i + c] = beside((double(a) + double(b)) / 2, 1);
                break;
            }
            case 1:
                v[4 * i + c] = beside(((k >> 8) % 255 + 0.5) / 255, 3);
                break;
            case 2:
                v[4 * i + c] = beside(((k >> 8) % 1023 + 0.5) / 1023, 3);
                break;
            case 3:
                v[4 * i + c] = beside(((k >> 8) % 2048 + 0.5) / 2048, 2);
                break;
            case 4: {
                const float special[] = {bitsOf(0x7fc00000u), INFINITY, -INFINITY, -0.0f, -0.3f, 1.7f, 0.0f, 1.0f};
                v[4 * i + c] = special[(k >> 8) % 8];
                break;
            }
            default:
                v[4 * i + c] = std::uniform_real_distribution<float>(-0.05f, 1.05f)(rng);
            }
        }
    return v;
}

struct Config {
    uint32_t format;
    OSType fourcc;
};

struct Surface {
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = nullptr;
};

Surface makeSurface(OSType fourcc)
{
    Surface out;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    if (!CVPixelBufferCreate(nullptr, W, H, fourcc, (__bridge CFDictionaryRef)attrs, &out.pb))
        out.s = CVPixelBufferGetIOSurface(out.pb);
    return out;
}

// Plane 0's codes then plane 1's, `bytes` a code
std::vector<uint16_t> planes(IOSurfaceRef s, uint32_t bytes)
{
    std::vector<uint16_t> out;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t p = 0; p < 2; p++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        for (size_t y = 0; y < IOSurfaceGetHeightOfPlane(s, p); y++)
            for (size_t x = 0; x < W; x++) {
                const uint8_t *at = b + y * IOSurfaceGetBytesPerRowOfPlane(s, p) + x * bytes;
                out.push_back(bytes == 2 ? uint16_t(at[0] | at[1] << 8) : at[0]);
            }
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

void scribble(IOSurfaceRef s, uint32_t seed)
{
    IOSurfaceLock(s, 0, nullptr);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        for (size_t i = 0; i < IOSurfaceGetBytesPerRowOfPlane(s, p) * IOSurfaceGetHeightOfPlane(s, p); i++)
            b[i] = uint8_t(i * 31 + seed);
    }
    IOSurfaceUnlock(s, 0, nullptr);
}

std::vector<uint16_t> reference(const Config &c, const std::vector<float> &v, raw::YcbcrTexelSource source)
{
    raw::YcbcrCodeTarget t;
    const bool codes = raw::ycbcrCodeTarget(c.format, t);
    const bool down = c.format == 500 || c.format == 540 || c.format == 543;
    std::vector<uint16_t> luma(N), chroma(W * (down ? H / 2 : H));
    if (codes)
        raw::ycbcrStore(t, v.data(), W, H, source, luma.data(), chroma.data());
    else
        raw::ycbcrStoreConverted(c.fourcc == '420f' || c.fourcc == '422f', raw::YcbcrMatrix::Bt601, down, v.data(), W,
                                 H, luma.data(), chroma.data());
    const int shift = t.max == 1023 ? 6 : 0;
    std::vector<uint16_t> out;
    for (uint16_t x : luma)
        out.push_back(uint16_t(x << shift));
    for (uint16_t x : chroma)
        out.push_back(uint16_t(x << shift));
    return out;
}

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLLibrary> lib;

    id<MTLRenderPipelineState> state(NSString *fragment, std::initializer_list<MTLPixelFormat> formats,
                                     id<MTLLibrary> from = nil, NSString *vertex = @"strip")
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [(from ?: lib) newFunctionWithName:vertex];
        d.fragmentFunction = [(from ?: lib) newFunctionWithName:fragment];
        NSUInteger i = 0;
        for (MTLPixelFormat f : formats)
            d.colorAttachments[i++].pixelFormat = f;
        NSError *e = nil;
        id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        if (!s)
            fprintf(stderr, "state %s: %s\n", fragment.UTF8String, e.localizedDescription.UTF8String);
        return s;
    }

    // One pass into `targets`: a clear of `clear` (no state) or a draw of `values` with `s`
    void pass(NSArray<id<MTLTexture>> *targets, id<MTLRenderPipelineState> s, const float *values, MTLClearColor clear,
              id<MTLTexture> sampled = nil)
    {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (NSUInteger i = 0; i < targets.count; i++) {
            rp.colorAttachments[i].texture = targets[i];
            rp.colorAttachments[i].loadAction = s ? MTLLoadActionDontCare : MTLLoadActionClear;
            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
            rp.colorAttachments[i].clearColor = clear;
        }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (s) {
            [e setRenderPipelineState:s];
            if (values)
                [e setFragmentBuffer:[dev newBufferWithBytes:values length:N * 16 options:MTLResourceStorageModeShared]
                              offset:0
                             atIndex:0];
            if (sampled)
                [e setFragmentTexture:sampled atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
};

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcrstoreoracle <TwoPlaneStandIn.metal>\n");
            return 2;
        }
        Gpu g;
        g.dev = MTLCreateSystemDefaultDevice();
        g.q = [g.dev newCommandQueue];
        NSError *e = nil;
        g.lib = [g.dev newLibraryWithSource:kValues options:nil error:&e];
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        id<MTLLibrary> standIn = src ? [g.dev newLibraryWithSource:src options:nil error:&e] : nil;
        if (!g.lib || !standIn) {
            fprintf(stderr, "libraries: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        const Config configs[] = {{540, '420v'}, {540, '420f'}, {541, '422v'}, {541, '422f'},
                                  {543, 'x420'}, {543, 'xf20'}, {544, 'x422'}, {544, 'xf22'},
                                  {500, '420v'}, {500, '420f'}, {502, '422v'}};
        std::vector<std::vector<float>> sets;
        for (uint32_t k = 0; k < 6; k++)
            sets.push_back(valueSet(540 + k));
        uint32_t passes = 0, bad = 0, routeBad = 0, routePasses = 0;
        for (const Config &c : configs) {
            Surface s = makeSurface(c.fourcc);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.format)
                                                                                         width:W
                                                                                        height:H
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> t = s.s ? [g.dev newTextureWithDescriptor:d iosurface:s.s plane:0] : nil;
            id<MTLRenderPipelineState> floats = g.state(@"floats", {MTLPixelFormat(c.format)}),
                                       halves = g.state(@"halves", {MTLPixelFormat(c.format)});
            if (!t || !floats || !halves) {
                printf("format %u over %c%c%c%c: not made\n", c.format, char(c.fourcc >> 24), char(c.fourcc >> 16),
                       char(c.fourcc >> 8), char(c.fourcc));
                bad++;
                continue;
            }
            const uint32_t bytes = c.format == 543 || c.format == 544 ? 2 : 1;
            uint32_t off = 0;
            auto check = [&](const std::vector<float> &v, raw::YcbcrTexelSource source) {
                const std::vector<uint16_t> want = reference(c, v, source), got = planes(s.s, bytes);
                passes++;
                for (size_t i = 0; i < want.size() && i < got.size(); i++)
                    if (want[i] != got[i]) {
                        if (off++ < 4)
                            printf("  %u over %c%c%c%c pass %u: code %zu Metal %u rule %u\n", c.format,
                                   char(c.fourcc >> 24), char(c.fourcc >> 16), char(c.fourcc >> 8), char(c.fourcc),
                                   passes, i, got[i], want[i]);
                        bad++;
                        break;
                    }
            };
            for (uint32_t k = 0; k < sets.size(); k++) {
                scribble(s.s, k);
                g.pass(@[ t ], floats, sets[k].data(), {});
                check(sets[k], raw::YcbcrTexelSource::DrawnFloat);
                std::vector<float> h(sets[k].size());
                for (size_t i = 0; i < h.size(); i++)
                    h[i] = float(_Float16(sets[k][i]));
                scribble(s.s, k + 7);
                g.pass(@[ t ], halves, sets[k].data(), {});
                check(h, raw::YcbcrTexelSource::DrawnHalf);
            }
            for (int i = 0; i <= 120; i++) {
                const double x = i / 110.0 - 0.04;
                const MTLClearColor k = MTLClearColorMake(x, 1.07 - x, std::fmod(x * 5.3, 1.0), 1);
                std::vector<float> v(4 * N);
                for (uint32_t j = 0; j < N; j++)
                    for (int ch = 0; ch < 4; ch++)
                        v[4 * j + ch] = float((&k.red)[ch]);
                scribble(s.s, uint32_t(i));
                g.pass(@[ t ], nil, nullptr, k);
                check(v, raw::YcbcrTexelSource::Clear);
            }
            // the raw route of the 8-bit code formats
            if (c.format == 540 || c.format == 541) {
                const bool down = c.format == 540;
                Surface r = makeSurface(c.fourcc);
                MTLTextureDescriptor *td =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(raw::kYcbcrTargetStandIn)
                                                                       width:W
                                                                      height:H
                                                                   mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                id<MTLTexture> stand = [g.dev newTextureWithDescriptor:td];
                td.pixelFormat = MTLPixelFormatRG8Unorm;
                td.storageMode = MTLStorageModeShared;
                id<MTLTexture> codes = [g.dev newTextureWithDescriptor:td];
                td.pixelFormat = MTLPixelFormatR8Unorm;
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModeShared;
                id<MTLTexture> luma = [g.dev newTextureWithDescriptor:td iosurface:r.s plane:0];
                id<MTLRenderPipelineState> into = g.state(@"floats", {MTLPixelFormat(raw::kYcbcrTargetStandIn)}),
                                           store = g.state(@"two_plane_store",
                                                           {MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm}, standIn,
                                                           @"two_plane_cover");
                for (uint32_t k = 0; k < sets.size(); k++) {
                    scribble(s.s, k);
                    g.pass(@[ t ], floats, sets[k].data(), {});
                    const std::vector<uint16_t> metal = planes(s.s, 1);
                    g.pass(@[ stand ], into, sets[k].data(), {});
                    g.pass(@[ luma, codes ], store, nullptr, {}, stand);
                    std::vector<uint8_t> perTexel(W * H * 2), halved(W * (down ? H / 2 : H));
                    [codes getBytes:perTexel.data()
                        bytesPerRow:W * 2
                         fromRegion:MTLRegionMake2D(0, 0, W, H)
                        mipmapLevel:0];
                    raw::fx::referenceDownsample(perTexel.data(), W * 2, W, H, halved.data(), W, W / 2,
                                                 down ? H / 2 : H, 2);
                    const std::vector<uint16_t> lumaBytes = planes(r.s, 1);
                    routePasses++;
                    bool same = true;
                    for (uint32_t i = 0; i < N; i++)
                        same &= lumaBytes[i] == metal[i];
                    for (size_t i = 0; i < halved.size(); i++)
                        same &= halved[i] == metal[N + i];
                    routeBad += !same;
                }
                CVPixelBufferRelease(r.pb);
            }
            CVPixelBufferRelease(s.pb);
        }
        printf(
            "%s: %u passes into two-plane YCbCr attachments, %u with a code off the rule; the raw route of the 8-bit "
            "code formats: %u passes, %u with a byte off Metal's\n",
            g.dev.name.UTF8String, passes, bad, routePasses, routeBad);
        return bad || routeBad ? 1 : 0;
    }
}
