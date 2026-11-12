// SPDX-License-Identifier: MIT
// Passes into levels, slices, cube faces and 3D planes, layered passes, and viewport and scissor arrays, read back as the
// methods raw::Device submitted (the kernel model keeps each job's push words).
#include "raw_device_harness.h"
#include "../raw/fx/fx_image.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_mme.h"
#include "../raw/runtime/raw_record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <map>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangleStrip = 4;

struct Method {
    uint32_t method, value;
};

// The 3D subchannel's methods of `words`, in order (INC, NON_INC, ONE_INC and IMMD headers)
std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (op == 4) {
            if (subc == 0)
                out.push_back({method, count});
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            continue;
        for (uint32_t k = 0; k < count && i < words.size(); k++, i++)
            if (subc == 0)
                out.push_back({method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]});
    }
    return out;
}

// The last value each method took in `m`
std::map<uint32_t, uint32_t> last(const std::vector<Method> &m)
{
    std::map<uint32_t, uint32_t> out;
    for (const Method &x : m)
        out[x.method] = x.value;
    return out;
}

std::vector<uint32_t> valuesOf(const std::vector<Method> &m, uint32_t method)
{
    std::vector<uint32_t> out;
    for (const Method &x : m)
        if (x.method == method)
            out.push_back(x.value);
    return out;
}

class DeviceLayers : public harness::DeviceTest {
protected:
    raw::Pipeline *plain = nullptr, *choosing = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        plain = record(0xa1, false);
        choosing = record(0xa2, true);
        ASSERT_NE(plain, nullptr);
        ASSERT_NE(choosing, nullptr);
    }

    // A state of an RGBA8 target whose vertex program writes ViewportIndex (its header's OMAP bit 426) or not
    raw::Pipeline *record(uint64_t key, bool viewportIndex)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = viewportIndex ? "vs-viewport" : "vs";  // (placed programs are shared by id)
        r.programs[1].id = "fs";
        if (viewportIndex)
            r.programs[0].image[53] |= 4;
        r.pipeline.keyHash = key;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        return dev.addRecord(r, false, why);
    }

    // The methods of one pass over `pass`, with `body` encoding into it; empty where the pass or a draw was refused
    template <class Body> std::vector<Method> pass(const raw::PassDesc &p, Body body, std::string *why = nullptr)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        raw::RenderEncoder *e = cb->render(p);
        if (!e) {
            cb->abandon();
            if (why)
                *why = "no pass";
            return {};
        }
        const char *refused = nullptr;
        const bool ok = body(*e, &refused);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            if (why)
                *why = refused ? refused : "not committed";
            return {};
        }
        return methods(kernel_model::model.jobs.back().words);
    }

    static raw::PassDesc onto(raw::Texture &t, uint32_t layers = 1)
    {
        raw::PassDesc p;
        p.color[0].texture = &t;
        p.color[0].load = raw::ColorAttachment::Clear;
        p.color[0].clear[0] = 1;
        p.count = 1;
        p.layers = layers;
        return p;
    }

    static bool none(raw::RenderEncoder &, const char **)
    {
        return true;
    }
};

TEST_F(DeviceLayers, ALevelOfAnArraySliceIsTheLevelsImageAtThatLayer)
{
    raw::Texture t, part;
    ASSERT_TRUE(dev.createArray(64, 64, 3, kRGBA8, raw::fx::kTexTwoDArray, t, 4));
    ASSERT_NE(t.chain, nullptr);
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::arrayLayout(64, 64, 3, 4, 4, c));
    EXPECT_EQ(t.chain->layout.bytes, c.bytes);
    ASSERT_TRUE(raw::attachmentTarget(t, 2, 1, part));
    const uint64_t va = t.va + c.level[2].offset + c.arrayStride;
    EXPECT_EQ(part.va, va);
    EXPECT_EQ(part.layers, 2u);
    const auto m = last(pass(onto(part), none));
    ASSERT_FALSE(m.empty());
    const uint32_t ct = NVB197_SET_COLOR_TARGET_A(0);
    EXPECT_EQ(m.at(ct), uint32_t(va >> 32));
    EXPECT_EQ(m.at(ct + 4), uint32_t(va));
    EXPECT_EQ(m.at(ct + 8), c.level[2].rowBytes / 4);  // WIDTH: the level's row stride in texels
    EXPECT_EQ(m.at(ct + 12), 16u);
    EXPECT_EQ(m.at(ct + 20), uint32_t(c.level[2].yLog2) << 4);  // MEMORY: ONE_GOB wide, the level's height, array size
    EXPECT_EQ(m.at(ct + 24), 1u);                               // THIRD_DIMENSION
    EXPECT_EQ(m.at(ct + 28), uint32_t(c.arrayStride >> 2));     // ARRAY_PITCH
    EXPECT_EQ(m.at(ct + 32), 0u);                               // LAYER
}

TEST_F(DeviceLayers, ACubeFaceIsItsLayer)
{
    raw::Texture t, face;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, t));
    ASSERT_TRUE(raw::attachmentTarget(t, 0, 3, face));
    EXPECT_EQ(face.va, t.va + 3 * t.layerBytes);
    const auto m = last(pass(onto(face), none));
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_B(0)), uint32_t(t.va + 3 * t.layerBytes));
}

TEST_F(DeviceLayers, A3DPlaneIsTheLevelsPlaneUnderDepthSize)
{
    raw::Texture t, plane;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, true, t, 2));
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::volumeLayout(16, 16, 8, 2, 4, true, c));
    ASSERT_TRUE(raw::attachmentTarget(t, 1, 3, plane));
    const raw::fx::MipLevel &l = c.level[1];
    EXPECT_EQ(plane.va, t.va + l.offset + 3ull * l.rowBytes * l.rows);
    EXPECT_EQ(plane.layers, l.depth - 3);
    const auto m = last(pass(onto(plane), none));
    ASSERT_FALSE(m.empty());
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 16 & 1, 1u);  // THIRD_DIMENSION_DEFINES_DEPTH_SIZE
    EXPECT_EQ(m.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 8 & 15, 0u);  // one plane a block
    raw::Texture deep;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, deep));
    EXPECT_FALSE(raw::attachmentTarget(deep, 0, 1, plane)) << "a plane inside a block of several is no base address";
}

TEST_F(DeviceLayers, PitchMemoryRendersOneLayerOnly)
{
    raw::Texture t, part;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, false));
    ASSERT_FALSE(t.blockLinear);
    EXPECT_FALSE(raw::attachmentTarget(t, 0, 1, part));
    std::string why;
    EXPECT_TRUE(pass(onto(t, 2), none, &why).empty());
    EXPECT_EQ(why, "no pass");
}

TEST_F(DeviceLayers, ALayeredPassBindsAndClearsEveryLayer)
{
    raw::Texture cube, view;
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, cube));
    ASSERT_TRUE(raw::attachmentTarget(cube, 0, 0, view));
    raw::PassDesc p = onto(view, 6);
    p.hiddenZeta = true;
    const auto m = pass(p, none);
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 6u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 6u);
    const uint32_t call = NVB197_CALL_MME_MACRO(raw::mme::kClearLayers);
    const auto payloads = valuesOf(m, call), counts = valuesOf(m, call + 4);
    ASSERT_EQ(payloads.size(), 1u) << "one call clears color and the hidden zeta in all six";
    EXPECT_EQ(counts, std::vector<uint32_t>{6});
    EXPECT_EQ(payloads[0] & 3, 3u);
    EXPECT_EQ(payloads[0] >> 10, 0u) << "from layer 0";
    EXPECT_TRUE(valuesOf(m, NVB197_CLEAR_SURFACE).empty());
}

TEST_F(DeviceLayers, ALayeredPassPastItsTargetsLayersIsRefused)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 1, view));
    EXPECT_FALSE(pass(onto(view, 3), none).empty());
    std::string why;
    EXPECT_TRUE(pass(onto(view, 4), none, &why).empty());
}

TEST_F(DeviceLayers, ADrawInALayeredPassGoesWhereItsLayerIsKnown)
{
    raw::Texture array, view;
    ASSERT_TRUE(dev.createArray(32, 32, 4, kRGBA8, raw::fx::kTexTwoDArray, array));
    ASSERT_TRUE(raw::attachmentTarget(array, 0, 0, view));
    std::string why;
    const auto m = pass(
        onto(view, 4),
        [&](raw::RenderEncoder &e, const char **refused) {
            e.setPipeline(plain);
            return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
        },
        &why);
    EXPECT_TRUE(m.empty());
    EXPECT_NE(why.find("layered pass"), std::string::npos) << why;
}

TEST_F(DeviceLayers, ViewportsAndScissorsPastTheFirstAreWrittenAtTheirIndex)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, true));
    double viewports[raw::kMaxViewports][6];
    uint32_t rects[raw::kMaxViewports][4];
    for (uint32_t j = 0; j < raw::kMaxViewports; j++) {
        const double v[6] = {double(j % 4) * 16, double(j / 4) * 16, 16, 16, 0, 1};
        std::copy_n(v, 6, viewports[j]);
        const uint32_t r[4] = {(j % 4) * 16 + 1, (j / 4) * 16 + 2, 12, 10};
        std::copy_n(r, 4, rects[j]);
    }
    const auto m = pass(onto(t), [&](raw::RenderEncoder &e, const char **refused) {
        e.setPipeline(plain);
        e.setViewports(viewports, raw::kMaxViewports);
        e.setScissors(rects, raw::kMaxViewports);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
    });
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    for (uint32_t j = 1; j < raw::kMaxViewports; j++) {
        uint32_t w[8];
        raw::viewportWords(viewports[j], w);
        const uint32_t vp = NVB197_SET_VIEWPORT_SCALE_X(j);
        for (uint32_t i = 0; i < 6; i++)
            EXPECT_EQ(at.at(vp + 4 * i), w[i]) << "viewport " << j << " word " << i;
        EXPECT_EQ(at.at(NVB197_SET_VIEWPORT_CLIP_HORIZONTAL(j)), w[6]) << j;
        EXPECT_EQ(at.at(NVB197_SET_VIEWPORT_CLIP_VERTICAL(j)), w[7]) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_ENABLE(j)), 1u) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_HORIZONTAL(j)), (rects[j][0] + 12) << 16 | rects[j][0]) << j;
        EXPECT_EQ(at.at(NVB197_SET_SCISSOR_VERTICAL(j)), (rects[j][1] + 10) << 16 | rects[j][1]) << j;
    }
}

TEST_F(DeviceLayers, AVertexFunctionChoosingAmongViewportsIsRefusedItsRecordDrawsZero)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, t, false, true));
    double viewports[2][6] = {{0, 0, 32, 32, 0, 1}, {32, 32, 32, 32, 0, 1}};
    const auto draw = [&](uint32_t count) {
        std::string why;
        const auto m = pass(
            onto(t),
            [&](raw::RenderEncoder &e, const char **refused) {
                e.setPipeline(choosing);
                e.setViewports(viewports, count);
                return e.draw(kTriangleStrip, 0, 4, 1, 0, refused);
            },
            &why);
        return m.empty() ? why : std::string();
    };
    EXPECT_EQ(draw(1), "");
    EXPECT_NE(draw(2).find("viewports"), std::string::npos);
}

TEST_F(DeviceLayers, MipmappedArraysCubesAndVolumesAreChainsOfTheirLayout)
{
    raw::Texture cubes, volume, level;
    ASSERT_TRUE(dev.createArray(32, 32, 12, kRGBA8, raw::fx::kTexCubeArray, cubes, 6));
    ASSERT_NE(cubes.chain, nullptr);
    EXPECT_EQ(cubes.chain->layout.layers, 12u);
    EXPECT_EQ(cubes.chain->layout.levels, 6u);
    EXPECT_NE(cubes.chain->fmt2d, 0u) << "every layer's levels are the 2D engine's";
    ASSERT_TRUE(raw::chainLevelImage(cubes, 3, level));
    EXPECT_EQ(level.va, cubes.va + cubes.chain->layout.level[3].offset);
    EXPECT_EQ(level.layers, 12u);
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, volume, 3));
    ASSERT_NE(volume.chain, nullptr);
    EXPECT_EQ(volume.chain->fmt2d, 0u) << "the 2D engine halves no depth";
    ASSERT_TRUE(raw::chainLevelImage(volume, 1, level));
    EXPECT_EQ(level.type, raw::fx::kTexThreeD);
    EXPECT_EQ(level.depth, 4u);
    EXPECT_EQ(level.blockDepth, volume.chain->layout.level[1].zLog2);
    raw::Texture flat;
    EXPECT_FALSE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, flat, 7)) << "past 32's full chain";
}

TEST_F(DeviceLayers, BlockCompressedTexturesOfLevelsAndLayersAreNilsImage)
{
    constexpr uint32_t kASTC4x4 = 204, kETC2RGB8 = 180;
    raw::Texture astc, etc;
    ASSERT_TRUE(dev.createBlockTexture(100, 60, 7, 1, kASTC4x4, raw::fx::kTexTwoD, astc));
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::blockChainLayout(100, 60, 7, 1, 4, 4, 16, c));
    ASSERT_NE(astc.chain, nullptr);
    EXPECT_EQ(astc.chain->layout.bytes, c.bytes);
    EXPECT_EQ(astc.width, 25u) << "its blocks";
    ASSERT_TRUE(dev.createBlockTexture(64, 64, 1, 3, kETC2RGB8, raw::fx::kTexTwoDArray, etc));
    ASSERT_TRUE(raw::fx::blockChainLayout(64, 64, 1, 3, 4, 4, 8, c));
    EXPECT_EQ(etc.chain, nullptr);
    EXPECT_EQ(etc.layers, 3u);
    EXPECT_EQ(etc.layerBytes, c.arrayStride);
    EXPECT_NE(etc.tic, 0u);
    raw::Texture none;
    EXPECT_FALSE(dev.createBlockTexture(64, 64, 1, 1, kRGBA8, raw::fx::kTexTwoD, none)) << "not block-compressed";
}

TEST_F(DeviceLayers, ALayerOfADepthArrayIsItsZetaFromThere)
{
    constexpr uint32_t kDepth32Float = 252;
    raw::Texture shape, zeta, cube, face;
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(raw::zetaShape(32, 32, kDepth32Float, 1, shape, bytes, align));
    raw::ZetaMemory m;
    m.va = 0x7000000000;
    m.bytes = shape.layerBytes * 6;
    m.layerBytes = shape.layerBytes;
    m.pitch = shape.pitch;
    m.bpp = shape.bpp;
    m.blockHeight = shape.blockHeight;
    m.tiled = true;
    const char *why = nullptr;
    ASSERT_TRUE(dev.createZetaOver(m, 32, 32, kDepth32Float, true, zeta, &why, 6)) << why;
    EXPECT_EQ(zeta.layers, 6u);
    EXPECT_EQ(zeta.tic, 0u) << "an array's zeta is rendered, its samples m2v's";
    ASSERT_TRUE(dev.createArray(32, 32, 6, kRGBA8, raw::fx::kTexCube, cube));
    ASSERT_TRUE(raw::attachmentTarget(cube, 0, 2, face));
    raw::Texture part = zeta;
    part.va += 2 * zeta.layerBytes;
    part.layers -= 2;
    raw::PassDesc p = onto(face, 4);
    p.depthStencil = {&part, true, false, 1.0f, 0};
    const auto at = last(pass(p, none));
    ASSERT_FALSE(at.empty());
    EXPECT_EQ(at.at(NVB197_SET_ZT_B), uint32_t(m.va + 2 * zeta.layerBytes));
    EXPECT_EQ(at.at(NVB197_SET_ZT_ARRAY_PITCH), uint32_t(zeta.layerBytes >> 2));
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 4u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_LAYER), 0u);
    p.layers = 5;
    EXPECT_TRUE(pass(p, none).empty()) << "past the zeta's layers";
}

TEST_F(DeviceLayers, MemorylessArraysAreBackedWithThePassesLayers)
{
    constexpr uint32_t kDepth32Float = 252;
    raw::Texture color, depth, shape;
    ASSERT_TRUE(dev.createMemoryless(32, 32, kRGBA8, color, 1, 6));
    ASSERT_TRUE(dev.createMemoryless(32, 32, kDepth32Float, depth, 1, 6));
    EXPECT_FALSE(dev.createMemoryless(32, 32, kRGBA8, shape, 4, 2)) << "no multisampled array";
    EXPECT_EQ(color.layers, 6u);
    EXPECT_TRUE(raw::wholeAttachment(color, 0, 5));
    EXPECT_FALSE(raw::wholeAttachment(color, 0, 6));
    raw::PassDesc p = onto(color, 4);
    p.depthStencil = {&depth, true, false, 1.0f, 0};
    const auto m = pass(p, none);
    ASSERT_FALSE(m.empty());
    const auto at = last(m);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0)), 4u);
    EXPECT_EQ(at.at(NVB197_SET_COLOR_TARGET_MEMORY(0)) >> 12 & 1, 0u) << "block-linear";
    EXPECT_NE(at.at(NVB197_SET_COLOR_TARGET_ARRAY_PITCH(0)), 0u);
    EXPECT_EQ(at.at(NVB197_SET_ZT_SIZE_C), 4u);
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(raw::zetaShape(32, 32, kDepth32Float, 1, shape, bytes, align));
    EXPECT_EQ(at.at(NVB197_SET_ZT_ARRAY_PITCH), uint32_t(shape.layerBytes >> 2));
    const uint32_t call = NVB197_CALL_MME_MACRO(raw::mme::kClearLayers);
    EXPECT_EQ(valuesOf(m, call + 4), std::vector<uint32_t>{4});
    raw::PassDesc alone = onto(color, 3);
    EXPECT_FALSE(pass(alone, none).empty()) << "a layered pass of memoryless color alone backs it block-linear";
}

}  // namespace
