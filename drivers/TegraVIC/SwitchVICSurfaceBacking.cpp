// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "TegraVICSurface.h"
#include "TegraVIC.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

namespace {
void releaseSurface(void *context)
{
    auto *surface = static_cast<TegraVICSurface *>(context);
    tegra_vic_surface_release(surface);
    IODelete(surface, TegraVICSurface, 1);
}
}  // namespace

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *surface)
{
    switch_gpu_backing *backing = nullptr;
    const uint64_t length = surface->memory->getLength();
    if (!length || length > kTegraVICWindowBytes)
        return nullptr;
    const uint32_t count = static_cast<uint32_t>((length + 4095) >> 12);
    auto *owner = IONew(TegraVICSurface, 1);
    uint64_t *pages = IONew(uint64_t, count);
    if (!owner || !pages) {
        if (owner)
            IODelete(owner, TegraVICSurface, 1);
        if (pages)
            IODelete(pages, uint64_t, count);
        return nullptr;
    }
    *owner = *surface;
    owner->prepared = false;
    owner->surface->retain();
    uint32_t actual = 0;
    if (tegra_vic_surface_prepare(owner, length, pages, count, &actual) == kIOReturnSuccess && actual == count) {
        backing = switch_gpu_backing_register(pages, count, owner->id, owner, releaseSurface);
    }
    IODelete(pages, uint64_t, count);
    if (!backing)
        releaseSurface(owner);
    return backing;
}
