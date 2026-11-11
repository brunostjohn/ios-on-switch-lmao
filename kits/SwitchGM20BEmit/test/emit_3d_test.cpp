// SPDX-License-Identifier: MIT
// The kit's 3D state sequences and barriers against the raw path's, for the same inputs. Pure raw code is driven
// directly (raw_target.h's target words, raw_emit.cpp's blend registers). The sequences raw_gpu.cpp writes inline are
// replayed through raw::Push with raw_gpu.cpp's own constants (driver_constants.h reads them from its text), and the
// replayed lines are checked to still be raw_gpu.cpp's: a change there fails here until the replay follows it.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_emit.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_push.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_target.h"
#include "kits/SwitchGM20B/test/driver_constants.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmit3D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "push_words.h"

namespace {

namespace g = driver::raw_gpu_cpp;
using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;
using switch_gm20b_test::Words;
using testing::HasSubstr;

const std::string &rawGpu()
{
    static const std::string text =
        switch_ios::test::SourceText("../../../drivers/TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
    return text;
}

void expectRawSays(const char *line)
{
    EXPECT_THAT(rawGpu(), HasSubstr(line)) << "raw_gpu.cpp changed: replay its new form here";
}

uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

template <typename F> std::vector<uint32_t> ours(F emit)
{
    std::vector<uint32_t> words(256);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    emit(&p);
    words.resize(nv_push_dw_count(&p));
    return words;
}

template <typename F> std::vector<uint32_t> theirs(F emit)
{
    std::vector<uint32_t> words(256);
    raw::Push p{words.data(), words.data() + words.size()};
    emit(p);
    words.resize(size_t(p.at - words.data()));
    return words;
}

TEST(Emit3D, ViewportAndScissorAsRawWritesThem)
{
    expectRawSays(
        "p.incArray(VIEWPORT_SCALE_X, viewport_, 6);\n        p.inc(VIEWPORT_CLIP_H, {viewport_[6], viewport_[7]});");
    expectRawSays("p.inc(SCISSOR_ENABLE, {1, scissor_[0], scissor_[1]});");
    const switch_gm20b_viewport_state v = {{640.0f, -360.0f, 1.0f}, {640.0f, 360.0f, 0.0f}, 0, 1280, 0, 720};
    const uint32_t viewport[8] = {bits(640.0f), bits(-360.0f), bits(1.0f),  bits(640.0f),
                                  bits(360.0f), bits(0.0f),    1280u << 16, 720u << 16};
    const uint32_t scissor[2] = {200u << 16 | 10u, 300u << 16 | 20u};
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_viewport(p, 0, &v);
                  switch_gm20b_scissor(p, 0, 10, 200, 20, 300);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.incArray(uint32_t(g::c_VIEWPORT_SCALE_X), viewport, 6);
                  p.inc(uint32_t(g::c_VIEWPORT_CLIP_H), {viewport[6], viewport[7]});
                  p.inc(uint32_t(g::c_SCISSOR_ENABLE), {1, scissor[0], scissor[1]});
              })));
}

TEST(Emit3D, BlendConstantAsRawWritesIt)
{
    expectRawSays("p.incArray(BLEND_CONST_RED, blendColor_, 4);");
    const float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const uint32_t words[4] = {bits(0.25f), bits(0.5f), bits(0.75f), bits(1.0f)};
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_blend_constant(p, rgba);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.incArray(uint32_t(g::c_BLEND_CONST_RED), words, 4);
              })));
}

TEST(Emit3D, ClearAsRawWritesIt)
{
    expectRawSays(
        "p.inc(COLOR_CLEAR_VALUE, {v[0], v[1], v[2], v[3]});\n        p.inc(CLEAR_RECT_H, {c.x1 << 16 | c.x0, "
        "c.y1 << 16 | c.y0});\n        p.m(CLEAR_SURFACE_CONTROL, 0x10);  // USE_CLEAR_RECT\n        "
        "p.m(RENDER_ENABLE_OVERRIDE, 1);\n        p.m(CLEAR_SURFACE, 0x3c | c.target << 6 | (area ? zsBits : "
        "0));\n        p.m(RENDER_ENABLE_OVERRIDE, 0);");
    for (const bool depth : {false, true}) {
        const switch_gm20b_clear_state c = {{0x3f800000, 0, 0x3f000000, 0x3f800000}, 4, 1280, 8, 720, 2, depth, depth};
        const uint32_t zsBits = depth ? 3 : 0;
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_clear(p, &c);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.inc(uint32_t(g::c_COLOR_CLEAR_VALUE), {c.value[0], c.value[1], c.value[2], c.value[3]});
                      p.inc(uint32_t(g::c_CLEAR_RECT_H),
                            {uint32_t(c.xmax) << 16 | c.xmin, uint32_t(c.ymax) << 16 | c.ymin});
                      p.m(uint32_t(g::c_CLEAR_SURFACE_CONTROL), 0x10);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 1);
                      p.m(uint32_t(g::c_CLEAR_SURFACE), 0x3c | c.target << 6 | zsBits);
                      p.m(uint32_t(g::c_RENDER_ENABLE_OVERRIDE), 0);
                  })));
    }
}

TEST(Emit3D, RenderTargetsAsRawWritesThem)
{
    expectRawSays(
        "targetWords(p, COLOR_TARGET + 0x40 * j, *t);\n            p.m(COLOR_COMPRESSION + 4 * j, t->comptags);");
    expectRawSays("disabledTargetWords(w);\n            p.incArray(COLOR_TARGET + 0x40 * j, w, kColorTargetWords);\n"
                  "            p.m(COLOR_COMPRESSION + 4 * j, 1);");
    expectRawSays("p.m(CT_SELECT, ct | count);\n    p.inc(SURFACE_CLIP_H, {areaW_ << 16, areaH_ << 16});");
    raw::ColorTargetDesc pitch;
    pitch.va = 0x1234560000;
    pitch.pitch = 5120;
    pitch.height = 720;
    pitch.bpp = 4;
    pitch.rtFormat = NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8;
    raw::ColorTargetDesc level = pitch;
    level.blockLinear = true;
    level.pitch = 2560;
    level.height = 360;
    level.blockHeight = 3;
    level.layerBytes = 0x500000;
    for (uint32_t j : {0u, 3u}) {
        for (const raw::ColorTargetDesc &d : {pitch, level}) {
            switch_gm20b_target t = {};
            t.address = d.va;
            t.width = d.blockLinear ? d.pitch / d.bpp : d.pitch;
            t.height = d.height;
            t.format = uint8_t(d.rtFormat);
            t.pitch = !d.blockLinear;
            t.block_height_log2 = d.blockHeight;
            t.layers = 1;
            t.array_pitch = d.blockLinear ? uint32_t(d.layerBytes >> 2) : 0;
            uint32_t w[raw::kColorTargetWords];
            raw::colorTargetWords(d, w);
            EXPECT_EQ(Words(ours([&](nv_push *p) {
                          switch_gm20b_color_target(p, j, &t, false);
                      })),
                      Words(theirs([&](raw::Push &p) {
                          p.incArray(uint32_t(g::c_COLOR_TARGET) + 0x40 * j, w, raw::kColorTargetWords);
                          p.m(uint32_t(g::c_COLOR_COMPRESSION) + 4 * j, 0);
                      })));
        }
        switch_gm20b_target off = {};
        off.width = 64;
        off.layers = 1;
        uint32_t w[raw::kColorTargetWords];
        raw::disabledTargetWords(w);
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_color_target(p, j, &off, true);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.incArray(uint32_t(g::c_COLOR_TARGET) + 0x40 * j, w, raw::kColorTargetWords);
                      p.m(uint32_t(g::c_COLOR_COMPRESSION) + 4 * j, 1);
                  })));
    }
    for (uint32_t count : {1u, 4u}) {
        uint32_t ct = 0;
        for (uint32_t k = 0; k < 8; k++)
            ct |= k << (4 + 3 * k);
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_render_area(p, count, 1280, 720);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      p.m(uint32_t(g::c_CT_SELECT), ct | count);
                      p.inc(uint32_t(g::c_SURFACE_CLIP_H), {1280u << 16, 720u << 16});
                  })));
    }
}

TEST(Emit3D, BarriersAsRawWritesThem)
{
    expectRawSays(
        "tiledCacheDrain(p_, dev_->k_.info());\n    p_.m(WAIT_FOR_IDLE, 0);\n    p_.m(INVALIDATE_SHADER_CACHES, "
        "kBetweenPasses);\n    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);");
    expectRawSays("p.m(PIXEL_SHADER_BARRIER, 0);\n    p.m(INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED, 0);\n}");
    for (const bool tiled : {false, true}) {
        EXPECT_EQ(Words(ours([&](nv_push *p) {
                      switch_gm20b_pass_barrier(p, tiled);
                  })),
                  Words(theirs([&](raw::Push &p) {
                      if (tiled)
                          p.m(uint32_t(driver::raw_tiled_cache_h::c_TILED_CACHE_FLUSH), 0);
                      p.m(uint32_t(g::c_WAIT_FOR_IDLE), 0);
                      p.m(uint32_t(g::c_INVALIDATE_SHADER_CACHES), uint32_t(g::c_kBetweenPasses));
                      p.m(uint32_t(g::c_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI), 0);
                  })));
    }
    EXPECT_EQ(Words(ours([&](nv_push *p) {
                  switch_gm20b_pixel_shader_barrier(p);
              })),
              Words(theirs([&](raw::Push &p) {
                  p.m(uint32_t(g::c_PIXEL_SHADER_BARRIER), 0);
                  p.m(uint32_t(g::c_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED), 0);
              })));
}

// raw_emit.cpp's blend registers for a key, as (method, value) pairs the hardware reads
std::vector<Method> rawBlend(const std::string &key, uint32_t first, uint32_t last)
{
    std::array<uint8_t, 80> header = {};
    header[72] = 0xff;  // the fragment program writes all four components of targets 0 and 1
    raw::EmitProgram fs = {header.data(), 16, false, 0};
    std::vector<raw::EmitRegister> regs;
    std::string why;
    EXPECT_TRUE(raw::emitState(key, {}, fs, regs, why)) << why;
    std::vector<Method> out;
    for (const raw::EmitRegister &r : regs)
        if (r.method >= first && r.method < last)
            out.push_back({SUBC_NVB197, r.method, r.value});
    return out;
}

TEST(Emit3D, BlendAsRawEmitsIt)
{
    // Target 1 blends source-over (SRC_ALPHA, ONE_MINUS_SRC_ALPHA; ONE, ONE_MINUS_SRC_ALPHA; ADD), target 0 MAX of
    // ONEs; both write all components
    const std::string key = "samples=1 ds=0/0 c0=80:15:1:1:1:4:1:1:4 c1=80:15:1:4:5:0:1:5:0";
    const switch_gm20b_blend_state over = {NVB197_SET_BLEND_PER_TARGET_COLOR_OP_V_OGL_FUNC_ADD,
                                           NVB197_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF_V_OGL_SRC_ALPHA,
                                           NVB197_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF_V_OGL_ONE_MINUS_SRC_ALPHA,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_OP_V_OGL_FUNC_ADD,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF_V_OGL_ONE,
                                           NVB197_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF_V_OGL_ONE_MINUS_SRC_ALPHA};
    const switch_gm20b_blend_state max = {NVB197_SET_BLEND_PER_TARGET_COLOR_OP_V_OGL_MAX,
                                          NVB197_SET_BLEND_PER_TARGET_COLOR_SOURCE_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_COLOR_DEST_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_OP_V_OGL_MAX,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_SOURCE_COEFF_V_OGL_ONE,
                                          NVB197_SET_BLEND_PER_TARGET_ALPHA_DEST_COEFF_V_OGL_ONE};
    const uint32_t perTarget1 = NVB197_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(1);
    std::vector<Method> raw1 = rawBlend(key, NVB197_SET_BLEND(1), NVB197_SET_BLEND(2));
    std::vector<Method> rawPer1 = rawBlend(key, perTarget1, perTarget1 + 0x20);
    raw1.insert(raw1.end(), rawPer1.begin(), rawPer1.end());
    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_blend(p, 1, &over);
              })),
              raw1);

    // Target 0: raw leaves SEPARATE_FOR_ALPHA(0) to the context's baseline; the kit writes it, as NVK does
    std::vector<Method> raw0 = rawBlend(key, NVB197_SET_BLEND(0), NVB197_SET_BLEND(1));
    std::vector<Method> rawPer0 = rawBlend(key, NVB197_SET_BLEND_PER_TARGET_COLOR_OP(0), perTarget1);
    raw0.push_back({SUBC_NVB197, NVB197_SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(0), 1});
    raw0.insert(raw0.end(), rawPer0.begin(), rawPer0.end());
    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_blend(p, 0, &max);
              })),
              raw0);

    EXPECT_EQ(Methods(ours([&](nv_push *p) {
                  switch_gm20b_ct_write(p, 1, 0xf);
              })),
              rawBlend(key, NVB197_SET_CT_WRITE(1), NVB197_SET_CT_WRITE(2)));
}

}  // namespace
