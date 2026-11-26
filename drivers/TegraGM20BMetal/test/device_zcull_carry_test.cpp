// SPDX-License-Identifier: MIT
// A zcull region carried from one job of a Metal command buffer to its next range's (zcull_planes.h ZcullCarry), on
// gm20b::Device over the kernel model with the X1's zcull layout: the storing job's STORE_ZCULL is a store only where a
// pass loads the region, the next job loads it from the same slot, and a job submitted in between leaves it nothing.
#include "device_forms.h"
#include "TegraGM20BZcullABI.h"

namespace {

constexpr uint32_t kLoadZcullMethod = 0x1500, kStoreZcullMethod = 0x1464, kStorageA = 0x07e8;
constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

class DeviceZcullCarry : public DevicePassForms {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, colour, false, true));
        ASSERT_TRUE(dev.createOwnedZeta(32, 32, kDepth32Float, 1, true, zeta));
    }

    gm20b::PassDesc depthPass(bool clears)
    {
        gm20b::PassDesc p;
        p.color[0] = kept(colour, gm20b::ColorAttachment::Load);
        p.count = 1;
        p.depthStencil = {&zeta, clears, false, 1.0f, 0};
        return p;
    }

    // One job of the passes `clears` names in order (true: clears the depth, false: loads it), resuming what was parked
    // for (`resumeTo`, kRange) and keeping regions `kept` for (`carryTo`, kRange): its methods, empty where refused
    std::vector<Method> job(std::initializer_list<bool> clears, const void *resumeTo = nullptr,
                            const void *carryTo = nullptr, uint32_t kept = 0)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        const size_t first = kernel_model::model.jobs.size();
        if (resumeTo)
            cb->resumeZcull(resumeTo, kRange);
        for (const bool clear : clears) {
            const char *rule = nullptr;
            gm20b::RenderEncoder *e = cb->render(depthPass(clear), &rule);
            if (!e) {
                cb->abandon();
                return {};
            }
            e->end();
        }
        cb->carryZcull(carryTo, kRange, kept);
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return {};
        }
        return submitted(first);
    }

    static constexpr uint64_t kRange = 40;
    gm20b::Texture colour, zeta;
    const int commandBuffer = 0;
};

size_t unstoredWords(size_t first)
{
    size_t n = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++)
        n += size_t(std::count(kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end(),
                               gm20b::kZcullUnstoredWord));
    return n;
}

// A region no pass loads is never stored: its pass's STORE_ZCULL word stays a NO_OPERATION
TEST_F(DeviceZcullCarry, ARegionNoPassLoadsIsNotStored)
{
    const size_t first = kernel_model::model.jobs.size();
    const auto ms = job({true});
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(values(ms, kStoreZcullMethod).size(), 0u);
    EXPECT_EQ(unstoredWords(first), 1u);
    EXPECT_EQ(dev.stats.zcullUnstored.load(), 1u);
    EXPECT_EQ(dev.stats.zcullStores.load(), 0u);
}

// A later pass of the same job that loads the depth makes the earlier pass's word a store, and loads from its slot
TEST_F(DeviceZcullCarry, APassOfTheSameJobThatLoadsTheRegionMakesItAStore)
{
    const auto ms = job({true, false});
    ASSERT_FALSE(ms.empty());
    EXPECT_EQ(values(ms, kStoreZcullMethod).size(), 1u);
    EXPECT_EQ(values(ms, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(dev.stats.zcullLoads.load(), 1u);
    EXPECT_EQ(dev.stats.zcullCarried.load(), 0u);
}

// The region the storing job keeps for its command buffer's next range is stored, and that range's job loads it from
// the same storage
TEST_F(DeviceZcullCarry, TheNextRangesJobLoadsTheRegionTheJobBeforeItKept)
{
    const auto stored = job({true}, nullptr, &commandBuffer, 1);
    ASSERT_FALSE(stored.empty());
    EXPECT_EQ(values(stored, kStoreZcullMethod).size(), 1u);
    const uint64_t storage = addressOf(stored, kStorageA);
    ASSERT_NE(storage, 0u);
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(addressOf(loaded, kStorageA), storage);
    EXPECT_EQ(dev.stats.zcullCarried.load(), 1u);
}

// A job submitted between the storing range and the next leaves the next range no region
TEST_F(DeviceZcullCarry, AJobSubmittedInBetweenLeavesTheNextRangeNoRegion)
{
    ASSERT_FALSE(job({true}, nullptr, &commandBuffer, 1).empty());
    const int other = 0;
    ASSERT_FALSE(job({true}, nullptr, &other, 0).empty());
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 0u);
    EXPECT_EQ(dev.stats.zcullCarriesLost.load(), 1u);
}

// A job encoded and abandoned in between submits nothing: the next range still loads the region, from a slot the
// abandoned job could not take
TEST_F(DeviceZcullCarry, AnAbandonedJobInBetweenTakesNotTheCarriedSlot)
{
    const auto stored = job({true}, nullptr, &commandBuffer, 1);
    ASSERT_FALSE(stored.empty());
    const uint64_t storage = addressOf(stored, kStorageA);
    gm20b::CommandBuffer *between = dev.begin();
    ASSERT_NE(between, nullptr);
    const char *rule = nullptr;
    gm20b::RenderEncoder *e = between->render(depthPass(true), &rule);
    ASSERT_NE(e, nullptr) << (rule ? rule : "");
    e->end();
    between->abandon();
    const auto loaded = job({false}, &commandBuffer);
    ASSERT_FALSE(loaded.empty());
    EXPECT_EQ(values(loaded, kLoadZcullMethod).size(), 1u);
    EXPECT_EQ(addressOf(loaded, kStorageA), storage);
}

}  // namespace
