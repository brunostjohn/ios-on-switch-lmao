// SPDX-License-Identifier: MIT
// A state's storage (interlocked) form binds the colors it fetches as storage images, which a pass's attachment can be
// viewed as only where its texel size and samples are the form's and its memory is not compressed: the GM20B's surface
// loads and stores read a compressible kind's raw bytes, never what its compression tags say. Where an attachment does not
// fit, the draw takes the state's barrier form (its twin in the pack, resolved with the state), which orders its fetches
// with barriers instead. Metal's lossless-compression flag (Texture::compressed) is only how m2v clears and filters such a
// texture: the memory is compressed only where Texture::comptags says so.
#pragma once
#include <cstdint>

namespace raw {

enum class StorageFallback : uint8_t {
    None,
    Metadata,  // the pack's storage record cannot be used (raw_storage.h usableStorageMetadata): its barrier record is
    TexelBytes,  // an attachment's texel size is not the storage view's
    Samples,     // an attachment's samples are not the form's
    Compressed,  // an attachment's memory carries compression tags
    NoTwin,      // one of the above with no barrier form to take: the encoder is refused
    Count
};

constexpr size_t kStorageFallbacks = size_t(StorageFallback::Count);
constexpr const char *kStorageFallbackNames[kStorageFallbacks] = {"-",       "metadata",   "texel",
                                                                  "samples", "compressed", "notwin"};

// Why attachment `t` cannot be viewed as the storage image of a form whose view has `viewTexelBytes` and `samples`
template <typename Texture>
constexpr StorageFallback storageViewMismatch(uint32_t viewTexelBytes, uint32_t samples, const Texture &t)
{
    if (viewTexelBytes != t.bpp)
        return StorageFallback::TexelBytes;
    if (samples != t.samples)
        return StorageFallback::Samples;
    if (t.comptags)
        return StorageFallback::Compressed;
    return StorageFallback::None;
}

}  // namespace raw
