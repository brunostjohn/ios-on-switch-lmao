// SPDX-License-Identifier: MIT
// Core Animation's blur-chain draws (raw_ca_route.h CaRoute::Blur) take their state's fragment program. Choosing another
// route for them needs their per-draw bytes (the taps, texture matrix, corners and indices), which the encoder would read on
// the CPU every frame, so no state carries a blur route for the draw path to classify.
#include <gtest/gtest.h>
#include <type_traits>
#include <utility>
#include "../raw/runtime/raw_ca_route.h"
#include "../raw/runtime/raw_gpu.h"

namespace {

template <class T, class = void> struct NamesABlurRoute : std::false_type {};

template <class T>
struct NamesABlurRoute<T, std::void_t<decltype(std::declval<T &>().blurFunction)>> : std::true_type {};

TEST(BlurDraw, BlurChainStatesCarryNoDrawRoute)
{
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tds4A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tdb2A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_FALSE(NamesABlurRoute<raw::Pipeline>::value);
}

}  // namespace
