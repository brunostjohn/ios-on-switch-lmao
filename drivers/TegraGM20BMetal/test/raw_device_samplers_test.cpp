// SPDX-License-Identifier: MIT
// The sampler headers raw::Device draws with: a planar texture's address map (Texture::addressMap) applied to a sampler
// state's header and to a constexpr sampler's, which the device places with the sampler.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"

namespace {

using Header = std::array<uint32_t, 8>;

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

// RenderBox's primitive_image_fragment samples its image through a linear constexpr sampler; these address it clamped to
// zero (TSC BORDER on every axis) and clamped to the edge
constexpr Header kLinearZero = {0x260DB, 0x2a2, 0x40000, 0, 0, 0, 0, 0};
constexpr Header kLinearClamp = {0x26092, 0x2a2, 0x40000, 0, 0, 0, 0, 0};

class DeviceSamplers : public harness::DeviceTest {
protected:
    raw::Texture target, image;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, image));
    }

    // A state whose fragment texture 0 is sampled through constexpr sampler `header`
    raw::Pipeline *constexprState(const Header &header, uint64_t key)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 constexpr " + std::to_string(key);
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        r.pipeline.keyHash = key;
        r.pipeline.entries = 1;
        r.pipeline.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerConstexpr, 0, 0};
        r.pipeline.setBytes = 32;
        r.samplers = {header};
        r.blob = {0x1a00, 0x1111};
        std::string why;
        raw::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // One draw of `state` sampling `t` at fragment texture 0: its set's handle word, or *why
    bool drawnHandle(raw::Pipeline *state, const raw::Texture &t, uint32_t &handle, std::string &why)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e) {
            why = "no pass";
            return false;
        }
        e->setPipeline(state);
        e->setTexture(0, &t);
        const char *refused = nullptr;
        const bool drawn = e->draw(kTriangle, 0, 3, 1, 0, &refused);
        const uint8_t *set = drawn ? raw::DeviceHarness::upload(dev, raw::DeviceHarness::preparedSet(*e), 4) : nullptr;
        if (set)
            memcpy(&handle, set, 4);
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno))
            cb->abandon();
        why = refused ? refused : set ? "" : "no set uploaded";
        return drawn && set;
    }
};

// Boot 21 WidgetRenderer: a two-plane 4:4:4 texture (address map 2) sampled through a constexpr sampler was refused
// (enc.planarclamp a=0 b=2) and its command buffer failed. The draw samples through the constexpr header with the
// clamps to zero sent to the edge.
TEST_F(DeviceSamplers, APlanarTextureSampledThroughAConstexprSamplerTakesItsMappedHeader)
{
    raw::Pipeline *state = constexprState(kLinearZero, 0x51);
    ASSERT_NE(state, nullptr);
    raw::Texture planar = image;
    planar.addressMap = 2;
    uint32_t handle = 0;
    std::string why;
    ASSERT_TRUE(drawnHandle(state, planar, handle, why)) << why;
    const uint32_t index = state->rec->entry[0].sampler;
    const uint32_t mapped = raw::DeviceHarness::constexprTsc(dev, index, 2);
    EXPECT_NE(mapped, raw::DeviceHarness::constexprTsc(dev, index, 0));
    EXPECT_EQ(handle >> 20, mapped);
    EXPECT_EQ(handle & 0xFFFFF, planar.tic);
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, mapped), kLinearClamp);
    ASSERT_TRUE(drawnHandle(state, image, handle, why)) << why;
    EXPECT_EQ(handle >> 20, raw::DeviceHarness::constexprTsc(dev, index, 0)) << "a plain texture: the sampler's own";
}

// A header no map changes is its own under every map: no header is placed for it
TEST_F(DeviceSamplers, AConstexprSamplerTheMapLeavesAloneSharesItsHeader)
{
    raw::Pipeline *state = constexprState(kLinearClamp, 0x52);
    ASSERT_NE(state, nullptr);
    const uint32_t index = state->rec->entry[0].sampler;
    for (uint8_t map = 1; map < 4; map++)
        EXPECT_EQ(raw::DeviceHarness::constexprTsc(dev, index, map), raw::DeviceHarness::constexprTsc(dev, index, 0));
}

// The map on a header's own fields is the map on the sampler state's address modes (Device::samplerVariant's rule
// before it moved to the header): every mode of every axis, every map
TEST(SamplerAddressMaps, TheHeaderMapIsTheDescriptorMap)
{
    for (uint8_t s = 0; s < 6; s++)
        for (uint8_t t = 0; t < 6; t++)
            for (uint8_t r = 0; r < 6; r++)
                for (uint8_t map = 1; map < 4; map++) {
                    raw::SamplerDesc d;
                    d.minFilter = d.magFilter = 1;
                    d.sAddress = s, d.tAddress = t, d.rAddress = r;
                    raw::SamplerDesc mapped = d;
                    const bool wrapped = map & 1, clamps = map & 2;
                    for (uint8_t *m : {&mapped.sAddress, &mapped.tAddress, &mapped.rAddress})
                        if ((wrapped && *m >= 1 && *m <= 3) || (clamps && *m >= 4))
                            *m = 0;
                    uint32_t want[8], got[8];
                    raw::samplerHeader(mapped, want);
                    raw::samplerHeader(d, got);
                    raw::mapSamplerAddresses(got, map);
                    EXPECT_EQ(memcmp(want, got, sizeof want), 0)
                        << int(s) << "/" << int(t) << "/" << int(r) << " map " << int(map);
                }
}

TEST_F(DeviceSamplers, ASamplerStatesVariantIsItsHeaderUnderTheMap)
{
    raw::SamplerDesc d;
    d.sAddress = d.tAddress = 4;
    raw::Sampler s;
    ASSERT_TRUE(dev.createSampler(d, s));
    ASSERT_TRUE(dev.samplerVariant(s, 2));
    ASSERT_NE(s.mapped[2], s.tsc);
    raw::SamplerDesc edge = d;
    edge.sAddress = edge.tAddress = 0;
    Header want;
    raw::samplerHeader(edge, want.data());
    EXPECT_EQ(raw::DeviceHarness::tscHeader(dev, s.mapped[2]), want);
    ASSERT_TRUE(dev.samplerVariant(s, 1));
    EXPECT_EQ(s.mapped[1], s.tsc) << "nothing wraps";
}

}  // namespace
