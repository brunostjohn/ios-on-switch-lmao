// SPDX-License-Identifier: MIT
// Rule clear.twod's colour words: Metal's codes packed as the UNORM view's solid colour, sRGB channels encoded.
#include <gtest/gtest.h>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_twod_fill.h"

namespace {

TEST(TwodFill, WordsInTheUnormViewsPacking)
{
    const float rgba[4] = {1.0f, 0.5f, 0.0f, 0.25f};
    EXPECT_EQ(raw::twodFillWord(80, rgba), 0x40ff8000u);  // A8R8G8B8: alpha, red, green, blue
    EXPECT_EQ(raw::twodFillWord(70, rgba), 0x400080ffu);  // A8B8G8R8: alpha, blue, green, red
    EXPECT_EQ(raw::twodFillWord(81, rgba), 0x40ffbc00u);  // green 0.5 encodes to 188
    const float out[4] = {2.0f, -1.0f, 0.0f, 1.0f};
    EXPECT_EQ(raw::twodFillWord(80, out), 0xffff0000u);
}

TEST(TwodFill, NoFormatHoldsBeforeTheChipsCells)
{
    for (uint32_t f : {115u})
        EXPECT_FALSE(raw::twodFillHolds(f)) << f;
    EXPECT_TRUE(raw::twodFillHolds(71u));
    EXPECT_TRUE(raw::twodFillHolds(81u));
    EXPECT_TRUE(raw::twodFillHolds(70u));
    EXPECT_TRUE(raw::twodFillHolds(80u));
}

}  // namespace
