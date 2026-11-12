// SPDX-License-Identifier: MIT
// See raw_zeta.h. Method numbers: NVIDIA's class headers (kits/SwitchGM20B: Mesa's clb197.h); the target words follow
// Mesa NVK nvk_CmdBeginRendering, the state's emission nvk_flush_ds_state, the clears nvk_CmdClearAttachments (all MIT).
#include "raw_zeta.h"

#include <cstring>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../fx/fx_image.h"
#include "raw_gpu.h"
#include "raw_samples.h"

namespace raw {
namespace {
enum : uint32_t {
    Z_CLEAR_VALUE = NVB197_SET_Z_CLEAR_VALUE,
    STENCIL_CLEAR_VALUE = NVB197_SET_STENCIL_CLEAR_VALUE,
    ZT_A = NVB197_SET_ZT_A,
    STENCIL_COMPRESSION = NVB197_SET_STENCIL_COMPRESSION,
    ZT_SPARSE = NVB197_SET_ZT_SPARSE,
    ZT_SIZE_A = NVB197_SET_ZT_SIZE_A,
    ZT_SELECT = NVB197_SET_ZT_SELECT,
    ZT_LAYER = NVB197_SET_ZT_LAYER,
    Z_COMPRESSION = NVB197_SET_Z_COMPRESSION,
};

constexpr uint32_t kDepthStencilMethods[kDepthStencilRegs] = {NVB197_SET_DEPTH_TEST,
                                                              NVB197_SET_DEPTH_WRITE,
                                                              NVB197_SET_DEPTH_FUNC,
                                                              NVB197_SET_STENCIL_TEST,
                                                              NVB197_SET_STENCIL_OP_FAIL,
                                                              NVB197_SET_STENCIL_OP_ZFAIL,
                                                              NVB197_SET_STENCIL_OP_ZPASS,
                                                              NVB197_SET_STENCIL_FUNC,
                                                              NVB197_SET_STENCIL_FUNC_REF,
                                                              NVB197_SET_STENCIL_FUNC_MASK,
                                                              NVB197_SET_STENCIL_MASK,
                                                              NVB197_SET_BACK_STENCIL_OP_FAIL,
                                                              NVB197_SET_BACK_STENCIL_OP_ZFAIL,
                                                              NVB197_SET_BACK_STENCIL_OP_ZPASS,
                                                              NVB197_SET_BACK_STENCIL_FUNC,
                                                              NVB197_SET_BACK_STENCIL_FUNC_REF,
                                                              NVB197_SET_BACK_STENCIL_MASK,
                                                              NVB197_SET_BACK_STENCIL_FUNC_MASK};

uint32_t floatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
}  // namespace

bool zetaShape(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t, uint64_t &bytes,
               uint64_t &align)
{
    const uint32_t i = zetaFormatIndex(format);
    if (i == kZetaFormats)
        return false;
    const uint32_t bpp = kZetaFormatInfo[i].bpp;
    fx::MipChain c;
    if (samples > 1 ? !fx::msLayout(width, height, samples, bpp, c) : !fx::mipChainLayout(width, height, 1, bpp, c))
        return false;
    const fx::MipLevel &l = c.level[0];
    t = Texture();
    t.width = width;
    t.height = height;
    t.format = format;
    t.samples = uint8_t(samples);
    t.bpp = bpp;
    t.pitch = l.rowBytes;
    t.blockLinear = true;
    t.blockHeight = l.yLog2;
    t.layerBytes = uint64_t(l.rowBytes) * l.rows;  // NIL's array stride: one level, whole tiles already
    t.zeta = true;
    bytes = c.bytes;
    align = c.align;
    return true;
}

void zetaTargetWords(Push &p, const Texture *z)
{
    if (!z) {
        p.m(ZT_SELECT, 0);
        return;
    }
    const ZetaTargetValues v = zetaTargetValues(*z, sampleRows(*z));
    p.incArray(ZT_A, v.a, 5);
    p.m(ZT_SELECT, 1);
    p.incArray(ZT_SIZE_A, v.size, 3);
    p.m(ZT_LAYER, 0);
    p.m(Z_COMPRESSION, z->comptags);
    p.m(STENCIL_COMPRESSION, z->comptags);
    p.m(ZT_SPARSE, 0);
}

bool DepthStencilDesc::active() const
{
    return depthCompare != 7 || depthWrite || !stencilNoop(front) || !stencilNoop(back);
}

bool DepthStencilRegs::operator==(const DepthStencilRegs &o) const
{
    return !memcmp(v, o.v, sizeof v);
}

void emitDepthStencil(Push &p, const DepthStencilRegs &want, DepthStencilRegs &held)
{
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        if (want.v[i] != held.v[i])
            p.m(kDepthStencilMethods[i], want.v[i]);
    held = want;
}

void zetaClearValues(Push &p, bool depth, float clearDepth, bool stencil, uint32_t clearStencil)
{
    if (depth)
        p.m(Z_CLEAR_VALUE, floatBits(clearDepth));
    if (stencil)
        p.m(STENCIL_CLEAR_VALUE, clearStencil & 0xff);
}

}  // namespace raw
