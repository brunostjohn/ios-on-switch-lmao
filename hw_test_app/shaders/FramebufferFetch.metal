#include <metal_stdlib>
using namespace metal;

struct FetchVertex {
    float4 position [[position]];
    uint step [[flat]];
};

vertex FetchVertex fetch_vertex(uint vid [[vertex_id]], uint instance [[instance_id]])
{
    const float2 points[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return {float4(points[vid % 3], 0, 1), vid / 3 + instance};
}

fragment float4 fetch_order_fragment(FetchVertex v [[stage_in]], float4 previous [[color(0)]],
                                     constant uint &discardOdd [[buffer(0)]])
{
    if (discardOdd && (v.step & 1))
        discard_fragment();
    const float3 next = float3((v.step * 3 + 1) % 7, (v.step * 5 + 2) % 7, (v.step + 3) % 7) / 8.0f;
    return float4(previous.rgb * 0.5f + next * 0.5f, 1);
}

// fetch_order_fragment's step, discarded over the left `rule.y` columns on odd steps when rule.x is set: whole regions of
// an interlocked draw that never reach the pixel's ticket
fragment float4 fetch_region_fragment(FetchVertex v [[stage_in]], float4 previous [[color(0)]],
                                      constant uint2 &rule [[buffer(0)]])
{
    if (rule.x && (v.step & 1) && v.position.x < float(rule.y))
        discard_fragment();
    const float3 next = float3((v.step * 3 + 1) % 7, (v.step * 5 + 2) % 7, (v.step + 3) % 7) / 8.0f;
    return float4(previous.rgb * 0.5f + next * 0.5f, 1);
}

vertex float4 fetch_depth_vertex(uint vid [[vertex_id]], constant float &depth [[buffer(0)]])
{
    const float2 points[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(points[vid % 3], depth, 1);
}

fragment float4 fetch_add_fragment(float4 previous [[color(0)]], constant float4 &value [[buffer(0)]])
{
    if (value.a < 0)
        discard_fragment();
    return float4(previous.rgb + value.rgb, 1);
}
