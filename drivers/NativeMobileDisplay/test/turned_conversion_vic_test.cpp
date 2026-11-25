// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativePlanesTurn.h's turnedConversionOf through VIC's own builder and the unit's model (TegraVIC test/vic_model.h,
// which reproduces the boots' transpose and sub-rectangle lines): the plane the conversion job writes is the layer's
// turned surface where it lands on the screen, pixel for pixel, for every word, clipped and scaled.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../NativePlanes.h"
#include "../../TegraVIC/test/TegraVICProbe.h"
#include "../../TegraVIC/test/vic_model.h"

using namespace native_planes;

namespace {
constexpr uint64_t kSource = 0x10000000, kTarget = 0x18000000;
constexpr uint32_t kSourceWidth = 56, kSourceHeight = 48, kScreenWidth = 40, kScreenHeight = 36;

uint32_t pixel(uint32_t x, uint32_t y)
{
    return 0xff000000u | (x * 17u & 255u) << 16 | (y * 29u & 255u) << 8 | ((x + y) * 11u & 255u);
}

// The turned whole surface's pixel at (ox, oy), by NativePlanesTurn.h's mapping.
uint32_t turnedPixel(uint32_t transform, int64_t ox, int64_t oy)
{
    const Turn t = turnOf(transform);
    int64_t a = t.transpose ? oy : ox, b = t.transpose ? ox : oy;
    if (t.flipX)
        a = kSourceWidth - 1 - a;
    if (t.flipY)
        b = kSourceHeight - 1 - b;
    return pixel(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
}

struct Converted {
    TurnedConversion conversion = {};
    std::vector<uint8_t> target;
    uint32_t pitch = 0;
    VicModel model{};
};

// numerator / denominator: the destination's size against the turned source rectangle's.
void convert(uint32_t transform, uint32_t numerator, uint32_t denominator, int32_t x, int32_t y, Layer &l,
             Converted &run)
{
    l = {};
    l.kind = kKindBgra;
    l.surface.width = kSourceWidth;
    l.surface.height = kSourceHeight;
    l.src = {4, 8, 24, 16};
    l.transform = transform;
    const Rect o = turnedRect(l.src, kSourceWidth, kSourceHeight, transform);
    l.dst = {x, y, static_cast<int32_t>(o.w * numerator / denominator),
             static_cast<int32_t>(o.h * numerator / denominator)};
    ASSERT_TRUE(turnedConversionOf(l, kScreenWidth, kScreenHeight, run.conversion));
    const auto &c = run.conversion;

    static std::vector<uint8_t> source(size_t(TEGRA_VIC_PROBE_PITCH) * kSourceHeight);
    for (uint32_t row = 0; row != kSourceHeight; ++row)
        for (uint32_t column = 0; column != kSourceWidth; ++column) {
            const uint32_t value = pixel(column, row);
            memcpy(source.data() + size_t(row) * TEGRA_VIC_PROBE_PITCH + size_t(column) * 4, &value, 4);
        }
    run.pitch = (static_cast<uint32_t>(c.width) * 4 + 255) & ~255u;
    run.target.assign(size_t(run.pitch) * uint32_t(c.height), TEGRA_VIC_POISON_BYTE);
    run.model.region[0] = {kSource, source.data(), source.size()};
    run.model.region[1] = {kTarget, run.target.data(), run.target.size()};
    run.model.regions = 2;
    run.model.flips = VIC_MODEL_FLIP_SURFACE;

    const auto input = tegra_vic_probe_bgra(kSource, source.size(), kSourceWidth, kSourceHeight);
    auto output = tegra_vic_probe_bgra(kTarget, run.target.size(), uint32_t(c.width), uint32_t(c.height));
    output.pitch[0] = run.pitch;
    TegraVICOp op = tegra_vic_probe_op(1, 1, uint32_t(c.width), uint32_t(c.height), transform);
    op.srcX = c.srcX;
    op.srcY = c.srcY;
    op.srcW = c.srcW;
    op.srcH = c.srcH;
    TegraVICJob job{};
    const char *why = tegra_vic_build(&input, &output, &op, &kTegraVICTraits, 0x80001000, 0x80002000, &job);
    ASSERT_EQ(why, nullptr) << why;
    vic_model_run(&run.model, &job);
    EXPECT_EQ(run.model.clipped + run.model.faults, 0u);
    EXPECT_EQ(run.model.writes, uint64_t(c.width) * uint64_t(c.height));
}

TEST(TurnedConversionVic, EveryWordWritesTheTurnedSurfaceWhereItLands)
{
    struct Place {
        int32_t x, y;
    };

    // Whole on the screen, cut on the left and top, cut on the right and bottom; even, so a 2x scale stays exact.
    static constexpr Place kPlaces[] = {{2, 4}, {-4, -6}, {28, 20}};
    for (uint32_t transform = 0; transform != 8; ++transform)
        for (const uint32_t scale : {1u, 2u})
            for (const Place place : kPlaces) {
                SCOPED_TRACE(testing::Message()
                             << "word " << transform << " scale " << scale << " at " << place.x << "," << place.y);
                Layer l;
                Converted run;
                convert(transform, scale, 1, place.x, place.y, l, run);
                if (HasFatalFailure())
                    return;
                const auto &c = run.conversion;
                const Rect o = turnedRect(l.src, kSourceWidth, kSourceHeight, transform);
                unsigned wrong = 0;
                for (int32_t row = 0; row != c.screen.h; ++row)
                    for (int32_t column = 0; column != c.screen.w; ++column) {
                        const int64_t ox = o.x + (c.screen.x + column - l.dst.x) / int32_t(scale);
                        const int64_t oy = o.y + (c.screen.y + row - l.dst.y) / int32_t(scale);
                        uint32_t got = 0;
                        memcpy(&got, run.target.data() + size_t(row) * run.pitch + size_t(column) * 4, 4);
                        if (got != turnedPixel(transform, ox, oy) && wrong++ == 0)
                            ADD_FAILURE() << "first wrong pixel " << column << "," << row << ": " << std::hex << got
                                          << " for " << turnedPixel(transform, ox, oy);
                    }
                EXPECT_EQ(wrong, 0u);
            }
}

// Shown smaller than the turned source, down to 1/6 (a YUV layer's published limit), and down to a sliver one pixel on
// the screen: VIC takes the job and writes the whole plane inside its bounds.
TEST(TurnedConversionVic, ADownscaledTurnIsWithinVICsBounds)
{
    for (uint32_t transform = 0; transform != 8; ++transform)
        for (const uint32_t denominator : {2u, 4u, 6u})
            for (const int32_t at : {0, -1, int32_t(kScreenWidth) - 1}) {
                SCOPED_TRACE(testing::Message() << "word " << transform << " 1/" << denominator << " at " << at);
                Layer l;
                Converted run;
                convert(transform, 1, denominator, at, at < 0 ? at : 0, l, run);
                if (HasFatalFailure())
                    return;
            }
}
}  // namespace
