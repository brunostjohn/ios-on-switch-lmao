// SPDX-License-Identifier: MIT
// See raw_multisample.h and Device::createStoredMultisample, createOwnedZeta (raw_gpu.h).
#include <algorithm>
#include "raw_gpu.h"
#include "raw_multisample.h"

namespace raw {

bool Device::createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t)
{
    fx::MipChain c;
    if (!shape(width, height, format, t) || !t.renderable || blockCompressed(formatInfo(format)) ||
        xrForm(format) != XrForm::None || !fx::msLayout(t.width, t.height, samples, t.bpp, c))
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!blAlloc(c.bytes, c.align, t.span))
        return false;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        return false;
    }
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
    t.samples = uint8_t(samples);
    writeTic(t);
    return true;
}

bool Device::createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled,
                             Texture &t)
{
    uint64_t bytes = 0, align = 0;
    if (samples < 2 || !zetaShape(width, height, format, samples, t, bytes, align))
        return false;
    const uint32_t z = zetaFormatIndex(format);
    std::lock_guard<std::mutex> l(lock_);
    Span span;
    if (!tiledAlloc(zChunks_[z], kZetaFormatInfo[z].kind, bytes, std::max(align, kBlAlign), span))
        return false;
    if (sampled && !(t.tic = allocTic())) {
        tiledFree(zChunks_[z], span);
        return false;
    }
    t.va = span.va;
    t.storage = {span.va, bytes, false};
    ownedZeta_[span.va] = {z, span};
    if (t.tic)
        writeTic(t);
    return true;
}

}  // namespace raw
