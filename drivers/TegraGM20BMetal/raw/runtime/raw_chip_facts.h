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

}  // namespace raw
