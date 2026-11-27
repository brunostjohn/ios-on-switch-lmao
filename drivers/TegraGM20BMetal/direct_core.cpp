// SPDX-License-Identifier: MIT
// See direct_core.h.
#include "direct_core.h"

namespace gm20b {

bool DirectCore::configureKind(std::string_view kind)
{
    const uint32_t slots = stageTexturesOf(kind);
    if (!slots)
        return false;
    stageTextures_ = slots;
    kindName_.assign(kind.data(), kind.size());
    return true;
}

}  // namespace gm20b
