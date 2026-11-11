// SPDX-License-Identifier: MIT
// FERMI_TWOD_A solid fills in the form xf86-video-nouveau's EXA fill writes (nvc0_exa.c NVC0EXAPrepareSolid/Solid):
// SRCCOPY, colour key off, render enable on, clip set to the destination, and the destination's own colour format with
// one packed colour word. The first boot's probe fill (RF32x4 colour format, four float words, clip off, unbracketed after
// clear-only passes) left GR busy until a context-switch timeout (x1-units UNITS.md). The caller brackets a fill as
// raw/fx/fx_mip.cpp brackets 2D work: tiled cache drained and the 3D engine idle before, the 2D then 3D engine idle and
// the texture cache invalidated after.
#pragma once

#include "../SwitchGM20B/SwitchGM20B2D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

struct switch_gm20b_2d_surface {
    uint64_t address;
    uint32_t format;  // NV902D_SET_DST_FORMAT_V_*
    bool pitch;
    uint32_t pitch_bytes;       // pitch layout
    uint8_t block_height_log2;  // block-linear layout
    uint32_t width, height;
};

#define SWITCH_GM20B_2D_FILL_WORDS 31u

// [x0, x1) x [y0, y1) of `dst` set to `color`, one word packed in `dst`'s own format
void switch_gm20b_2d_fill(struct nv_push *p, const struct switch_gm20b_2d_surface *dst, uint32_t color, uint32_t x0,
                          uint32_t y0, uint32_t x1, uint32_t y1);

#ifdef __cplusplus
}
#endif
