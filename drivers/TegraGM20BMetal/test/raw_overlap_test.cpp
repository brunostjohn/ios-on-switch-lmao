// SPDX-License-Identifier: MIT
// raw/runtime/raw_overlap.h on the host: a framebuffer-fetching draw's triangles split into runs no two of whose
// triangles can cover the same pixel: one run for Core Animation's quads, nine-part grids and strips joined by degenerate
// triangles; a new run where triangles overlap, fold over or meet at a vertex that is not one of both.
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "../raw/runtime/raw_overlap.h"

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
const float kPixels[16] = {2.0f / kWidth, 0, 0, 0, 0, -2.0f / kHeight, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};

using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

struct Mesh {
    std::vector<float> positions;
    std::vector<uint16_t> indices;

    uint32_t vertex(float x, float y)
    {
        positions.insert(positions.end(), {x, y, 0.5f, 1});
        return uint32_t(positions.size() / 4 - 1);
    }

    raw::DrawGeometry geometry(uint32_t count) const
    {
        raw::DrawGeometry g;
        g.bounded = true;
        g.matrix = reinterpret_cast<const uint8_t *>(kPixels);
        g.positions = {reinterpret_cast<const uint8_t *>(positions.data()), 16, 0, 4};
        g.positionBytes = positions.size() * sizeof(float);
        g.count = count;
        g.viewport = {0, 0, kWidth, kHeight};
        g.limit = {0, 0, kWidth, kHeight};
        if (!indices.empty()) {
            g.indices = reinterpret_cast<const uint8_t *>(indices.data());
            g.indexSize = 2;
        }
        return g;
    }

    // a w x h grid of quads, each split along its diagonal, as an indexed triangle list
    void grid(float x0, float y0, const std::vector<float> &xs, const std::vector<float> &ys)
    {
        std::vector<std::vector<uint32_t>> at(ys.size() + 1, std::vector<uint32_t>(xs.size() + 1));
        float y = y0;
        for (size_t j = 0; j <= ys.size(); y += j < ys.size() ? ys[j] : 0, j++) {
            float x = x0;
            for (size_t i = 0; i <= xs.size(); x += i < xs.size() ? xs[i] : 0, i++)
                at[j][i] = vertex(x, y);
        }
        for (size_t j = 0; j < ys.size(); j++)
            for (size_t i = 0; i < xs.size(); i++) {
                const uint16_t a = at[j][i], b = at[j][i + 1], c = at[j + 1][i], d = at[j + 1][i + 1];
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
    }
};

Runs split(const Mesh &m, bool strip, uint32_t count, bool *bounded = nullptr)
{
    raw::TriangleRuns t;
    Runs runs;
    const bool ok = t.split(m.geometry(count), strip, runs);
    if (bounded)
        *bounded = ok;
    else
        EXPECT_TRUE(ok);
    return runs;
}

TEST(RawOverlap, QuadStripAndListAreOneRun)
{
    Mesh strip;
    strip.vertex(10, 30), strip.vertex(20, 30), strip.vertex(10, 50), strip.vertex(20, 50);
    EXPECT_TRUE(split(strip, true, 4).empty());
    Mesh list;
    for (const auto &[x, y] :
         std::vector<std::pair<float, float>>{{10, 30}, {20, 30}, {10, 50}, {20, 30}, {20, 50}, {10, 50}})
        list.vertex(x, y);
    EXPECT_TRUE(split(list, false, 6).empty());
}

TEST(RawOverlap, NinePartGridIsOneRun)
{
    Mesh m;
    m.grid(100.25f, 40, {12, 200.5f, 12}, {12, 90, 12});
    EXPECT_TRUE(split(m, false, uint32_t(m.indices.size())).empty());
}

TEST(RawOverlap, OverlappingQuadsSplitWhereTheSecondBegins)
{
    Mesh m;
    m.grid(0, 0, {100}, {100});
    m.grid(50, 50, {100}, {100});
    EXPECT_EQ(split(m, false, 12), (Runs{{0, 6}, {6, 12}}));
}

TEST(RawOverlap, IndicesCountFromTheBaseVertex)
{
    // two overlapping quads from vertex 4, after a quad no index reaches from there
    Mesh m;
    m.grid(500, 500, {10}, {10});
    m.indices.clear();
    m.grid(0, 0, {100}, {100});
    m.grid(50, 50, {100}, {100});
    for (uint16_t &i : m.indices)
        i = uint16_t(i - 4);
    raw::TriangleRuns t;
    Runs runs;
    raw::DrawGeometry g = m.geometry(12);
    g.first = 4;
    EXPECT_TRUE(t.split(g, false, runs));
    EXPECT_EQ(runs, (Runs{{0, 6}, {6, 12}}));
}

TEST(RawOverlap, FoldedStripSplits)
{
    Mesh m;
    m.vertex(10, 30), m.vertex(20, 30), m.vertex(20, 50), m.vertex(10, 50);
    EXPECT_EQ(split(m, true, 4), (Runs{{0, 3}, {1, 4}}));
}

TEST(RawOverlap, AVertexOnAnEdgeThatIsNotOneOfItsEndsSplits)
{
    Mesh m;  // a T-junction: the second and third triangles meet the first along its edge at a vertex of their own
    m.vertex(0, 0), m.vertex(100, 0), m.vertex(0, 100);
    m.vertex(0, 100), m.vertex(50, 50), m.vertex(100, 100);
    m.vertex(50, 50), m.vertex(100, 0), m.vertex(100, 100);
    EXPECT_FALSE(split(m, false, 9).empty());
}

TEST(RawOverlap, AnAxisAlignedTJunctionIsOneRun)
{
    Mesh m;  // the right two triangles meet the left one's vertical edge at a vertex of their own, exactly on it
    m.vertex(0, 0), m.vertex(100, 0), m.vertex(100, 100);
    m.vertex(100, 0), m.vertex(200, 50), m.vertex(100, 50);
    m.vertex(100, 50), m.vertex(200, 50), m.vertex(100, 100);
    EXPECT_TRUE(split(m, false, 9).empty());
}

TEST(RawOverlap, StripsJoinedByDegenerateTrianglesAreOneRun)
{
    Mesh m;  // two quads in one strip, joined by repeating the first's last and the second's first vertex
    const uint32_t a = m.vertex(0, 0), b = m.vertex(10, 0), c = m.vertex(0, 10), d = m.vertex(10, 10);
    const uint32_t e = m.vertex(50, 0), f = m.vertex(60, 0), g = m.vertex(50, 10), h = m.vertex(60, 10);
    for (uint32_t v : {a, b, c, d, d, e, e, f, g, h})
        m.indices.push_back(uint16_t(v));
    EXPECT_TRUE(split(m, true, uint32_t(m.indices.size())).empty());
}

TEST(RawOverlap, RestartsBeginNewStrips)
{
    Mesh m;
    const uint32_t a = m.vertex(0, 0), b = m.vertex(10, 0), c = m.vertex(0, 10), d = m.vertex(10, 10);
    const uint32_t e = m.vertex(5, 5), f = m.vertex(15, 5), g = m.vertex(5, 15), h = m.vertex(15, 15);
    for (uint32_t v : {a, b, c, d, 0xffffu, e, f, g, h})
        m.indices.push_back(uint16_t(v));
    EXPECT_EQ(split(m, true, uint32_t(m.indices.size())), (Runs{{0, 4}, {5, 9}}));
}

TEST(RawOverlap, TooManyTrianglesOrABadVertexLeaveTheDrawWhole)
{
    Mesh many;
    for (uint32_t i = 0; i < 3 * (raw::TriangleRuns::kMaxTriangles + 1); i++)
        many.vertex(float(i % 7), float(i % 5));
    bool bounded = true;
    split(many, false, uint32_t(many.positions.size() / 4), &bounded);
    EXPECT_FALSE(bounded);
    Mesh behind;
    behind.vertex(0, 0), behind.vertex(10, 0), behind.vertex(0, 10);
    behind.positions[3] = -1;
    split(behind, false, 3, &bounded);
    EXPECT_FALSE(bounded);
    Mesh past;
    past.vertex(0, 0), past.vertex(10, 0), past.vertex(0, 10);
    raw::TriangleRuns t;
    Runs runs;
    raw::DrawGeometry g = past.geometry(3);
    g.positionBytes = 2 * 16;
    EXPECT_FALSE(t.split(g, false, runs));
}

TEST(RawOverlap, BoundsCoverageAndSelfOrderingUseTheSameSnapshot)
{
    Mesh m;
    m.grid(0, 0, {float(kWidth)}, {float(kHeight)});
    auto g = m.geometry(6);
    g.strip = false;
    raw::GeometryCache cache;
    raw::GeometryBudget budget{6, 1};
    auto analysis = cache.analysis(g, 1, budget);
    raw::TriangleRuns triangles;
    Runs runs;
    ASSERT_TRUE(triangles.split(analysis, runs));
    EXPECT_TRUE(runs.empty());
    ASSERT_TRUE(analysis.covers(true, kWidth, kHeight));
    EXPECT_EQ(budget.vertices, 0u);
    EXPECT_EQ(budget.pairs, 0u);
    // The caller retains ownership; no second read or quadratic comparison is required for identical draws.
    for (unsigned i = 0; i < 10000; ++i) {
        auto next = cache.analysis(g, 1, budget);
        ASSERT_TRUE(triangles.split(next, runs));
        ASSERT_TRUE(next.covers(true, kWidth, kHeight));
    }
}

TEST(RawOverlap, PairBudgetExhaustionRefusesTheProofAndClearsPartialRuns)
{
    Mesh m;
    m.grid(0, 0, {100}, {100});
    m.grid(10, 10, {100}, {100});
    auto g = m.geometry(12);
    g.strip = false;
    raw::GeometryBudget budget{12, 1};
    raw::GeometryCache cache;
    auto analysis = cache.analysis(g, 1, budget);
    raw::TriangleRuns triangles;
    Runs runs{{0, 12}};
    EXPECT_FALSE(triangles.split(analysis, runs));
    EXPECT_TRUE(runs.empty());
    EXPECT_EQ(budget.pairs, 0u);
    EXPECT_FALSE(triangles.split(analysis, runs));
    EXPECT_EQ(budget.vertices, 0u);
}

TEST(RawOverlap, OrderedRunBoundsReuseTheWholeIndexedDrawProjection)
{
    Mesh m;
    m.grid(0, 0, {100}, {100});
    m.grid(50, 50, {100}, {100});
    auto g = m.geometry(12);
    g.strip = false;
    raw::GeometryBudget budget{12, 100};
    raw::GeometryCache cache;
    auto whole = cache.analysis(g, 1, budget);
    raw::TriangleRuns triangles;
    Runs runs;
    ASSERT_TRUE(triangles.split(whole, runs));
    ASSERT_EQ(runs, (Runs{{0, 6}, {6, 12}}));
    g.indices += 6 * g.indexSize;
    g.count = 6;
    auto second = cache.analysis(g, 1, budget);
    raw::PixelBox box;
    ASSERT_TRUE(second.bounds(box));
    EXPECT_GT(box.x0, 40u);
    EXPECT_GT(box.y0, 40u);
    EXPECT_LT(box.x1, 160u);
    EXPECT_EQ(budget.vertices, 0u);
}

TEST(RawOverlap, ListAllOnesIndexIsNotAStripRestart)
{
    Mesh m;
    m.vertex(0, 0); m.vertex(10, 0); m.vertex(0, 10);
    m.indices = {0, 1, 0xffff};
    bool bounded = true;
    split(m, false, 3, &bounded);
    EXPECT_FALSE(bounded); // a list really addresses vertex65535, outside this allocation
    split(m, true, 3, &bounded);
    EXPECT_TRUE(bounded); // the restart leaves no triangle
}

TEST(RawOverlap, StripRunKeepsWindingParityAndRestartResetsIt)
{
    Mesh m;
    m.vertex(10, 30); m.vertex(20, 30); m.vertex(20, 50); m.vertex(10, 50);
    auto g = m.geometry(4);
    raw::GeometryAnalysis folded(g);
    raw::TriangleRuns triangles;
    Runs runs;
    ASSERT_TRUE(triangles.split(folded, runs));
    ASSERT_EQ(runs, (Runs{{0, 3}, {1, 4}}));
    const auto winding = raw::TriangleRuns::winding(folded, runs);
    EXPECT_FALSE(winding[0]);
    EXPECT_TRUE(winding[1]);
    m.indices = {0, 1, 2, 0xffff, 0, 1, 2};
    raw::GeometryAnalysis restarted(m.geometry(7));
    ASSERT_TRUE(triangles.split(restarted, runs));
    ASSERT_EQ(runs, (Runs{{0, 3}, {4, 7}}));
    EXPECT_FALSE(raw::TriangleRuns::winding(restarted, runs)[1]);
    raw::GeometryCache cache;
    raw::GeometryBudget budget;
    auto full = cache.analysis(g, 1, budget);
    raw::PixelBox box;
    ASSERT_TRUE(full.bounds(box));
    g.first = 1;
    g.count = 3;
    auto subrange = cache.analysis(g, 1, budget);
    EXPECT_FALSE(raw::TriangleRuns::winding(subrange, Runs{{0, 3}})[0]);
}

TEST(RawOverlap, UnavailableCpuGeometryStillHasOrderedDirectQuadPlan)
{
    raw::DrawGeometry g;
    g.bounded = true;
    g.count = 6;
    g.strip = false;
    // No CPU-accessible input: overlap analysis must fail without touching memory.
    raw::GeometryAnalysis analysis(g);
    raw::TriangleRuns triangles;
    Runs runs;
    ASSERT_FALSE(triangles.split(analysis, runs));
    std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
    ASSERT_TRUE(raw::TriangleRuns::sequential(3, 6, 1, false, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {3, 6}}));
    EXPECT_FALSE(winding[0]);
    EXPECT_FALSE(winding[1]);
    ASSERT_TRUE(raw::TriangleRuns::sequential(4, 4, 1, false, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {1, 4}}));
    EXPECT_FALSE(winding[0]);
    EXPECT_TRUE(winding[1]);
}

TEST(RawOverlap, SequentialPlanPreservesEveryPrimitiveAndItsProvokingVertex)
{
    for (uint32_t primitive : {3u, 4u}) {
        for (uint32_t count = 0; count <= 290; ++count) {
            Runs runs;
            std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
            const uint32_t expected = primitive == 3 ? count / 3 : (count > 2 ? count - 2 : 0);
            const bool admitted = raw::TriangleRuns::sequential(primitive, count, 1, false, runs, winding);
            ASSERT_EQ(admitted, expected <= raw::TriangleRuns::kMaxTriangles);
            if (!admitted) {
                EXPECT_TRUE(runs.empty());
                continue;
            }
            ASSERT_EQ(runs.size(), expected);
            for (uint32_t i = 0; i < expected; ++i) {
                const uint32_t first = primitive == 3 ? i * 3 : i;
                EXPECT_EQ(runs[i].first, first); // same first/provoking vertex; only raster winding changes
                EXPECT_EQ(runs[i].second, first + 3);
                EXPECT_EQ(winding[i], primitive == 4 && (i & 1));
            }
        }
    }
}

TEST(RawOverlap, SequentialPlanRefusesUnknownRestartAndInstanceOrdering)
{
    Runs runs{{0, 6}};
    std::array<bool, raw::TriangleRuns::kMaxTriangles> winding;
    EXPECT_FALSE(raw::TriangleRuns::sequential(4, 6, 1, true, runs, winding));
    EXPECT_TRUE(runs.empty());
    ASSERT_TRUE(raw::TriangleRuns::sequential(3, 9, 1, true, runs, winding));
    EXPECT_EQ(runs, (Runs{{0, 3}, {3, 6}, {6, 9}}));
    EXPECT_FALSE(raw::TriangleRuns::sequential(3, 6, 2, false, runs, winding));
    EXPECT_FALSE(raw::TriangleRuns::sequential(2, 6, 1, false, runs, winding));
    EXPECT_FALSE(raw::TriangleRuns::sequential(4, UINT32_MAX, 1, false, runs, winding));
    EXPECT_TRUE(runs.empty());
}

}  // namespace
