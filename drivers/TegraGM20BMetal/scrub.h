// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// A freed span of a VOL=1 write-combined chunk (Shared buffers' alone), counted in its chunk's used bytes until the copy
// engine has zeroed it (scrubs.cpp): wanted, then filled at the front of a command buffer, then scrubbing in the job
// `seqno` on channel `generation`
struct Scrub {
    uint32_t chunk = 0;
    uint64_t offset = 0, bytes = 0, va = 0;
    uint32_t seqno = 0;
    uint64_t generation = 0;
};

}  // namespace gm20b
