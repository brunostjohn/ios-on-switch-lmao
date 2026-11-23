// SPDX-License-Identifier: MIT
#include "device_harness.h"

namespace {

using kernel_model::model;

constexpr uint32_t kBgra8 = 80, kRgba16Float = 115;

class ChunkMemory : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::RenderServer)) << err;
        importsAtOpen = model.imports.size();
    }

    size_t importsSinceOpen() const
    {
        return model.imports.size() - importsAtOpen;
    }

    size_t importsAtOpen = 0;
};

// A render server's chunks (pool, block-linear) are cached BoAllocs, which the kernel serves from its zeroed spares: no
// import of anonymous pages, which the CPU zero-fills on the creating thread (boot 21: 9 ms per 16 MiB chunk)
TEST_F(ChunkMemory, TargetsOfAScaleFrameImportNoAnonymousPages)
{
    gm20b::Texture screen, scaled, blur;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, screen, false, true));
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, scaled, false, true));
    ASSERT_TRUE(dev.createTexture(1344, 768, kRgba16Float, blur, false, true));
    gm20b::Buffer shared;
    ASSERT_TRUE(dev.createBuffer(64 << 10, shared));
    EXPECT_EQ(importsSinceOpen(), 0u);
    ASSERT_NE(scaled.cpu, nullptr);
    scaled.cpu[0] = 1;
    dev.destroyTexture(scaled);
    gm20b::Texture again;
    ASSERT_TRUE(dev.createTexture(2048, 1288, kBgra8, again, false, true));
    EXPECT_EQ(importsSinceOpen(), 0u);
    dev.destroyTexture(again);
    dev.destroyTexture(blur);
    dev.destroyTexture(screen);
}

TEST_F(ChunkMemory, AChunkGivenBackIsFreedNotLeftMapped)
{
    gm20b::Texture big;
    ASSERT_TRUE(dev.createTexture(2048, 2160, kBgra8, big, false, true));
    const size_t live = model.bos.size();
    dev.destroyTexture(big);
    dev.trim();
    EXPECT_LT(model.bos.size(), live);
}

// A launching app's chunks are the kernel's spares too (22 Nov: Photos, Maps and MercuryPosterExtension zero-filled 4-16
// MiB chunks of their own pages on the creating thread, 4.3-9.1 ms each): no import of the app's anonymous pages
TEST(ClientChunkMemory, AClientsChunksAreCachedBoAllocsNotImportsOfItsPages)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    const size_t before = model.imports.size();
    gm20b::Texture tiled, pitch, launch;
    ASSERT_TRUE(dev.createTexture(1280, 720, kBgra8, tiled, false, true));
    ASSERT_TRUE(dev.createTexture(256, 256, kBgra8, pitch, false, false));
    ASSERT_TRUE(dev.createTexture(1792, 1472, kRgba16Float, launch, false, true));
    gm20b::Buffer pooled;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled));
    EXPECT_EQ(model.imports.size(), before);
    dev.destroyBuffer(pooled);
    dev.destroyTexture(launch);
    dev.destroyTexture(pitch);
    dev.destroyTexture(tiled);
}

// A no-copy buffer over a buffer's own contents would import a cached BO's pages from past its first page, which the
// kernel hands to another process's BoAlloc once the chunk goes: refused with its line, nothing imported
TEST(ClientChunkMemory, ANoCopyBufferOverTheDevicesOwnMemoryIsRefused)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    gm20b::Buffer pooled;
    ASSERT_TRUE(dev.createBuffer(64 << 10, pooled, false));
    ASSERT_NE(pooled.cpu, nullptr);
    const size_t before = model.imports.size();
    gm20b::Buffer over;
    bool tiled = false;
    EXPECT_FALSE(dev.createBufferNoCopy(pooled.cpu + 16384, 16384, over, tiled));
    EXPECT_FALSE(tiled);
    EXPECT_EQ(model.imports.size(), before);
    dev.destroyBuffer(pooled);
}

// The memory line's split: a chunk a few small buffers keep is pinned until a quarter of it is used; a shared heap (the
// task's pages) and a private one (a kernel BO no task maps, outside the footprint) count apart
TEST(ClientChunkMemory, TheHeldSplitNamesAPinnedChunkAndEachHeapApart)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string err;
    ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    constexpr uint64_t kSmall = 64 << 10;
    std::vector<gm20b::Buffer> buffers(1);
    ASSERT_TRUE(dev.createBuffer(kSmall, buffers[0]));
    gm20b::Device::Held h;
    ASSERT_TRUE(dev.held(h, false));
    ASSERT_EQ(h.chunkCount, 1u);
    EXPECT_EQ(h.pinnedCount, 1u) << "one 64 KiB buffer in a " << (h.chunks >> 10) << " KiB chunk";
    EXPECT_EQ(h.pinned, h.chunks);
    const uint64_t chunk = h.chunks;
    while (buffers.size() * kSmall * 4 <= chunk) {
        buffers.emplace_back();
        ASSERT_TRUE(dev.createBuffer(kSmall, buffers.back()));
    }
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.chunks, chunk) << "the buffers share the one chunk";
    EXPECT_EQ(h.pinnedCount, 0u) << "more than a quarter of it used";
    EXPECT_EQ(h.heaps, 0u);
    uint32_t shared = 0, gpuOnly = 0;
    ASSERT_TRUE(dev.createArena(4 << 20, false, shared));
    ASSERT_TRUE(dev.createArena(2 << 20, true, gpuOnly));
    ASSERT_TRUE(dev.held(h, false));
    EXPECT_EQ(h.heaps, uint64_t(4 << 20));
    EXPECT_EQ(h.gpuHeaps, uint64_t(2 << 20));
    EXPECT_EQ(h.chunks, chunk) << "a heap is not a chunk";
    dev.destroyArena(gpuOnly);
    dev.destroyArena(shared);
    for (gm20b::Buffer &b : buffers)
        dev.destroyBuffer(b);
}

}  // namespace
