// SPDX-License-Identifier: MIT
// raw_texture3d.h: the bound texture a state's texture3d arguments sample, found by type. Boot 15's first key press drew Core
// Animation's generic state (two colour cubes) with a 1-texel BGRA8 texture at index 12; the facade sent it to m2v, whose
// pipelines for the keyboard took 18-20 s to make while the render thread waited for the command buffer's scheduling.
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_texture3d.h"

namespace {

struct Tex {
    bool volume;
};

const Tex flat{false}, cube{true}, other{true};

raw::Texture3dBinding<Tex> bound(std::initializer_list<std::pair<int, const Tex *>> at, uint32_t named = 0)
{
    const Tex *textures[16] = {};
    for (const auto &[k, t] : at)
        textures[k] = t;
    return raw::texture3dBinding(textures, 16, named, [](const Tex &t) {
        return t.volume;
    });
}

TEST(RawTexture3d, AnEarlierStatesTwoDTextureLeavesTheArgumentsEmpty)
{
    const auto b = bound({{12, &flat}}, 1u << 0);
    EXPECT_EQ(b.texture, nullptr);
    EXPECT_FALSE(b.ambiguous);
}

TEST(RawTexture3d, NothingBoundLeavesThemEmpty)
{
    const auto b = bound({});
    EXPECT_EQ(b.texture, nullptr);
    EXPECT_FALSE(b.ambiguous);
}

TEST(RawTexture3d, OneCubeTakesEveryArgument)
{
    const auto b = bound({{12, &cube}, {3, &flat}});
    EXPECT_EQ(b.texture, &cube);
    EXPECT_EQ(b.slot, 12);
    EXPECT_FALSE(b.ambiguous);
    const auto twice = bound({{12, &cube}, {13, &cube}});
    EXPECT_EQ(twice.texture, &cube);
    EXPECT_FALSE(twice.ambiguous);
}

TEST(RawTexture3d, ACubeAtAnIndexTheStateNamesIsNoArgumentsBinding)
{
    const auto b = bound({{3, &cube}}, 1u << 3);
    EXPECT_EQ(b.texture, nullptr);
    EXPECT_FALSE(b.ambiguous);
}

TEST(RawTexture3d, DistinctCubesCannotTellWhichTakesWhich)
{
    const auto b = bound({{12, &cube}, {13, &other}});
    EXPECT_TRUE(b.ambiguous);
    EXPECT_EQ(b.slot, 13);
}

}  // namespace
