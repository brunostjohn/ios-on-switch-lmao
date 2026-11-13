// SPDX-License-Identifier: MIT
// raw/runtime/raw_zbc.h's entries past the 8-bit unorm and half ones, from the clear words raw_clear.h hands the 3D
// engine: each holds the texel the ROP writes for the clear, packed in the format's channel order, or none is made.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../raw/runtime/raw_clear.h"
#include "../raw/runtime/raw_zbc.h"

namespace {

bool entry(uint32_t mtl, const float rgba[4], TegraGM20BAbiZbc &e)
{
    uint32_t ds[4];
    if (!raw::clearValue(mtl, rgba, ds))
        return false;
    return raw::zbcColorEntry(mtl, ds, e);
}

float linear(uint32_t code)
{
    const double e = code / 255.0;
    return float(e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4));
}

// gpu.raw.compressed_targets zbc_bgra8srgb and zbc_rgba8srgb
TEST(RawZbc, SrgbEntriesHoldTheEncodedCodes)
{
    TegraGM20BAbiZbc e;
    for (uint32_t k = 0; k < 256; k++) {
        const float rgba[4] = {linear(k), linear(255 - k), linear(k / 2), k / 255.0f};
        ASSERT_TRUE(entry(81, rgba, e)) << k;
        EXPECT_EQ(e.format, raw::kZbcA8Rl8Gl8Bl8);
        EXPECT_EQ(e.l2[0], (k / 2) | (255 - k) << 8 | k << 16 | k << 24) << k;
        ASSERT_TRUE(entry(71, rgba, e)) << k;
        EXPECT_EQ(e.format, raw::kZbcA8Bl8Gl8Rl8);
        EXPECT_EQ(e.l2[0], k | (255 - k) << 8 | (k / 2) << 16 | k << 24) << k;
    }
    const float off[4] = {0.5f, 0.5f, 0.5f, 1};
    ASSERT_TRUE(entry(81, off, e)) << "raw_clear.h hands a code's linear value for any colour";
    EXPECT_EQ(e.l2[0], 0xffbcbcbcu);
    uint32_t ds[4] = {raw::clear_detail::bitsOf(0.5f), 0, 0, raw::clear_detail::bitsOf(1.0f)};
    EXPECT_FALSE(raw::zbcColorEntry(81, ds, e)) << "a linear value between two codes";
}

// gpu.raw.compressed_targets zbc_rgb10a2
TEST(RawZbc, Rgb10a2EntriesPackRedLowAlphaHigh)
{
    TegraGM20BAbiZbc e;
    const float codes[4] = {1.0f, 0.0f, 512 / 1023.0f, 2 / 3.0f};
    ASSERT_TRUE(entry(90, codes, e));
    EXPECT_EQ(e.format, raw::kZbcA2B10G10R10);
    EXPECT_EQ(e.l2[0], 1023u | 512u << 20 | 2u << 30);
    const float any[4] = {0.3f, 0.6f, 0.9f, 0.5f};
    ASSERT_TRUE(entry(90, any, e)) << "raw_clear.h hands exact codes";
    EXPECT_EQ(e.l2[0], 307u | 614u << 10 | 921u << 20 | 2u << 30);
    uint32_t ds[4] = {raw::clear_detail::bitsOf(0.3f), 0, 0, raw::clear_detail::bitsOf(1.0f)};
    EXPECT_FALSE(raw::zbcColorEntry(90, ds, e)) << "306.9: no code";
}

}  // namespace
