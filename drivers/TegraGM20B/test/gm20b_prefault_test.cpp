// SPDX-License-Identifier: MIT
// TegraGM20BSurfacePrefault.h over a mock create ABI (two create methods, the ID at 0x18 of their structure output, as
// a kind's table would give them): which methods' outputs name a new surface, and which surfaces are faulted in.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../TegraGM20BSurfacePrefault.h"

namespace
{
constexpr TegraGM20BSurfaceCreateAbi kMock = {{0, 6, ~0u, ~0u}, 0x18};

std::vector<uint8_t> output(uint32_t bytes, uint32_t id)
{
	std::vector<uint8_t> out(bytes, 0xee);
	if (bytes >= 0x18 + sizeof(id))
		memcpy(out.data() + 0x18, &id, sizeof(id));
	return out;
}
}  // namespace

TEST(GM20BPrefault, ACreatesOutputNamesTheNewSurface)
{
	const std::vector<uint8_t> out = output(0x4c8, 0x1234);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 0, out.data(), out.size()), 0x1234u);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 6, out.data(), out.size()), 0x1234u);
}

TEST(GM20BPrefault, OtherMethodsAndShortOutputsNameNone)
{
	const std::vector<uint8_t> out = output(0x4c8, 0x1234);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 1, out.data(), out.size()), 0u);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 0, nullptr, 0x4c8), 0u);
	const std::vector<uint8_t> shortOut = output(0x1b, 0);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 0, shortOut.data(), shortOut.size()), 0u);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 0, out.data(), 0x10), 0u);
	const std::vector<uint8_t> exact = output(0x1c, 7);
	EXPECT_EQ(tegra_gm20b_created_surface(kMock, 0, exact.data(), exact.size()), 7u);
}

// The table without create methods (every entry ~0u): no method's output names a surface, whatever its selector.
TEST(GM20BPrefault, AnEmptyTableMatchesNothing)
{
	constexpr TegraGM20BSurfaceCreateAbi empty = {{~0u, ~0u, ~0u, ~0u}, 0};
	const std::vector<uint8_t> out = output(0x4c8, 0x1234);
	for (uint32_t selector : {0u, 1u, 6u, 100u})
		EXPECT_EQ(tegra_gm20b_created_surface(empty, selector, out.data(), out.size()), 0u);
}

TEST(GM20BPrefault, OnlyLargeSurfacesAtNormalPressure)
{
	EXPECT_TRUE(tegra_gm20b_prefault_wanted(1280 * 720 * 4, 0));
	EXPECT_TRUE(tegra_gm20b_prefault_wanted(kTegraGM20BPrefaultMinBytes, 0));
	EXPECT_FALSE(tegra_gm20b_prefault_wanted(kTegraGM20BPrefaultMinBytes - 1, 0));
	for (int level : {1, 2, 4})
		EXPECT_FALSE(tegra_gm20b_prefault_wanted(1280 * 720 * 4, level));
}

// The reply fill every create and lookup ends in: word 0 the client's CPU address, the ID at 0x18
TEST(GM20BPrefault, AReplyThatMapsTheSurfaceNamesIt)
{
	std::vector<uint8_t> reply = output(0xc68, 0x77);
	const uint64_t address = 0x1'2345'0000;
	memcpy(reply.data(), &address, sizeof(address));
	EXPECT_EQ(tegra_gm20b_mapped_surface(0xc68, 0x18, reply.data(), reply.size()), 0x77u);
	EXPECT_EQ(tegra_gm20b_mapped_surface(0xc68, 0x18, reply.data(), 0xc60), 0u);
	EXPECT_EQ(tegra_gm20b_mapped_surface(0, 0x18, reply.data(), 0), 0u);
	EXPECT_EQ(tegra_gm20b_mapped_surface(0xc68, 0x18, nullptr, 0xc68), 0u);
	const uint64_t none = 0;
	memcpy(reply.data(), &none, sizeof(none));
	EXPECT_EQ(tegra_gm20b_mapped_surface(0xc68, 0x18, reply.data(), reply.size()), 0u);
}
