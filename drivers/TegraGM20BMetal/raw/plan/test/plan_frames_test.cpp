// SPDX-License-Identifier: MIT
// Golden graphs of recorded frames: their nodes, and the barriers the edge rules place against today's unconditional
// prologue. The glass frames are Core Animation's as recorded with tile shaders off (out/agent-scratch/x1-glass/vp/
// out-deny.txt); the scroll frame's composition is assumed from pacing.list's measured shape.
#include "../plan_validate.h"
#include "fixtures.h"
#include "today_model.h"
#include <gtest/gtest.h>
#include <cstdio>

namespace raw::plan::test {
namespace {

void print(const char *name, const Counts &c)
{
    std::printf("%-26s drains %2u (switch %u)  wfi %2u  shinv %2u  shinv_nowfi %u  texinv %2u  ps %u  tiled %2u\n",
                name, c.drains, c.switchDrains, c.wfi, c.shaderInval, c.shaderInvalNoWfi, c.texInval, c.psBarriers,
                c.tiledFlushes);
}

struct Frame {
    Built built;
    Counts edges, relaxed, today;
};

Frame planFrame(const Recording &r, const char *name)
{
    Frame f;
    f.built = plan(r, chipCaps(), JobContext{});
    EXPECT_TRUE(validate(r, f.built, Answers{}, ChannelState{}).plannable()) << name;
    f.edges = countGraph(f.built.graph);
    f.relaxed = countGraph(plan(r, chipCaps(1u << uint32_t(Probe::PassPixelBarrier)), JobContext{}).graph);
    f.today = todayCounts(f.built.graph, r);
    std::printf("%s\n", name);
    print("  today (unconditional)", f.today);
    print("  edges (selectable)", f.edges);
    print("  edges + pixel probe", f.relaxed);
    std::printf("%s", describe(f.built.graph).c_str());
    return f;
}

TEST(Frames, GlassFirstFrame)
{
    const Recording r = glassFrame0();
    const Frame f = planFrame(r, "glass frame 0");
    const WorkGraph &g = f.built.graph;
    ASSERT_EQ(g.nodes.size(), 7u);
    for (uint32_t i = 0; i < 6; ++i)
        EXPECT_EQ(g.nodes[i].kind, NodeKind::RenderPass);
    EXPECT_EQ(g.nodes[6].kind, NodeKind::Present);
    EXPECT_EQ(g.nodes[0].before, 0) << "the content pass depends on nothing in this command buffer";
    for (uint32_t i = 1; i < 5; ++i)
        EXPECT_EQ(g.nodes[i].before, TiledCacheFlush | WaitForIdle | InvalidateTextureData) << i;
    EXPECT_EQ(g.nodes[5].before, TiledCacheFlush | WaitForIdle | InvalidateTextureData | InvalidateShaderData)
        << "the glass pass samples the pyramid and its interlock reads the drawable as storage";
    EXPECT_EQ(g.runs[g.nodes[5].runs.first + 1].before,
              TiledCacheFlush | FlushShaderData | InvalidateShaderData | InvalidateTextureData)
        << "the merge's storage access meets the background draws' colour writes";
    EXPECT_EQ(g.nodes[6].before, TiledCacheFlush | FlushShaderData) << "the present reads what the interlock stored";
    EXPECT_EQ(f.edges.shaderInval, 3u);
    EXPECT_LT(f.edges.drains, f.today.drains);
    EXPECT_LT(f.edges.shaderInval, f.today.shaderInval);
    EXPECT_EQ(f.relaxed.wfi, 0u);
}

TEST(Frames, GlassSteadyFrame)
{
    const Recording r = glassFrame1();
    const Frame f = planFrame(r, "glass frame 1");
    EXPECT_EQ(f.edges.wfi, 0u);
    EXPECT_EQ(f.edges.shaderInval, 2u);
    EXPECT_EQ(f.today.wfi, 1u);
}

TEST(Frames, Scroll)
{
    const Frame f = planFrame(scrollFrame(true), "scroll frame (synthetic, memoryless aliased per tag)");
    EXPECT_EQ(f.edges.shaderInval, 0u) << "no storage writes and no GPU-written constants: no shader-cache drain";
    EXPECT_GE(f.today.drains, 13u);
    const Frame own = planFrame(scrollFrame(false), "scroll frame (synthetic, a memoryless backing per pass)");
    EXPECT_LE(own.edges.drains, 4u);
}

}  // namespace
}  // namespace raw::plan::test
