// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchVICPicture.h"
#include "TegraVICSurface.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchNativeSurfaceCrop {
    uint32_t x, y, width, height;
};

struct SwitchNativeSurfaceView {
    OSObject *owner;
    IOMemoryDescriptor *descriptor;
    switch_gpu_backing *logical;
    switch_gpu_backing *native;
    switch_gpu_backing_guard guard;
    switch_gpu_backing_guard representation;
    switch_gpu_backing_token producer;
    switch_gpu_backing_layout layout;
    SwitchNativeSurfaceCrop crop;
};

// A view retains both backings. Every engine read submits both uses and both guards in one checked ownership transaction.
// avoid: -16 instead of waiting for a publication in progress, which may itself wait for leases the caller holds.
int switch_native_surface_acquire(const TegraVICSurface *surface, SwitchNativeSurfaceView *view, bool avoid);
void switch_native_surface_release(SwitchNativeSurfaceView *view);
// why: on an error, the refusal (a literal) for the caller's one line. fresh: on success, whether the publication
// needed a new surface object rather than a retired one.
int switch_native_surface_publish(const TegraVICSurface *surface, const TegraVICPicture *picture,
                                  const char **why = nullptr, bool *fresh = nullptr);

// The caller holds a descriptor CPU reservation through the ensuing IOSurface CPU lock and unlock.
extern "C" int switch_native_surface_prepare(OSObject *surface, uint32_t id, void *task, uint32_t options,
                                              switch_gpu_backing **logical, switch_gpu_cpu_lease **owned);

// The descriptor reservation and returned logical lease cover checked consumer publication; conversion stays on hardware.
extern "C" int switch_native_surface_prepare_gpu(OSObject *surface, uint32_t id, void *task, uint32_t options,
                                                  switch_gpu_backing **logical, switch_gpu_cpu_lease **owned);

// The caller holds the descriptor CPU reservation; this query neither converts nor waits for GPU work.
extern "C" int switch_native_surface_gpu_admission(OSObject *surface, uint32_t access,
                                                    switch_gpu_backing *expected);

#if DEVELOPMENT || DEBUG
// Since boot: native publications, and how many of them needed a new surface object (the rest reused a retired one).
extern "C" void switch_native_surface_publications(uint64_t *published, uint64_t *allocated);
#endif
