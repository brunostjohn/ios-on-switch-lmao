// SPDX-License-Identifier: MIT
// The fetch ladder's fixed-function rungs (raw_pack.h PackFetchRung): where a state that blends by fetching its own
// colour may instead draw through the ROP. Both need a pack record whose program writes the source and fetches nothing
// (a key-changing pack variant the pack does not hold yet: PackPipeline::fetchRung marks the candidates only), so no draw
// takes them until the pack does.
#pragma once
#include <cstdint>

namespace raw {

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
