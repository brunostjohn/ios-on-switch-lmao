// SPDX-License-Identifier: MIT
// The fetch ladder's fixed-function rungs (raw_pack.h PackFetchRung): where a state that blends by fetching its own
// colour may instead draw through the ROP. Both need a pack record whose program writes the source and fetches nothing
// (a key-changing pack variant the pack does not hold yet: PackPipeline::fetchRung marks the candidates only), so no draw
// takes them until the pack does.
#pragma once
#include <cstdint>

namespace raw {

// Rung 2, rule fetch.blend: the ROP's blend. GM20B's blender is exact integer arithmetic on the source and source alpha
// quantised to 8-bit codes by the store rule, divided by 255 and rounded once (x1-units UNITS.md, fitted to every device
// count of gpu.x1.blend_exact; bench blendff 81,920 of 81,920): within one code of Metal for 8-bit content at any depth,
// up to 2 codes for shader-computed colour stacked twice, 4 codes deeper. It is taken only where that error cannot be seen
// as data: an 8-bit unorm target the client never reads back, not re-blended across frames, of standard factors.
struct FixedBlendAsk {
    uint16_t format = 0;           // MTLPixelFormat
    bool readable = false;         // CPU-visible storage, a blit or compute pass reads it as data, or a HW Test golden
    bool accumulates = false;      // a feedback target re-blended across frames
    bool standardFactors = false;  // Metal's blend factor pairs, not the iterated unit's programs
    bool eightBitSources = false;  // what blends into it is 8-bit content (textures of unorm8 images, 8-bit colours)
    uint8_t computedLayers = 0;    // shader-computed colours stacked over one pixel, where the sources are not 8-bit
};

constexpr bool fixedBlendAcceptable(const FixedBlendAsk &a)
{
    const bool unorm8 = a.format == 70 || a.format == 80;  // RGBA8Unorm, BGRA8Unorm
    return unorm8 && !a.readable && !a.accumulates && a.standardFactors && (a.eightBitSources || a.computedLayers <= 2);
}

// Rung 3, rule fetch.iterated: the ROP's iterated blend (kits/SwitchGM20BEmit/SwitchGM20BEmitIterated.h) for blends with
// no fixed-function equation (Core Animation's colour-matrix group and separable modes). Its setup is the one x1-units
// derived from the class errors; it is taken once gpu.x1.iterated_blend reads nvn_accepted=nvn_run and
// nvn_within_one=nvn_run on the chip.
constexpr bool iteratedBlendProven()
{
    return false;
}

struct IteratedAsk {
    bool candidate = false;      // PackPipeline::fetchRung carries kFetchCandidateIterated for the colour
    uint32_t colourTargets = 0;  // targets the pass binds (the unit refuses a draw with more than one: 0x59)
    uint32_t programWords = 0;   // its microcode, which ends in a write of TEMP0
    uint32_t constants = 0;      // half-float constants it reads
};

constexpr bool iteratedBlendUsable(const IteratedAsk &a)
{
    return iteratedBlendProven() && a.candidate && a.colourTargets == 1 && a.programWords >= 1 &&
           a.programWords <= 128 && a.constants <= 24;
}

}  // namespace raw
