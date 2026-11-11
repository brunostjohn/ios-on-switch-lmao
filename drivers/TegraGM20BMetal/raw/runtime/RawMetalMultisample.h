// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// A stored multisampled texture the raw path keeps: private, 2D multisampled at a count the device answers yes for (2, 4), one
// level, no rotation; a colour format it renders (Device::createStoredMultisample), or a depth/stencil one only rendered and
// sampled (Device::createOwnedZeta)
enum class RMStoredSamples : uint8_t {
    None,
    Colour,
    Zeta
};

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d);
bool makeStoredMultisample(MTLTextureDescriptor *d, RMStoredSamples kind, raw::Texture &t);

// The pass's own `count` sample positions (getSamplePositions:nil count:0) for a pass of `samples` samples
// (raw_sample_positions.h); false: ones the raw path keeps Metal's refusal of
bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out);

// The first stored multisampled depth/stencil texture range [from, to) of `cb` loads, stores, binds or copies: m2v's twin of
// it is a texture of its own memory, so a replay of the range fails by name, before m2v encodes any of it, rather than read
// or keep depth the raw path's memory does not hold. A range that only attaches it, cleared or not loaded and not stored,
// replays on the twin.
RMTexture *ownedZetaIn(RMCommandBuffer *cb, size_t from, size_t to);
void failOwnedZeta(RMCommandBuffer *cb, RMTexture *t);
