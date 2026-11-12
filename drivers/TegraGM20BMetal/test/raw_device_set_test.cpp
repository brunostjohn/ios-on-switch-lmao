// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"

namespace {

using kernel_model::model;

class DeviceSet : public harness::DeviceTest {
protected:
    // A write-combined buffer in a chunk made for it, freed again: the chunk is the device's empty spare, a member the
    // CPU lease of the buffer's creation took out of the kernel's set while the device's list still names it
    uint64_t spareWcChunk()
    {
        const uint64_t chunk = model.nextHandle;
        raw::Buffer wc;
        if (!dev.createBuffer(4096, wc, true) || !wc.wc)
            return 0;
        dev.destroyBuffer(wc);
        return chunk;
    }

    std::string refusal() const
    {
        return "the kernel refused the set for member BO " + std::to_string(model.refusedHandle) +
               (freed(model.refusedHandle) ? ", which the device had freed" : "");
    }
};

// Boot 18 (T-v4b, SwitchHWTest 20:27:37-20:27:47): the app's device runs, a second raw device opens and closes in the
// process on a connection of its own (HW Test's bench), the device's idle give-back takes its pool to no chunks and
// frees its spare write-combined chunk, and the next allocation makes the set again. Every command buffer after it runs
// raw, on the set, and the kernel refuses no registration.
TEST_F(DeviceSet, TheIdleGiveBackAfterABenchLeavesASetTheKernelTakes)
{
    raw::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u);
    const uint64_t spare = spareWcChunk();
    ASSERT_NE(spare, 0u);
    dev.destroyBuffer(a);
    dev.destroyBuffer(b);
    {
        raw::Device bench;
        std::string err;
        ASSERT_TRUE(bench.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        raw::Buffer x, y;
        ASSERT_TRUE(bench.createBuffer(4096, x));
        ASSERT_TRUE(bench.createBuffer(4096, y));
        raw::CommandBuffer *cb = bench.begin();
        raw::BlitEncoder *e = cb ? cb->blit() : nullptr;
        ASSERT_NE(e, nullptr);
        e->copyBytes(x.va, y.va, 4096);
        e->end();
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
        ASSERT_TRUE(bench.wait(seqno));
    }
    ASSERT_TRUE(model.others.empty()) << "the bench's connection stayed open";
    dev.trim();
    EXPECT_EQ(dev.stats.poolChunks.load(), 0u);
    EXPECT_TRUE(freed(spare)) << "the give-back kept the spare write-combined chunk";
    const unsigned jobs = model.setJobs;
    raw::Buffer c, d;
    ASSERT_TRUE(dev.createBuffer(4096, c));
    ASSERT_TRUE(dev.createBuffer(4096, d));
    for (int i = 0; i < 4; i++)
        EXPECT_NE(copy(c, d, 4096), 0u) << "command buffer " << i << " was refused: " << refusal();
    EXPECT_EQ(model.setJobs, jobs + 4) << "command buffers left the set";
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

// The same without the bench: the give-back's free of the spare chunk alone is enough
TEST_F(DeviceSet, AFreedSpareChunkLeavesTheSet)
{
    const uint64_t spare = spareWcChunk();
    ASSERT_NE(spare, 0u);
    dev.trim();
    ASSERT_TRUE(freed(spare));
    raw::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    EXPECT_NE(copy(a, b, 4096), 0u) << refusal();
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

// A second raw device's life on its own connection leaves the first device's set and channel as they were
TEST_F(DeviceSet, ABenchOnItsOwnConnectionLeavesTheSetAlone)
{
    raw::Buffer a, b;
    ASSERT_TRUE(dev.createBuffer(4096, a));
    ASSERT_TRUE(dev.createBuffer(4096, b));
    ASSERT_NE(copy(a, b, 4096), 0u);
    const unsigned stale = model.staleSets;
    {
        raw::Device bench;
        std::string err;
        ASSERT_TRUE(bench.open(harness::emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        raw::Buffer x;
        ASSERT_TRUE(bench.createBuffer(4096, x));
        bench.destroyBuffer(x);
        bench.trim();
    }
    EXPECT_NE(copy(b, a, 4096), 0u) << refusal();
    EXPECT_EQ(model.staleSets, stale);
    EXPECT_EQ(model.setRefusals, 0u) << refusal();
}

}  // namespace
