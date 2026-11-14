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

// Boot 21: each new 1280x720 surface Core Animation made on a close frame was cleaned on its render thread at its first
// import (`imp 1 1.8ms ... clean 3600 KiB 1.6 ms`), its pages already faulted in by the prefault. The prefault cleans them
// and keeps them wired; the first import of a surface nothing wrote cleans nothing.
TEST(GM20BPrefault, AFreshSurfaceCleanedAtCreationIsNotCleanedAtImport)
{
	EXPECT_FALSE(tegra_gm20b_import_cleans(false, true, true));
	EXPECT_TRUE(tegra_gm20b_import_cleans(false, false, true)) << "the import came first";
	EXPECT_TRUE(tegra_gm20b_import_cleans(false, true, false)) << "written, or imported before";
	EXPECT_FALSE(tegra_gm20b_import_cleans(true, false, false)) << "another import cleaned it";
}

TEST(GM20BPrefault, HeldWiringsGoToTheirImportOrBackOldestFirst)
{
	int memory[kTegraGM20BPrefaultHeld + 1];
	TegraGM20BPrefaultHeld<int> held;
	EXPECT_EQ(held.next(), 0u);
	for (uint32_t i = 0; i < kTegraGM20BPrefaultHeld; ++i)
		EXPECT_EQ(held.hold(&memory[i], 100 + i), nullptr);
	EXPECT_EQ(held.next(), 100u);
	EXPECT_EQ(held.hold(&memory[kTegraGM20BPrefaultHeld], 200), &memory[0])
		<< "a ninth pushes the oldest out";
	EXPECT_EQ(held.take(&memory[0]), nullptr);
	EXPECT_EQ(held.take(&memory[3]), &memory[3]);
	EXPECT_EQ(held.take(&memory[3]), nullptr) << "taken once";
	EXPECT_EQ(held.expire(101, false), &memory[1]);
	EXPECT_EQ(held.expire(101, false), nullptr);
	EXPECT_EQ(held.next(), 102u);
	unsigned dropped = 0;
	while (held.expire(0, true))
		++dropped;
	EXPECT_EQ(dropped, kTegraGM20BPrefaultHeld - 2) << "memory pressure gives every one back";
	EXPECT_EQ(held.next(), 0u);
}
