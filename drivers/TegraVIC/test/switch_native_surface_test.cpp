// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <future>
#include <vector>
#include "switch_native_surface_stubs.h"
#include "../SwitchNativeSurface.cpp"
#include "../SwitchNativeScanout.cpp"
#include "../SwitchNativeGpuView.cpp"
#include "../SwitchVICSurfaceAccess.cpp"
#include "../../../kernel/switch/iokit/memory_attachment.cpp"

struct switch_gpu_backing {
    std::atomic<unsigned> refs{1};
    uint64_t pages[1024] = {};
    uint32_t pageCount = 1024, readers = 0, writers = 0, concurrentReaders = 0, concurrentWriters = 0;
    uint64_t content = 1;
    switch_gpu_backing_token producer = {11, 1, SWITCH_GPU_ENGINE_NVDEC, 7};
    std::mutex lock;
    std::condition_variable changed;
};
struct switch_gpu_cpu_lease {
    switch_gpu_backing *backing;
    uint32_t access;
};
namespace {
struct Surface : OSObject {
    IOMemoryDescriptor *memory;
    TegraVICSurfaceDesc desc = {};
    uint32_t id;
    uint64_t offset = 0;
    Surface(IOMemoryDescriptor *descriptor, uint32_t number) : memory(descriptor), id(number) { memory->retain(); }
    ~Surface() override { memory->release(); }
};
std::atomic<unsigned> transfers{0}, seeds{0}, descriptions{0}, registrations{0}, finishedViews{0};
int transferError = 0, generationError = 0;
unsigned gpuReservations = 0, queuedTransfers = 0, cpuAcquisitions = 0;
bool reservingGpu = false;
bool ready = true;
TegraVICSurfaceDesc convertedDesc;
std::function<void()> duringTransfer, duringGeneration, duringExport;
}
extern "C" void switch_gpu_surface_finish(void *descriptor)
{
    ++finishedViews;
    SwitchMemoryAttachment::leaveCpu(static_cast<IOMemoryDescriptor *>(descriptor));
}
extern "C" IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *object)
{
    auto *surface = dynamic_cast<Surface *>(object);
    return surface ? surface->memory : nullptr;
}
extern "C" IOReturn switch_vic_surface_from_object(OSObject *object, uint32_t id, TegraVICSurface *out)
{
    ++descriptions;
    *out = {};
    auto *surface = dynamic_cast<Surface *>(object);
    if (!surface || (id && surface->id != id))
        return -22;
    surface->retain();
    *out = {surface, surface->memory, surface->desc, surface->offset, id, false};
    return 0;
}
extern "C" void tegra_vic_surface_release(TegraVICSurface *surface)
{
    if (surface->surface)
        surface->surface->release();
    *surface = {};
}
extern "C" void tegra_vic_surface_written(const TegraVICSurface *) { ++seeds; }
extern "C" bool tegra_vic_present_ready() { return ready; }
switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *surface)
{
    ++registrations;
    auto *backing = surface->memory->backing;
    if (backing)
        ++backing->refs;
    return backing;
}
extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing) { ++backing->refs; }
extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing) { --backing->refs; }
extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    *info = {};
    info->pages = backing->pages;
    info->page_count = backing->pageCount;
    info->content_generation = backing->content;
}
extern "C" int switch_gpu_backing_cpu_acquire_owned(switch_gpu_backing *backing, uint64_t offset,
                                                    uint64_t bytes, uint32_t access, switch_gpu_cpu_lease **out)
{
    if (!reservingGpu)
        ++cpuAcquisitions;
    *out = nullptr;
    if (offset || bytes != uint64_t(backing->pageCount) * 4096 || !(access & kNoCache))
        return -22;
    const bool writable = access & kWrite, shared = access & kConcurrent;
    std::unique_lock held(backing->lock);
    auto conflict = [&] {
        return backing->writers > (shared ? backing->concurrentWriters : 0) ||
            (writable && backing->readers > (shared ? backing->concurrentReaders : 0));
    };
    if (conflict() && (access & kAvoidSync))
        return -16;
    backing->changed.wait(held, [&] { return !conflict(); });
    ++(writable ? backing->writers : backing->readers);
    if (shared)
        ++(writable ? backing->concurrentWriters : backing->concurrentReaders);
    ++backing->refs;
    *out = new switch_gpu_cpu_lease{backing, access};
    return 0;
}
extern "C" int switch_gpu_backing_conversion_reserve(switch_gpu_backing *backing, uint32_t access, int avoid,
                                                        switch_gpu_cpu_lease **out)
{
    ++gpuReservations;
    reservingGpu = true;
    const int error = switch_gpu_backing_cpu_acquire_owned(backing, 0, uint64_t(backing->pageCount) * 4096,
        access | kNoCache | (avoid ? kAvoidSync : 0), out);
    reservingGpu = false;
    return error;
}
extern "C" int switch_gpu_backing_admission_ready(switch_gpu_backing *backing, uint32_t access)
{
    std::lock_guard held(backing->lock);
    return !backing->writers && (!(access & kWrite) || !backing->readers);
}
extern "C" int switch_gpu_backing_conversion_ready(switch_gpu_backing *backing, uint32_t access)
{
    return switch_gpu_backing_admission_ready(backing, access);
}
extern "C" void switch_gpu_admission_changed(void) {}
extern "C" int switch_gpu_backing_export_read(switch_gpu_backing *backing,
                                                switch_gpu_backing_token *producer, switch_gpu_cpu_lease **owned)
{
    if (duringExport)
        duringExport();
    *producer = backing->producer;
    return switch_gpu_backing_cpu_acquire_owned(backing, 0, uint64_t(backing->pageCount) * 4096,
                                                kRead | kNoCache | kAvoidSync, owned);
}
extern "C" int switch_gpu_backing_snapshot_read(switch_gpu_backing *backing,
                                                switch_gpu_backing_token *producer, switch_gpu_cpu_lease **owned)
{
    return switch_gpu_backing_export_read(backing, producer, owned);
}
extern "C" int switch_gpu_backing_snapshot_read_wait(switch_gpu_backing *backing,
                                                switch_gpu_backing_token *producer, switch_gpu_cpu_lease **owned)
{
    if (duringExport)
        duringExport();
    const int error = switch_gpu_backing_cpu_acquire_owned(backing, 0, uint64_t(backing->pageCount) * 4096,
                                                          kRead | kNoCache, owned);
    if (!error)
        *producer = backing->producer;
    return error;
}
extern "C" int switch_gpu_backing_cpu_release_owned(switch_gpu_cpu_lease **owned, int wrote)
{
    EXPECT_EQ(wrote, 0);
    auto *lease = *owned;
    if (!lease)
        return -22;
    auto *backing = lease->backing;
    std::lock_guard held(backing->lock);
    --((lease->access & kWrite) ? backing->writers : backing->readers);
    if (lease->access & kConcurrent)
        --((lease->access & kWrite) ? backing->concurrentWriters : backing->concurrentReaders);
    --backing->refs;
    backing->changed.notify_all();
    delete lease;
    *owned = nullptr;
    return 0;
}
extern "C" int switch_gpu_backing_cpu_publish_generation(switch_gpu_backing *backing,
        switch_gpu_cpu_lease *owned, volatile uint64_t *word, uint64_t expected, uint64_t next)
{
    if (duringGeneration)
        duringGeneration();
    if (generationError)
        return generationError;
    std::lock_guard held(backing->lock);
    if (!owned || owned->backing != backing || !(owned->access & kWrite) ||
        backing->writers != 1 || backing->readers)
        return -22;
    return __atomic_compare_exchange_n(word, &expected, next, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)
        ? 0 : SWITCH_GPU_BACKING_STALE;
}
extern "C" int switch_vic_present_surface_owned(const TegraVICPicture *picture, const TegraVICSurface *surface,
                                                switch_gpu_cpu_lease *owned, const char **)
{
    EXPECT_NE(picture->backing, owned->backing);
    EXPECT_TRUE(owned->access & kWrite);
    EXPECT_EQ(owned->backing->writers, 1u);
    EXPECT_EQ(owned->backing->readers, 0u);
    EXPECT_EQ(picture->surfaceID, surface->id);
    convertedDesc = surface->desc;
    ++transfers;
    if (duringTransfer)
        duringTransfer();
    return transferError;
}
extern "C" int switch_vic_present_surface_queue(const TegraVICPicture *picture, const TegraVICSurface *surface,
                                                switch_gpu_cpu_lease *owned, const char **why)
{
    ++queuedTransfers;
    const int error = switch_vic_present_surface_owned(picture, surface, owned, why);
    if (!error) {
        ++owned->backing->content;
        owned->backing->producer = {22, 1, SWITCH_GPU_ENGINE_VIC, 8};
    }
    return error;
}
namespace {
class NativeSurface : public testing::Test {
protected:
    switch_gpu_backing native, logical, replacement;
    IOMemoryDescriptor *memory = nullptr;
    Surface *object = nullptr;
    TegraVICSurface surface = {};
    TegraVICPicture picture = {};
    void SetUp() override
    {
        transfers = seeds = descriptions = registrations = 0;
        ready = true;
        gpuReservations = queuedTransfers = cpuAcquisitions = 0;
        reservingGpu = false;
        transferError = generationError = 0;
        duringTransfer = duringGeneration = duringExport = {};
        memory = new IOMemoryDescriptor;
        memory->length = 1536 * 720 * 3 / 2;
        memory->backing = &logical;
        logical.pageCount = (memory->length + 4095) / 4096;
        object = new Surface(memory, 7);
        object->desc.fourcc = 0x34323076;
        object->desc.width = 1280;
        object->desc.height = 720;
        object->desc.planes = 2;
        object->desc.length = memory->length;
        object->desc.planeBytesPerRow[0] = object->desc.planeBytesPerRow[1] = 1536;
        object->desc.planeWidth[0] = 1280;
        object->desc.planeWidth[1] = 640;
        object->desc.planeHeight[0] = 720;
        object->desc.planeHeight[1] = 360;
        object->desc.planeOffset[1] = 1536 * 720;
        surface = {object, memory, object->desc, 0, 7, false};
        picture.backing = &native;
        picture.pages = native.pages;
        picture.pageCount = native.pageCount;
        picture.pitch = 1536;
        picture.rows = 736;
        picture.chromaOffset = picture.pitch * picture.rows;
        picture.bytes = uint64_t(picture.chromaOffset) * 3 / 2;
        picture.blockHeightLog2 = 1;
        picture.width = 1280;
        picture.height = 720;
        picture.surfaceID = 7;
        picture.sampleBytes = 1;
    }
    void TearDown() override
    {
        if (object)
            object->release();
        if (memory)
            memory->release();
        EXPECT_EQ(native.refs, 1u);
        EXPECT_EQ(logical.refs, 1u);
        EXPECT_EQ(replacement.refs, 1u);
    }
    int prepareCpu(Surface *input, uint32_t options, switch_gpu_backing **backing, switch_gpu_cpu_lease **owned)
    {
        const int entered = SwitchMemoryAttachment::enterCpu(input->memory, options & 2);
        if (entered)
            return entered;
        return switch_native_surface_prepare(input, input->id, nullptr, options, backing, owned);
    }
    int prepareGpu(uint32_t options, switch_gpu_backing **backing, switch_gpu_cpu_lease **owned)
    {
        const int entered = SwitchMemoryAttachment::enterCpu(memory, options & 2);
        if (entered)
            return entered;
        return switch_native_surface_prepare_gpu(object, object->id, nullptr, options, backing, owned);
    }
    void finishCpu(Surface *input, switch_gpu_backing *backing, switch_gpu_cpu_lease **owned)
    {
        if (*owned)
            switch_gpu_backing_cpu_release_owned(owned, 0);
        if (backing)
            switch_gpu_backing_release(backing);
        SwitchMemoryAttachment::leaveCpu(input->memory);
    }
};

TEST_F(NativeSurface, UnattachedCpuAccessDoesNoPageWalkOrGeometryWork)
{
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(object, 1, &backing, &owned), 0);
    EXPECT_EQ(backing, nullptr);
    EXPECT_EQ(owned, nullptr);
    EXPECT_EQ(registrations, 0u);
    EXPECT_EQ(descriptions, 0u);
    finishCpu(object, backing, &owned);
}

TEST_F(NativeSurface, PublicationRetainsDecoderAllocationAndGuardedViews)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    EXPECT_EQ(transfers, 0u);
    EXPECT_EQ(seeds, 1u);
    EXPECT_EQ(native.readers, 1u);
    SwitchNativeSurfaceView view = {};
    const unsigned finished = finishedViews;
    ASSERT_EQ(switch_native_surface_acquire(&surface, &view, false), 0);
    EXPECT_EQ(finishedViews, finished + 1);
    EXPECT_EQ(view.logical, &logical);
    EXPECT_EQ(view.native, &native);
    EXPECT_EQ(view.guard.backing, &logical);
    EXPECT_EQ(view.representation.backing, &logical);
    EXPECT_EQ(view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_NVDEC));
    EXPECT_EQ(view.producer.value, 11u);
    EXPECT_EQ(*view.guard.generation, view.guard.expected);
    EXPECT_EQ(*view.representation.generation, view.representation.expected);
    EXPECT_EQ(view.layout.planes[0].height, 736u);
    EXPECT_EQ(view.crop.height, 720u);
    switch_gpu_cpu_lease *writeLease = nullptr;
    EXPECT_EQ(acquire(&native, kWrite | kAvoidSync, &writeLease), -16);
    switch_native_surface_release(&view);
}

TEST_F(NativeSurface, ReadOnlyRowsMaterializeOnceAndReturnContinuousOwnership)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    for (unsigned index = 0; index != 2; ++index) {
        switch_gpu_backing *backing = nullptr;
        switch_gpu_cpu_lease *owned = nullptr;
        ASSERT_EQ(prepareCpu(object, 1, &backing, &owned), 0);
        EXPECT_NE(owned, nullptr);
        EXPECT_EQ(owned->backing, &logical);
        EXPECT_EQ(transfers, 1u);
        EXPECT_EQ(seeds, 1u);
        EXPECT_EQ(native.readers, 1u);
        finishCpu(object, backing, &owned);
    }
}

TEST_F(NativeSurface, AvoidSyncRefusesMissingRowsWithoutKickingHardware)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    EXPECT_EQ(prepareCpu(object, 3, &backing, &owned), -16);
    EXPECT_EQ(transfers, 0u);
    EXPECT_EQ(backing, nullptr);
    EXPECT_EQ(owned, nullptr);
    finishCpu(object, backing, &owned);
}

TEST_F(NativeSurface, MutableCpuAccessInvalidatesRetainedNativeViewsBeforeExposingRows)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView view = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &view, false), 0);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(object, 0, &backing, &owned), 0);
    EXPECT_NE(*view.guard.generation, view.guard.expected);
    EXPECT_NE(*view.representation.generation, view.representation.expected);
    EXPECT_EQ(native.readers, 0u);
    EXPECT_EQ(transfers, 1u);
    SwitchNativeSurfaceView stale = {};
    EXPECT_EQ(switch_native_surface_acquire(&surface, &stale, false), SWITCH_GPU_BACKING_STALE);
    finishCpu(object, backing, &owned);
    switch_native_surface_release(&view);
}

TEST_F(NativeSurface, FailedMaterializationPreservesNativeAuthorityAndLayout)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView before = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &before, false), 0);
    transferError = -5;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    EXPECT_EQ(prepareCpu(object, 0, &backing, &owned), -5);
    EXPECT_EQ(backing, nullptr);
    EXPECT_EQ(owned, nullptr);
    EXPECT_EQ(native.readers, 1u);
    EXPECT_EQ(*before.guard.generation, before.guard.expected);
    finishCpu(object, backing, &owned);
    switch_native_surface_release(&before);
}

TEST_F(NativeSurface, DescriptorRecycleStalesOldViewsAndReleasesOldDecoderLease)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView previous = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &previous, false), 0);
    picture.backing = &replacement;
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    EXPECT_NE(*previous.guard.generation, previous.guard.expected);
    EXPECT_NE(*previous.representation.generation, previous.representation.expected);
    EXPECT_EQ(native.readers, 0u);
    EXPECT_EQ(replacement.readers, 1u);
    EXPECT_EQ(seeds, 2u);
    SwitchNativeSurfaceView next = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &next, false), 0);
    EXPECT_EQ(next.native, &replacement);
    switch_native_surface_release(&next);
    switch_native_surface_release(&previous);
}

TEST_F(NativeSurface, FailedReplacementRestoresPreviousAuthority)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView previous = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &previous, false), 0);
    picture.backing = &replacement;
    generationError = -16;
    EXPECT_EQ(switch_native_surface_publish(&surface, &picture), -16);
    EXPECT_EQ(*previous.guard.generation, previous.guard.expected);
    EXPECT_EQ(native.readers, 1u);
    EXPECT_EQ(replacement.readers, 0u);
    EXPECT_EQ(seeds, 1u);
    SwitchNativeSurfaceView after = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &after, false), 0);
    EXPECT_EQ(after.native, &native);
    switch_native_surface_release(&after);
    switch_native_surface_release(&previous);
}

TEST_F(NativeSurface, DifferentlyFormattedAliasMaterializesCanonicalPitchRows)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    auto *alias = new Surface(memory, 8);
    alias->desc = object->desc;
    alias->desc.fourcc = 0x42475241;
    alias->offset = 4096;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(alias, 1, &backing, &owned), 0);
    EXPECT_EQ(convertedDesc.fourcc, object->desc.fourcc);
    EXPECT_EQ(convertedDesc.length, memory->length);
    EXPECT_EQ(backing, &logical);
    finishCpu(alias, backing, &owned);
    alias->release();
}

TEST_F(NativeSurface, AliasWithDifferentGeometryCannotSampleNativePlanes)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    auto alias = surface;
    alias.desc.width = 32;
    SwitchNativeSurfaceView view = {};
    EXPECT_EQ(switch_native_surface_acquire(&alias, &view, false), -95);
    EXPECT_EQ(view.owner, nullptr);
}

TEST_F(NativeSurface, RefusesPartialDescriptorAndUnsupportedLazyConversion)
{
    auto alias = surface;
    alias.baseOffset = 4096;
    const char *why = nullptr;
    EXPECT_EQ(switch_native_surface_publish(&alias, &picture, &why), -22);
    EXPECT_STREQ(why, "native export into a surface at a base offset");
    alias = surface;
    alias.desc.length -= 4096;
    EXPECT_EQ(switch_native_surface_publish(&alias, &picture, &why), -22);
    EXPECT_STREQ(why, "native export into a surface whose allocation is not its descriptor's length");
    ready = false;
    EXPECT_EQ(switch_native_surface_publish(&surface, &picture, &why), -95);
    EXPECT_STREQ(why, "native export without VIC to materialize it");
    EXPECT_EQ(seeds, 0u);
    EXPECT_EQ(native.readers, 0u);
}
TEST_F(NativeSurface, ConcurrentCpuViewsShareOneMaterializationAndContinuousReadReservations)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    std::promise<void> conversionStarted, completeConversion, finishUsers;
    auto converted = completeConversion.get_future().share();
    auto finish = finishUsers.get_future().share();
    std::mutex usersLock;
    std::condition_variable usersChanged;
    unsigned preparedUsers = 0;
    duringTransfer = [&] { conversionStarted.set_value(); converted.wait(); };
    auto reader = [&] {
        switch_gpu_backing *backing = nullptr;
        switch_gpu_cpu_lease *owned = nullptr;
        const int result = prepareCpu(object, 1, &backing, &owned);
        if (!result) {
            {
                std::lock_guard held(backing->lock);
                if (owned->access & kWrite) {
                    --backing->writers;
                    ++backing->readers;
                    ++backing->concurrentReaders;
                    owned->access = kRead | kNoCache | kConcurrent;
                    backing->changed.notify_all();
                }
            }
            {
                std::lock_guard held(usersLock);
                ++preparedUsers;
                usersChanged.notify_all();
            }
            finish.wait();
        }
        finishCpu(object, backing, &owned);
        return result;
    };
    auto first = std::async(std::launch::async, reader);
    conversionStarted.get_future().wait();
    auto second = std::async(std::launch::async, reader);
    completeConversion.set_value();
    {
        std::unique_lock held(usersLock);
        EXPECT_TRUE(usersChanged.wait_for(held, std::chrono::seconds(5), [&] { return preparedUsers == 2; }));
    }
    finishUsers.set_value();
    EXPECT_EQ(first.get(), 0);
    EXPECT_EQ(second.get(), 0);
    EXPECT_EQ(transfers, 1u);
    EXPECT_EQ(native.readers, 1u);
    EXPECT_EQ(logical.readers, 0u);
    EXPECT_EQ(logical.writers, 0u);
}

TEST_F(NativeSurface, RetainedViewOwnsAllocationAfterDescriptorAndSurfaceDestruction)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView view = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &view, false), 0);
    object->release();
    object = nullptr;
    memory->release();
    memory = nullptr;
    EXPECT_EQ(native.readers, 1u);
    EXPECT_EQ(*view.guard.generation, view.guard.expected);
    switch_native_surface_release(&view);
    EXPECT_EQ(native.readers, 0u);
}

TEST_F(NativeSurface, AcquiredViewCannotEscapeFailedPublication)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    picture.backing = &replacement;
    generationError = -16;
    std::promise<void> replacing, releaseReplacement;
    auto finish = releaseReplacement.get_future().share();
    duringGeneration = [&] { replacing.set_value(); finish.wait(); };
    auto publisher = std::async(std::launch::async, [&] { return switch_native_surface_publish(&surface, &picture); });
    replacing.get_future().wait();
    SwitchNativeSurfaceView view = {};
    auto reader = std::async(std::launch::async, [&] { return switch_native_surface_acquire(&surface, &view, false); });
    EXPECT_EQ(reader.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    releaseReplacement.set_value();
    EXPECT_EQ(publisher.get(), -16);
    EXPECT_EQ(reader.get(), 0);
    EXPECT_EQ(view.native, &native);
    EXPECT_EQ(*view.guard.generation, view.guard.expected);
    switch_native_surface_release(&view);
}
TEST_F(NativeSurface, PitchViewCreatedBeforeNativePublicationBecomesStale)
{
    auto *stamp = SwitchMemoryAttachment::generation(memory);
    ASSERT_NE(stamp, nullptr);
    const uint64_t pitchGeneration = __atomic_load_n(stamp, __ATOMIC_ACQUIRE);
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    EXPECT_NE(uint64_t(__atomic_load_n(stamp, __ATOMIC_ACQUIRE)), pitchGeneration);
    const uint64_t nativeGeneration = __atomic_load_n(stamp, __ATOMIC_ACQUIRE);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(object, 1, &backing, &owned), 0);
    EXPECT_EQ(uint64_t(__atomic_load_n(stamp, __ATOMIC_ACQUIRE)), nativeGeneration);
    finishCpu(object, backing, &owned);
    ASSERT_EQ(prepareCpu(object, 0, &backing, &owned), 0);
    EXPECT_NE(uint64_t(__atomic_load_n(stamp, __ATOMIC_ACQUIRE)), nativeGeneration);
    finishCpu(object, backing, &owned);
}
}  // namespace

TEST_F(NativeSurface, GpuReadQueuesMaterializationAndKeepsOwnershipUntilPublication)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    cpuAcquisitions = gpuReservations = 0;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareGpu(1, &backing, &owned), 0);
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(backing, &logical);
    EXPECT_EQ(logical.writers, 1u);
    EXPECT_EQ(queuedTransfers, 1u);
    EXPECT_EQ(gpuReservations, 1u);
    EXPECT_EQ(cpuAcquisitions, 0u);
    finishCpu(object, backing, &owned);
    ASSERT_EQ(prepareGpu(1, &backing, &owned), 0);
    EXPECT_EQ(logical.readers, 1u);
    EXPECT_EQ(queuedTransfers, 1u);
    EXPECT_EQ(cpuAcquisitions, 0u);
    finishCpu(object, backing, &owned);
}
TEST_F(NativeSurface, MutableGpuPreparationInvalidatesOldNativeViewsAfterQueuedConversion)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeSurfaceView view = {};
    ASSERT_EQ(switch_native_surface_acquire(&surface, &view, false), 0);
    cpuAcquisitions = 0;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareGpu(0, &backing, &owned), 0);
    EXPECT_EQ(queuedTransfers, 1u);
    EXPECT_EQ(cpuAcquisitions, 0u);
    EXPECT_NE(*view.guard.generation, view.guard.expected);
    EXPECT_NE(*view.representation.generation, view.representation.expected);
    EXPECT_EQ(logical.writers, 1u);
    finishCpu(object, backing, &owned);
    switch_native_surface_release(&view);
}
TEST_F(NativeSurface, FailedGpuMaterializationPreservesNativeAuthority)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    transferError = -5;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    EXPECT_EQ(prepareGpu(0, &backing, &owned), -5);
    EXPECT_EQ(backing, nullptr); EXPECT_EQ(owned, nullptr);
    finishCpu(object, backing, &owned);
    SwitchNativeSurfaceView view = {};
    EXPECT_EQ(switch_native_surface_acquire(&surface, &view, false), 0);
    switch_native_surface_release(&view);
}
TEST_F(NativeSurface, AvoidSyncGpuReadCanQueueWithoutWaitingAndCpuReadStillAcquires)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    cpuAcquisitions = 0;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareGpu(3, &backing, &owned), 0);
    EXPECT_EQ(queuedTransfers, 1u); EXPECT_EQ(cpuAcquisitions, 0u);
    finishCpu(object, backing, &owned);
    ASSERT_EQ(prepareCpu(object, 1, &backing, &owned), 0);
    EXPECT_EQ(cpuAcquisitions, 1u); EXPECT_EQ(transfers, 1u);
    finishCpu(object, backing, &owned);
}

TEST_F(NativeSurface, ScanoutOwnsOldPhysicalFrameAfterMutablePitchDetach)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeScanout scanout = {};
    ASSERT_EQ(switch_native_scanout_acquire(object, &scanout), 0);
    EXPECT_EQ(scanout.view.native, &native);
    EXPECT_EQ(native.readers, 2u);
    EXPECT_EQ(logical.readers, 0u);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareGpu(0, &backing, &owned), 0);
    EXPECT_EQ(native.readers, 1u);
    EXPECT_NE(*scanout.view.guard.generation, scanout.view.guard.expected);
    finishCpu(object, backing, &owned);
    switch_native_scanout_release(&scanout);
    EXPECT_EQ(native.readers, 0u);
}
TEST_F(NativeSurface, ScanoutSnapshotRefusesConflictingLogicalWriterWithoutWaiting)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    switch_gpu_cpu_lease *writer = nullptr;
    ASSERT_EQ(switch_gpu_backing_conversion_reserve(&logical, kWrite, 1, &writer), 0);
    SwitchNativeScanout scanout = {};
    EXPECT_EQ(switch_native_scanout_acquire(object, &scanout), -16);
    EXPECT_EQ(scanout.lease, nullptr);
    EXPECT_EQ(scanout.view.native, nullptr);
    EXPECT_EQ(native.readers, 1u);
    EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&writer, 0), 0);
}

TEST_F(NativeSurface, PitchScanoutQueuesOnceAndPinsTheExactVicProducer)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeScanout scanout = {};
    ASSERT_EQ(switch_native_scanout_pitch(object, &scanout), 0);
    EXPECT_EQ(queuedTransfers, 1u);
    EXPECT_EQ(scanout.view.native, &logical);
    EXPECT_EQ(scanout.view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    EXPECT_EQ(scanout.view.producer.value, 22u);
    EXPECT_EQ(logical.readers, 1u);
    EXPECT_EQ(logical.writers, 0u);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 0u);
    switch_native_scanout_release(&scanout);
    EXPECT_EQ(logical.readers, 0u);
}
TEST_F(NativeSurface, ScanoutAcquireDoesNotWaitForAPublicationWaitingOnTheScreen)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    SwitchNativeScanout onScreen = {};
    ASSERT_EQ(switch_native_scanout_pitch(object, &onScreen), 0);
    ASSERT_EQ(logical.readers, 1u);
    picture.backing = &replacement;
    auto publisher = std::async(std::launch::async, [&] { return switch_native_surface_publish(&surface, &picture); });
    while (SwitchMemoryAttachment::enterExclusive(memory, true) == 0)
        SwitchMemoryAttachment::leaveExclusive(memory);
    SwitchNativeScanout next = {};
    auto display = std::async(std::launch::async, [&] { return switch_native_scanout_acquire(object, &next); });
    ASSERT_EQ(display.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(display.get(), -16);
    EXPECT_EQ(next.lease, nullptr);
    EXPECT_EQ(publisher.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    switch_native_scanout_release(&onScreen);
    EXPECT_EQ(publisher.get(), 0);
    ASSERT_EQ(switch_native_scanout_acquire(object, &next), 0);
    EXPECT_EQ(next.view.native, &replacement);
    switch_native_scanout_release(&next);
}
TEST_F(NativeSurface, PitchMutationDuringSnapshotHandoffCannotChangeTheAcceptedImage)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    duringExport = [&] { ++logical.content; };
    SwitchNativeScanout scanout = {};
    EXPECT_EQ(switch_native_scanout_pitch(object, &scanout), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(scanout.lease, nullptr);
    EXPECT_EQ(logical.readers, 0u);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 0u);
}

TEST_F(NativeSurface, GpuAdapterTransfersExactLeaseAndInvalidationGuards)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    const unsigned references = native.refs;
    switch_gpu_native_surface_view view = {};
    ASSERT_EQ(switch_native_surface_gpu_acquire(object, &view), 0);
    EXPECT_EQ(view.logical, &logical);
    EXPECT_EQ(view.native, &native);
    EXPECT_EQ(view.producer.value, 11u);
    EXPECT_EQ(view.layout.planes[0].height, 736u);
    EXPECT_EQ(view.crop_height, 720u);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(object, 0, &backing, &owned), 0);
    EXPECT_NE(*view.payload.generation, view.payload.expected);
    EXPECT_NE(*view.representation.generation, view.representation.expected);
    finishCpu(object, backing, &owned);
    switch_native_surface_gpu_release(&view);
    EXPECT_EQ(view.owner, nullptr);
    EXPECT_EQ(native.refs, references - 1);
    switch_native_surface_gpu_release(&view);
}

TEST_F(NativeSurface, OrdinaryPitchGpuViewIsInvalidatedByFirstNativePublication)
{
    switch_gpu_native_surface_view view = {};
    const unsigned references = logical.refs;
    ASSERT_EQ(switch_native_surface_gpu_acquire(object, &view), 0);
    EXPECT_EQ(view.native, &logical);
    EXPECT_EQ(view.logical, &logical);
    EXPECT_EQ(logical.refs, references + 2);
    EXPECT_EQ(logical.readers, 0u);
    EXPECT_EQ(view.payload.generation, nullptr);
    EXPECT_EQ(view.layout.planes[0].layout, uint32_t(SWITCH_GPU_BACKING_PITCH));
    EXPECT_EQ(view.layout.planes[1].offset, object->desc.planeOffset[1]);
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    EXPECT_NE(*view.representation.generation, view.representation.expected);
    switch_native_surface_gpu_release(&view);
}
TEST_F(NativeSurface, DetachedGpuViewReturnsGuardedPitchWithoutRetainingCpuOwnership)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareCpu(object, 0, &backing, &owned), 0);
    finishCpu(object, backing, &owned);
    switch_gpu_native_surface_view view = {};
    ASSERT_EQ(switch_native_surface_gpu_acquire(object, &view), 0);
    EXPECT_EQ(view.native, &logical);
    EXPECT_EQ(view.owner, nullptr);
    EXPECT_EQ(view.layout.planes[0].layout, uint32_t(SWITCH_GPU_BACKING_PITCH));
    EXPECT_EQ(logical.readers, 0u);
    EXPECT_EQ(logical.writers, 0u);
    EXPECT_EQ(*view.representation.generation, view.representation.expected);
    switch_native_surface_gpu_release(&view);
}

TEST_F(NativeSurface, WaitingPitchSnapshotCapturesTheCompletedCpuGenerationWithoutGpuWait)
{
    duringExport = [&] { ++logical.content; logical.producer = {}; };
    SwitchNativeScanout scanout = {};
    ASSERT_EQ(switch_native_scanout_pitch_wait(object, &scanout), 0);
    EXPECT_EQ(scanout.allocation.content_generation, logical.content);
    EXPECT_EQ(scanout.view.producer.engine, 0u);
    EXPECT_EQ(logical.readers, 1u);
    EXPECT_EQ(SwitchMemoryAttachment::cpuUsers(memory), 0u);
    switch_native_scanout_release(&scanout);
    EXPECT_EQ(logical.readers, 0u);
}

TEST_F(NativeSurface, PublicationReservesMetadataWithoutAcquiringCpuAccessToOldGpuContent)
{
    const auto previous = logical.producer;
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    EXPECT_EQ(gpuReservations, 1u);
    EXPECT_EQ(cpuAcquisitions, 1u); // The export shim uses this to model its immutable native reservation.
    EXPECT_EQ(logical.producer.value, previous.value);
    EXPECT_EQ(logical.producer.engine, previous.engine);
    EXPECT_EQ(logical.writers, 0u);
    EXPECT_EQ(queuedTransfers, 0u);
    EXPECT_EQ(transfers, 0u);
}

TEST_F(NativeSurface, AdmissionReservesConversionWriteOnlyWhileNativePitchIsMissing)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(memory, true), 0);
    logical.readers = 1;
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &logical), 0);
    logical.readers = 0;
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &logical), 1);
    EXPECT_EQ(transfers, 0u);
    SwitchMemoryAttachment::leaveCpu(memory);
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    ASSERT_EQ(prepareGpu(1, &backing, &owned), 0);
    finishCpu(object, backing, &owned);
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(memory, true), 0);
    logical.readers = 1;
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &logical), 1);
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kWrite, &logical), 0);
    logical.readers = 0;
    SwitchMemoryAttachment::leaveCpu(memory);
}
TEST_F(NativeSurface, AdmissionDoesNotWaitForPreparationAndRejectsAnotherBacking)
{
    ASSERT_EQ(switch_native_surface_publish(&surface, &picture), 0);
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(memory, true), 0);
    auto *payload = copy(memory);
    ASSERT_NE(payload, nullptr);
    ASSERT_EQ(beginPrepare(payload, true), 0);
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &logical), 0);
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &replacement), SWITCH_GPU_BACKING_STALE);
    endPrepare(payload);
    EXPECT_EQ(switch_native_surface_gpu_admission(object, kRead, &logical), 1);
    payload->release();
    SwitchMemoryAttachment::leaveCpu(memory);
}
