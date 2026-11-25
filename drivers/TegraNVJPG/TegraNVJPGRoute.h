// SPDX-License-Identifier: BSD-2-Clause-Patent
// How a planned decode reaches its destination once GM20B has said how it laid the destination out. Plain C, header
// only: TegraNVJPGUserClient.cpp and the host test (test/request_host.c) compile the same code.
#ifndef TEGRA_NVJPG_ROUTE_H
#define TEGRA_NVJPG_ROUTE_H
#include <stdbool.h>
#include <stdint.h>
#include "TegraNVJPGRequest.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

// What GM20B answered for the destination's first page: `held` the hold's tiled answer (the surface decided
// block-linear), `described` whether tegra_gm20b_surface_layout gave `layout`, `compressed` a compressible kind.
typedef struct {
    bool held, described, compressed;
    struct tgm_layout layout;
} TegraNVJPGDestinationLayout;

enum {
    TEGRA_NVJPG_ROUTE_ROWS = 0,         // NVJPG writes the destination's rows
    TEGRA_NVJPG_ROUTE_COPY_ROWS = 1,    // NVJPG writes scratch rows, the copy engine the destination's rows
    TEGRA_NVJPG_ROUTE_COPY_BLOCKS = 2,  // NVJPG writes scratch rows, the copy engine the destination's blocks
    TEGRA_NVJPG_ROUTE_COUNT = 3,
};

// The copy engine writes a block-linear side only whole (TegraGM20BCopy.cpp's layout): the plan's one copy must be the
// layout's every texel, from the destination's first byte.
static inline bool tegra_nvjpg_copy_covers_blocks(const TegraNVJPGPlan *p, const struct tgm_layout *l)
{
    return p->planes == 1 && !p->fillChroma && p->copyDstStart[0] == 0 &&
           (uint64_t)l->width * l->bpp == p->copyRowBytes[0] && l->height == p->copyRows[0];
}

// `p` (as tegra_nvjpg_plan left it, a single-surface plan: not VIC's re-pack) and its setup's output fields moved onto
// the destination's layout `l`. NVJPG writes rows only (TegraNVJPGJpeg.h), so a destination GM20B laid out
// block-linear takes them through the copy engine, which writes its blocks. `maxRepack` the copy engine's scratch,
// `copyEngine` whether the copy engine can reach the destination's memory. NULL with *route, or why the destination's
// layout cannot be written.
static inline const char *tegra_nvjpg_route(const TegraNVJPGDecodeRequest *r, const TegraNVJPGSurface *d,
                                            uint64_t maxRepack, bool copyEngine, const TegraNVJPGDestinationLayout *l,
                                            TegraNVJPGPlan *p, nvjpg_dec_drv_pic_setup_s *s, uint32_t *route)
{
    if (p->repack == TEGRA_NVJPG_REPACK_VIC)
        return "VIC's re-pack has no single destination layout to route";
    if (!l->held) {
        *route = p->repack == TEGRA_NVJPG_REPACK_CE ? TEGRA_NVJPG_ROUTE_COPY_ROWS : TEGRA_NVJPG_ROUTE_ROWS;
        return NULL;
    }
    if (!l->described)
        return "destination decided block-linear, but GM20B describes no layout of it";
    if (l->compressed)
        return "compressed block-linear destination";
    *route = TEGRA_NVJPG_ROUTE_COPY_BLOCKS;
    if (!copyEngine)
        return "block-linear destination the copy engine cannot reach";
    if (p->repack == TEGRA_NVJPG_REPACK_CE)
        return tegra_nvjpg_copy_covers_blocks(p, &l->layout)
                   ? NULL
                   : "copy-engine re-pack into part of a block-linear destination";
    return tegra_nvjpg_plan_fallback_ce(r, d, maxRepack, true, l->layout.width, l->layout.height, l->layout.bpp,
                                        false, p, s)
               ? NULL
               : "a picture that is not the whole of its block-linear destination, or past the copy engine's scratch";
}

#ifdef __cplusplus
}
#endif
#endif
