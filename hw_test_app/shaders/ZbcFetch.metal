#include <metal_stdlib>
using namespace metal;

// tests/CompressedZbcArms.swift: the cleared target's texel, read by framebuffer fetch, copied into colour 1
struct ZbcCopy {
    float4 kept [[color(0)]];
    float4 copy [[color(1)]];
};

fragment ZbcCopy zbc_fetch_copy_fragment(float4 fetched [[color(0)]])
{
    return {fetched, fetched};
}
