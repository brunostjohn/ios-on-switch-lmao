// SPDX-License-Identifier: MIT
// raw GM20B path: a texel copy between two images of one texel size, each destination texel (x, y) the source's texel at
// (ax x + ay y + cx, bx x + by y + cy): a turned plane's texels moved between its frame and its plane (raw/runtime/
// raw_turn_copy.h). Colour images are viewed as unsigned integers of the texel's size, so every bit is moved as it is; a depth
// image's depth is read and written as the float it holds, which is its code. One function a texel size or depth format: the
// pack keys an own state by its function's name.
#include <metal_stdlib>
using namespace metal;

struct Map {
    int ax, ay, cx, bx, by, cy;
};

struct Out {
    float4 position [[position]];
};

vertex Out turn_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

static uint2 source(float4 position, constant Map &m)
{
    const int2 d = int2(position.xy);
    return uint2(m.ax * d.x + m.ay * d.y + m.cx, m.bx * d.x + m.by * d.y + m.cy);
}

fragment uint4 turn_copy8_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                               constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy16_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy32_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy64_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy128_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                 constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

struct Depth {
    float depth [[depth(any)]];
};

fragment Depth turn_depth16_frag(Out in [[stage_in]], depth2d<float, access::read> s [[texture(0)]],
                                 constant Map &m [[buffer(0)]])
{
    return {s.read(source(in.position, m))};
}

fragment Depth turn_depth32f_frag(Out in [[stage_in]], depth2d<float, access::read> s [[texture(0)]],
                                  constant Map &m [[buffer(0)]])
{
    return {s.read(source(in.position, m))};
}
