// SPDX-License-Identifier: MIT
// raw/runtime/raw_fetch_rungs.h: where the ROP's blend and iterated blend may stand for an in-shader blend.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h"

namespace {

TEST(RawFetchRungs, IteratedBlendWaitsForTheDeviceAndOneTarget)
{
    raw::IteratedAsk a;
    a.candidate = true;
    a.colourTargets = 1;
    a.programWords = 13;
    a.constants = 3;
    EXPECT_EQ(raw::iteratedBlendUsable(a), raw::iteratedBlendProven());
    EXPECT_FALSE(raw::iteratedBlendProven()) << "gpu.x1.iterated_blend has not passed on the chip";
    a.colourTargets = 2;
    EXPECT_FALSE(raw::iteratedBlendUsable(a));
}

}  // namespace
