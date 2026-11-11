// SPDX-License-Identifier: MIT
// The plan cache: a repeated frame structure is planned once and costs a key and a lookup after that; parameters and the
// drawable's identity do not change the key, structure does.
#include "../plan_cache.h"
#include "../plan_key.h"
#include "fixtures.h"
#include <gtest/gtest.h>

namespace raw::plan::test {
namespace {

TEST(Cache, RepeatedStructureIsALookup)
{
    PlanCache cache;
    const Caps caps = chipCaps();
    const Plan *first = &cache.get(glassFrame1(), caps, Answers{});
    for (int frame = 0; frame < 60; ++frame) {
        Recording r = glassFrame1();
        for (Draw &d : r.draws)
            d.count += uint32_t(frame);
        EXPECT_EQ(&cache.get(r, caps, Answers{}), first);
    }
    EXPECT_EQ(cache.plans(), 1u);
    EXPECT_EQ(cache.hits(), 60u);
    EXPECT_TRUE(first->finding.plannable());
    EXPECT_FALSE(first->ops.empty());
}

TEST(Cache, AnotherDrawableOfTheSameShapeHits)
{
    Recording a = glassFrame1(), b = glassFrame1();
    b.resources[0].handle = {77, 9};
    b.resources[0].memory += 1000;
    EXPECT_EQ(structuralKey(a, chipCaps()), structuralKey(b, chipCaps()));
}

TEST(Cache, StructureMisses)
{
    PlanCache cache;
    const Caps caps = chipCaps();
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(glassFrame1(), caps, Answers{});
    Recording stale = glassFrame1();
    stale.resources[0].flags |= ResourceFacts::ImageStale;
    cache.get(stale, caps, Answers{});
    cache.get(glassFrame1(), chipCaps(1u << uint32_t(Probe::PassPixelBarrier)), Answers{});
    EXPECT_EQ(cache.plans(), 4u);
    EXPECT_EQ(cache.hits(), 0u);
}

TEST(Cache, LeastRecentlyUsedGoesFirst)
{
    PlanCache cache(2);
    const Caps caps = chipCaps();
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(glassFrame1(), caps, Answers{});
    cache.get(glassFrame0(), caps, Answers{});
    cache.get(scrollFrame(), caps, Answers{});
    EXPECT_EQ(cache.size(), 2u);
    cache.get(glassFrame0(), caps, Answers{});
    EXPECT_EQ(cache.hits(), 2u);
    cache.get(glassFrame1(), caps, Answers{});
    EXPECT_EQ(cache.plans(), 4u);
}

}  // namespace
}  // namespace raw::plan::test
