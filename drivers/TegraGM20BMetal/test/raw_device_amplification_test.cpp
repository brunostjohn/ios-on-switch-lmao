// SPDX-License-Identifier: MIT
// raw_amplification.cpp on raw::Device: a state of vertex amplification drawn once per amplification, a count the state
// does not take refused, a viewport offset refused, and a dynamic vertex stride taken from the bind up to the stream's
// 12-bit field.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

raw::RuntimeRecord record(uint64_t key, uint8_t amplificationMax, bool countRead, bool dynamicStride)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    r.pipeline.amplificationMax = amplificationMax;
    r.pipeline.amplificationIdWord = amplificationMax ? 3 : 0;
    r.pipeline.amplificationCountRead = countRead;
    r.pipeline.pushMask = amplificationMax ? 1u << 2 : 0;
    if (dynamicStride) {
        r.pipeline.vertexSlot = 1;
        r.pipeline.vertexStrideDynamic = 1;
        r.pipeline.pulledStride[0][0] = 2;
        r.pipeline.pulledStride[0][1] = 5;
        r.pipeline.pushMask |= 1u << 4;
    } else {
        r.pipeline.vertexSlot = 0xff;
    }
    return r;
}

class DeviceAmplification : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer vertices;
    raw::Pipeline *amplified = nullptr, *counted = nullptr, *plain = nullptr, *strided = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, vertices));
        std::string why;
        amplified = dev.addRecord(record(0xa301, 2, false, false), false, why);
        ASSERT_NE(amplified, nullptr) << why;
        counted = dev.addRecord(record(0xa302, 2, true, false), false, why);
        ASSERT_NE(counted, nullptr) << why;
        plain = dev.addRecord(record(0xa303, 0, false, false), false, why);
        ASSERT_NE(plain, nullptr) << why;
        strided = dev.addRecord(record(0xa304, 0, false, true), false, why);
        ASSERT_NE(strided, nullptr) << why;
    }

    struct Drawn {
        std::string why;
        uint32_t draws = 0;
    };

    // One draw of `pl` at amplification `count` (`viewportOffsets`: a mapping offsets the viewport), the vertex buffer at
    // slot 1 bound at `stride`
    Drawn drawWith(raw::Pipeline *pl, uint32_t count, bool viewportOffsets = false, uint32_t stride = UINT32_MAX)
    {
        Drawn d;
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {"no command buffer"};
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return {"no pass"};
        }
        e->setPipeline(pl);
        e->setBuffer(1, 1, &vertices, 0);
        e->setVertexStride(1, stride);
        e->setAmplification(count, viewportOffsets);
        const char *why = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        e->end();
        d.draws = cb->draws;
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            d.why = why ? why : "not committed";
        }
        return d;
    }
};

TEST_F(DeviceAmplification, EachAmplificationIsADrawOfItsOwn)
{
    const Drawn two = drawWith(amplified, 2);
    EXPECT_EQ(two.why, "");
    EXPECT_EQ(two.draws, 2u);
    const Drawn one = drawWith(amplified, 1);
    EXPECT_EQ(one.why, "");
    EXPECT_EQ(one.draws, 1u);
}

TEST_F(DeviceAmplification, ACountTheStateDoesNotTakeIsRefused)
{
    EXPECT_NE(drawWith(amplified, 3).why, "");
    EXPECT_NE(drawWith(plain, 2).why, "");
    EXPECT_EQ(drawWith(plain, 1).why, "");
    EXPECT_NE(drawWith(counted, 1).why, "");
    EXPECT_EQ(drawWith(counted, 2).why, "");
}

TEST_F(DeviceAmplification, AViewportOffsetGoesElsewhere)
{
    EXPECT_NE(drawWith(amplified, 2, true).why, "");
    EXPECT_EQ(drawWith(amplified, 1, true).why, "");
}

TEST_F(DeviceAmplification, ADynamicStrideIsTheBindsUpToTheStreamField)
{
    EXPECT_EQ(drawWith(strided, 1, false, 16).why, "");
    EXPECT_EQ(drawWith(strided, 1, false, 0xfff).why, "");
    EXPECT_EQ(drawWith(strided, 1).why, "");
    EXPECT_NE(drawWith(strided, 1, false, 0x1000).why, "");
}

}  // namespace
