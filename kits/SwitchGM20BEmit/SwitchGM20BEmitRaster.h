// SPDX-License-Identifier: MIT
// MAXWELL_B raster and ROP shortcuts no open driver runs on GM20B: the ROP writing a constant colour without the pixel
// program's output. gv100's class errors name the constraint the hardware checks at a draw:
// CONSTANT_COLOR_RENDERING_WITHOUT_PRE_PS_COVERAGE (0x138), coverage known before the pixel program runs.
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_CONSTANT_COLOR_WORDS 6u

// The ROP writes `rgba` (the methods' 32-bit words, red first) for every covered sample until switched off
void switch_gm20b_constant_color(struct nv_push *p, const uint32_t rgba[4]);
void switch_gm20b_constant_color_off(struct nv_push *p);

#ifdef __cplusplus
}
#endif
