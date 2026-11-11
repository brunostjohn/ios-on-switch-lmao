// SPDX-License-Identifier: MIT
// raw/runtime/raw_fetch_rungs.h: where the ROP's blend and iterated blend may stand for an in-shader blend.
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h"

namespace {

raw::FixedBlendAsk screenLayer()
{
    raw::FixedBlendAsk a;
    a.format = 80;
    a.standardFactors = true;
    a.eightBitSources = true;
    return a;
}

TEST(RawFetchRungs, AnEightBitLayerOnAScreenTargetTakesTheRop)
{
    EXPECT_TRUE(raw::fixedBlendAcceptable(screenLayer()));
    raw::FixedBlendAsk a = screenLayer();
    a.format = 70;
    EXPECT_TRUE(raw::fixedBlendAcceptable(a));
}

TEST(RawFetchRungs, ComputedColourTakesItTwoLayersDeep)
{
    raw::FixedBlendAsk a = screenLayer();
    a.eightBitSources = false;
    a.computedLayers = 2;
    EXPECT_TRUE(raw::fixedBlendAcceptable(a));
    a.computedLayers = 3;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a)) << "4 codes off by eight layers (UNITS.md stack table)";
}

TEST(RawFetchRungs, DataTargetsKeepTheExactBlend)
{
    for (uint16_t format : {uint16_t(90), uint16_t(115), uint16_t(110), uint16_t(71)}) {
        raw::FixedBlendAsk a = screenLayer();
        a.format = format;
        EXPECT_FALSE(raw::fixedBlendAcceptable(a)) << format;
    }
    raw::FixedBlendAsk a = screenLayer();
    a.readable = true;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
    a = screenLayer();
    a.accumulates = true;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
    a = screenLayer();
    a.standardFactors = false;
    EXPECT_FALSE(raw::fixedBlendAcceptable(a));
}

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
