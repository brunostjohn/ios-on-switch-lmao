// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICBand.h"
#include "TegraVICRequest.h"
#include "SwitchVICPicture.h"

// A picture of 16-bit samples (P010 into 'x420'/'xf20') goes as an 8-bit one of twice the width: both layouts hold
// the same bytes in the same places, and the 4:2:0 pass-through moves bytes untouched (kTegraVICTraits.blockLinearIn).
inline const char *switch_vic_picture_bytes(const TegraVICPicture *wide, const TegraVICSurfaceDesc *d, TegraVICPicture *p,
                     TegraVICSurfaceDesc *bytes)
{
    *p = *wide;
    *bytes = *d;
    if (wide->sampleBytes == 1)
        return nullptr;
    if (wide->sampleBytes != 2)
        return "picture sample size";
    const uint32_t video = TEGRA_VIC_FOURCC('x', '4', '2', '0');
    const uint32_t full = TEGRA_VIC_FOURCC('x', 'f', '2', '0');
    if (d->fourcc != video && d->fourcc != full)
        return "16-bit picture into a surface that is not 'x420' or 'xf20'";
    if ((d->width | wide->cropX | wide->width | wide->dstX) > UINT32_MAX / 2)
        return "16-bit picture width overflow";
    bytes->fourcc = d->fourcc == video ? TEGRA_VIC_FOURCC('4', '2', '0', 'v') : TEGRA_VIC_FOURCC('4', '2', '0', 'f');
    bytes->width = d->width * 2;
    p->cropX = wide->cropX * 2;
    p->width = wide->width * 2;
    p->dstX = wide->dstX * 2;
    p->sampleBytes = 1;
    return nullptr;
}

// Where the picture lands: its size clipped to the destination, and the rows it covers in each plane.
struct SwitchVICPlacement {
    TegraVICImage whole;  // the surface at address 0, for its format, pitches and colour standard
    uint32_t width, height;
    TegraVICBand band[2];
    uint32_t pages[2];
};

inline const char *switch_vic_picture_place(const TegraVICPicture *p, const TegraVICSurfaceDesc *d, SwitchVICPlacement *pl)
{
    const char *why = tegra_vic_image_from(d, 0, &pl->whole);
    if (why)
        return why;
    if (pl->whole.format->kind == TEGRA_VIC_KIND_RGB || pl->whole.format->planes != 2)
        return "destination is not 4:2:0";
    // Odd sizes are refused rather than rounded: the column or row VIC could not write would show what the buffer held.
    if ((p->cropX | p->cropY | p->dstX | p->dstY) & 1)
        return "picture window or placement on odd coordinates";
    if (p->dstX >= d->width || p->dstY >= d->height)
        return "picture placed outside the destination";
    pl->width = p->width < d->width - p->dstX ? p->width : d->width - p->dstX;
    pl->height = p->height < d->height - p->dstY ? p->height : d->height - p->dstY;
    if ((pl->width | pl->height) & 1)
        return "picture of odd size";
    if (pl->width < TEGRA_VIC_MIN_DIM || pl->height < TEGRA_VIC_MIN_DIM)
        return "picture under 2x2";
    if (p->cropX > UINT32_MAX - pl->width || p->cropY > UINT32_MAX - pl->height)
        return "picture crop overflow";
    if (!p->pages || !p->bytes || p->bytes > static_cast<uint64_t>(p->pageCount) << 12 ||
        p->bytes > static_cast<uint64_t>(8192u) << 12)
        return "picture memory outside its page list or over 32 MiB";
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t firstRow = k ? p->dstY / 2 : p->dstY;
        const uint32_t rows = k ? pl->height / 2 : pl->height;
        if (p->dstX > pl->whole.pitch[k] || pl->width > pl->whole.pitch[k] - p->dstX ||
            firstRow > pl->whole.rows[k] || rows > pl->whole.rows[k] - firstRow)
            return "picture exceeds destination plane geometry";
        pl->band[k] =
            tegra_vic_band_420(d->planeOffset[k], pl->whole.pitch[k], k, p->dstX, p->dstY, pl->width, pl->height);
        pl->pages[k] = tegra_vic_band_pages(&pl->band[k]);
    }
    if (static_cast<uint64_t>(pl->pages[0]) + pl->pages[1] > 8192u)
        return "destination rows over 32 MiB";
    return nullptr;
}

// The source and destination keep their allocation mappings; plane bases select only the requested rectangle.
// The source takes the destination's format and colour standard for a byte-preserving pass-through.
inline void switch_vic_picture_describe(const TegraVICPicture *p, const SwitchVICPlacement &pl, uint64_t srcBase, uint64_t dstBase, TegraVICImage *src,
              TegraVICImage *dst, TegraVICOp *op)
{
    *dst = {};
    dst->format = pl.whole.format;
    dst->width = pl.width;
    dst->height = pl.height;
    dst->standard = pl.whole.standard;
    dst->transfer = pl.whole.transfer; dst->primaries = pl.whole.primaries;
    dst->chromaSite = pl.whole.chromaSite;
    dst->lastRowExact = 1;
    for (uint32_t k = 0; k < 2; k++) {
        dst->pitch[k] = pl.band[k].pitch;
        dst->rows[k] = pl.band[k].rows;
        dst->base[k] = dstBase + pl.band[k].start;
        dst->span[k] = tegra_vic_band_end(&pl.band[k]) - pl.band[k].start;
    }
    *src = {};
    src->format = pl.whole.format;
    src->width = p->cropX + pl.width;
    src->height = p->cropY + pl.height;
    src->pitch[0] = src->pitch[1] = p->pitch;
    src->rows[0] = p->rows;
    src->rows[1] = p->rows / 2;
    src->base[0] = srcBase;
    src->span[0] = p->chromaOffset;
    src->base[1] = srcBase + p->chromaOffset;
    src->span[1] = p->chromaOffset < p->bytes ? p->bytes - p->chromaOffset : 0;
    src->standard = pl.whole.standard;
    src->transfer = pl.whole.transfer; src->primaries = pl.whole.primaries;
    src->chromaSite = pl.whole.chromaSite;
    src->blockLinear = 1;
    src->blockHeightLog2 = p->blockHeightLog2;
    *op = {};
    op->srcX = static_cast<uint64_t>(p->cropX) << 16;
    op->srcY = static_cast<uint64_t>(p->cropY) << 16;
    op->srcW = static_cast<uint64_t>(pl.width) << 16;
    op->srcH = static_cast<uint64_t>(pl.height) << 16;
    op->dstW = pl.width;
    op->dstH = pl.height;
    op->nearest = 1;  // 1:1
}

