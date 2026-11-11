#include <metal_stdlib>
using namespace metal;

/// gpu.x1.rotated_target: shapes of `perColor` vertices each, one colour a shape; points three pixels wide
struct RotatedVertex {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float size [[point_size]];
};

vertex RotatedVertex rotated_vertex(uint vid [[vertex_id]], device const float4 *corners [[buffer(0)]],
                                    device const float4 *colors [[buffer(1)]], constant uint &perColor [[buffer(2)]])
{
    return RotatedVertex{corners[vid], colors[vid / perColor], corners[vid].xy, 3.0f};
}

fragment float4 rotated_fragment(RotatedVertex in [[stage_in]])
{
    return in.color;
}

/// Reads its position: a turned plane draws its turned variant
fragment float4 rotated_position_fragment(RotatedVertex in [[stage_in]])
{
    return float4(float(uint(in.position.x) & 15) / 15.0f, float(uint(in.position.y) & 15) / 15.0f, 0.0f, 1.0f);
}

/// Takes one axis's derivative: a turned plane draws its turned variant
fragment float4 rotated_derivative_fragment(RotatedVertex in [[stage_in]])
{
    return float4(dfdx(in.uv.x) > 0.0f ? 1.0f : 0.0f, dfdy(in.uv.y) > 0.0f ? 1.0f : 0.0f, 0.0f, 1.0f);
}

/// Reads the colour it writes, as framebuffer fetch: its own pixel in either orientation
fragment float4 rotated_fetch_fragment(RotatedVertex in [[stage_in]], float4 destination [[color(0)]])
{
    return destination * 0.5f + in.color * 0.5f;
}

/// Takes fwidth alone, the same along either axis: a turned plane draws the program as it is
fragment float4 rotated_fwidth_fragment(RotatedVertex in [[stage_in]])
{
    return float4(fwidth(in.uv.x) > 1.0f / 96.0f ? 1.0f : 0.0f, fwidth(in.uv.y) > 1.0f / 64.0f ? 1.0f : 0.0f, 0.0f, 1.0f);
}
