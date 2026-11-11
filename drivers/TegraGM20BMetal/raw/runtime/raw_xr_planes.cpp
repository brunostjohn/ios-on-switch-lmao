// SPDX-License-Identifier: MIT
// See raw_xr_planes.h.
#include "raw_xr_planes.h"

#include "raw_gpu.h"

namespace raw {

bool Device::createXrPlanesTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                   uint64_t word, uint32_t wordPitch, uint64_t alpha, uint32_t alphaPitch,
                                   uint32_t width, uint32_t height, Texture &t)
{
    constexpr uint32_t kR8Unorm = 10, kRG8Unorm = 30, kRGB10A2Unorm = 90, kXr = 550;
    Texture *w = new Texture, *a = new Texture, *temp = new Texture;
    const bool made =
        createTexture(width, height, kXr, t, false, true) &&
        createSurfaceTexture(surfaceId, base, allocBytes, surface, word, width, height, wordPitch, kRGB10A2Unorm, *w) &&
        createSurfaceTexture(surfaceId, base, allocBytes, surface, alpha, width, height, alphaPitch, kR8Unorm, *a) &&
        !w->blockLinear && !a->blockLinear && createTexture(width, height, kRG8Unorm, *temp);
    if (!made) {
        for (Texture *x : {w, a, temp}) {
            destroyTexture(*x);
            delete x;
        }
        destroyTexture(t);
        return false;
    }
    t.luma = w;
    t.chroma = a;
    t.stencilTemp = temp;
    t.cpu = nullptr;  // Metal's CPU access reads and writes the planes: the surface's texture on m2v
    return true;
}

void BlitEncoder::xrPlanes(const Texture &t, bool split)
{
    const Texture &word = *t.luma, &alpha = *t.chroma, &temp = *t.stencilTemp;
    cb_->use(word, split);
    cb_->use(alpha, split);
    cb_->use(t, !split);
    cb_->use(temp, true);
    RAW_DIAGNOSTIC(dev_->stats.ceOps += 3);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(t.width) * t.height * xrPlaneBytes(split));
    room(kXrPlaneWords);
    const CopySurface image =
        t.blockLinear ? CopySurface{t.va, t.pitch, 0, 0, t.height, true, t.blockHeight} : CopySurface{t.va, t.pitch};
    emitXrPlanes(
        p_,
        {image, CopySurface{word.va, word.pitch}, CopySurface{alpha.va, alpha.pitch}, CopySurface{temp.va, temp.pitch}},
        t.width, t.height, split);
}

}  // namespace raw
