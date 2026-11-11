// SPDX-License-Identifier: MIT
// A turned pass whose attachments passRotation takes once their texels are moved (raw_rotated_pass.h turnablePass, raw_turn_copy.h):
// before it, each rotation-4 plane it loads mirrored across its width and every rotation-4 attachment drawn as rotation 1, and
// each unturned attachment it loads or stores moved into a plane-shaped stand-in the pass renders turned (loaded: its texels
// turned in); after it, each stored stand-in turned back into its texture.
#pragma once
#include <deque>
#include <vector>
#include "raw_gpu.h"
#include "raw_rotated_pass.h"

namespace raw {

class TurnedPass {
public:
    // `p` as the pass draws it, the copies before it encoded; false: *why (rc's to abandon)
    bool begin(CommandBuffer &rc, PassDesc &p, const char **why);
    // The stored stand-ins back, after the pass and its resolves
    bool end(CommandBuffer &rc, const char **why);

private:
    struct Back {
        const Texture *into;
        Texture *standIn;
    };

    Texture *standIn(CommandBuffer &rc, Texture &t, uint32_t tag, bool loaded, const char **why);

    Rotation rot_;
    std::vector<Back> backs_;
    std::deque<Texture> held_;  // what the pass's attachments point at until it ends
};

}  // namespace raw
