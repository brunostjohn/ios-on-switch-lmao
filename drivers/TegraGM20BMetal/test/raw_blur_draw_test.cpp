// SPDX-License-Identifier: MIT
// Core Animation's blur-chain and one-colour fill draws (raw_ca_route.h) take their state's fragment program. Choosing the
// 2D engine's blit or the ROP's constant colour for them needs their per-draw bytes (taps, texture matrix, corners,
// indices, every vertex's colour), which the encoder would read on the CPU every frame, so neither route exists.
#include <gtest/gtest.h>
#include <type_traits>
#include <utility>
#include "../raw/runtime/raw_ca_route.h"
#include "../raw/runtime/raw_gpu.h"

namespace raw {
struct EncoderHarness {
    using Shortcut = RenderEncoder::Shortcut;
};
}  // namespace raw

namespace {

template <class T, class = void> struct NamesABlurRoute : std::false_type {};

template <class T>
struct NamesABlurRoute<T, std::void_t<decltype(std::declval<T &>().blurFunction)>> : std::true_type {};

template <class T, class = void> struct NamesAConstantColour : std::false_type {};

template <class T>
struct NamesAConstantColour<T, std::void_t<decltype(std::declval<T &>().constant)>> : std::true_type {};

TEST(BlurDraw, OneColourFillsCarryNoConstantColourShortcut)
{
    EXPECT_FALSE(NamesAConstantColour<raw::EncoderHarness::Shortcut>::value);
}

TEST(BlurDraw, BlurChainStatesCarryNoDrawRoute)
{
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tds4A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tdb2A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_FALSE(NamesABlurRoute<raw::Pipeline>::value);
}

}  // namespace
