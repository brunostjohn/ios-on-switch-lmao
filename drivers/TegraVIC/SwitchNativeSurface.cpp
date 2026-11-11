// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <switch/memory_attachment.h>
#include "SwitchNativePicture.h"
#include "TegraVICPresent.h"
#include "SwitchVICPicturePlan.h"
#include "SwitchVICSurfaceRange.h"
#include "../TegraGM20B/TegraGM20BViews.h"
#include "../TegraGM20B/SwitchGpuAdmission.h"

extern "C" IOMemoryDescriptor *tegra_vic_ios_memory(OSObject *surface);

class SwitchNativeSurface : public OSObject {
    OSDeclareDefaultStructors(SwitchNativeSurface)
public:
    bool initialize() { return init(); }
    switch_gpu_backing *native = nullptr;
    switch_gpu_cpu_lease *readLease = nullptr;
    switch_gpu_backing_token producer = {};
    TegraVICPicture picture = {};
    TegraVICSurfaceDesc pitch = {};
    switch_gpu_backing_layout layout = {};
    SwitchNativeSurfaceCrop crop = {};
    alignas(8) volatile uint64_t generation = 1;
    bool pitchValid = false;
    IOLock *prepareLock = nullptr;
    bool preparing = false;
    void free() override;
};
OSDefineMetaClassAndStructors(SwitchNativeSurface, OSObject)

void SwitchNativeSurface::free()
{
    if (readLease)
        switch_gpu_backing_cpu_release_owned(&readLease, 0);
    if (native)
        switch_gpu_backing_release(native);
    if (prepareLock)
        IOLockFree(prepareLock);
    OSObject::free();
}

namespace {
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t kNoCache = SWITCH_GPU_BACKING_CPU_NO_CACHE, kAvoidSync = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kConcurrent = SWITCH_GPU_BACKING_CPU_CONCURRENT;

SwitchNativeSurface *copy(IOMemoryDescriptor *memory)
{
    auto *object = SwitchMemoryAttachment::copy(memory);
    auto *native = OSDynamicCast(SwitchNativeSurface, object);
    if (object && !native)
        object->release();
    return native;
}

bool active(const SwitchNativeSurface *native)
{
    return __atomic_load_n(&native->generation, __ATOMIC_ACQUIRE) == 1;
}

bool matches(const TegraVICSurface *surface, const SwitchNativeSurface *native)
{
    const auto &a = surface->desc;
    const auto &b = native->pitch;
    return !surface->baseOffset && a.fourcc == b.fourcc && a.width == b.width && a.height == b.height &&
           a.planes == b.planes && a.length == b.length && a.protection == b.protection &&
           !memcmp(a.planeOffset, b.planeOffset, sizeof(a.planeOffset)) &&
           !memcmp(a.planeBytesPerRow, b.planeBytesPerRow, sizeof(a.planeBytesPerRow)) &&
           !memcmp(a.planeWidth, b.planeWidth, sizeof(a.planeWidth)) &&
           !memcmp(a.planeHeight, b.planeHeight, sizeof(a.planeHeight));
}

TegraVICSurface canonical(const TegraVICSurface *surface, const SwitchNativeSurface *native)
{
    auto result = *surface;
    result.baseOffset = 0;
    result.desc = native->pitch;
    result.prepared = false;
    return result;
}

int acquire(switch_gpu_backing *backing, uint32_t access, switch_gpu_cpu_lease **owned)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    return switch_gpu_backing_cpu_acquire_owned(backing, 0, uint64_t(info.page_count) << 12, access | kNoCache, owned);
}

int advanceRepresentation(IOMemoryDescriptor *memory, switch_gpu_backing *logical, switch_gpu_cpu_lease *owned)
{
    auto *word = SwitchMemoryAttachment::generation(memory);
    if (!word)
        return -12;
    const uint64_t previous = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if (previous == UINT64_MAX)
        return -75;
    return switch_gpu_backing_cpu_publish_generation(logical, owned, word, previous, previous + 1);
}

int invalidate(SwitchNativeSurface *native, switch_gpu_backing *logical, switch_gpu_cpu_lease *owned)
{
    if (!active(native))
        return 0;
    const int error = switch_gpu_backing_cpu_publish_generation(logical, owned, &native->generation, 1, 2);
    if (!error && native->readLease)
        switch_gpu_backing_cpu_release_owned(&native->readLease, 0);
    return error;
}

int beginPrepare(SwitchNativeSurface *native, bool avoid)
{
    IOLockLock(native->prepareLock);
    while (native->preparing && !avoid)
        IOLockSleep(native->prepareLock, native, THREAD_UNINT);
    const bool admitted = !native->preparing;
    if (admitted)
        native->preparing = true;
    IOLockUnlock(native->prepareLock);
    return admitted ? 0 : -16;
}

void endPrepare(SwitchNativeSurface *native)
{
    IOLockLock(native->prepareLock);
    native->preparing = false;
    IOLockWakeup(native->prepareLock, native, false);
    IOLockUnlock(native->prepareLock);
    switch_gpu_admission_changed();
}

int prepare(const TegraVICSurface *surface, SwitchNativeSurface *native, uint32_t options, bool gpu,
            switch_gpu_backing **logical, switch_gpu_cpu_lease **owned)
{
    int error = beginPrepare(native, options & 2);
    if (error)
        return error;
    auto pitch = canonical(surface, native);
    auto *backing = switch_vic_surface_backing(&pitch);
    if (!backing) {
        endPrepare(native);
        return -12;
    }
    const bool mutableAccess = !(options & 1);
    const uint32_t avoid = options & 2 ? kAvoidSync : 0;
    const bool exclusive = active(native) && (mutableAccess || !__atomic_load_n(&native->pitchValid, __ATOMIC_ACQUIRE));
    const uint32_t access = exclusive ? kWrite : (mutableAccess ? kWrite : kRead) | kConcurrent;
    error = gpu ? switch_gpu_backing_conversion_reserve(backing, exclusive || mutableAccess ? kWrite : kRead,
                                                        avoid != 0, owned)
                : acquire(backing, access | avoid, owned);
    if (!error && active(native) && !__atomic_load_n(&native->pitchValid, __ATOMIC_ACQUIRE)) {
        if (avoid && !gpu) {
            error = -16;
        } else {
            auto picture = native->picture;
            picture.surfaceID = surface->id;
            const char *why = nullptr;
            error = gpu ? switch_vic_present_surface_queue(&picture, &pitch, *owned, &why)
                        : switch_vic_present_surface_owned(&picture, &pitch, *owned, &why);
            if (!error)
                __atomic_store_n(&native->pitchValid, true, __ATOMIC_RELEASE);
        }
    }
    if (!error && mutableAccess && active(native)) {
        error = advanceRepresentation(surface->memory, backing, *owned);
        if (!error)
            error = invalidate(native, backing, *owned);
    }
    if (error) {
        if (*owned)
            switch_gpu_backing_cpu_release_owned(owned, 0);
        switch_gpu_backing_release(backing);
    } else {
        *logical = backing;
    }
    endPrepare(native);
    return error;
}
}  // namespace

int switch_native_surface_acquire(const TegraVICSurface *surface, SwitchNativeSurfaceView *view, bool avoid)
{
    if (!view)
        return -22;
    *view = {};
    if (!surface || !surface->surface || !surface->memory)
        return -22;
    const int entered = SwitchMemoryAttachment::enterCpu(surface->memory, avoid);
    if (entered)
        return entered;
    auto *native = copy(surface->memory);
    if (!native) {
        switch_gpu_surface_finish(surface->memory);
        return -2;
    }
    int error = !matches(surface, native) ? -95 : !active(native) ? SWITCH_GPU_BACKING_STALE : 0;
    auto *representation = error ? nullptr : SwitchMemoryAttachment::generation(surface->memory);
    auto *logical = error || !representation ? nullptr : switch_vic_surface_backing(surface);
    if (!error && !logical)
        error = -12;
    if (error) {
        native->release();
        switch_gpu_surface_finish(surface->memory);
        return error;
    }
    switch_gpu_backing_retain(native->native);
    surface->memory->retain();
    *view = {native, surface->memory, logical, native->native, {logical, &native->generation, 1},
             {logical, representation, __atomic_load_n(representation, __ATOMIC_ACQUIRE)},
             native->producer, native->layout, native->crop};
    view->layout.color_matrix = surface->desc.colorMatrix;
    view->layout.transfer = surface->desc.transfer; view->layout.primaries = surface->desc.primaries;
    view->layout.chroma_top = surface->desc.chromaTop; view->layout.chroma_bottom = surface->desc.chromaBottom;
    switch_gpu_surface_finish(surface->memory);
    return 0;
}

void switch_native_surface_release(SwitchNativeSurfaceView *view)
{
    if (!view)
        return;
    if (view->native)
        switch_gpu_backing_release(view->native);
    if (view->logical)
        switch_gpu_backing_release(view->logical);
    if (view->owner)
        view->owner->release();
    if (view->descriptor)
        view->descriptor->release();
    *view = {};
}

int switch_native_surface_publish(const TegraVICSurface *surface, const TegraVICPicture *picture, const char **why)
{
    const char *unused = nullptr;
    if (!why)
        why = &unused;
    *why = "native export into a surface or of a picture without memory";
    if (!surface || !surface->surface || !surface->memory || !picture || !picture->backing)
        return -22;
    *why = "native export into a surface at a base offset";
    if (surface->baseOffset)
        return -22;
    *why = "native export into a surface whose allocation is not its descriptor's length";
    if (!switch_vic_surface_owns_descriptor(surface->memory->getLength(), surface->baseOffset, surface->desc.length,
                                            PAGE_SIZE))
        return -22;
    *why = "native export of another surface's picture";
    if (surface->id != picture->surfaceID)
        return -22;
    switch_gpu_backing_layout layout = {};
    SwitchNativeSurfaceCrop crop = {};
    if ((*why = switch_native_picture(*picture, surface->desc, layout, crop)))
        return -95;
    TegraVICPicture bytePicture = {};
    TegraVICSurfaceDesc byteTarget = {};
    SwitchVICPlacement placement = {};
    *why = "native export without VIC to materialize it";
    if (!tegra_vic_present_ready())
        return -95;
    if ((*why = switch_vic_picture_bytes(picture, &surface->desc, &bytePicture, &byteTarget)) ||
        (*why = switch_vic_picture_place(&bytePicture, &byteTarget, &placement)))
        return -95;
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(picture->backing, &info);
    *why = "native export of a picture its backing cannot hold";
    if (!picture->bytes || picture->bytes > (uint64_t(info.page_count) << 12))
        return -22;
    *why = "native export without memory for its surface object";
    auto *next = new SwitchNativeSurface;
    if (!next || !next->initialize()) {
        if (next)
            next->release();
        return -12;
    }
    next->prepareLock = IOLockAlloc();
    if (!next->prepareLock) {
        next->release();
        return -12;
    }
    next->native = picture->backing;
    switch_gpu_backing_retain(next->native);
    next->picture = *picture;
    next->picture.pages = info.pages;
    next->picture.pageCount = info.page_count;
    next->pitch = surface->desc;
    next->layout = layout;
    next->crop = crop;
    *why = "native export the GPU backing would not lease for reading";
    int error = switch_gpu_backing_export_read(next->native, &next->producer, &next->readLease);
    if (error) {
        next->release();
        return error;
    }
    *why = "native export into a surface another publication holds";
    error = SwitchMemoryAttachment::enterExclusive(surface->memory, false);
    if (error) {
        next->release();
        return error;
    }
    auto *logical = switch_vic_surface_backing(surface);
    switch_gpu_cpu_lease *owned = nullptr;
    *why = !logical                  ? "native export into a surface without a backing of its own"
        : logical == next->native    ? "native export into a surface its picture already backs"
                                     : "native export into a surface whose backing would not reserve a conversion";
    error = !logical ? -12 : logical == next->native ? -22
        : switch_gpu_backing_conversion_reserve(logical, kWrite, 0, &owned);
    auto *old = error ? nullptr : copy(surface->memory);
    bool installed = false;
    if (!error) {
        installed = SwitchMemoryAttachment::replace(surface->memory, old, next);
        *why = installed ? "native export whose surface's representation would not advance"
                         : "native export that another publication beat to the surface";
        error = installed ? advanceRepresentation(surface->memory, logical, owned) : -16;
    }
    if (!error && old) {
        *why = "native export whose surface's earlier representation would not invalidate";
        error = invalidate(old, logical, owned);
    }
    if (error && installed && !SwitchMemoryAttachment::replace(surface->memory, next, old))
        panic("SwitchSurface: native publication rollback lost attachment");
    if (old)
        old->release();
    if (!error)
        tegra_vic_surface_written(surface);
    if (owned)
        switch_gpu_backing_cpu_release_owned(&owned, 0);
    if (logical)
        switch_gpu_backing_release(logical);
    SwitchMemoryAttachment::leaveExclusive(surface->memory);
    next->release();
    return error;
}

static int prepareSurface(OSObject *object, uint32_t id, uint32_t options, bool gpu,
                          switch_gpu_backing **logical, switch_gpu_cpu_lease **owned)
{
    if (!logical || !owned)
        return -22;
    *logical = nullptr;
    *owned = nullptr;
    auto *memory = object ? tegra_vic_ios_memory(object) : nullptr;
    auto *native = copy(memory);
    if (!native)
        return 0;
    TegraVICSurface surface = {};
    const int described = switch_vic_surface_from_object(object, id, &surface);
    const int error = described ? -22 : prepare(&surface, native, options, gpu, logical, owned);
    native->release();
    tegra_vic_surface_release(&surface);
    return error;
}

extern "C" int switch_native_surface_prepare(OSObject *object, uint32_t id, void *, uint32_t options,
                                              switch_gpu_backing **logical, switch_gpu_cpu_lease **owned)
{
    return prepareSurface(object, id, options, false, logical, owned);
}

extern "C" int switch_native_surface_prepare_gpu(OSObject *object, uint32_t id, void *, uint32_t options,
                                                  switch_gpu_backing **logical, switch_gpu_cpu_lease **owned)
{
    return prepareSurface(object, id, options, true, logical, owned);
}

extern "C" int switch_native_surface_gpu_admission(OSObject *object, uint32_t access,
                                                    switch_gpu_backing *expected)
{
    if (!object || !expected || !access || (access & ~(kRead | kWrite)))
        return -22;
    auto *memory = tegra_vic_ios_memory(object);
    auto *native = copy(memory);
    if (!native)
        return switch_gpu_backing_admission_ready(expected, access);
    TegraVICSurface surface = {};
    int result = switch_vic_surface_from_object(object, 0, &surface) ? -22 : 0;
    auto pitch = canonical(&surface, native);
    auto *logical = result ? nullptr : switch_vic_surface_backing(&pitch);
    if (!result && logical != expected)
        result = logical ? SWITCH_GPU_BACKING_STALE : -12;
    if (!result) {
        IOLockLock(native->prepareLock);
        const bool write = (access & kWrite) ||
            (active(native) && !__atomic_load_n(&native->pitchValid, __ATOMIC_ACQUIRE));
        result = native->preparing ? 0 : switch_gpu_backing_conversion_ready(logical, write ? kWrite : kRead);
        IOLockUnlock(native->prepareLock);
    }
    if (logical)
        switch_gpu_backing_release(logical);
    native->release();
    tegra_vic_surface_release(&surface);
    return result;
}
