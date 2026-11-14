// SPDX-License-Identifier: MIT
// A draw's fixed-function shortcut behind its rule (raw_quad_fill.h): it
// reads only what the CPU owns (setVertexBytes and setFragmentBytes uploads, indices under the command buffer's lease)
// and takes the draw's own route where anything it needs is missing.
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"
#include "raw_ca_route.h"
#include "raw_draw_hooks.h"
#include "raw_gpu.h"
#include "raw_kit.h"
#include "raw_quad_fill.h"

namespace raw {

const char *Device::packKey(const PackPipeline &rec) const
{
    const PackPipeline *packed = pack_.section<PackPipeline>(PackPipelines);
    const bool inPack = packed && &rec >= packed && &rec < packed + pack_.bytes(PackPipelines) / sizeof(PackPipeline);
    return inPack ? pack_.string(rec.keyString) : nullptr;
}

// The vertex stream from the draw's first vertex where setVertexBytes uploaded it, with the state's position and colour
bool RenderEncoder::cpuStream(const Pipeline &pl, uint32_t first, FillStream &s) const
{
    const PackPipeline &rec = *pl.rec;
    const Bound &stream = bufs_[1][rec.vertexSlot < 31 ? rec.vertexSlot : 0];
    const uint64_t skip = uint64_t(first) * rec.vertexStride;
    if (stream.buffer || !stream.cpu || !rec.vertexStride || skip >= stream.size || !pl.positionComponents)
        return false;
    s.bytes = stream.cpu + skip;
    s.size = stream.size - skip;
    s.stride = rec.vertexStride;
    s.positionOffset = pl.positionOffset;
    s.positionComponents = pl.positionComponents;
    s.colorOffset = fillColorOffset(dev_->packKey(rec));
    return true;
}

// The CPU's copy of the draw's first `count` indices where its lease covers them
bool RenderEncoder::cpuIndices(uint32_t count, uint32_t out[6]) const
{
    if (!drawIndices_ || count > 6 ||
        (!drawIndexCpuSafe_ && (!drawIndexBuffer_ || !cb_->acquireGeometry(*drawIndexBuffer_, drawIndices_,
                                                                           uint64_t(count) * drawIndexSize_))))
        return false;
    for (uint32_t i = 0; i < count; i++)
        out[i] = drawIndexSize_ == 2 ? reinterpret_cast<const uint16_t *>(drawIndices_)[i]
                                     : reinterpret_cast<const uint32_t *>(drawIndices_)[i];
    return true;
}

RenderEncoder::Shortcut RenderEncoder::shortcut(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count,
                                                bool indexed)
{
    Shortcut out;
    if (!fillViaTriangleBox() || CaRoute(pl.caRoute) != CaRoute::Fill || !pl.rec)
        return out;
    FillStream s;
    if (!cpuStream(pl, indexed ? 0 : first, s))
        return out;
    uint32_t list[6];
    const bool quadList = primitive == 3 && count == 6, quadStrip = primitive == 4 && count == 4;
    if (indexed && (!quadList || !cpuIndices(6, list)))
        return out;
    if (!indexed && quadList)
        for (uint32_t i = 0; i < 6; i++)
            list[i] = i;
    const Bound &matrix = bufs_[1][2];
    const uint8_t *m = !matrix.buffer && matrix.cpu && matrix.size >= 64 ? matrix.cpu : nullptr;
    uint32_t corners[4] = {0, 1, 2, 3};
    float window[8];
    if (fillViaTriangleBox() && (quadStrip || (quadList && quadOfIndices(list, corners))) &&
        quadWindow(s, corners, m, vp_, window)) {
        const float swapped[8] = {window[0], window[1], window[4], window[5],
                                  window[2], window[3], window[6], window[7]};
        out.bbox = axisAlignedQuad(window) || axisAlignedQuad(swapped);
    }
    return out;
}

void RenderEncoder::shortcutBegin(const Shortcut &s)
{
    if (s.bbox)
        kit(p_, switch_gm20b_fill_via_triangle, uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_FILL_BBOX));
}

void RenderEncoder::shortcutEnd(const Shortcut &s)
{
    if (s.bbox)
        kit(p_, switch_gm20b_fill_via_triangle, uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_DISABLED));
}

}  // namespace raw
