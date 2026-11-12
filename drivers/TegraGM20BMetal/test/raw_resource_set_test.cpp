// SPDX-License-Identifier: MIT
#include "raw_kernel_model.h"
#include "../raw/runtime/raw_resource_set.h"
#include <gtest/gtest.h>
#include <cstdio>
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

// The device's own imported pages and cached BOs join; a BO of another set stays out, by name
TEST_F(ResourceSetTest, ImportsAndCachedBosJoinAndAnotherSetsBoStaysOut)
{
    raw::Bo cached, imported;
    ASSERT_TRUE(kernel.alloc(0x10000, false, cached));
    static uint8_t pages[0x4000] __attribute__((aligned(0x4000)));
    ASSERT_TRUE(kernel.import(pages, sizeof pages, imported));
    EXPECT_TRUE(set.add(kernel, {{&root, kWrite}, {&cached, kWrite}, {&imported, kRead}}));
    set.view(view);
    EXPECT_TRUE(view.holds(root));
    EXPECT_TRUE(view.holds(cached));
    EXPECT_TRUE(view.holds(imported));
    ASSERT_TRUE(job());
    EXPECT_TRUE(used(cached.handle, kWrite));
    EXPECT_TRUE(used(imported.handle, kRead));
    raw::ResourceSet other;
    EXPECT_FALSE(other.add(kernel, root, kRead)) << "a member of one set joined a second";
}

// A CPU lease takes a member out for good: the next job is refused once, the set is made again without it, and the uses
// the command buffer recorded of it become uses of the job
TEST_F(ResourceSetTest, ACpuLeasedMembersRecordedUsesBecomeTheJobsUses)
{
    ASSERT_TRUE(set.add(kernel, {{&root, kWrite}, {&ring, kWrite}}));
    set.view(view);
    const uint32_t removals = view.removals;
    const std::vector<raw::SetUse> recorded = {{ring.handle, kWrite}, {root.handle, kWrite}};
    ASSERT_TRUE(kernel.cpuAccess(ring, 0, 0x1000, TegraGM20BCpuRead, true));
    raw::SurfaceList uses;
    raw::KernelJob j;
    j.channel = channel;
    uint32_t seqno = 0;
    ASSERT_TRUE(raw::submitInSet(kernel, set, view, true, j, seqno, nullptr, &uses, &recorded, removals));
    EXPECT_EQ(model.staleSets, 1u);
    EXPECT_FALSE(view.holds(ring));
    EXPECT_TRUE(used(ring.handle, kWrite)) << "a use of the member the CPU took out went unnamed";
    ASSERT_EQ(uses.uses().size(), 1u) << "a member still in the set was named as a use";
}

// An always-used BO the kernel keeps out is a use of every job
TEST_F(ResourceSetTest, AnAlwaysUsedBoOutsideIsAUseOfEveryJob)
{
    ASSERT_TRUE(set.add(kernel, root, kWrite, true));
    ASSERT_TRUE(kernel.cpuAccess(root, 0, 0x1000, TegraGM20BCpuRead, true));
    for (int i = 0; i < 2; ++i) {
        raw::SurfaceList uses;
        raw::KernelJob j;
        j.channel = channel;
        uint32_t seqno = 0;
        ASSERT_TRUE(raw::submitInSet(kernel, set, view, true, j, seqno, nullptr, &uses));
        EXPECT_TRUE(used(root.handle, kWrite));
    }
    EXPECT_EQ(view.outside.size(), 1u);
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

// A compositor frame as SubmitAddress named it, 132 uses: 9 control BOs, 93 ranges of the device's own chunks (block-linear
// imports, write-combined and cached BOs), 24 IOSurface layers and 6 native views. With the set, the job names the 30
// shared ones and nothing of the device's own memory.
TEST_F(ResourceSetTest, ACompositorFramesJobNamesOnlyItsSharedUses)
{
    static uint8_t tiled[2][0x40000] __attribute__((aligned(0x4000)));
    static uint8_t layers[24][0x4000] __attribute__((aligned(0x4000)));
    std::vector<raw::Bo> control(6), chunks(5), surfaces(24), views(6);
    for (auto &bo : control)
        ASSERT_TRUE(kernel.allocControl(0x1000, bo, raw::ControlUse::TextureHeaders));
    ASSERT_TRUE(kernel.import(tiled[0], sizeof tiled[0], chunks[0]));
    ASSERT_TRUE(kernel.import(tiled[1], sizeof tiled[1], chunks[1]));
    ASSERT_TRUE(kernel.alloc(0x40000, true, chunks[2]));
    ASSERT_TRUE(kernel.alloc(0x40000, true, chunks[3]));
    ASSERT_TRUE(kernel.alloc(0x40000, false, chunks[4]));
    for (int i = 0; i < 24; ++i)
        ASSERT_TRUE(kernel.import(layers[i], sizeof layers[i], surfaces[i]));
    for (auto &bo : views)
        ASSERT_TRUE(kernel.alloc(0x4000, true, bo));
    ASSERT_TRUE(set.add(kernel, {{&root, kWrite}, {&tickets, kWrite}, {&ring, kRead}}, true));
    for (auto &bo : control)
        ASSERT_TRUE(set.add(kernel, bo, kRead, true));
    for (auto &bo : chunks)
        ASSERT_TRUE(set.add(kernel, bo, kWrite));
    set.view(view);
    raw::SurfaceList before, after;
    std::vector<raw::SetUse> recorded;
    for (const raw::Bo *bo : {&root, &tickets, &ring})
        before.add(bo->handle, true);
    for (const auto &bo : control)
        before.add(bo.handle, false);
    for (uint32_t i = 0; i < 93; ++i) {
        const raw::Bo &chunk = chunks[i % chunks.size()];
        const uint64_t va = chunk.va + (i / chunks.size()) * 0x2000;
        before.addAddress(va, 0x1000, i % 3 == 0, TegraGM20BSurfaceRange);
        if (const raw::SetMember *m = view.find(va, 0x1000))
            recorded.push_back({m->handle, i % 3 == 0 ? kWrite : kRead});
        else
            after.addAddress(va, 0x1000, i % 3 == 0, TegraGM20BSurfaceRange);
    }
    for (const auto &bo : surfaces) {
        before.add(bo.handle, &bo == &surfaces[0]);
        after.add(bo.handle, &bo == &surfaces[0]);
    }
    for (const auto &bo : views) {
        before.add(bo.handle, false);
        after.add(bo.handle, false);
    }
    raw::KernelJob j;
    j.channel = channel;
    uint32_t seqno = 0;
    ASSERT_TRUE(raw::submitInSet(kernel, set, view, true, j, seqno, nullptr, &after, &recorded, view.removals));
    EXPECT_EQ(before.uses().size(), 132u);
    EXPECT_EQ(after.uses().size(), 30u) << "the job named more than its IOSurfaces and native views";
    EXPECT_EQ(model.setJobs, 1u);
    printf("compositor frame: %zu uses by SubmitAddress, %zu beside the set by SubmitJob, %zu recorded member uses\n",
           before.uses().size(), after.uses().size(), recorded.size());
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
