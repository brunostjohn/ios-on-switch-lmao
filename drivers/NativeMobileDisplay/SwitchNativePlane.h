// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativePlanes.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

inline bool switch_native_plane_image(const native_planes::Layer &layer, uint32_t width, uint32_t height,
                                      const switch_gpu_backing_layout &layout, uint32_t cropX, uint32_t cropY,
                                      native_planes::Image &image)
{
    using namespace native_planes;
    if (!isNv12(layer.kind) || layout.format != layer.surface.format || layout.plane_count != 2 || cropX || cropY ||
        layer.src.x || layer.src.y || layer.src.w != int32_t(layer.surface.width) ||
        layer.src.h != int32_t(layer.surface.height) || layer.dst.x < 0 || layer.dst.y < 0 ||
        int64_t(layer.dst.x) + layer.dst.w > width || int64_t(layer.dst.y) + layer.dst.h > height ||
        layout.color_matrix > 3 || layout.color_range != (layer.kind == kKindNv12Full ? 2u : 1u))
        return false;
    Layer native = layer;
    native.surface.colorMatrix = layout.color_matrix;
    native.surface.transfer = layout.transfer; native.surface.primaries = layout.primaries;
    native.surface.chromaTop = layout.chroma_top; native.surface.chromaBottom = layout.chroma_bottom;
    if (!imageOf(native, width, height, image) || !image.visible || image.hOffset ||
        image.start[0] != layer.surface.offset[0] || image.start[1] != layer.surface.offset[1])
        return false;
    const auto &luma = layout.planes[0], &chroma = layout.planes[1];
    if (luma.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR || chroma.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR ||
        luma.block_height_log2 > 5 || luma.block_height_log2 != chroma.block_height_log2 ||
        luma.bytes_per_element != 1 || chroma.bytes_per_element != 2 ||
        luma.width < layer.surface.width || luma.height < layer.surface.height ||
        uint64_t(chroma.width) * 2 < layer.surface.width || uint64_t(chroma.height) * 2 < layer.surface.height)
        return false;
    uint64_t low = UINT64_MAX, high = 0;
    for (unsigned plane = 0; plane < 2; ++plane) {
        const auto &p = layout.planes[plane];
        const uint64_t rows = 8ULL << p.block_height_log2;
        if (!p.pitch || p.pitch > UINT16_MAX || (p.pitch & 63) || (p.offset & 255) ||
            uint64_t(p.width) * p.bytes_per_element > p.pitch || p.height % rows ||
            p.bytes < uint64_t(p.pitch) * p.height || p.bytes > UINT64_MAX - 4095 ||
            p.offset > UINT64_MAX - p.bytes - 4095)
            return false;
        image.start[plane] = p.offset;
        if (p.offset < low) low = p.offset;
        if (p.offset + p.bytes > high) high = p.offset + p.bytes;
    }
    if (luma.offset + luma.bytes > chroma.offset && chroma.offset + chroma.bytes > luma.offset)
        return false;
    image.readLow = low & ~4095ULL;
    image.readHigh = (high + 4095) & ~4095ULL;
    image.lineStride = (chroma.pitch << 16) | luma.pitch;
    image.surfaceKind = uint8_t(kSurfaceKindBlock | (luma.block_height_log2 << kSurfaceKindBlockHeightShift));
    return true;
}
