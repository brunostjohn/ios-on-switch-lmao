// SPDX-License-Identifier: MIT
// A shared MTLHeap's arena purged and made again on gm20b::Device over the kernel model (device_harness.h,
// arena.cpp): the purge frees its BO and gives its write-combined pages back, its GPU and CPU addresses and texture
// headers stay, and the restore imports the same host range again and binds each of its mappings where it was. What the
// heap held comes back zero, as the CPU and the GPU read the fresh pages alike (Metal leaves it undefined). No purge
// while a job submitted may still reach it, an m2v view holds it, or a command buffer is committed.
#include "device_harness.h"
#include "../heap_members.h"
#include <algorithm>
#include <atomic>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint64_t kArenaBytes = 1u << 20;

class DeviceHeapPurge : public harness::DeviceTest {
protected:
    uint32_t arena = 0;
    std::atomic<uint32_t> committed{0};

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createArena(kArenaBytes, false, arena));
    }

    uint64_t bo() const
    {
        return gm20b::DeviceHarness::arena(dev, arena).handle;
    }

    static uint64_t boAt(uint64_t va)
    {
        return kernel_model::model.resolve({va, TegraGM20BSurfaceGpuAddress, 0});
    }

    // A job of one copy whose command buffer used `heap`: what it names
    std::vector<TegraGM20BAbiSurfaceUse> submitUsing(const gm20b::HeapMembers &heap)
    {
        gm20b::Buffer from, to;
        EXPECT_TRUE(dev.createBuffer(64, from) && dev.createBuffer(64, to));
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        cb->internalCompletion();
        cb->useHeap(heap);
        cb->use(from, false);
        cb->use(to, true, false);
        gm20b::BlitEncoder *e = cb->blit();
        e->copyBytes(from.va, to.va, 64);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return kernel_model::model.jobs.back().uses;
    }

    static size_t named(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint64_t handle)
    {
        return size_t(std::count_if(uses.begin(), uses.end(), [&](const TegraGM20BAbiSurfaceUse &u) {
            return u.handle == handle;
        }));
    }

    static bool zero(const uint8_t *bytes, size_t n)
    {
        return std::all_of(bytes, bytes + n, [](uint8_t b) {
            return b == 0;
        });
    }

    void completeEverything()
    {
        kernel_model::Slot *s = liveSlot();
        ASSERT_NE(s, nullptr);
        kernel_model::model.fence(liveChannel() - 1).completed = s->submitted;
    }
};

TEST_F(DeviceHeapPurge, APurgedArenaComesBackZeroAtTheSameAddressesAndHeaders)
{
    ASSERT_TRUE(gm20b::DeviceHarness::arenaWriteCombined(dev, arena));
    gm20b::Buffer b;
    gm20b::Texture pitch, chain;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, pitch));
    gm20b::TextureAsk mips;
    mips.kind = gm20b::TextureAsk::Mips;
    mips.width = mips.height = 64;
    mips.levels = 4;
    mips.format = kRGBA8;
    ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, mips, chain));  // the shared arena's block-linear view
    memset(b.cpu, 0x5A, 4096);
    const uint64_t before = bo();
    const auto pitchHeader = gm20b::DeviceHarness::header(dev, pitch.tic),
               chainHeader = gm20b::DeviceHarness::header(dev, chain.tic);
    uint8_t *const cpu = b.cpu;
    const uint64_t va = b.va, chainVa = chain.va;
    EXPECT_EQ(boAt(va), before);
    EXPECT_EQ(boAt(chainVa), before);

    dev.setArenaVolatile(arena, true);
    dev.purgeArena(arena, committed);
    ASSERT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(kernel_model::model.bos.count(before), 0u);
    EXPECT_EQ(boAt(va), 0u);
    EXPECT_EQ(boAt(chainVa), 0u);
    EXPECT_EQ(dev.arenaBytes(), 0u);
    EXPECT_TRUE(zero(cpu, 4096));  // the host range is fresh pages already, at the same address
    const gm20b::HeapMembers heap(arena, true);
    EXPECT_EQ(named(submitUsing(heap), before), 0u);

    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_FALSE(dev.arenaPurged(arena));
    const uint64_t after = bo();
    ASSERT_NE(after, before);
    const kernel_model::BoState &made = kernel_model::model.bos.at(after);
    EXPECT_TRUE(made.import);
    EXPECT_EQ(made.address, uint64_t(uintptr_t(gm20b::DeviceHarness::arenaPages(dev, arena))));
    EXPECT_EQ(made.bytes, kArenaBytes);
    EXPECT_EQ(boAt(va), after);
    EXPECT_EQ(boAt(pitch.va), after);
    EXPECT_EQ(boAt(chainVa), after);
    EXPECT_EQ(b.cpu, cpu);
    EXPECT_TRUE(zero(cpu, 4096));
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, pitch.tic), pitchHeader);
    EXPECT_EQ(gm20b::DeviceHarness::header(dev, chain.tic), chainHeader);
    EXPECT_EQ(dev.arenaBytes(), kArenaBytes);

    gm20b::Buffer later;
    ASSERT_TRUE(dev.arenaBuffer(arena, 256, later));
    EXPECT_EQ(boAt(later.va), after);
    memset(later.cpu, 0x3C, 256);
    EXPECT_EQ(cpu[later.va - va], 0x3C);
    const auto uses = submitUsing(heap);
    EXPECT_EQ(named(uses, after), 1u);
    EXPECT_EQ(named(uses, before), 0u);
}

TEST_F(DeviceHeapPurge, NoPurgeWhileAJobAViewOrACommittedCommandBufferMayReachIt)
{
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    const gm20b::HeapMembers heap(arena, true);
    kernel_model::model.inFlight = true;
    submitUsing(heap);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a job in flight";
    completeEverything();

    gm20b::DeviceHarness::holdArena(dev, arena, true);
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a lease (a CPU access, or a placement made aliasable)";
    gm20b::DeviceHarness::holdArena(dev, arena, false);

    committed = 1;
    dev.purgeArena(arena, committed);
    EXPECT_FALSE(dev.arenaPurged(arena)) << "a command buffer committed";
    EXPECT_EQ(dev.purgedArenas(), 0u);
    committed = 0;

    dev.purgeArena(arena, committed);
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_EQ(dev.purgedArenas(), 1u);
}

// A private heap's arena is a BO from the kernel's spares, no import of pages the creating thread zero-fills, bound at the
// arena's VAs with no CPU mapping; a purge frees it and a restore binds another at the same VAs (22 Nov: backboardd's 8 MiB
// heaps, restored after each idle give-back, zero-filled their pages on the render thread, 4.4 ms each)
TEST_F(DeviceHeapPurge, APrivateArenaIsASpareBoAtItsVasAndComesBackAsAnother)
{
    const size_t imports = kernel_model::model.imports.size();
    uint32_t priv = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaTexture(priv, 64, 64, kRGBA8, t));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(priv, 4096, b));
    const uint64_t first = gm20b::DeviceHarness::arena(dev, priv).handle;
    ASSERT_NE(first, 0u);
    EXPECT_FALSE(kernel_model::model.bos.at(first).import);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, priv).cpu, nullptr);
    EXPECT_EQ(boAt(t.va), first);
    EXPECT_EQ(boAt(b.va), first);
    EXPECT_EQ(kernel_model::model.imports.size(), imports);

    dev.setArenaVolatile(priv, true);
    dev.purgeArena(priv, committed);
    ASSERT_TRUE(dev.arenaPurged(priv));
    EXPECT_EQ(kernel_model::model.bos.count(first), 0u);
    EXPECT_EQ(boAt(t.va), 0u);

    ASSERT_TRUE(dev.restoreArena(priv));
    const uint64_t again = gm20b::DeviceHarness::arena(dev, priv).handle;
    ASSERT_NE(again, first);
    EXPECT_FALSE(kernel_model::model.bos.at(again).import);
    EXPECT_EQ(boAt(t.va), again);
    EXPECT_EQ(boAt(b.va), again);
    EXPECT_EQ(kernel_model::model.imports.size(), imports);
    dev.arenaRelease(b);
    dev.arenaRelease(t);
    dev.destroyArena(priv);
}

// A private heap's arena is one BO: the idle trim gives none of it back (a shared heap's own pages are what it trims)
TEST_F(DeviceHeapPurge, TheTrimLeavesAPrivateArenaWhole)
{
    uint32_t priv = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, true, priv));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(priv, 4096, b));
    const uint64_t whole = gm20b::DeviceHarness::arena(dev, priv).handle;
    dev.trimArenas(committed);
    EXPECT_EQ(gm20b::DeviceHarness::arena(dev, priv).handle, whole);
    EXPECT_EQ(kernel_model::model.bos.count(whole), 1u);
    dev.arenaRelease(b);
    dev.destroyArena(priv);
}

TEST_F(DeviceHeapPurge, OnlyVolatileArenasGoAtTheDevicesPurge)
{
    uint32_t kept = 0;
    ASSERT_TRUE(dev.createArena(kArenaBytes, false, kept));
    dev.setArenaVolatile(arena, true);
    dev.purgeArenas(committed);
    EXPECT_TRUE(dev.arenaPurged(arena));
    EXPECT_FALSE(dev.arenaPurged(kept));
    dev.setArenaVolatile(arena, false);
    ASSERT_TRUE(dev.restoreArena(arena));
    EXPECT_EQ(dev.purgedArenas(), 0u);
}

}  // namespace
