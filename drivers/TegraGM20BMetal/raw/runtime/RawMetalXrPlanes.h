// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 textures over a two-plane surface on the raw path (raw_xr_planes.h): made over the surface, and kept in step
// with it through one encodeRaw range. The image is merged from the planes only where the surface moved since it last held
// them (its seed, or a write the seed does not show: RawMetalSurfaceWrites.mm), and split into them once, after the range's
// work, where the range wrote it: another texture over the same planes in that range reads them as the range found them.
#pragma once
#import "RawMetalInternal.h"

inline bool xrOverPlanes(const raw::Texture &t)
{
    return raw::xrForm(t.format) == raw::XrForm::Planes && t.luma && t.chroma && t.stencilTemp;
}

// 550 over plane 0 of a two-plane surface of a 4-byte and a 1-byte plane, both the descriptor's size: a 2D texture, one level,
// one sample, no rotation, rendered or sampled
bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
bool makeXrPlanes(RMTexture *t, IOSurfaceRef s);

class XrPlaneSync {
public:
    explicit XrPlaneSync(raw::CommandBuffer &rc) : rc_(rc)
    {}

    // Before work reads the image, or writes part of it: the planes merged into it here, or in `open` (then a barrier)
    void read(RMTexture *t, raw::BlitEncoder *open = nullptr);
    // A pass's color attachment: read unless its Clear reaches every texel; the image written
    void attach(RMTexture *t, MTLLoadAction load, const RMPass &pass);
    // A blit encoder's copy between a buffer and a texture (OpCopyBufferToTexture, OpCopyTextureToBuffer), in `be`; copies
    // between two textures with such a texture are m2v's (RawMetalBlit.mm)
    void blit(const Cmd &c, raw::BlitEncoder *be);
    // The written images into their planes, after the range's work
    void flush();
    // The range submitted: each image holds its surface as the range left it
    void submitted();

private:
    struct Entry {
        RMTexture *t;
        uint32_t seed;
        uint64_t writes;
        bool fresh, written;
    };

    Entry *entry(RMTexture *t);
    void merge(Entry &e, raw::BlitEncoder *open);
    raw::CommandBuffer &rc_;
    std::vector<Entry> entries_;
};
