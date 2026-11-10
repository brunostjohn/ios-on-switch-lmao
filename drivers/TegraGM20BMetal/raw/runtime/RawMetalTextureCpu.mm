// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "../fx/fx_image.h"

bool transferTexture(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice,
                     void *bytes, NSUInteger row, NSUInteger image, bool write)
{
    if (!texture->_rawOk || texture->_kind == RMTextureBuffer || texture->_kind == RMTextureView ||
        (texture->_heap && texture->_heap->_descriptor.storageMode == MTLStorageModeShared))
        return false;
    if (!region.size.width || !region.size.height || !region.size.depth)
        return true;
    for (const NSUInteger value : {region.origin.x, region.origin.y, region.origin.z,
                                   region.size.width, region.size.height, region.size.depth, level, slice})
        if (value > UINT32_MAX)
            return false;
    const uint32_t origin[] = {uint32_t(region.origin.x), uint32_t(region.origin.y), uint32_t(region.origin.z)};
    const uint32_t size[] = {uint32_t(region.size.width), uint32_t(region.size.height), uint32_t(region.size.depth)};
    if (write)
        declareGpuWrites(texture);
    auto owner = std::shared_ptr<void>((__bridge_retained void *)texture, [](void *held) { CFRelease(held); });
    const auto result = gRM->raw->transferTexture(texture->_raw, uint32_t(level), uint32_t(slice), origin, size,
                                                 bytes, row, image, write, std::move(owner));
    // A failed write published nothing and a failed read touched only its staging: the CPU paths take either over
    if (result != raw::TextureTransferResult::Complete)
        return false;
    if (write)
        texture->_gpuDirty.store(true, std::memory_order_release);
    return true;
}

void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(from) & ~uintptr_t(63); a < uintptr_t(from) + bytes; a += 64)
        if (invalidate)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)from;
    (void)bytes;
    (void)invalidate;
#endif
}

const tgm_layout *textureCpuImportLayout(RMTexture *texture)
{
    while (texture) {
        if (texture->_raw.import && texture->_raw.import->kind == TGM_LAYOUT_KIND)
            return &texture->_raw.import->layout;
        texture = texture->_kind == RMTextureView ? texture->_parent : nil;
    }
    return nullptr;
}

// The caller holds CPU backing ownership through every read or partial write.
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    tgm_layout l;
    if (!r.size.width || !r.size.height)
        return;
    if (const auto *import = textureCpuImportLayout(t))
        l = *import;
    else if (!tgm_layout_tiled(t->_raw.width, t->_raw.height, t->_raw.bpp, &l))
        return;
    r.origin.x += t->_raw.surfaceX;
    r.origin.y += t->_raw.surfaceY;
    if (write) {
        tgm_layout_write(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                         uint32_t(r.size.height), bytes, row);
        return;
    }
    tgm_layout_read(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                    uint32_t(r.size.height), bytes, row);
}

// replaceRegion/getBytes of level `level` of a shared mipmapped texture: in its chain, in the CPU's view of kind-0xFE memory
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture::Chain &c = *t->_raw.chain;
    if (level >= c.layout.levels)
        return;
    const raw::fx::MipLevel &lv = c.layout.level[level];
    if (r.origin.x + r.size.width > lv.width || r.origin.y + r.size.height > lv.height)
        return;
    uint8_t *chain = t->_raw.cpu;
    if (write) {
        raw::fx::writeLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                            uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
    } else {
        raw::fx::readLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                           uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
    }
}

// replaceRegion/getBytes of a box of a 3D texture: in NIL's 3D image (fx_image.h), in the CPU's view of kind-0xFE memory
void cpuVolume(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!raw::fx::volumeLayout(x.width, x.height, x.depth, 1, x.bpp, !x.blockDepth, c))
        return;
    const uint32_t origin[3] = {uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.origin.z)},
                   size[3] = {uint32_t(r.size.width), uint32_t(r.size.height), uint32_t(r.size.depth)};
    const size_t planeBytes = image ? image : size_t(row) * r.size.height;
    if (write) {
        raw::fx::writeVolume(c, x.cpu, 0, origin, size, bytes, row, planeBytes);
        return;
    }
    raw::fx::readVolume(c, x.cpu, 0, origin, size, bytes, row, planeBytes);
}

// replaceRegion/getBytes of a rectangle of a layer of a 2D array or cube: the layer's 2D image, arrayStride on
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!raw::fx::arrayLayout(x.width, x.height, x.layers, 1, x.bpp, c))
        return;
    uint8_t *layer = x.cpu + slice * c.arrayStride;
    if (write) {
        raw::fx::writeLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                            uint32_t(r.size.height), bytes, row);
        return;
    }
    raw::fx::readLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                       uint32_t(r.size.height), bytes, row);
}
