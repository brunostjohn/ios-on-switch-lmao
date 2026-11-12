// SPDX-License-Identifier: MIT
// An MTLHeap an encoder used (useHeap) on raw::Device (raw_device_harness.h, raw_heap_members.h): a command buffer names it
// once, and its job names the heap's arena as one BO however many resources the arena holds, plus each resource the heap
// made outside it. A shared heap's arena is write-combined and host-written; a private heap's places buffers too.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_heap_members.h"

namespace {

constexpr uint32_t kRGBA8 = 70;

class DeviceHeapUse : public harness::DeviceTest {
protected:
    uint32_t arena = 0;

    void makeArena(bool blockLinear, uint64_t bytes = 32u << 20)
    {
        ASSERT_TRUE(dev.createArena(bytes, blockLinear, arena));
    }

    // One command buffer of one copy that used `heap` `uses` times: its job's uses
    std::vector<TegraGM20BAbiSurfaceUse> submit(const raw::HeapMembers &heap, uint32_t uses = 1)
    {
        raw::Buffer from, to;
        EXPECT_TRUE(dev.createBuffer(64, from) && dev.createBuffer(64, to));
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        cb->internalCompletion();
        for (uint32_t i = 0; i < uses; i++)
            cb->useHeap(heap);
        raw::BlitEncoder *e = cb->blit();
        e->copyBytes(from.va, to.va, 64);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return kernel_model::model.jobs.back().uses;
    }

    size_t named(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint64_t handle, uint32_t *access = nullptr)
    {
        size_t n = 0;
        for (const auto &use : uses)
            if (use.handle == handle) {
                n++;
                if (access)
                    *access = use.access;
            }
        return n;
    }

    void place(uint32_t textures)
    {
        for (uint32_t i = 0; i < textures; i++) {
            raw::Texture t;
            ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, t)) << i;
        }
    }
};

TEST_F(DeviceHeapUse, AHeapsArenaIsOneUseHoweverManyResourcesItHolds)
{
    makeArena(true);
    const raw::HeapMembers heap(arena, false);
    const uint64_t bo = raw::DeviceHarness::arena(dev, arena).handle;
    place(4);
    const auto few = submit(heap, 3);
    place(400);
    const auto many = submit(heap, 3);
    uint32_t access = 0;
    EXPECT_EQ(named(few, bo), 1u);
    EXPECT_EQ(named(many, bo, &access), 1u);
    EXPECT_EQ(access & (TegraGM20BSurfaceWrite | TegraGM20BSurfaceHostWritten), 0u);
    EXPECT_EQ(few.size(), many.size());
}

TEST_F(DeviceHeapUse, AResourceMadeOutsideTheArenaIsAUseUntilItGoes)
{
    makeArena(true);
    raw::HeapMembers heap(arena, false);
    raw::Buffer outside;
    ASSERT_TRUE(dev.createBuffer(4096, outside));
    const uint64_t bo = kernel_model::model.resolve({outside.va, TegraGM20BSurfaceGpuAddress, 0});
    const size_t before = named(submit(heap), bo);
    heap.add({outside.va, outside.length, false});
    EXPECT_EQ(named(submit(heap), bo), before + 1);
    heap.remove(outside.va);
    EXPECT_EQ(named(submit(heap), bo), before);
    EXPECT_EQ(heap.outside(), 0u);
}

TEST_F(DeviceHeapUse, ASharedHeapsArenaIsWriteCombinedAndNamedHostWritten)
{
    makeArena(false);
    EXPECT_TRUE(raw::DeviceHarness::arenaWriteCombined(dev, arena));
    raw::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 256, b));
    EXPECT_TRUE(b.wc);
    const raw::HeapMembers heap(arena, true);
    uint32_t access = 0;
    EXPECT_EQ(named(submit(heap), raw::DeviceHarness::arena(dev, arena).handle, &access), 1u);
    EXPECT_TRUE(access & TegraGM20BSurfaceHostWritten);
}

TEST_F(DeviceHeapUse, APrivateHeapPlacesBuffersInItsPitchView)
{
    makeArena(true);
    EXPECT_FALSE(raw::DeviceHarness::arenaWriteCombined(dev, arena));
    raw::Buffer b;
    raw::Texture t;
    ASSERT_TRUE(dev.arenaBuffer(arena, 1000, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 64, 64, kRGBA8, t));
    EXPECT_TRUE(b.arena);
    EXPECT_FALSE(b.wc);
    EXPECT_NE(b.va - b.span.offset, t.va - t.span.offset);  // the pitch view and the block-linear view of one BO
}

TEST(HeapMembers, ChangeOnlyAsTheHeapMakesAndFreesResources)
{
    raw::HeapMembers heap(3, true);
    EXPECT_EQ(heap.arena(), 3u);
    EXPECT_TRUE(heap.hostVisible());
    const uint64_t first = heap.generation();
    heap.add({0x10000, 256, true});
    heap.add({0x20000, 512, false});
    heap.add({0, 64, false});  // nothing to name
    EXPECT_EQ(heap.outside(), 2u);
    heap.remove(0x10000);
    heap.remove(0x99999);
    EXPECT_EQ(heap.outside(), 1u);
    EXPECT_EQ(heap.generation(), first + 3);
    uint64_t va = 0;
    heap.each([&](const raw::HeapMember &m) {
        va = m.va;
    });
    EXPECT_EQ(va, 0x20000u);
}

}  // namespace
