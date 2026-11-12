// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#include "raw_render_key.h"

namespace raw {

// A function's metallib bytes (the AIR slice its library was made from) and the library function it was made from, as
// whoever made the descriptor's functions keeps them; false: none
using FunctionSource = bool (*)(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name);

// `source`: how the descriptor's functions name their AIR; null: they are m2v's (NativeBridgeMetalFrontend.mm)
bool renderCacheKey(MTLRenderPipelineDescriptor *descriptor, const std::string &key, bool own, uint64_t stack,
                    CacheKey &out, FunctionSource source = nullptr);

}  // namespace raw
