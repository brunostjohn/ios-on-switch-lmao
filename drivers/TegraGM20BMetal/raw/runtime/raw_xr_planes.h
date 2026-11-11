// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 (550) over a two-plane surface (CoreVideo's 30RGBLE_8A_BiPlanar, 'b3a8'): Metal's bytes are the surface's
// planes, plane 0 the color word (raw_xr.h's BGR10 word, bits 30 and 31 clear), 4 bytes a texel, and plane 1 the alpha byte. The
// raw path renders and samples its own form of the format (raw_xr.h: the word, then the alpha byte as a word) in a block-linear
// pool image, which the copy engine fills from the planes (merge) before work reads it and writes back into them (split) after
// work wrote it.
// One remap cannot move a byte to or from every eighth byte of an image, so the alpha byte goes through a temp of two bytes a
// texel as the third two-byte component of the texel: the copies NVK moves Depth32Float_Stencil8's stencil byte with
// through its stencil_copy_temp (nvk_cmd_copy.c nvk_remap_insert_aspect, extract_aspect), with the bytes beside the alpha
// written zero from CONST_A instead of kept.
#pragma once
#include <array>
#include <cstdint>
#include "raw_kit.h"

namespace raw {

enum class XrPlaneSide : uint8_t {
    image,
    word,
    alpha,
    temp
};

struct XrPlaneTransfer {
    XrPlaneSide source{}, destination{};
    uint32_t remap = 0;
};

using XrPlaneTransfers = std::array<XrPlaneTransfer, 3>;

constexpr uint32_t kXrRemapConstA = NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A,
                   kXrRemapNoWrite = NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE;

// SET_REMAP_COMPONENTS: DST_X..W each SRC_X..W (0..3), CONST_A or NO_WRITE, `size`-byte components, the sides' counts
constexpr uint32_t xrRemap(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t source,
                           uint32_t destination)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (source - 1) << 20 | (destination - 1) << 24;
}

// The copies of a merge (planes into the image) or a split (the image into the planes), in order
constexpr XrPlaneTransfers xrPlaneTransfers(bool split)
{
    using S = XrPlaneSide;
    constexpr uint32_t A = kXrRemapConstA, W = kXrRemapNoWrite;
    if (split)
        return {{{S::image, S::word, xrRemap(0, W, W, W, 4, 2, 1)},
                 {S::image, S::temp, xrRemap(2, W, W, W, 2, 4, 1)},
                 {S::temp, S::alpha, xrRemap(0, W, W, W, 1, 2, 1)}}};
    return {{{S::alpha, S::temp, xrRemap(0, A, W, W, 1, 1, 2)},
             {S::word, S::image, xrRemap(0, W, W, W, 4, 1, 2)},
             {S::temp, S::image, xrRemap(W, W, 0, A, 2, 1, 4)}}};
}

constexpr uint32_t kXrPlaneWords = 2 + 3 * SWITCH_GM20B_COPY_RECT_WORDS(1);

// One merge or split of a `width` x `height` image, `sides` by XrPlaneSide. Each launch waits for every copy before it (a
// NON_PIPELINED launch): the temp is read after it is written and the image's two halves are written by two launches, and
// a split's or a merge's first copy waits for the copies of the work before it.
inline void emitXrPlanes(Push &p, const std::array<CopySurface, 4> &sides, uint32_t width, uint32_t height, bool split)
{
    p.m(NVB0B5_SET_REMAP_CONST_A, 0, SUBC_COPY);
    for (const XrPlaneTransfer &t : xrPlaneTransfers(split))
        emitCopy(p, sides[size_t(t.source)], sides[size_t(t.destination)], width, height, t.remap, true);
}

// The copies' bytes a texel: each launch reads every component of its source's texels and writes its destination's
constexpr uint32_t xrPlaneBytes(bool split)
{
    uint32_t bytes = 0;
    for (const XrPlaneTransfer &t : xrPlaneTransfers(split)) {
        const uint32_t size = (t.remap >> 16 & 3) + 1;
        bytes += size * ((t.remap >> 20 & 3) + 1) + size * ((t.remap >> 24 & 3) + 1);
    }
    return bytes;
}

// The copy-free form of a pass into such a texture, for a front end that writes the two planes' codes as two outputs: the color
// plane as an RU32 target at the attachment's slot `slot`, the alpha plane as an RU8 target at the lowest slot the pass binds
// nothing at, m. It takes a state whose pixel program writes target `slot`'s red alone (the word) and target m's red (the alpha
// code): the output map of its header, word 18, four bits a target from bit 4i (NVA097_SPHV3T2_OMAP_RED(i) .. ALPHA(i)). m, or
// -1 where the state does not write so or every slot is bound.
constexpr int xrAlphaOutput(const uint32_t *sph, uint8_t attachments, unsigned slot)
{
    if (slot >= 8 || !(attachments >> slot & 1))
        return -1;
    int m = 0;
    while (m < 8 && (attachments >> m & 1))
        m++;
    const uint32_t outputs = sph[18];
    if (m == 8 || (outputs >> 4 * slot & 0xf) != 1 || !(outputs >> 4 * m & 1))
        return -1;
    return m;
}

}  // namespace raw
