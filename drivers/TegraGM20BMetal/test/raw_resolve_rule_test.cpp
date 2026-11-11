// SPDX-License-Identifier: MIT
// Rule resolve.twod: where it is taken, and the bound it rests on. The 2D engine's 2:1 box is the samples' mean stored by
// the ROP's unorm rule (nearest, ties down); Metal's resolve is (sum + n/2) / n (ties up). Read as one rounding, or as
// two rounded 2:1 steps (gpu.x1.twod_resolve's pair readings), the two are never more than one code apart.
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_rungs.h"

namespace {

uint32_t metal(const uint32_t *s, uint32_t n)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += s[i];
    return (sum + n / 2) / n;
}

uint32_t tiesDown(uint32_t sum, uint32_t n)
{
    return (2 * sum + n - 1) / (2 * n);
}

TEST(ResolveRule, TakenForScreenBoundUnorm8Only)
{
    raw::ResolveAsk a;
    a.format = 80;
    a.samples = 4;
    a.wholeTarget = true;
    EXPECT_TRUE(raw::twodResolveAcceptable(a));
    a.samples = 2;
    EXPECT_TRUE(raw::twodResolveAcceptable(a));
    a.format = 81;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.format = 70;
    a.readable = true;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.readable = false;
    a.wholeTarget = false;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
    a.wholeTarget = true;
    a.samples = 8;
    EXPECT_FALSE(raw::twodResolveAcceptable(a));
}

TEST(ResolveRule, TiesDownIsWithinOneCodeOfMetal)
{
    for (uint32_t a = 0; a < 256; a += 3)
        for (uint32_t b = 0; b < 256; b += 5) {
            const uint32_t two[2] = {a, b};
            const uint32_t twod = tiesDown(a + b, 2);
            EXPECT_LE(std::abs(int(twod) - int(metal(two, 2))), 1);
            EXPECT_EQ(twod != metal(two, 2), (a + b) % 2 == 1);
            for (uint32_t c : {0u, 1u, 128u, 255u}) {
                const uint32_t four[4] = {a, b, c, (a + c) / 2};
                const uint32_t once = tiesDown(a + b + c + four[3], 4);
                const uint32_t twice = tiesDown(tiesDown(a + b, 2) + tiesDown(c + four[3], 2), 2);
                EXPECT_LE(std::abs(int(once) - int(metal(four, 4))), 1);
                EXPECT_LE(std::abs(int(twice) - int(metal(four, 4))), 1);
            }
        }
}

}  // namespace
