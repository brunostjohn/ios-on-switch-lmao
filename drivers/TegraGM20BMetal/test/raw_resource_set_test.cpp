// SPDX-License-Identifier: MIT
#include "raw_kernel_model.h"
#include "../raw/runtime/raw_resource_set.h"
#include <gtest/gtest.h>
#include <string>

namespace {

using kernel_model::model;

constexpr uint32_t kEngines = TegraGM20BEngine3D | TegraGM20BEngineCopy;
constexpr uint32_t kRead = TegraGM20BSurfaceRead, kWrite = TegraGM20BSurfaceWrite;

class ResourceSetTest : public testing::Test {
protected:
    raw::Kernel kernel;
    raw::ResourceSet set;
    raw::SetView view;
    raw::Bo root, tickets, ring;
    uint32_t channel = 0;
    const volatile uint32_t *fence = nullptr;

    void SetUp() override
    {
        model = {};
        std::string err;
        ASSERT_TRUE(kernel.open(err)) << err;
        ASSERT_TRUE(kernel.allocControl(0x1000, root, raw::ControlUse::RootTable));
        ASSERT_TRUE(kernel.allocControl(0x1000, tickets, raw::ControlUse::InterlockTickets));
        ASSERT_TRUE(kernel.allocControl(0x10000, ring, raw::ControlUse::Ring));
        ASSERT_TRUE(kernel.channel(kEngines, channel, fence));
    }

    bool job(const std::vector<TegraGM20BAbiSurfaceUse> &uses = {}, int *result = nullptr, bool named = true)
    {
        raw::KernelJob j;
        j.channel = channel;
        j.uses = uses.data();
        j.useCount = uint32_t(uses.size());
        uint32_t seqno = 0;
        return raw::submitInSet(kernel, set, view, named, j, seqno, result);
    }

    const kernel_model::Job &lastJob() const
    {
        return model.jobs.back();
    }

    bool used(uint64_t handle, uint32_t access) const
    {
        for (const auto &use : lastJob().uses)
            if (use.handle == handle && (use.access & access))
                return true;
        return false;
    }
};

// The set's members go with every job that names it, none of them as a use of the job
TEST_F(ResourceSetTest, MembersGoWithTheSetNotAsUses)
{
    ASSERT_TRUE(set.add(kernel, {{&root, kWrite}, {&tickets, kWrite}, {&ring, kRead}}));
    ASSERT_TRUE(job());
    EXPECT_EQ(model.setJobs, 1u);
    EXPECT_EQ(model.addressJobs, 0u);
    EXPECT_TRUE(used(root.handle, kWrite));
    EXPECT_TRUE(used(ring.handle, kRead));
    ASSERT_TRUE(job({{0x5000, kRead, 0}}));
    EXPECT_EQ(model.setJobs, 2u);
    EXPECT_EQ(model.staleSets, 0u);
}

// An import, a cached BO and a BO of another set are refused as members: they stay uses, the rest join
TEST_F(ResourceSetTest, WhatTheKernelRefusesStaysAUse)
{
    raw::Bo cached, imported;
    ASSERT_TRUE(kernel.alloc(0x10000, false, cached));
    static uint8_t pages[0x4000] __attribute__((aligned(0x4000)));
    ASSERT_TRUE(kernel.import(pages, sizeof pages, imported));
    EXPECT_FALSE(set.add(kernel, {{&root, kWrite}, {&cached, kWrite}, {&imported, kRead}}));
    set.view(view);
    EXPECT_TRUE(view.holds(root));
    EXPECT_FALSE(view.holds(cached));
    EXPECT_FALSE(view.holds(imported));
    ASSERT_TRUE(job({{cached.handle, kWrite, 0}, {imported.handle, kRead, 0}}));
    EXPECT_TRUE(used(cached.handle, kWrite));
    raw::ResourceSet other;
    EXPECT_FALSE(other.add(kernel, root, kRead)) << "a member of one set joined a second";
}

// A freed member leaves the view before its VA can go to another BO, and the set is made again without it, so the next job
// names a current generation
TEST_F(ResourceSetTest, AFreedMemberLeavesBeforeItsAddressIsGivenBack)
{
    ASSERT_TRUE(set.add(kernel, {{&root, kWrite}, {&ring, kRead}}));
    set.view(view);
    const uint64_t ringVa = ring.va;
    ASSERT_TRUE(view.holds(ringVa, 0x100));
    set.free(kernel, ring);
    set.view(view);
    EXPECT_FALSE(view.holds(ringVa, 0x100));
    EXPECT_TRUE(view.holds(root));
    ASSERT_TRUE(job());
    EXPECT_EQ(model.staleSets, 0u) << "a free the device made itself sent a job to the kernel with an ended generation";
    EXPECT_FALSE(used(ringVa, kRead));
}

// A channel killed after naming the set ends its generation when it is destroyed; the next job, on the channel made
// again, is refused once, renews the set and runs
TEST_F(ResourceSetTest, AKilledChannelsSetIsMadeAgainForTheNextJob)
{
    ASSERT_TRUE(set.add(kernel, {{&root, kWrite}, {&tickets, kWrite}}));
    ASSERT_TRUE(job());
    model.inFlight = true;
    ASSERT_TRUE(job());
    model.kill(channel, 2);
    EXPECT_EQ(model.undefined.count(tickets.handle), 1u);
    kernel.closeChannel(channel);
    model.inFlight = false;
    ASSERT_TRUE(kernel.channel(kEngines, channel, fence));
    const uint32_t ended = model.sets[0].generation;
    ASSERT_TRUE(job());
    EXPECT_EQ(model.staleSets, 1u);
    EXPECT_EQ(model.sets[0].generation, ended + 1) << "the set was not made again once, for the refused job";
    EXPECT_EQ(model.undefined.count(tickets.handle), 0u);
    ASSERT_TRUE(job());
    EXPECT_EQ(model.staleSets, 1u);
}

// kIOReturnNotReady while the set's generation is current is a stale native view: the job is refused and the set left as
// it is
TEST_F(ResourceSetTest, ANotReadyTheMirrorDoesNotExplainIsTheJobs)
{
    ASSERT_TRUE(set.add(kernel, root, kWrite));
    model.staleViews = 1;
    const uint32_t generation = model.sets[0].generation;
    int result = KERN_SUCCESS;
    EXPECT_FALSE(job({}, &result));
    EXPECT_EQ(result, kIOReturnNotReady);
    EXPECT_EQ(model.sets[0].generation, generation) << "a stale native view made the set again";
    EXPECT_TRUE(job());
}

// A member named as a use is refused by the kernel; a job that does not name the set may not touch one
TEST_F(ResourceSetTest, AMemberIsNeverAUse)
{
    ASSERT_TRUE(set.add(kernel, root, kWrite));
    int result = KERN_SUCCESS;
    EXPECT_FALSE(job({{root.handle, kWrite, 0}}, &result));
    EXPECT_EQ(result, kIOReturnBadArgument);
    EXPECT_FALSE(job({{root.va, kRead | TegraGM20BSurfaceGpuAddress, 0}}, &result, false));
    EXPECT_EQ(result, kIOReturnBadArgument);
}

TEST(SetView, HoldsOnlyRangesInsideOneMember)
{
    raw::SetView view;
    view.members = {{1, 0x10000, 0x2000, kRead}, {2, 0x20000, 0x1000, kWrite}};
    EXPECT_TRUE(view.holds(0x10000, 0x2000));
    EXPECT_TRUE(view.holds(0x11fff, 1));
    EXPECT_FALSE(view.holds(0x11fff, 2));
    EXPECT_FALSE(view.holds(0xffff, 1));
    EXPECT_FALSE(view.holds(0x12000, 1));
    EXPECT_TRUE(view.holds(0x20800, 0x800));
    EXPECT_FALSE(view.holds(0x21000, 1));
}

}  // namespace
