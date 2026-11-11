// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

raw::DepthStencilState depthState(uint8_t compare, bool write = false)
{
    raw::DepthStencilDesc d;
    d.depthCompare = compare;
    d.depthWrite = write;
    return raw::DepthStencilState(d);
}

class DeviceHiddenDepth : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = 0xd0;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    // A triangle under `state` in a pass with no depth/stencil attachment, given the hidden one or not: drawn, or why not
    std::string draw(const raw::DepthStencilState &state, bool hidden)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.hiddenZeta = hidden;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        e->setDepthStencil(&state);
        const char *why = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceHiddenDepth, EveryCompareButAlwaysDrawsOverTheHiddenAttachment)
{
    for (uint8_t compare = 0; compare < 7; compare++)
        EXPECT_EQ(draw(depthState(compare, compare & 1), true), "") << int(compare);
    raw::DepthStencilDesc stencil;
    stencil.front.compare = stencil.back.compare = 5;
    EXPECT_EQ(draw(raw::DepthStencilState(stencil), true), "");
}

TEST_F(DeviceHiddenDepth, AlwaysNeedsNoHiddenAttachment)
{
    EXPECT_EQ(draw(depthState(7), false), "");
    EXPECT_EQ(draw(depthState(7, true), false), "");
}

TEST_F(DeviceHiddenDepth, APassNotGivenItRefusesAStateThatTestsDepth)
{
    EXPECT_NE(draw(depthState(1), false), "");
    EXPECT_NE(draw(depthState(0), false), "");
}

}  // namespace
