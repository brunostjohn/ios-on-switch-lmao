// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalObjects.h"
#include "raw_compute_lookup.h"

@interface RMComputeState : RMObject {
@public
    bool _keyed;
    raw::AirHash _air;
    std::string _constants;
    uint32_t _maxThreads, _threadLimit;
    bool _multipleOfWidth;
    raw::ComputeKernelCache<raw::ComputeState> _kernels;
    MTLComputePipelineDescriptor *_direct;
    id<MTLComputePipelineState> _metadata;
    id<MTLComputePipelineState> _compatibility;
    id<MTLDevice> _fallback;
    std::once_flag _twinOnce;
}
@end
