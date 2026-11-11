// SPDX-License-Identifier: MIT
// No rule reads draw data: counts, offsets, instances, clear values, event values, binding offsets, handles and memory
// numbering can all change and the key, every node, engine, rule, run and barrier, and the lowered emitter sequence stay.
#include "../plan_key.h"
#include "../plan_lower.h"
#include "fixtures.h"
#include <gtest/gtest.h>
#include <map>
#include <random>

namespace raw::plan::test {
namespace {

Recording perturb(Recording r, std::mt19937 &rng)
{
    std::uniform_int_distribution<uint32_t> any(0, 100000);
    for (Draw &d : r.draws) {
        d.first = any(rng);
        d.count = any(rng) + 1;
        d.instances = any(rng) % 8 + 1;
        d.baseInstance = any(rng);
        d.baseVertex = int32_t(any(rng)) - 50000;
        d.indexOffset = uint64_t(any(rng)) * (d.indexSize ? d.indexSize : 1);
        d.argumentOffset = uint64_t(any(rng)) * 4;
    }
    for (PassDesc &p : r.passes) {
        for (Attachment &a : p.color)
            for (float &c : a.clear)
                c = float(any(rng)) / 1000.0f;
        p.clearDepth = double(any(rng)) / 100000.0;
        p.clearStencil = any(rng) & 0xff;
    }
    for (Binding &b : r.bindings)
        b.offset = uint64_t(any(rng)) * 256;
    for (Event &e : r.events)
        e.value = any(rng);
    for (Transfer &t : r.transfers) {
        t.sourceOffset = any(rng);
        t.destinationOffset = any(rng);
        t.value = uint8_t(any(rng));
    }
    std::map<uint32_t, uint32_t> renumbered;
    for (ResourceFacts &f : r.resources) {
        f.handle.generation = any(rng);
        if (f.memory != kNone)
            f.memory = renumbered.emplace(f.memory, 5000 + any(rng) * 16 + uint32_t(renumbered.size())).first->second;
    }
    return r;
}

void expectSame(const Built &a, const Built &b)
{
    ASSERT_EQ(a.graph.nodes.size(), b.graph.nodes.size());
    for (size_t i = 0; i < a.graph.nodes.size(); ++i) {
        const Node &x = a.graph.nodes[i], &y = b.graph.nodes[i];
        EXPECT_EQ(x.kind, y.kind) << i;
        EXPECT_EQ(x.engine, y.engine) << i;
        EXPECT_EQ(x.rule, y.rule) << i;
        EXPECT_EQ(x.before, y.before) << i;
        EXPECT_EQ(x.continues, y.continues) << i;
        EXPECT_EQ(x.runs.count, y.runs.count) << i;
    }
    ASSERT_EQ(a.graph.runs.size(), b.graph.runs.size());
    for (size_t i = 0; i < a.graph.runs.size(); ++i) {
        EXPECT_EQ(a.graph.runs[i].before, b.graph.runs[i].before) << i;
        EXPECT_EQ(a.graph.runs[i].variant, b.graph.runs[i].variant) << i;
        EXPECT_EQ(a.graph.runs[i].rung, b.graph.runs[i].rung) << i;
    }
    EXPECT_EQ(a.graph.entry, b.graph.entry);
    EXPECT_EQ(a.graph.exit, b.graph.exit);
    EXPECT_EQ(a.graph.edges, b.graph.edges);
}

TEST(Property, NoRuleReadsDrawData)
{
    std::mt19937 rng(20261111);
    for (const Recording &base : {glassFrame0(), glassFrame1(), scrollFrame(true), scrollFrame(false)}) {
        for (uint32_t probes : {0u, 1u << uint32_t(Probe::PassPixelBarrier)}) {
            const Caps caps = chipCaps(probes);
            const Built reference = plan(base, caps, JobContext{});
            const std::vector<EmitOp> ops = lower(reference.graph, base, JobContext{});
            for (int trial = 0; trial < 25; ++trial) {
                const Recording r = perturb(base, rng);
                EXPECT_EQ(structuralKey(r, caps), structuralKey(base, caps));
                const Built b = plan(r, caps, JobContext{});
                expectSame(reference, b);
                const std::vector<EmitOp> lowered = lower(b.graph, r, JobContext{});
                ASSERT_EQ(lowered.size(), ops.size());
                for (size_t i = 0; i < ops.size(); ++i) {
                    EXPECT_EQ(lowered[i].emitter, ops[i].emitter) << i;
                    EXPECT_EQ(lowered[i].node, ops[i].node) << i;
                }
            }
        }
    }
}

}  // namespace
}  // namespace raw::plan::test
