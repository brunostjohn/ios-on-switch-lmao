// SPDX-License-Identifier: BSD-2-Clause-Patent
// Deferred quarter turns (V3, NativeMobileDisplay/SwitchLandscape.h). Core Animation turns a landscape video layer on
// an internal display through the IOSurfaceAccelerator into a destination it then detaches. Once window A's column scan
// of NV12 is proven (SwitchDisplayTraits columnScanYuv), TegraVIC records that destination as the decoder's picture
// turned (switch_native_surface_publish_turned) instead of copying, and the display scans the picture itself. The rows
// are made by VIC, as the copy would have made them, only when something other than the display reads the destination:
// a CPU lock, a GPU import, another VIC job (switch_native_surface_prepare / _prepare_gpu, the seams every reader of a
// surface with a native picture already passes).
#pragma once
#include "SwitchNativeSurface.h"
#include "../NativeMobileDisplay/SwitchDisplayTraits.h"

// The turn `op` puts on a whole destination `d` from a picture visible at `crop`: nullptr with `turn` filled, or why
// not. Only a quarter turn of a whole-pixel, even source rectangle inside the picture onto all of an NV12 destination.
static inline const char *switch_vic_turn_of(const TegraVICOp &op, const TegraVICSurfaceDesc &d,
                                             const SwitchNativeSurfaceCrop &crop, SwitchNativeSurfaceTurn &turn)
{
    if (op.fill || op.transform > 7 || !(op.transform & TEGRA_VIC_ROT90))
        return "not a quarter turn";
    if (d.planes != 2 ||
        (d.fourcc != TEGRA_VIC_FOURCC('4', '2', '0', 'v') && d.fourcc != TEGRA_VIC_FOURCC('4', '2', '0', 'f')))
        return "a destination window A does not scan";
    if (op.dstX || op.dstY || op.dstW != d.width || op.dstH != d.height)
        return "a turn onto part of the destination";
    if ((op.srcX | op.srcY | op.srcW | op.srcH) & 0xffff)
        return "a source rectangle in fractions of a pixel";
    const uint64_t x = op.srcX >> 16, y = op.srcY >> 16, w = op.srcW >> 16, h = op.srcH >> 16;
    if ((x | y | w | h) & 1 || w < 2 || h < 2 || x + w > crop.width || y + h > crop.height)
        return "a source rectangle not on whole chroma pairs inside the picture";
    turn = {op.transform, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h)};
    return nullptr;
}

// VIC's turn of `source` (a decoder's picture) into `destination`'s rows under the caller's write lease `owned`: queued
// (the destination's producer published before returning, for a GPU reader) or completed before returning (a CPU one).
int switch_vic_turn_surface(const SwitchNativeSurfaceView &source, const TegraVICOp &op, uint32_t chromaSite,
                            const TegraVICSurface *destination, switch_gpu_cpu_lease *owned, bool queue,
                            const char **why);

// TegraVIC's transform worker, before it copies: 1 when the job is recorded on its destination instead (nothing to
// submit, the caller completes it), 0 to copy as before. A job with events copies: the events wait for VIC's work.
int switch_vic_turn_defer(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                          uint32_t chromaSite, bool events, const SwitchDisplayTraits &traits = kSwitchDisplayTraits);
