// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstring>
#include "switch_nvdec_output_stubs.h"
#include "../SwitchNVDECOutput.cpp"

namespace {
IOBufferMemoryDescriptor oldMemory, nextMemory;
IOMemoryMap oldMap, nextMap;
switch_gpu_backing oldBacking, nextBacking;
switch_gpu_cpu_lease lease;
uint64_t pages[512];
bool vicReady, gpuReady, displayReady;
int available, allocationError, lookupError, publishError;
uint32_t allocations, frees, releases, lookups, surfaceReleases, publishes;
uint32_t requestedId;
task_t requestedTask;
TegraVICPicture exportedPicture;
IOMemoryDescriptor surfaceMemory;
}

extern "C" bool tegra_vic_present_ready() { return vicReady; }
extern "C" bool switch_native_surface_gpu_ready() { return gpuReady; }
extern "C" bool switch_native_surface_display_ready() { return displayReady; }
extern "C" int switch_gpu_backing_cpu_acquire_owned(switch_gpu_backing *backing, uint64_t offset, uint64_t bytes,
                                                     uint32_t access, switch_gpu_cpu_lease **out)
{
    EXPECT_EQ(backing, &oldBacking);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(bytes, 2u * 1024 * 1024);
    EXPECT_EQ(access, SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC);
    if (!available) {
        lease.backing = backing;
        *out = &lease;
    }
    return available;
}
extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
    EXPECT_EQ(*owned, &lease);
    EXPECT_EQ(wrote, 0);
    ++releases;
    *owned = nullptr;
    return 0;
}
extern "C" int tegra_nvdec_buffer_alloc(uint64_t bytes, TegraNVDECBlock *block)
{
    ++allocations;
    if (allocationError)
        return allocationError;
    *block = {};
    block->size = bytes;
    block->iova = 0x60000000;
    block->pages.memory = &nextMemory;
    block->backing = &nextBacking;
    return 0;
}
extern "C" void tegra_nvdec_buffer_free(TegraNVDECBlock *block)
{
    ++frees;
    --block->backing->refs;
    block->pages.memory->release();
    *block = {};
}
extern "C" IOReturn tegra_vic_surface_lookup(uint32_t id, task_t task, TegraVICSurface *surface)
{
    ++lookups;
    requestedId = id;
    requestedTask = task;
    *surface = {};
    surface->id = id;
    surface->memory = &surfaceMemory;
    surface->desc.length = 1695744;
    surface->baseOffset = 0;
    return lookupError;
}
extern "C" void tegra_vic_surface_release(TegraVICSurface *) { ++surfaceReleases; }
int switch_native_surface_publish(const TegraVICSurface *surface, const TegraVICPicture *picture, const char **why)
{
    ++publishes;
    EXPECT_EQ(surface->id, picture->surfaceID);
    exportedPicture = *picture;
    if (publishError && why)
        *why = "native export into a surface whose allocation is not its descriptor's length";
    return publishError;
}

namespace {
class SwitchNVDECOutputTest : public testing::Test {
protected:
    SwitchNVDECBuffer buffer;
    TegraNVDECPresent request;
    task_t task = reinterpret_cast<task_t>(0x1234);
    void SetUp() override
    {
        oldMemory = {};
        nextMemory = {};
        oldMap = {};
        nextMap = {};
        oldBacking = {};
        nextBacking = {};
        nextMemory.mapping = &nextMap;
        vicReady = gpuReady = displayReady = true;
        available = allocationError = lookupError = publishError = 0;
        allocations = frees = releases = lookups = surfaceReleases = publishes = 0;
        requestedId = 0;
        requestedTask = nullptr;
        exportedPicture = {};
        buffer = {};
        buffer.handle = 19;
        buffer.map = &oldMap;
        buffer.block.size = 2 * 1024 * 1024;
        buffer.block.iova = 0x50000000;
        buffer.block.backing = &oldBacking;
        buffer.block.pages.memory = &oldMemory;
        buffer.block.pages.pages = pages;
        buffer.block.pages.count = 512;
        request = {};
        request.pitch = 1536;
        request.rows = 736;
        request.chroma_offset = request.pitch * request.rows;
        request.block_height_log2 = 1;
        request.width = 1280;
        request.height = 720;
        request.sample_bytes = 1;
        request.surface_id = 52;
    }
    void unchanged() const
    {
        EXPECT_EQ(buffer.handle, 19u);
        EXPECT_EQ(buffer.map, &oldMap);
        EXPECT_EQ(buffer.block.backing, &oldBacking);
        EXPECT_EQ(buffer.block.pages.memory, &oldMemory);
        EXPECT_EQ(oldMap.refs, 1u);
        EXPECT_EQ(oldMemory.refs, 1u);
        EXPECT_EQ(oldBacking.refs, 1u);
    }
};

TEST_F(SwitchNVDECOutputTest, UnusedAllocationReusesMapAndHandle)
{
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    unchanged();
    EXPECT_EQ(releases, 1u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(frees, 0u);
}

TEST_F(SwitchNVDECOutputTest, ExportedAllocationGetsIndependentReplacement)
{
    available = -16;
    oldBacking.refs = 2;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), 0);
    EXPECT_EQ(buffer.handle, 19u);
    EXPECT_EQ(buffer.map, &nextMap);
    EXPECT_EQ(buffer.block.backing, &nextBacking);
    EXPECT_EQ(buffer.block.iova, 0x60000000u);
    EXPECT_EQ(nextMemory.mappedTask, task);
    EXPECT_EQ(nextMemory.mappedOptions, kIOMapAnywhere | kIOMapDefaultCache);
    EXPECT_EQ(oldBacking.refs, 1u);
    EXPECT_EQ(oldMap.refs, 0u);
    EXPECT_EQ(frees, 1u);
    EXPECT_EQ(releases, 0u);
}

TEST_F(SwitchNVDECOutputTest, FailedAllocationPreservesCurrentOutput)
{
    available = -16;
    allocationError = -12;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -12);
    unchanged();
    EXPECT_EQ(frees, 0u);
}

TEST_F(SwitchNVDECOutputTest, FailedMappingDropsOnlyReplacement)
{
    available = -16;
    nextMemory.mapping = nullptr;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -12);
    unchanged();
    EXPECT_EQ(frees, 1u);
    EXPECT_EQ(nextBacking.refs, 0u);
    EXPECT_EQ(nextMemory.refs, 0u);
}

TEST_F(SwitchNVDECOutputTest, FailedOwnershipDoesNotAllocateOrRevive)
{
    available = -5;
    EXPECT_EQ(switch_nvdec_output_recycle(&buffer, task), -5);
    unchanged();
    EXPECT_EQ(allocations, 0u);
}

TEST_F(SwitchNVDECOutputTest, ExportRequiresEveryConsumerReady)
{
    gpuReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    gpuReady = true;
    displayReady = false;
    EXPECT_FALSE(switch_nvdec_native_output_ready());
    displayReady = true;
    vicReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    EXPECT_EQ(lookups, 0u);
    EXPECT_EQ(publishes, 0u);
}

TEST_F(SwitchNVDECOutputTest, FullNativeExportPublishesWithoutPixelCopy)
{
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), 0);
    EXPECT_EQ(requestedId, request.surface_id);
    EXPECT_EQ(requestedTask, task);
    EXPECT_EQ(exportedPicture.backing, &oldBacking);
    EXPECT_EQ(exportedPicture.pages, pages);
    EXPECT_EQ(exportedPicture.pitch, 1536u);
    EXPECT_EQ(exportedPicture.rows, 736u);
    EXPECT_EQ(exportedPicture.bytes, 1695744u);
    EXPECT_EQ(publishes, 1u);
    EXPECT_EQ(surfaceReleases, 1u);
}

TEST_F(SwitchNVDECOutputTest, FailedPublishReleasesLookupAndPreservesBuffer)
{
    publishError = -16;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -16);
    EXPECT_EQ(surfaceReleases, 1u);
    unchanged();
}

TEST_F(SwitchNVDECOutputTest, AFailedExportNamesItsRefusalAndTheSurfacesSizes)
{
    SwitchNVDECExportRefusal refusal = {};
    vicReady = false;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -95);
    EXPECT_STREQ(refusal.why, "native output while VIC, the GPU or the display cannot read it");
    vicReady = true;
    request.dst_x = 2;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -95);
    EXPECT_STREQ(refusal.why, "native output of a placed or 16-bit picture");
    request.dst_x = 0;
    publishError = -22;
    surfaceMemory.length = 1703936;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task, &refusal), -22);
    EXPECT_STREQ(refusal.why, "native export into a surface whose allocation is not its descriptor's length");
    EXPECT_EQ(refusal.allocationBytes, 1695744u);
    EXPECT_EQ(refusal.descriptorBytes, 1703936u);
    EXPECT_EQ(refusal.baseOffset, 0u);
}

TEST_F(SwitchNVDECOutputTest, UnsupportedPlacementAndWideSamplesDoNotPublish)
{
    request.dst_x = 2;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    request.dst_x = 0;
    request.sample_bytes = 2;
    request.width = 640;
    EXPECT_EQ(switch_nvdec_output_export(&buffer.block, &request, task), -95);
    EXPECT_EQ(lookups, 0u);
}

TEST_F(SwitchNVDECOutputTest, InvalidGeometryLeavesOutputUnchanged)
{
    TegraVICPicture picture = {};
    picture.width = 73;
    auto invalid = request;
    invalid.crop_x = UINT32_MAX;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    EXPECT_EQ(picture.width, 73u);
    invalid = request;
    invalid.chroma_offset = 256;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    invalid = request;
    invalid.rows = UINT32_MAX;
    EXPECT_EQ(switch_nvdec_output_picture(&buffer.block, &invalid, &picture), -22);
    EXPECT_EQ(picture.width, 73u);
}
}  // namespace
