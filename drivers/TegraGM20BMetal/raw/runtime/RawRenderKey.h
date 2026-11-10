// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#include "raw_render_key.h"

namespace raw {

bool renderCacheKey(MTLRenderPipelineDescriptor *descriptor, const std::string &key, bool own, uint64_t stack,
                    CacheKey &out);

}  // namespace raw
