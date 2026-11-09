// SPDX-License-Identifier: MIT
// The texture a state's texture3d arguments sample (Core Animation's colour cubes). The record knows no Metal index for one (m2v
// binds its own 3D view where the probe's 2D texture meets it: raw_pack.h EntryTexture3d), so the texture is found by type: a
// texture3d argument takes a 3D texture, so it is the 3D texture bound where none of the state's 2D entries names an index.
// Any other texture there is no binding of one (an encoder's bindings outlive the states that set them) and leaves the arguments
// empty, as nothing bound does. Boot 15's first key press drew the generic state with a 1-texel BGRA8 texture at index 12,
// which none of its 2D entries names, and the facade sent the command buffer to m2v.
// Every texture3d argument takes that one texture: an argument the draw leaves unbound is one Core Animation does not sample.
// Only distinct 3D textures cannot tell which argument takes which.
#pragma once
#include <cstdint>

namespace raw {

template <typename Texture> struct Texture3dBinding {
    const Texture *texture = nullptr;
    int slot = -1;  // texture's first index, or the first index of a distinct one when ambiguous
    bool ambiguous = false;
};

// named: the indices the state's 2D texture entries name
template <typename Texture, typename IsVolume>
Texture3dBinding<Texture> texture3dBinding(const Texture *const *textures, uint32_t count, uint32_t named,
                                           IsVolume isVolume)
{
    Texture3dBinding<Texture> b;
    for (uint32_t k = 0; k < count && k < 32; k++) {
        const Texture *t = textures[k];
        if (!t || (named >> k & 1) || !isVolume(*t))
            continue;
        if (!b.texture) {
            b.texture = t;
            b.slot = int(k);
        } else if (t != b.texture) {
            b.ambiguous = true;
            b.slot = int(k);
            return b;
        }
    }
    return b;
}

}  // namespace raw
