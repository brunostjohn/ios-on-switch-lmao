// SPDX-License-Identifier: MIT
// The size a constant buffer is bound at (SET_CONSTANT_BUFFER_SELECTOR_A): whole 256-byte units, as NVK binds every
// constant buffer before Turing (nvk_cmd_draw.c align(size, nvk_min_cbuf_alignment), 256 below TURING_A), at most
// NVK_MAX_CBUF_SIZE. A binding's memory is allocated in the same units, so the rounding never reaches past it.
#pragma once
#include <algorithm>
#include <cstdint>

namespace raw {

constexpr uint32_t kCbufUnit = 256, kCbufMaxBytes = 0x10000;

constexpr uint32_t cbufBindBytes(uint32_t bytes)
{
    return std::min((bytes + kCbufUnit - 1) & ~(kCbufUnit - 1), kCbufMaxBytes);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// HW Test's debug switch split-frontier-exact: `bytes` as they are where `exact`, the size the split frontier was bound at
// before it was rounded, so one boot compares the two
constexpr uint32_t cbufBindBytes(uint32_t bytes, bool exact)
{
    return exact ? bytes : cbufBindBytes(bytes);
}
#endif

}  // namespace raw
