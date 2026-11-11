// SPDX-License-Identifier: MIT
// The chip facts (raw_pack.h kChipAssumptions) a device probe has proved on GM20B. The pack builder compiles the variants
// that assume them and stamps them into the pack header (PackHeader::chipFacts, pack/packfile.py chip_facts), whose draws
// then hold them. A bit is set here by hand, from the probe's log line, never at run time.
//   AssumeTexsF16: SwitchHWTest gpu.raw.texs_f16: every unorm8_* and rgba16f_* "_f16_wrong" 0, its "_proof" 0x5346....
//   AssumeRopRounding: no probe yet (COMPILER.md E3)
#pragma once
#include <cstdint>

namespace raw {

constexpr uint32_t kProvenChipFacts = 0;

// The ROP units a device probe proved the raw path drives right, set by hand from the probe's line (never at run time):
//   ProvenFixedBlend: SwitchHWTest gpu.x1.blend_exact "model=chip chip_misses=0" (T-v2, 11 Nov: the 8-bit integer blender
//     reproduces every blended channel of 8.4 M per factor pair on raw)
enum ProvenUnit : uint32_t {
    ProvenFixedBlend = 1u << 0,
};

constexpr uint32_t kProvenUnits = ProvenFixedBlend;

// The longest iterated blend program, in passes, the device showed within its bound of the storage form and faster than
// it: SwitchHWTest gpu.raw.fetch_rung3 "passes_ceiling" (no beyond=), with gpu.x1.iterated_blend "nvn_within_one" equal
// to "nvn_run". 0: no program has been, so none is taken.
constexpr uint32_t kIteratedBlendPasses = 0;

}  // namespace raw
