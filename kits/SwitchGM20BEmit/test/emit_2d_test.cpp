// SPDX-License-Identifier: MIT
// The 2D solid fill against the methods of x1-units' reference fill (raw_probe_ops.h, defb0dac0) and xf86-video-nouveau's
// EXA fill: the destination's own format and one packed word, SRCCOPY, clip at the surface.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t k2D = SUBC_NV902D;

std::vector<uint32_t> fill(const switch_gm20b_2d_surface &s)
{
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_2d_fill(&p, &s, 0xff336699u, 8, 4, 40, 20);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_2D_FILL_WORDS);
    return words;
}

std::vector<Method> fillTail(const switch_gm20b_2d_surface &s)
{
    return {
        {k2D, NV902D_SET_OPERATION, NV902D_SET_OPERATION_V_SRCCOPY},
        {k2D, NV902D_SET_COLOR_KEY_ENABLE, 0},
        {k2D, NV902D_SET_RENDER_ENABLE_C, NV902D_SET_RENDER_ENABLE_C_MODE_TRUE},
        {k2D, NV902D_SET_CLIP_X0, 0},
        {k2D, NV902D_SET_CLIP_Y0, 0},
        {k2D, NV902D_SET_CLIP_WIDTH, s.width},
        {k2D, NV902D_SET_CLIP_HEIGHT, s.height},
        {k2D, NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_TRUE},
        {k2D, NV902D_RENDER_SOLID_PRIM_MODE, NV902D_RENDER_SOLID_PRIM_MODE_V_RECTS},
        {k2D, NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT, s.format},
        {k2D, NV902D_SET_RENDER_SOLID_PRIM_COLOR, 0xff336699u},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(0), 8},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_Y(0), 4},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_SET_X(1), 40},
        {k2D, NV902D_RENDER_SOLID_PRIM_POINT_Y(1), 20},
        {k2D, NV902D_SET_CLIP_ENABLE, NV902D_SET_CLIP_ENABLE_V_FALSE},
    };
}

TEST(Emit2D, ABlockLinearFillIsTheReferenceFill)
{
    switch_gm20b_2d_surface s = {};
    s.address = 0x0123456000ull;
    s.format = NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    s.block_height_log2 = 4;
    s.width = 1280;
    s.height = 720;
    std::vector<Method> expected = {
        {k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0},
        {k2D, NV902D_SET_COMPRESSION, 0},
        {k2D, NV902D_SET_DST_FORMAT, NV902D_SET_DST_FORMAT_V_A8R8G8B8},
        {k2D, NV902D_SET_DST_MEMORY_LAYOUT, NV902D_SET_DST_MEMORY_LAYOUT_V_BLOCKLINEAR},
        {k2D, NV902D_SET_DST_BLOCK_SIZE, 4u << 4},
        {k2D, NV902D_SET_DST_DEPTH, 1},
        {k2D, NV902D_SET_DST_LAYER, 0},
        {k2D, NV902D_SET_DST_WIDTH, 1280},
        {k2D, NV902D_SET_DST_HEIGHT, 720},
        {k2D, NV902D_SET_DST_OFFSET_UPPER, 0x01},
        {k2D, NV902D_SET_DST_OFFSET_LOWER, 0x23456000},
    };
    const auto tail = fillTail(s);
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_EQ(Methods(fill(s)), expected);
}

TEST(Emit2D, APitchFillNamesItsPitch)
{
    switch_gm20b_2d_surface s = {};
    s.address = 0x0100000000ull;
    s.format = NV902D_SET_DST_FORMAT_V_A8B8G8R8;
    s.pitch = true;
    s.pitch_bytes = 5120;
    s.width = 1280;
    s.height = 720;
    std::vector<Method> expected = {
        {k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 0},
        {k2D, NV902D_SET_COMPRESSION, 0},
        {k2D, NV902D_SET_DST_FORMAT, NV902D_SET_DST_FORMAT_V_A8B8G8R8},
        {k2D, NV902D_SET_DST_MEMORY_LAYOUT, NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH},
        {k2D, NV902D_SET_DST_PITCH, 5120},
        {k2D, NV902D_SET_DST_WIDTH, 1280},
        {k2D, NV902D_SET_DST_HEIGHT, 720},
        {k2D, NV902D_SET_DST_OFFSET_UPPER, 0x01},
        {k2D, NV902D_SET_DST_OFFSET_LOWER, 0},
    };
    const auto tail = fillTail(s);
    expected.insert(expected.end(), tail.begin(), tail.end());
    EXPECT_EQ(Methods(fill(s)), expected);
}

TEST(Emit2D, CompressionAndZetaAreTheSurfaces)
{
    switch_gm20b_2d_surface s = {};
    s.format = NV902D_SET_DST_FORMAT_V_A8R8G8B8;
    s.block_height_log2 = 4;
    s.width = 64;
    s.height = 64;
    s.compressed = true;
    s.zeta = true;
    const auto m = Methods(fill(s));
    ASSERT_GE(m.size(), 2u);
    EXPECT_EQ(m[0], (Method{k2D, NV902D_SET_DST_COLOR_RENDER_TO_ZETA_SURFACE, 1}));
    EXPECT_EQ(m[1], (Method{k2D, NV902D_SET_COMPRESSION, 1}));
}

TEST(Emit2D, AnSrgbSurfaceIsFilledThroughItsUnormView)
{
    const std::pair<uint32_t, uint32_t> twins[] = {
        {NV902D_SET_DST_FORMAT_V_A8RL8GL8BL8, NV902D_SET_DST_FORMAT_V_A8R8G8B8},
        {NV902D_SET_DST_FORMAT_V_A8BL8GL8RL8, NV902D_SET_DST_FORMAT_V_A8B8G8R8},
        {NV902D_SET_DST_FORMAT_V_X8RL8GL8BL8, NV902D_SET_DST_FORMAT_V_X8R8G8B8},
        {NV902D_SET_DST_FORMAT_V_X8BL8GL8RL8, NV902D_SET_DST_FORMAT_V_X8B8G8R8},
        {NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16, NV902D_SET_DST_FORMAT_V_RF16_GF16_BF16_AF16},
    };
    for (const auto &[srgb, unorm] : twins) {
        switch_gm20b_2d_surface s = {};
        s.format = srgb;
        s.block_height_log2 = 4;
        s.width = 64;
        s.height = 64;
        uint32_t dst = 0, solid = 0;
        for (const Method &m : Methods(fill(s))) {
            if (m.method == NV902D_SET_DST_FORMAT)
                dst = m.value;
            if (m.method == NV902D_SET_RENDER_SOLID_PRIM_COLOR_FORMAT)
                solid = m.value;
        }
        EXPECT_EQ(switch_gm20b_2d_fill_format(srgb), unorm);
        EXPECT_EQ(dst, unorm) << srgb;
        EXPECT_EQ(solid, unorm) << srgb;
    }
}

}  // namespace
