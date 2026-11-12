#include <metal_stdlib>
using namespace metal;

// gpu.sched.long_draw_bands: a full-screen draw whose fragment work alone lasts over ten milliseconds on GM20B, straight-line
// so its static length is what it executes, and whose result depends on every pixel's position, so a pixel drawn twice
// or left out by its bands shows.
#define BANDS_STEP(v) v = fract(fma(v, float4(1.618034f, 1.414214f, 1.732051f, 2.236068f), float4(0.1f, 0.2f, 0.3f, 0.4f)));
#define BANDS_4(v) BANDS_STEP(v) BANDS_STEP(v) BANDS_STEP(v) BANDS_STEP(v)
#define BANDS_16(v) BANDS_4(v) BANDS_4(v) BANDS_4(v) BANDS_4(v)
#define BANDS_64(v) BANDS_16(v) BANDS_16(v) BANDS_16(v) BANDS_16(v)

struct BandsVertex {
    float4 position [[position]];
};

vertex BandsVertex bands_vertex(uint vid [[vertex_id]])
{
    const float2 corner = float2(vid == 1 ? 3.0f : -1.0f, vid == 2 ? 3.0f : -1.0f);
    return {float4(corner, 0, 1)};
}

static float4 bands_work(float2 at)
{
    float4 v = float4(at.x * 0.0013f, at.y * 0.0017f, (at.x + at.y) * 0.0007f, 0.5f);
    BANDS_64(v) BANDS_64(v) BANDS_64(v) BANDS_64(v)
    return v;
}

fragment float4 bands_blend_fragment(BandsVertex v [[stage_in]])
{
    return float4(bands_work(v.position.xy).rgb, 0.5f);
}

fragment float4 bands_fetch_fragment(BandsVertex v [[stage_in]], float4 fetched [[color(0)]])
{
    return float4(fract(fetched.rgb + bands_work(v.position.xy).rgb), 1.0f);
}
