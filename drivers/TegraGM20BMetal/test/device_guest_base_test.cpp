// SPDX-License-Identifier: MIT
// A state's SlotGuestBase holes (Pipeline::guestBases: m2v's Metal address of a buffer slot its programs read, by a pointer
// compare, ptrtoint or a stored pointer) on gm20b::Device over the kernel model: each draw writes the bound slot's address
// into the hole's push words, a set*Bytes binding's being its upload copy, which holds the bytes; a buffer's is its
// address plus the bind's offset.
#include "device_harness.h"
#include "../record.h"
#include <cstring>
#include <string>

namespace {

class DeviceGuestBase : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(32, 32, 70, target));
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = "vs-guest";
        r.programs[1].id = "fs-guest";
        r.pipeline.keyHash = 0x6b05;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
        pl->guestBases[0] = {0, 5, 3};  // the vertex stage's buffer 5, push words 2 and 3
    }

    // One command buffer of one pass and one draw, its vertex buffer 5 bound by `bind`: the address the root table's push
    // words 2 and 3 hold after it
    template <typename Bind> uint64_t drawn(Bind &&bind)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return 0;
        gm20b::PassDesc desc;
        desc.color[0].texture = &target;
        desc.color[0].load = gm20b::ColorAttachment::Clear;
        desc.count = 1;
        gm20b::RenderEncoder *e = cb->render(desc);
        EXPECT_NE(e, nullptr);
        if (!e)
            return 0;
        e->setPipeline(pl);
        bind(*e);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(3, 0, 3, 1, 0, &why)) << (why ? why : "");
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return uint64_t(gm20b::DeviceHarness::rootPush(dev, 3)) << 32 | gm20b::DeviceHarness::rootPush(dev, 2);
    }
};

TEST_F(DeviceGuestBase, ADrawWritesASetBytesSlotsUploadCopyIntoItsHole)
{
    const uint32_t first[4] = {0x11, 0x22, 0x33, 0x44}, second[4] = {0x55, 0x66, 0x77, 0x88};
    for (const uint32_t *bytes : {first, second}) {
        const uint64_t va = drawn([&](gm20b::RenderEncoder &e) {
            e.setBytes(1, 5, bytes, 16);
        });
        ASSERT_NE(va, 0u);
        const uint8_t *copy = gm20b::DeviceHarness::upload(dev, va, 16);
        ASSERT_NE(copy, nullptr) << "the address is not in the upload ring";
        EXPECT_EQ(memcmp(copy, bytes, 16), 0);
    }
}

TEST_F(DeviceGuestBase, ADrawWritesABuffersAddressAndOffsetIntoItsHole)
{
    gm20b::Buffer buffer;
    ASSERT_TRUE(dev.createBuffer(256, buffer, true));
    const uint64_t va = drawn([&](gm20b::RenderEncoder &e) {
        e.setBuffer(1, 5, &buffer, 64);
    });
    EXPECT_EQ(va, buffer.va + 64);
    dev.destroyBuffer(buffer);
}

}  // namespace
