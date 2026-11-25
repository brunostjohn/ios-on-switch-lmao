// SPDX-License-Identifier: MIT
// band_rates.h on the host: a state is priced by its instructions until one of its large draws is
// measured, then by the measured rate, so a draw bound by memory or the ROP is banded as long as it really is.
#include <gtest/gtest.h>

#include <cstdint>

#include "../band_rates.h"
#include "../draw_bands.h"

namespace {

using gm20b::BandDraw;
using gm20b::BandRate;

constexpr uint64_t kScreen = 720 * 1280;

uint32_t bandsOf(const BandRate &rate, uint64_t units, uint32_t instructions)
{
    BandDraw d;
    d.fragmentNs = gm20b::estimatedNs(rate, units, gm20b::workNs(gm20b::laneWork(units, instructions)));
    d.rows = 1280;
    return gm20b::bandCount(d, TGM_APP_BAND_NS, gm20b::kBandsPerCommandBuffer);
}

// The poster's 4x resolve (T-v2 gpuprof: 11.5-12 ms over a 720x1280 drawable, 642 instructions): priced 3 ms, so four
// bands of what are really three-millisecond pieces; measured once, twelve bands of one
TEST(BandRates, AMeasuredResolveIsBandedByItsMeasuredLength)
{
    BandRate rate;
    EXPECT_EQ(gm20b::estimatedNs(rate, kScreen, 3'009'000), 3'009'000u);
    EXPECT_EQ(bandsOf(rate, kScreen, 642), 4u);
    gm20b::learn(rate, 11'500'000, kScreen);
    EXPECT_EQ(rate.samples.load(), 1u);
    EXPECT_NEAR(double(gm20b::estimatedNs(rate, kScreen, 0)), 11.5e6, 2e3);
    EXPECT_EQ(bandsOf(rate, kScreen, 642), 12u);
}

// A full-screen draw of a ten-instruction program into a 4x RGBA16F target with blending: 0.19 ms by its instructions,
// five milliseconds of ROP traffic; once measured it is banded
TEST(BandRates, AMemoryBoundDrawIsBandedOnceMeasured)
{
    BandRate rate;
    const uint64_t units = kScreen * 4;
    EXPECT_EQ(bandsOf(rate, units, 10), 1u);
    gm20b::learn(rate, 5'000'000, units);
    EXPECT_EQ(bandsOf(rate, units, 10), 5u);
    EXPECT_EQ(bandsOf(rate, units / 4, 10), 2u);
}

TEST(BandRates, LongerSamplesRaiseAtOnceAndShorterFallByAQuarter)
{
    BandRate rate;
    gm20b::learn(rate, 4'000'000, 1'000'000);
    EXPECT_EQ(rate.psPerUnit.load(), 4000u);
    gm20b::learn(rate, 8'000'000, 1'000'000);
    EXPECT_EQ(rate.psPerUnit.load(), 8000u);
    gm20b::learn(rate, 4'000'000, 1'000'000);
    EXPECT_EQ(rate.psPerUnit.load(), 7000u);
    gm20b::learn(rate, 0, 1'000'000);
    gm20b::learn(rate, 1'000'000, 0);
    EXPECT_EQ(rate.psPerUnit.load(), 7000u);
    EXPECT_EQ(rate.samples.load(), 3u);
    gm20b::learn(rate, 1, 1'000'000'000);
    EXPECT_GE(rate.psPerUnit.load(), 1u);
}

// Small draws are never measured; a state's first large ones are, then one in kRemeasureEvery
TEST(BandRates, ItsFirstLargeDrawsAreMeasuredThenOneInAWhile)
{
    BandRate rate;
    EXPECT_FALSE(gm20b::measure(rate, gm20b::kMeasureUnits - 1));
    for (uint32_t i = 0; i < gm20b::kWarmSamples; i++) {
        EXPECT_TRUE(gm20b::measure(rate, gm20b::kMeasureUnits));
        gm20b::learn(rate, 1'000'000, gm20b::kMeasureUnits);
    }
    uint32_t measured = 0;
    for (uint32_t i = 0; i < 4 * gm20b::kRemeasureEvery; i++)
        measured += gm20b::measure(rate, gm20b::kMeasureUnits);
    EXPECT_EQ(measured, 4u);
}

// A geometry-bound state (VectorKit's ground draws, 25 Nov: 48-208 instructions, 8-17 ms in Maps at 1.0-2.9 ms a band)
// drawn, measured and drawn again: every band repeats its vertices, so a rate that booked that repeat as pixel cost
// priced each next draw longer and banded it more. With the band's cost learned apart, it settles on few bands at its
// pixels' own rate.
TEST(BandRates, BandsOfAGeometryBoundStateDoNotFeedTheirOwnPrice)
{
    constexpr uint64_t kPixelNs = 500'000, kPerBandNs = 2'000'000;
    constexpr uint32_t kInstructions = 64;
    const uint64_t staticPixelNs = gm20b::workNs(gm20b::laneWork(kScreen, kInstructions));
    BandRate rate;
    uint32_t bands = 0;
    for (int round = 0; round < 12; round++) {
        BandDraw d;
        d.fragmentNs = gm20b::estimatedNs(rate, kScreen, staticPixelNs);
        d.vertexNs = rate.nsPerBand.load();
        d.rows = 720;
        d.cols = 1280;
        d.unmeasured = !gm20b::measuredOnce(rate);
        bands = gm20b::bandGrid(d, TGM_APP_BAND_NS, gm20b::kMaxTileBands, gm20b::kMaxTileBands).total();
        gm20b::learn(rate, kPixelNs + bands * kPerBandNs, kScreen, bands, staticPixelNs);
        if (round >= 3)
            EXPECT_LE(bands, 2u) << "round " << round;
    }
    EXPECT_NEAR(double(gm20b::estimatedNs(rate, kScreen, staticPixelNs)), double(kPixelNs), kPixelNs * 0.5);
    EXPECT_NEAR(double(rate.nsPerBand.load()), double(kPerBandNs), kPerBandNs * 0.5);
}

TEST(BandRates, ACopiedStateKeepsItsRate)
{
    BandRate rate;
    gm20b::learn(rate, 2'000'000, 1'000'000);
    const BandRate copy = rate;
    EXPECT_EQ(copy.psPerUnit.load(), 2000u);
    EXPECT_EQ(copy.samples.load(), 1u);
}

TEST(BandRates, AHugeDrawsPriceDoesNotOverflow)
{
    BandRate rate;
    rate.psPerUnit = UINT32_MAX;
    EXPECT_GT(gm20b::estimatedNs(rate, UINT64_MAX / 2, 0), uint64_t(1e15));
}

}  // namespace
