// SPDX-License-Identifier: MIT
// A kernel event's signal released mid-job into its word (drivers/TegraGM20B/TegraGM20BJobABI.h method 40): the host's wait
// for idle and the L2's dirty lines to DRAM, as the kernel's external job end makes them for a CPU reader (gpu_exec.c
// tgm_chan_fence_job), then the 4-byte release and the nonstall interrupt the kernel completes the signal on. The engines'
// own releases (3D and compute SET_REPORT_SEMAPHORE with AWAKEN, the copy engine's) would save nothing here: the signal is
// every engine's work before it, and the L2 flush a listener needs waits for idle first.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_push.h"

namespace raw {

constexpr uint32_t kEventReleaseWords = 16;

inline void emitEventRelease(Push &p, uint64_t va, uint32_t payload)
{
    p.inc(NVB06F_WFI, {0});
    p.inc(NVB06F_FB_FLUSH, {0});
    p.inc(NVB06F_MEM_OP_C, {0, NVDEF(NVB06F, MEM_OP_D, OPERATION, L2_FLUSH_DIRTY)});
    p.inc(NVB06F_FB_FLUSH, {0});
    p.inc(NVB06F_SEMAPHOREA,
          {uint32_t(va >> 32) & 0xff, uint32_t(va), payload,
           NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) | NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, EN) |
               NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE)});
    p.inc(NVB06F_NON_STALL_INTERRUPT, {0});
}

// The payload a signal of `value` releases into a word of `base` whose last payload was `floor`; 0 where its job's end
// signals it instead (outside the word's window, or no later than a payload released before)
inline uint32_t eventReleasePayload(uint64_t value, uint64_t base, uint32_t floor)
{
    const uint64_t payload = value - base;
    return payload > floor && payload < (uint64_t(1) << 31) ? uint32_t(payload) : 0;
}

}  // namespace raw
