// SPDX-License-Identifier: MIT
#include <map>
#include <gtest/gtest.h>
#include "gm20b_surface_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "drivers/TegraGM20B/SwitchGpuSurfaceGeometry.h"
#include "drivers/TegraGM20B/TegraGM20BViews.h"

namespace {
struct Surface : OSObject {
	IOMemoryDescriptor memory;
	SwitchGpuSurfaceGeometry geometry = {};
	Surface(uint64_t bytes, uint64_t first)
	{
		memory.bytes = bytes;
		for (uint64_t offset = 0; offset < ((bytes + 16383) & ~uint64_t(16383)); offset += 4096)
			memory.pages.push_back(first + offset);
		geometry = {64, 64, 512, bytes, 8, 1, 1, 0, false};
	}
};
std::map<uint32_t, Surface *> visible;
std::map<uint32_t, switch_gpu_backing *> hints;
std::map<uint64_t, switch_gpu_backing *> allocations;
}

struct switch_gpu_backing {
	uint32_t refs = 1;
	std::vector<uint64_t> pages;
};

extern "C" {
OSObject *tegra_gm20b_ios_lookup(IOService *, uint32_t id, task_t)
{
	auto *surface = visible[id];
	if (surface) surface->retain();
	return surface;
}
IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *s) { return &static_cast<Surface *>(s)->memory; }
uint64_t tegra_vic_ios_alloc_size(OSObject *s) { return static_cast<Surface *>(s)->geometry.bytes; }
uint64_t tegra_vic_ios_width(OSObject *s) { return static_cast<Surface *>(s)->geometry.width; }
uint64_t tegra_vic_ios_height(OSObject *s) { return static_cast<Surface *>(s)->geometry.height; }
uint64_t tegra_vic_ios_bytes_per_row(OSObject *s) { return static_cast<Surface *>(s)->geometry.pitch; }
uint32_t tegra_vic_ios_plane_count(OSObject *s) { return static_cast<Surface *>(s)->geometry.planes; }
uint32_t tegra_gm20b_ios_bpe(OSObject *s) { return static_cast<Surface *>(s)->geometry.bpp; }
uint32_t tegra_gm20b_ios_element_width(OSObject *s) { return static_cast<Surface *>(s)->geometry.element_width; }
uint32_t tegra_gm20b_ios_element_height(OSObject *s) { return static_cast<Surface *>(s)->geometry.element_height; }
switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t id)
{
	auto *backing = hints[id];
	if (backing) ++backing->refs;
	return backing;
}
switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t pa)
{
	auto *backing = allocations[pa];
	if (backing) ++backing->refs;
	return backing;
}
void switch_gpu_backing_release(switch_gpu_backing *backing) { --backing->refs; }
void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
	*info = {};
	info->pages = backing->pages.data();
	info->page_count = uint32_t(backing->pages.size());
}
}

namespace {
class SurfaceClient : public testing::Test {
protected:
	void SetUp() override { visible.clear(); hints.clear(); allocations.clear(); }
};

TEST_F(SurfaceClient, NativeTailAttestationNeverQueriesBeyondTheDescriptor)
{
	for (uint64_t bytes : {uint64_t(1344 * 77), uint64_t(576 * 72), uint64_t(1088 * 136)}) {
		Surface surface(bytes, 0x80000000);
		visible[4] = &surface;
		EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, surface.memory.pages.data(),
			uint32_t(surface.memory.pages.size())), 1) << bytes;
		EXPECT_EQ(surface.memory.outside, 0u);
		EXPECT_EQ(surface.memory.prepared, surface.memory.completed);
		EXPECT_EQ(surface.refs, 1u);
	}
}

TEST_F(SurfaceClient, AnAttestedImportRetainsItsActualSurfaceIdentity)
{
	Surface current(32768, 0x90000000);
	visible[4] = &current;
	auto *held = switch_gpu_surface_hold(4, nullptr, current.memory.pages.data(), 8);
	ASSERT_EQ(held, &current);
	EXPECT_EQ(current.refs, 2u);
	held->release();
	EXPECT_EQ(current.refs, 1u);
	auto wrong = current.memory.pages;
	wrong.front() += 16384;
	EXPECT_EQ(switch_gpu_surface_hold(4, nullptr, wrong.data(), 8), nullptr);
	EXPECT_EQ(current.refs, 1u);
}

TEST_F(SurfaceClient, CpuOwnershipFollowsCurrentPagesAcrossNumericIdReuse)
{
	Surface current(32768, 0x90000000), old(65536, 0x80000000);
	switch_gpu_backing retiring{1, old.memory.pages}, live{1, current.memory.pages};
	visible[4] = &current;
	hints[4] = &retiring;
	allocations[old.memory.pages[0]] = &retiring;
	allocations[current.memory.pages[0]] = &live;
	switch_gpu_backing *resolved = nullptr;
	ASSERT_EQ(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, &live);
	EXPECT_EQ(retiring.refs, 1u);
	EXPECT_EQ(live.refs, 2u);
	switch_gpu_backing_release(resolved);
	EXPECT_EQ(current.memory.outside, 0u);
	EXPECT_EQ(current.memory.prepared, current.memory.completed);
	EXPECT_EQ(current.refs, 1u);
}

TEST_F(SurfaceClient, ReusedIdForAnUnimportedSurfaceDoesNotBorrowRetiringOwnership)
{
	Surface current(32768, 0x90000000), old(65536, 0x80000000);
	switch_gpu_backing retiring{1, old.memory.pages};
	visible[4] = &current;
	hints[4] = &retiring;
	allocations[old.memory.pages[0]] = &retiring;
	switch_gpu_backing *resolved = nullptr;
	EXPECT_EQ(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, nullptr);
	EXPECT_EQ(retiring.refs, 1u);
}

TEST_F(SurfaceClient, ASharedFirstPageCannotAttestDifferentRemainingPages)
{
	Surface current(32768, 0x90000000);
	switch_gpu_backing bad{1, current.memory.pages};
	bad.pages.back() += 16384;
	visible[4] = &current;
	hints[4] = &bad;
	allocations[current.memory.pages[0]] = &bad;
	switch_gpu_backing *resolved = nullptr;
	EXPECT_NE(switch_gpu_surface_backing(4, nullptr, &resolved), 0);
	EXPECT_EQ(resolved, nullptr);
	EXPECT_EQ(bad.refs, 1u);
}

TEST_F(SurfaceClient, GeometryIsTheUnderlyingSurfaceRegardlessOfTheFirstTextureView)
{
	Surface current(32768, 0x90000000);
	visible[4] = &current;
	SwitchGpuSurfaceGeometry geometry = {};
	ASSERT_EQ(switch_gpu_surface_geometry(4, nullptr, current.memory.pages.data(), 8, &geometry), 0);
	EXPECT_TRUE(switch_gpu_surface_packed(geometry));
	EXPECT_FALSE(switch_gpu_surface_shape_matches(geometry, 42, 42, 8, 512));
	EXPECT_TRUE(switch_gpu_surface_shape_matches(geometry, 64, 64, 8, 512));
	tgm_layout layout = {};
	ASSERT_TRUE(tgm_layout_tiled(uint32_t(geometry.width), uint32_t(geometry.height), geometry.bpp, &layout));
	EXPECT_EQ(layout.row_bytes, 512u);
	EXPECT_EQ(layout.bytes, 32768u);
}

TEST_F(SurfaceClient, BlocksAndPlanesCannotBecomePackedColorLayouts)
{
	SwitchGpuSurfaceGeometry geometry{64, 64, 512, 32768, 8, 1, 1, 0, false};
	geometry.element_width = 2;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
	geometry.element_width = 1;
	geometry.planes = 2;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
	geometry.planes = 0;
	geometry.bytes = 32767;
	EXPECT_FALSE(switch_gpu_surface_packed(geometry));
}

TEST_F(SurfaceClient, AnInaccessibleOrUnpreparableSurfaceCannotAttestPages)
{
	Surface current(32768, 0x90000000);
	EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, current.memory.pages.data(), 8), 0);
	visible[4] = &current;
	current.memory.failPrepare = true;
	EXPECT_EQ(switch_gpu_surface_pages_match(4, nullptr, current.memory.pages.data(), 8), 0);
	EXPECT_EQ(current.memory.prepared, 0u);
	EXPECT_EQ(current.memory.completed, 0u);
	EXPECT_EQ(current.refs, 1u);
}
}
