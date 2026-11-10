// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_surfaces.h"
#include <gtest/gtest.h>

TEST(RawSurfaces, PreservesEveryDependencyBeyondTheLegacyLimit)
{
    raw::SurfaceList uses;
    for (uint64_t handle = 1; handle <= 200; ++handle)
        uses.add(handle, false);
    uses.add(3, true);
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 200u);
    EXPECT_EQ(uses.uses()[2].access, uint32_t(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    size_t bytes = 100;
    EXPECT_EQ(uses.legacy(9, bytes), nullptr);
    EXPECT_EQ(bytes, 0u);
}

TEST(RawSurfaces, CapacityFailureSticksUntilANewJob)
{
    raw::SurfaceList uses;
    for (uint64_t handle = 1; handle <= TegraGM20BMaxJobUses; ++handle)
        uses.add(handle, false);
    uses.add(1, true);
    EXPECT_TRUE(uses.valid());
    uses.add(TegraGM20BMaxJobUses + 1, false);
    EXPECT_FALSE(uses.valid());
    uses.add(1, true);
    EXPECT_FALSE(uses.valid());
    uses.clear();
    uses.add(7, false);
    size_t bytes;
    const auto *legacy = uses.legacy(9, bytes);
    ASSERT_NE(legacy, nullptr);
    EXPECT_EQ(legacy->channel, 9u);
    EXPECT_EQ(legacy->count, 1u);
    EXPECT_EQ(legacy->flags, 0u);
    EXPECT_EQ(bytes, offsetof(TegraGM20BAbiExecSurfaces, uses) + sizeof(TegraGM20BAbiSurfaceUse));
}

TEST(RawSurfaces, AddressAndHandleNamespacesStayDistinctAndNeverReachLegacyAbi)
{
    raw::SurfaceList uses;
    uses.add(4096, false);
    uses.addAccess(4096, TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite);
    uses.addAccess(4096, TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead);
    ASSERT_EQ(uses.uses().size(), 2u);
    EXPECT_EQ(uses.uses()[0].access, uint32_t(TegraGM20BSurfaceRead));
    EXPECT_EQ(uses.uses()[1].access,
              uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    size_t bytes;
    EXPECT_EQ(uses.legacy(1, bytes), nullptr);
    uses.clear();
    uses.add(0, true);
    EXPECT_FALSE(uses.valid());
}

TEST(RawSurfaces, BufferRangesFoldThousandsOfInteriorCopiesIntoOneBackingUse)
{
    raw::SurfaceList uses;
    uses.addAddress(0x100000, 0x100000, false);
    for (uint64_t at = 0; at < 10000; ++at)
        uses.addAddress(0x100000 + at * 32, 1, true);
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_EQ(uses.uses()[0].access,
              uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    uses.addAddress(0x200000, 32, false);
    EXPECT_EQ(uses.uses().size(), 2u);
    uses.addAddress(UINT64_MAX - 16, 32, false);
    EXPECT_FALSE(uses.valid());
}
