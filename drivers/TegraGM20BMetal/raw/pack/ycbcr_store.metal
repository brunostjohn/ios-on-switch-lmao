// SPDX-License-Identifier: MIT
// raw GM20B path: Metal's store of a pass into a two-plane YCbCr attachment (runtime/raw_ycbcr_store.h), drawn from the pass's
// RGBA32Float stand-in into the planes: the luma plane one texel a fragment, the chroma plane one block a fragment.
//  - codes (RGB*_2P): each texel held as a half (a drawn float rounded to nearest even, or toward zero for 10-bit codes), its
//    code round(clamp(h, 0, 1) * max) with ties up, a NaN 0; a chroma block's code (sum + n / 2) / n of its n texels' codes;
//  - converted (YCBCR8_*_2P, m2v bind/ycbcr_write.rs): a texel's grid step x = rint(clamp(h, 0, 1) * 2048) of its nearest half,
//    a code's term t = floor(row . x * mul / 2^20), the code offset + ((sum t + 2^(5 + k)) >> (6 + k)) over a block of 2^k
//    texels, clamped to [0, 255].
// The codes are written as code << shift over the target's whole range, so the ROP's unorm store of the exact quotient is the
// code. One function a target format: the pack keys an own state by its function's name. Built without fast math
// (rawpack.py): the NaN tests and the half rounding are kept.
#include <metal_stdlib>
using namespace metal;

struct StoreShape {
    uint max;         // the largest code: 255 or 1023
    uint shift;       // a code's place in its unorm word: 0, or 6 for a 10-bit code in 16 bits
    uint towardZero;  // drawn floats held toward zero
    uint across;      // chroma blocks two texels across
    uint down;        // and two down
    uint converted;   // the matrix rule, else codes
    int offset[3];    // converted: luma's, Cb's and Cr's
    int row[3][3];    // and each code's red, green and blue coefficients
    uint mul[3];      // and its terms' scale over 2^20
};

struct Out {
    float4 position [[position]];
};

vertex Out ycbcr_store_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

typedef texture2d<float, access::read> StandIn;

static float held(float v, bool towardZero)
{
    half h = half(v);
    if (towardZero && float(h) > v)
        h = as_type<half>(ushort(as_type<ushort>(h) - 1));
    return float(h);
}

static uint texelCode(float v, constant StoreShape &s)
{
    if (isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return s.max;
    return uint(floor(held(v, s.towardZero != 0) * float(s.max) + 0.5f));
}

static int gridStep(float v)
{
    if (isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 2048;
    return int(rint(held(v, false) * 2048.0f));
}

// floor(S * mul / 2^20) in 32-bit steps: S = 4096 q + r, q mul = 256 s + u
static int scaled(int sum, uint mul)
{
    const int q = sum >> 12, r = sum & 4095, qm = q * int(mul);
    return (qm >> 8) + (((qm & 255) * 4096 + r * int(mul)) >> 20);
}

static int term(constant StoreShape &s, uint code, float3 rgb)
{
    const int x[3] = {gridStep(rgb.r), gridStep(rgb.g), gridStep(rgb.b)};
    int sum = 0;
    for (uint c = 0; c < 3; c++)
        sum += s.row[code][c] * x[c];
    return scaled(sum, s.mul[code]);
}

static uint encoded(constant StoreShape &s, uint code, int sum, uint k)
{
    const uint shift = 6 + k;
    return uint(clamp(s.offset[code] + ((sum + (1 << (shift - 1))) >> shift), 0, 255));
}

static uint lumaCode(StandIn stand_in, constant StoreShape &s, uint2 at)
{
    const float4 v = stand_in.read(at);
    return s.converted ? encoded(s, 0, term(s, 0, v.rgb), 0) : texelCode(v.g, s);
}

// The block's Cb and Cr codes
static uint2 chromaCodes(StandIn stand_in, constant StoreShape &s, uint2 block)
{
    const uint w = s.across ? 2 : 1, h = s.down ? 2 : 1, n = w * h, k = (s.across ? 1 : 0) + (s.down ? 1 : 0);
    const uint2 first = block * uint2(w, h);
    int cb = 0, cr = 0;
    for (uint dy = 0; dy < h; dy++)
        for (uint dx = 0; dx < w; dx++) {
            const float4 v = stand_in.read(first + uint2(dx, dy));
            if (s.converted) {
                cb += term(s, 1, v.rgb);
                cr += term(s, 2, v.rgb);
            } else {
                cb += int(texelCode(v.b, s));
                cr += int(texelCode(v.r, s));
            }
        }
    if (s.converted)
        return uint2(encoded(s, 1, cb, k), encoded(s, 2, cr, k));
    return (uint2(cb, cr) + n / 2) / n;
}

fragment float ycbcr_store_luma8_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                      constant StoreShape &s [[buffer(0)]])
{
    return float(lumaCode(stand_in, s, uint2(in.position.xy))) / 255.0f;
}

fragment float ycbcr_store_luma16_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                       constant StoreShape &s [[buffer(0)]])
{
    return float(lumaCode(stand_in, s, uint2(in.position.xy)) << s.shift) / 65535.0f;
}

fragment float4 ycbcr_store_chroma8_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                         constant StoreShape &s [[buffer(0)]])
{
    return float4(float2(chromaCodes(stand_in, s, uint2(in.position.xy))) / 255.0f, 0.0f, 1.0f);
}

fragment float4 ycbcr_store_chroma16_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                          constant StoreShape &s [[buffer(0)]])
{
    return float4(float2(chromaCodes(stand_in, s, uint2(in.position.xy)) << s.shift) / 65535.0f, 0.0f, 1.0f);
}
