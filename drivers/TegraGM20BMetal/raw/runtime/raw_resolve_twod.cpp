// SPDX-License-Identifier: MIT
// The 2D engine's blit-encoder work outside the probes. Rule resolve.twod (raw_fetch_rungs.h twodResolveAcceptable): an MSAA resolve as one 2D-engine scaled blit. The
// multisampled image is NIL's image of its sample grid (fx::msLayout: 2x is 2x1 samples a pixel, 4x 2x2), so the grid read
// at 2:1 bilinear, corner origin at half a step, gives each destination texel the mean of its samples. The blit
// encoder's opening wait orders the read after the pass that rendered the samples, as for every blit.
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h"
#include "../fx/fx_image.h"
#include "../fx/fx_mip.h"
#include "raw_gpu.h"
#include "raw_kit.h"

namespace raw {

namespace {

switch_gm20b_2d_surface gridSurface(const Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    switch_gm20b_2d_surface s = {};
    s.address = t.va;
    s.format = fmt2d;
    s.pitch = !t.blockLinear;
    s.pitch_bytes = t.pitch;
    s.block_height_log2 = t.blockHeight;
    s.width = width;
    s.height = height;
    s.compressed = t.comptags;
    return s;
}

}  // namespace

void BlitEncoder::resolveTwod(const Texture &ms, uint32_t sxLog2, uint32_t syLog2, const Texture &into, uint32_t fmt2d)
{
    room(SWITCH_GM20B_2D_SCALE_WORDS);
    cb_->use(ms, false);
    cb_->use(into, true);
    const switch_gm20b_2d_surface grid = gridSurface(ms, fmt2d, ms.width << sxLog2, ms.height << syLog2);
    const switch_gm20b_2d_surface out = gridSurface(into, fmt2d, into.width, into.height);
    const uint64_t du = (1ull << 32) << sxLog2, dv = (1ull << 32) << syLog2;
    const switch_gm20b_2d_scale_op op = {0, 0, into.width, into.height, du, dv, du / 2, dv / 2, true, true};
    kit(p_, switch_gm20b_2d_scale, &grid, &out, &op);
}

void BlitEncoder::fillTwod(const Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t word)
{
    room(SWITCH_GM20B_2D_FILL_WORDS);
    cb_->use(t, true);
    const uint32_t unorm =
        t.format == 80 || t.format == 81 ? NV902D_SET_DST_FORMAT_V_A8R8G8B8 : NV902D_SET_DST_FORMAT_V_A8B8G8R8;
    const switch_gm20b_2d_surface s = gridSurface(t, unorm, t.width, t.height);
    kit(p_, switch_gm20b_2d_fill, &s, word, x0, y0, x1, y1);
}

bool CommandBuffer::resolveTwod(const Texture &multisampled, const Texture &into)
{
    fx::MipChain grid;
    const uint32_t fmt2d = fx::twodFormat(into.format);
    if (!fmt2d || into.format != multisampled.format || into.samples > 1 ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid) ||
        grid.sxLog2 > 1 || grid.syLog2 > 1 || (!grid.sxLog2 && !grid.syLog2) || multisampled.width != into.width ||
        multisampled.height != into.height)
        return false;
    BlitEncoder *b = blit();
    if (!b)
        return false;
    b->resolveTwod(multisampled, grid.sxLog2, grid.syLog2, into, fmt2d);
    b->end();
    RAW_DIAGNOSTIC(dev_->stats.resolveTwod++);
    return true;
}

}  // namespace raw
