// SPDX-License-Identifier: MIT
// An indirect command buffer of dispatches: each command is kComputeSlotWords push words beside its regions' QMDs, root
// tables and set 0 in GPU memory, written when the CPU encodes the command, and a range of commands is one run of words the
// channel fetches by reference (ComputeEncoder::executeSlots). A command is its barrier (the serial encoder's shader-data
// flush, or no-ops), the gate macro called with its index (raw_mme.h kIcbGate), then one launch per region of its grid
// (raw_dispatch_regions.h). The gate sets the global render enable from the range in the macros' scratch, which the compute
// class's launches obey, so a command outside the range launches nothing; the execute's tail sets it true again. The call is
// at the same words in every command, so a copy to another index rewrites one word (kComputeIndexWord), as a draw's does.
// Commands of a buffer that inherits its pipeline or buffers are written the same way at each execute, into the command
// buffer's own memory: their QMDs name constant buffer addresses the inherited state decides.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "raw_icb.h"

namespace raw::icb {

constexpr uint32_t kComputeRegions = 8, kComputeCallWord = 4, kComputeIndexWord = kComputeCallWord + 1,
                   kComputeLaunchWords = 3, kComputeSlotWords = 32;
static_assert(kComputeIndexWord + 1 + kComputeRegions * kComputeLaunchWords <= kComputeSlotWords);

// A region's QMD, root table and set 0, in that order in its block: each at the 256 bytes a QMD address and a constant
// buffer's take
constexpr uint32_t kComputeQmdBytes = 256, kComputeRootBytes = 0x800, kComputeBlockAlign = 256;

inline uint32_t computeSetBytes(uint32_t recordSetBytes)
{
    return recordSetBytes ? (recordSetBytes + 255) & ~255u : 256;
}

inline uint32_t computeRegionBytes(uint32_t recordSetBytes)
{
    return kComputeQmdBytes + kComputeRootBytes + computeSetBytes(recordSetBytes);
}

struct ComputeCommand {
    uint64_t qmd[kComputeRegions];  // each region's QMD address, 256-byte aligned
    uint32_t regions;
    bool barrier;  // setBarrier: the commands before it have written before it launches
};

inline void computeEmpty(uint32_t *slot)
{
    for (uint32_t i = 0; i < kComputeSlotWords; i++)
        slot[i] = kNop;
}

inline void dispatch(uint32_t *slot, uint32_t index, const ComputeCommand &c)
{
    Push p{slot, slot + kComputeSlotWords};
    if (c.barrier) {
        p.m(NVB1C0_INVALIDATE_SHADER_CACHES,
            NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE),
            SUBC_COMPUTE);
        p.m(NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    }
    while (p.at < slot + kComputeCallWord)
        *p.at++ = kNop;
    mme::icbGateHeader(p);
    *p.at++ = index;
    for (uint32_t r = 0; r < c.regions && r < kComputeRegions; r++) {
        p.inc(NVB1C0_SEND_PCAS_A, {uint32_t(c.qmd[r] >> 8)}, SUBC_COMPUTE);
        p.m(NVB1C0_SEND_SIGNALING_PCAS_B,
            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE),
            SUBC_COMPUTE);
    }
    while (p.at < p.end)
        *p.at++ = kNop;
}

// Before an execute's commands: what a copy wrote of their words reaches the command fetch first, then the range into the
// macros' scratch (the CPU's), or the call whose two data words the caller appends as a GPFIFO continuation at the range
// buffer (kIcbRange)
inline void computeExecuteHead(Push &p, bool written, bool rangeFromMemory, uint32_t first, uint32_t count)
{
    if (written || rangeFromMemory) {
        p.m(NVB1C0_WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
        p.m(NVB1C0_INVALIDATE_SHADER_CACHES,
            NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE),
            SUBC_COMPUTE);
        // NV906F_SET_REFERENCE, as RenderEncoder::indirect: the fetches wait for the writes before them
        p.m(0x0050, 0);
    }
    if (rangeFromMemory)
        mme::icbRangeHeader(p);
    else
        mme::icbRange(p, first, count);
}

// After them: launches obey the render enable again as the channel left it before the first gate
inline void computeExecuteTail(Push &p)
{
    p.m(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE, SUBC_COMPUTE);
}

}  // namespace raw::icb
