#include <metal_stdlib>
using namespace metal;

/// gpu.x1.rop_conversion: the half of bits row * 256 + column, each of the 65,536 (NaN, infinities and negatives
/// included), in every channel
fragment half4 x1_half_bits(float4 position [[position]])
{
    return half4(as_type<half>(ushort(uint(position.y) * 256 + uint(position.x))));
}

/// gpu.x1.sampler_conversion: each texel as a nearest sample at its centre returns it as half
kernel void x1_sample_half(texture2d<half> texture [[texture(0)]], device ushort4 *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(texture.get_width(), texture.get_height());
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] =
            as_type<ushort4>(texture.sample(nearest, (float2(id) + 0.5f) / size, level(0)));
}

/// gpu.x1.blend_exact: pixel y * 4096 + x draws case (y * 4096 + x) / 256 of `pairs` (indices of `halves`: the
/// colour, then the alpha) as (s, s, s, sa)
fragment half4 x1_blend_source(float4 position [[position]], device const ushort2 *pairs [[buffer(0)]],
                               device const half *halves [[buffer(1)]])
{
    const uint p = uint(position.y) * 4096 + uint(position.x);
    const ushort2 pair = pairs[min(p / 256, 32895u)];
    const half s = halves[pair.x];
    return half4(s, s, s, halves[pair.y]);
}

/// gpu.x1.iterated_blend: a premultiplied ramp, red across and alpha down
fragment half4 x1_ramp_source(float4 position [[position]])
{
    const float u = float(uint(position.x)) / 255.0f, a = float(uint(position.y)) / 255.0f;
    return half4(float4(u * a, (1.0f - u) * a, 0.5f * a, a));
}

/// gpu.x1.f32_filter: one linear sample of `texture` at `uv`
kernel void x1_sample_linear(texture2d<float> texture [[texture(0)]], constant float2 &uv [[buffer(0)]],
                             device float4 *out [[buffer(1)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    out[0] = texture.sample(linear, uv, level(0));
}

struct X1ColoredVertex {
    float4 position [[position]];
    float4 color;
};

/// gpu.x1.viewport_swizzle: rectangles of six vertices each, one colour a rectangle
vertex X1ColoredVertex x1_colored_vertex(uint vid [[vertex_id]], device const float4 *corners [[buffer(0)]],
                                         device const float4 *colors [[buffer(1)]])
{
    return X1ColoredVertex{corners[vid], colors[vid / 6]};
}

fragment float4 x1_colored_fragment(X1ColoredVertex in [[stage_in]])
{
    return in.color;
}
