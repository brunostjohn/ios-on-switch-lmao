// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#define IONewZero(type, count) new (std::nothrow) type[count]()
#include "../SwitchNativePlane.h"
#include "../SwitchDisplayComposition.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../../TegraDisplayFlip/TegraScanoutCopy.h"

namespace {
SwitchNativeScanout nextNative, nextPitch;
unsigned leases, acquisitions, pitchAcquisitions;
int nativeError, pitchError;
struct Mapping {
    unsigned maps = 0, unmaps = 0, count = 0;
    uint64_t first = 0;
    bool fail = false;
    bool map(uint64_t, const uint64_t *pages, uint32_t n, bool write)
    { EXPECT_FALSE(write); ++maps; count = n; first = pages[0]; return !fail; }
    void unmap(uint64_t, uint32_t) { ++unmaps; }
};
struct Display {
    static constexpr unsigned kPlaneSlots = 2;
    static constexpr uint64_t kPlaneSlotSpan = 20ULL << 20;
    struct Slot { uint64_t low = 0, high = 0; uint32_t used = 0; bool tiled = false; SwitchNativeScanout *native = nullptr; };
    Slot planeSlots[kPlaneSlots];
    using PlaneSet = switch_display::Composition;
    PlaneSet planeNext;
    bool planeNextSet = false, stats = true;
    uint32_t planeSlotUses = 0, stPlaneMaps = 0, flushes = 0;
    bool busy[kPlaneSlots] = {};
    Mapping probeDomain;
    bool planeSlotBusy(int slot) const { return busy[slot]; }
    static const tgm_layout *surfaceLayout(OSObject *, tgm_layout &) { return nullptr; }
    static uint64_t planeSlotIova(int slot) { return 0x10000000ULL + uint64_t(slot) * kPlaneSlotSpan; }
    void cleanDomain() { ++flushes; }
    void planeDropSlot(unsigned slot)
    {
        if (planeSlots[slot].native && planeSlots[slot].native->lease) {
            probeDomain.unmap(planeSlotIova(int(slot)), uint32_t((planeSlots[slot].high - planeSlots[slot].low) >> 12));
            switch_native_scanout_release(planeSlots[slot].native);
        }
        auto *storage = planeSlots[slot].native;
        planeSlots[slot] = {};
        planeSlots[slot].native = storage;
    }
    ~Display() { for (unsigned i = 0; i < kPlaneSlots; ++i) { planeDropSlot(i); delete[] planeSlots[i].native; } }
#include "../SwitchNativePlaneDisplay.h"
};
}
int switch_native_scanout_acquire(OSObject *, SwitchNativeScanout *out)
{
    ++acquisitions;
    *out = nativeError ? SwitchNativeScanout{} : nextNative;
    if (out->lease) ++leases;
    return nativeError;
}
int switch_native_scanout_pitch(OSObject *, SwitchNativeScanout *out)
{
    ++pitchAcquisitions;
    *out = pitchError ? SwitchNativeScanout{} : nextPitch;
    if (out->lease) ++leases;
    return pitchError;
}
void switch_native_scanout_release(SwitchNativeScanout *value)
{
    if (value->lease) { EXPECT_GT(leases, 0u); --leases; }
    *value = {};
}
namespace {
class NativeScanoutMapping : public testing::Test {
protected:
    native_planes::Layer layer;
    native_planes::Image image;
    std::array<uint64_t, 1024> pages;
    Display *display;
    bool resolved;
    void SetUp() override
    {
        leases = acquisitions = pitchAcquisitions = 0;
        nativeError = pitchError = 0;
        for (uint32_t i = 0; i < pages.size(); ++i) pages[i] = 0x80000000ULL + uint64_t(i) * 4096;
        layer = {};
        layer.kind = native_planes::kKindNv12Video;
        layer.surface.format = native_planes::fourcc('4', '2', '0', 'v');
        layer.surface.width = 1280; layer.surface.height = 720; layer.surface.planes = 2;
        layer.surface.stride[0] = layer.surface.stride[1] = 1280;
        layer.surface.planeHeight[0] = 720; layer.surface.planeHeight[1] = 360;
        layer.surface.offset[1] = 1280 * 720;
        layer.src = {0, 0, 1280, 720}; layer.dst = {0, 437, 720, 405};
        nextNative = {};
        nextNative.lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
        nextNative.view.native = reinterpret_cast<switch_gpu_backing *>(1);
        nextNative.view.producer = {73, 1, SWITCH_GPU_ENGINE_NVDEC, 163};
        auto &layout = nextNative.view.layout;
        layout.format = layer.surface.format; layout.plane_count = 2; layout.color_matrix = 2; layout.color_range = 1;
        layout.planes[0] = {0, 1536 * 736, 1536, 736, 1536, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1536 * 736, 1536 * 368, 768, 368, 1536, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        nextNative.allocation.allocation_id = nextNative.allocation.allocation_generation = nextNative.allocation.content_generation = 1;
        nextNative.allocation.pages = pages.data(); nextNative.allocation.page_count = uint32_t(pages.size());
        nextPitch = nextNative;
        nextPitch.allocation.allocation_id = 2;
        nextPitch.view.producer = {92, 1, SWITCH_GPU_ENGINE_VIC, 164};
        display = new Display;
        resolved = false;
    }
    void TearDown() override { delete display; EXPECT_EQ(leases, 0u); }
    int admit() { return display->planeNativeSlotFor(nullptr, layer, 720, 1280, image, resolved); }
};
TEST_F(NativeScanoutMapping, NativeAllocationMapsOnceAndCarriesExactProducer)
{
    ASSERT_EQ(admit(), 0); EXPECT_TRUE(resolved);
    EXPECT_EQ(display->probeDomain.maps, 1u); EXPECT_EQ(display->probeDomain.first, pages[0]);
    EXPECT_EQ(leases, 1u);
    display->busy[0] = true;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(display->probeDomain.maps, 1u); EXPECT_EQ(leases, 1u);
    display->planeNextSet = true;
    display->planeNext.window[0].on = true;
    display->planeNext.window[0].slot = 0;
    TegraScanoutCopy copy = {};
    switch_gpu_backing_token tokens[2] = {};
    display->planeNextDependencies(copy, tokens);
    ASSERT_EQ(copy.dependencyCount, 1u); ASSERT_NE(copy.dependencies, nullptr);
    EXPECT_EQ(copy.dependencies[0].value, 73u);
    EXPECT_EQ(copy.dependencies[0].engine, uint32_t(SWITCH_GPU_ENGINE_NVDEC));
}
TEST_F(NativeScanoutMapping, ContentGenerationChangeCannotReuseLiveImageMapping)
{
    ASSERT_EQ(admit(), 0); display->busy[0] = true;
    ++nextNative.allocation.content_generation;
    ASSERT_EQ(admit(), 1);
    EXPECT_EQ(display->probeDomain.maps, 2u); EXPECT_EQ(leases, 2u);
    display->busy[1] = true;
    ++nextNative.allocation.content_generation;
    EXPECT_EQ(admit(), -11); EXPECT_EQ(leases, 2u);
    display->busy[0] = false;
    display->planeDropSlot(0);
    EXPECT_EQ(leases, 1u);
}
TEST_F(NativeScanoutMapping, UnsupportedCropUsesQueuedPitchAllocationAndAliasOffset)
{
    nextNative.view.crop.x = 2;
    nextPitch.pitchOffset = 4096;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(pitchAcquisitions, 1u); EXPECT_EQ(leases, 1u);
    EXPECT_EQ(image.surfaceKind, 0u); EXPECT_EQ(image.start[0], 4096u);
    EXPECT_EQ(display->probeDomain.first, pages[1]);
    EXPECT_EQ(display->planeSlots[0].native->view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
}
TEST_F(NativeScanoutMapping, FailedMappingUnwindsRetainedImageAndPartialPageTable)
{
    display->probeDomain.fail = true;
    EXPECT_EQ(admit(), -1); EXPECT_TRUE(resolved);
    EXPECT_EQ(leases, 0u); EXPECT_EQ(display->probeDomain.unmaps, 1u);
}
TEST_F(NativeScanoutMapping, OrdinarySurfaceCapturesExactBackingAndContentGeneration)
{
    nativeError = -2;
    ASSERT_EQ(admit(), 0); EXPECT_TRUE(resolved);
    EXPECT_EQ(pitchAcquisitions, 1u); EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(display->planeSlots[0].native->allocation.allocation_id, 2u);
    EXPECT_EQ(display->planeSlots[0].native->view.producer.value, 92u);
}
TEST_F(NativeScanoutMapping, IncompletePhysicalExtentCannotReachSmmu)
{
    nextNative.allocation.page_count = 1;
    EXPECT_EQ(admit(), -95); EXPECT_EQ(leases, 0u); EXPECT_EQ(display->probeDomain.maps, 0u);
}
TEST_F(NativeScanoutMapping, CpuOwnedOrdinarySurfaceDefersWithoutMappingOrDroppingItsProducer)
{
    nativeError = -2;
    pitchError = -16;
    EXPECT_EQ(admit(), -16);
    EXPECT_TRUE(resolved);
    EXPECT_EQ(display->probeDomain.maps, 0u);
    EXPECT_EQ(leases, 0u);
}
}  // namespace
