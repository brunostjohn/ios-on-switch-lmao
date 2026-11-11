// SPDX-License-Identifier: MIT
// See raw_turn_copy.h.
#include "raw_turn_copy.h"

#include <cstring>

#include "raw_gpu.h"

namespace raw {

bool CommandBuffer::turnCopy(const Texture &dst, Texture &src, const TurnMap &map, const char **why)
{
    static const DepthStencilState depthWrites = [] {
        DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return DepthStencilState(desc);
    }();
    const uint32_t format = dst.zeta ? dst.format : turnCopyFormat(dst.bpp);
    const char *state = dst.zeta ? turnDepthState(dst.format) : turnCopyState(dst.bpp);
    const FormatInfo *f = dst.zeta ? nullptr : formatInfo(format);
    Pipeline *pl = state ? dev_->pipeline(Pack::fnv(state, strlen(state))) : nullptr;
    if (!pl || (!dst.zeta && !f) || src.bpp != dst.bpp || src.zeta != dst.zeta) {
        *why = "no turn copy state for the attachment's format";
        return false;
    }
    if (!dev_->sampleable(src)) {
        *why = "no texture header for a turn copy's source";
        return false;
    }
    Texture target = dst, source = src;
    target.format = source.format = format;
    target.rotation = 0;
    if (!(source.tic = dst.zeta || src.format == format ? src.tic : dev_->viewTic(src, f->tic0))) {
        *why = "no texture header for a turn copy's source";
        return false;
    }
    PassDesc p;
    if (dst.zeta) {
        p.depthStencil.texture = &target;
    } else {
        p.color[0].texture = &target;
        p.color[0].load = ColorAttachment::DontCare;
        p.count = 1;
    }
    RenderEncoder *e = render(p);
    if (!e) {
        *why = "a turn copy's target the raw path cannot render";
        return false;
    }
    e->setPipeline(pl);
    if (dst.zeta)
        e->setDepthStencil(&depthWrites);
    e->setTexture(0, &source);
    e->setBytes(2, 0, &map, sizeof map);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    e->end();
    return true;
}

bool Device::sampleable(Texture &t)
{
    std::lock_guard<std::mutex> l(lock_);
    if (t.tic)
        return true;
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

Texture *CommandBuffer::turnScratch(uint32_t format, uint32_t width, uint32_t height, uint32_t tag)
{
    Texture m;
    uint64_t bytes = 0, align = 0;
    const bool zeta = zetaFormatIndex(format) < kZetaFormats;
    if (zeta ? !zetaShape(width, height, format, 1, m, bytes, align) : !shape(width, height, format, m))
        return nullptr;
    m.memoryless = true;
    std::lock_guard<std::mutex> l(dev_->lock_);
    Texture *b = dev_->backing(m, tag, true, false);
    if (b)
        backings_.push_back(b);
    return b;
}

}  // namespace raw
