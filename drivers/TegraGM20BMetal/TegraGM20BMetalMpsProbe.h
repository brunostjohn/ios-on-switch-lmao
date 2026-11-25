// SPDX-License-Identifier: MIT
// DEVELOPMENT: what MPSImageCanny asks of a compute encoder, to find the answer that makes it never encode its last kernel
// (canny_terminate_edges; HW Test gpu.gm20b.mps_control_flow). A command buffer is probed from the first canny_ state one of
// its encoders binds: each later call MPS makes on its compute encoders, and the answer it got, is one line.
#pragma once
#import <Foundation/Foundation.h>
#include "stats.h"

#if SWITCH_METAL_DIAGNOSTICS
#include <atomic>

inline std::atomic<const void *> gMpsProbed{nullptr};

// `cb` is probed from here on where `function` (a bound compute state's) is one of Canny's kernels
void gm20bMpsProbeBind(const void *cb, const void *encoder, NSString *function);
// One call on an encoder of a probed command buffer, with what the driver answered
void gm20bMpsProbeCall(const void *cb, const void *encoder, const char *call, NSString *detail);
#define SWITCH_MPS_PROBE(cb, encoder, call, detail)             \
    do {                                                        \
        if (gMpsProbed.load(std::memory_order_relaxed) == (cb)) \
            gm20bMpsProbeCall(cb, encoder, call, detail);       \
    } while (0)
#else
#define SWITCH_MPS_PROBE(cb, encoder, call, detail) static_cast<void>(0)
#endif
