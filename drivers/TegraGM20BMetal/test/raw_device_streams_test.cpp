// SPDX-License-Identifier: MIT
// raw_vertex_streams.cpp on raw::Device, read off the pushes the device submitted: a state of two fixed-function vertex
// streams binds each stream's location and limit at its own bind, a draw after the second stream's bind moved rebinds that
// stream alone, and a second stream of a bind stride takes its bind's stride in its format.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include <map>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;
constexpr uint32_t kFormat1 = 0x1c10, kLocation0 = 0x1c04, kLocation1 = 0x1c14, kLimit0 = 0x1f00, kLimit1 = 0x1f08;
constexpr uint32_t kStreamEnable = 1u << 12;

// (method, value) of every method a pushed word stream writes (raw_push.h's encodings)
std::multimap<uint32_t, uint32_t> methods(const std::vector<uint32_t> &words)
{
    std::multimap<uint32_t, uint32_t> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, method = (h & 0x1fff) << 2;
        if (op == 4) {
            out.emplace(method, count);
            continue;
        }
        for (uint32_t k = 0; k < count && i < words.size(); k++)
            out.emplace(op == 1 ? method + 4 * k : method, words[i++]);
    }
    return out;
}

// A state reading attribute 0 from buffer 1 (stream 0, 12 bytes apart) and attribute 1 from buffer 2 (stream 1, 16 bytes
// apart, or its bind's)
raw::RuntimeRecord twoStreams(uint64_t key, bool bindStride)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 a0=30:0:1:12:1:1 a1=31:0:2:16:1:1 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.pipeline.vertexSlot = 1;
    r.pipeline.vertexStride = 12;
    r.pipeline.vertexStreams[0] = uint8_t(2 + 1) | (bindStride ? raw::kStreamBindStride : 0);
    r.blob = {0x1a00, 0x1111, 0x1c00, kStreamEnable | 12, kFormat1, kStreamEnable | (bindStride ? 0 : 16)};
    return r;
}

class DeviceStreams : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer positions, colours, moved;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, positions));
        ASSERT_TRUE(dev.createBuffer(4096, colours));
        ASSERT_TRUE(dev.createBuffer(4096, moved));
    }

    // Two draws of `pl` in one pass, the second after `between`; every method the device submitted
    template <class Between> std::multimap<uint32_t, uint32_t> drawTwice(raw::Pipeline *pl, Between between)
    {
        const size_t before = kernel_model::model.jobs.size();
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        EXPECT_NE(e, nullptr);
        if (!e) {
            cb->abandon();
            return {};
        }
        e->setPipeline(pl);
        e->setBuffer(1, 1, &positions, 0);
        e->setBuffer(1, 2, &colours, 0);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        between(*e);
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        std::vector<uint32_t> words;
        for (size_t j = before; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        return methods(words);
    }
};

std::vector<uint32_t> values(const std::multimap<uint32_t, uint32_t> &m, uint32_t method)
{
    std::vector<uint32_t> out;
    for (auto [at, end] = m.equal_range(method); at != end; ++at)
        out.push_back(at->second);
    return out;
}

TEST_F(DeviceStreams, EachStreamIsBoundAtItsOwnBindAndAMovedOneAlone)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(twoStreams(0x57e2, false), false, why);
    ASSERT_NE(pl, nullptr) << why;
    EXPECT_EQ(pl->streamStride[1], 16u);
    const auto pushed = drawTwice(pl, [&](raw::RenderEncoder &e) {
        e.setBuffer(1, 2, &moved, 0);
    });
    EXPECT_EQ(values(pushed, kLocation0 + 4), std::vector<uint32_t>{uint32_t(positions.va)});
    EXPECT_EQ(values(pushed, kLimit0 + 4), std::vector<uint32_t>{uint32_t(positions.va + positions.length - 1)});
    EXPECT_EQ(values(pushed, kLocation1 + 4), (std::vector<uint32_t>{uint32_t(colours.va), uint32_t(moved.va)}));
    EXPECT_EQ(values(pushed, kLimit1 + 4), (std::vector<uint32_t>{uint32_t(colours.va + colours.length - 1),
                                                                  uint32_t(moved.va + moved.length - 1)}));
    for (const uint32_t format : values(pushed, kFormat1))
        EXPECT_EQ(format, kStreamEnable | 16);
}

TEST_F(DeviceStreams, ASecondStreamOfABindStrideTakesItsBindsStride)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(twoStreams(0x57e3, true), false, why);
    ASSERT_NE(pl, nullptr) << why;
    const auto pushed = drawTwice(pl, [](raw::RenderEncoder &e) {
        e.setVertexStride(2, 20);
    });
    EXPECT_EQ(values(pushed, kLocation1 + 4), std::vector<uint32_t>{uint32_t(colours.va)});
    const std::vector<uint32_t> formats = values(pushed, kFormat1);
    ASSERT_FALSE(formats.empty());
    EXPECT_EQ(formats.back(), kStreamEnable | 20);
}

}  // namespace
