// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <vector>
#include "switch_vic_backing_stubs.h"
#include "../TegraVICPresent.cpp"

struct SwitchVICMapping { uint64_t address; };
namespace {
OSObject surfaceObject;
IOMemoryDescriptor surfaceMemory;
switch_gpu_backing sourceBacking, destinationBacking;
TegraVICSurfaceDesc destinationDesc;
std::vector<int> events;
int admissionError = 0, completionError = 0, mappings = 0;
std::vector<std::array<uint32_t, 2>> ranges;
std::array<switch_gpu_backing_use, 2> declared;
}
extern "C" bool tegra_vic_ready() { return true; }
extern "C" const TegraVICTraits *tegra_vic_traits() { return &kTegraVICTraits; }
extern "C" uint64_t tegra_vic_config_address() { return 0x80001000; }
extern "C" uint64_t tegra_vic_filter_address() { return 0x80002000; }
extern "C" void switch_refusal(const char *, const char *, const char *, ...) {}
extern "C" IOReturn tegra_vic_surface_lookup(uint32_t id, task_t, TegraVICSurface *surface)
{
    if (id != 7)
        return -22;
    surfaceObject.retain();
    *surface = {&surfaceObject, &surfaceMemory, destinationDesc, 0, id, false};
    return 0;
}
extern "C" void tegra_vic_surface_release(TegraVICSurface *surface)
{
    if (surface->surface)
        surface->surface->release();
    *surface = {};
}
extern "C" void tegra_vic_surface_written(const TegraVICSurface *) { events.push_back(3); }
switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *)
{
    ++destinationBacking.refs;
    return &destinationBacking;
}
extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing) { --backing->refs; }
SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable)
{
    EXPECT_EQ(writable, backing == &destinationBacking);
    ++mappings;
    return new SwitchVICMapping{writable ? 0x40000000u : 0x30000000u};
}
SwitchVICMapping *switch_vic_mapping_acquire_range(switch_gpu_backing *backing, bool writable,
                                                   uint32_t firstPage, uint32_t pageCount)
{
    EXPECT_EQ(backing, &destinationBacking);
    EXPECT_TRUE(writable);
    ++mappings;
    ranges.push_back({firstPage, pageCount});
    return new SwitchVICMapping{0x40000000u + static_cast<uint64_t>(ranges.size()) * 0x1000000};
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (mapping) {
        --mappings;
        delete mapping;
    }
}
uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping) { return mapping->address; }
int switch_vic_submit_batch(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *acquire,
                            switch_gpu_backing_use *uses, uint32_t useCount,
                            void (*)(void *, int), void *, uint32_t *fence)
{
    EXPECT_EQ(count, 1u);
    EXPECT_GT(jobs->count, 0u);
    EXPECT_EQ(acquire, nullptr);
    EXPECT_EQ(useCount, 2u);
    declared = {uses[0], uses[1]};
    events.push_back(1);
    *fence = 12;
    return admissionError;
}
extern "C" int switch_vic_wait(uint32_t fence)
{
    EXPECT_EQ(fence, 12u);
    events.push_back(2);
    return completionError;
}

namespace {
class SwitchVICPresent : public testing::Test {
protected:
    uint64_t pages[512] = {};
    TegraVICPicture picture = {};
    void SetUp() override
    {
        events.clear();
        ranges.clear();
        admissionError = completionError = mappings = 0;
        picture.backing = &sourceBacking;
        picture.pages = pages;
        picture.pageCount = 512;
        picture.bytes = 1536u * 736 * 3 / 2;
        picture.pitch = 1536;
        picture.rows = 736;
        picture.chromaOffset = 1536 * 736;
        picture.blockHeightLog2 = 1;
        picture.width = 1280;
        picture.height = 720;
        picture.surfaceID = 7;
        picture.sampleBytes = 1;
        destinationDesc = {};
        destinationDesc.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
        destinationDesc.width = 1280;
        destinationDesc.height = 720;
        destinationDesc.planes = 2;
        destinationDesc.planeBytesPerRow[0] = destinationDesc.planeBytesPerRow[1] = 1536;
        destinationDesc.planeHeight[0] = 720;
        destinationDesc.planeHeight[1] = 360;
        destinationDesc.planeOffset[1] = 1536 * 720;
        destinationDesc.length = 1536 * 720 * 3 / 2;
        tegra_vic_present_start();
    }
    void TearDown() override
    {
        EXPECT_EQ(mappings, 0);
        EXPECT_EQ(surfaceObject.refs, 1u);
        EXPECT_EQ(destinationBacking.refs, 1u);
    }
};

TEST_F(SwitchVICPresent, OwnsBothAllocationsAndPublishesSeedOnlyAfterCompletion)
{
    const char *why = nullptr;
    ASSERT_EQ(tegra_vic_present(&picture, nullptr, &why), 0) << (why ? why : "");
    EXPECT_EQ(declared[0].backing, &sourceBacking);
    EXPECT_EQ(declared[0].access, uint32_t(SWITCH_GPU_BACKING_READ));
    EXPECT_EQ(declared[1].backing, &destinationBacking);
    EXPECT_EQ(declared[1].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0][0], 0u);
    EXPECT_EQ(ranges[1][0], uint32_t(destinationDesc.planeOffset[1] >> 12));
    EXPECT_LE(ranges[0][1] + ranges[1][1], 8192u);
}

TEST_F(SwitchVICPresent, FailedAdmissionDoesNotWaitOrPublishSeed)
{
    admissionError = -16;
    const char *why = nullptr;
    EXPECT_EQ(tegra_vic_present(&picture, nullptr, &why), -16);
    EXPECT_NE(why, nullptr);
    EXPECT_EQ(events, (std::vector<int>{1}));
}

TEST_F(SwitchVICPresent, FailedCompletionDoesNotPublishSeed)
{
    completionError = -19;
    const char *why = nullptr;
    EXPECT_EQ(tegra_vic_present(&picture, nullptr, &why), -19);
    EXPECT_EQ(events, (std::vector<int>{1, 2}));
}

TEST_F(SwitchVICPresent, UnsupportedPictureLeavesNextPictureAccelerated)
{
    const char *why = nullptr;
    picture.cropX = 1;
    EXPECT_EQ(tegra_vic_present(&picture, nullptr, &why), -95);
    EXPECT_TRUE(events.empty());
    picture.cropX = 0;
    EXPECT_EQ(tegra_vic_present(&picture, nullptr, &why), 0);
    EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}
}  // namespace
