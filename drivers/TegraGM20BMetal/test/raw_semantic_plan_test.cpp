// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/RawMetalPlanApply.h"
#include <map>

namespace {

void *object(uintptr_t value)
{
    return reinterpret_cast<void *>(value);
}

Cmd binding(Op op, uint8_t stage, uint16_t slot, uintptr_t value, uint64_t offset = 0)
{
    Cmd c = cmd(op, stage, slot);
    c.obj = object(value);
    c.c = offset;
    return c;
}

void draw(RMCommandPlan &plan, uint32_t first = 0)
{
    Cmd c = cmd(OpDraw, 3);
    c.a = first;
    c.b = 6;
    c.c = uint64_t(1) << 32;
    plan.record(c, {});
}

struct Sink {
    uint32_t bufferCalls = 0, textureCalls = 0, samplerCalls = 0, rasterCalls = 0, kernelCalls = 0;
    std::map<std::pair<uint32_t, uint32_t>, raw::BufferBinding> bindings;
    uint32_t cull = 0;

    void buffer(uint32_t stage, uint32_t slot, const raw::BufferBinding &value)
    {
        ++bufferCalls;
        if (value.source == raw::BufferBinding::Source::Offset)
            bindings[{stage, slot}].offset = value.offset;
        else
            bindings[{stage, slot}] = value;
    }

    void texture(uint32_t, uint32_t, void *)
    {
        ++textureCalls;
    }

    void sampler(uint32_t, uint32_t, void *)
    {
        ++samplerCalls;
    }

    void raster(const raw::RasterState &next, const raw::RasterState *previous)
    {
        ++rasterCalls;
        if (raw::rasterChanged(next, previous, raw::RasterState::Cull, &raw::RasterState::cull))
            cull = next.cull;
    }

    void kernel(const raw::KernelState &, const raw::KernelState *)
    {
        ++kernelCalls;
    }
};

TEST(SemanticPlan, SettersCollapseBeforeTheFirstDraw)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (uint64_t i = 0; i < 1000; ++i)
        EXPECT_FALSE(plan.record(binding(OpBuffer, 1, 0, 42, i * 16), {}));
    EXPECT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.renderStates.empty());
    draw(plan);
    ASSERT_EQ(plan.size(), 2u);
    const auto &state = plan.renderStates[plan.draw(1).state];
    EXPECT_EQ(plan.buffers[0][state.buffers[0]][0].offset, 999u * 16);
    EXPECT_EQ(plan.buffers[0].size(), 1u);
}

TEST(SemanticPlan, TenThousandUnchangedDrawsShareOneSnapshot)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    for (uint32_t i = 0; i < 10000; ++i) {
        plan.record(binding(OpBuffer, 1, 2, 100, 256), {});
        plan.record(binding(OpTexture, 2, 0, 200), {});
        plan.record(binding(OpSampler, 2, 0, 300), {});
        draw(plan, i);
    }
    ASSERT_EQ(plan.size(), 10001u);
    ASSERT_EQ(plan.renderStates.size(), 1u);
    Sink sink;
    raw::StateCursor cursor;
    for (size_t at = 1; at < plan.size(); ++at) {
        EXPECT_EQ(plan.operation(at).op, OpDraw);
        raw::applyRenderState(plan, plan.draw(at).state, cursor, sink);
    }
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
    EXPECT_EQ(sink.samplerCalls, 1u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, OffsetChangePreservesTheEarlierImmutableBinding)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 4, 77, 64), {});
    draw(plan);
    plan.record(binding(OpBufferOffset, 1, 4, 0, 128), {});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &second = plan.renderStates[plan.draw(2).state];
    EXPECT_EQ(plan.buffers[0][first.buffers[0]][4].offset, 64u);
    EXPECT_EQ(plan.buffers[0][second.buffers[0]][4].offset, 128u);
    EXPECT_EQ(first.raster, second.raster);
    EXPECT_EQ(first.textures[1], second.textures[1]);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyRenderState(plan, plan.draw(1).state, cursor, sink);
    raw::applyRenderState(plan, plan.draw(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 2u);
    EXPECT_EQ(sink.rasterCalls, 1u);
}

TEST(SemanticPlan, ArgumentBytesKeepTheirChunkAndExtent)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd bytes = cmd(OpBytes, 2, 3);
    bytes.a = 256;
    bytes.b = 80;
    bytes.c = 2;
    plan.record(bytes, {});
    draw(plan);
    bytes.a = 512;
    bytes.b = 96;
    plan.record(bytes, {});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &binding = plan.buffers[1][first.buffers[1]][3];
    EXPECT_EQ(binding.source, raw::BufferBinding::Source::Bytes);
    EXPECT_EQ(binding.chunk, 2u);
    EXPECT_EQ(binding.offset, 256u);
    EXPECT_EQ(binding.length, 80u);
}

TEST(SemanticPlan, FloatBitPatternsRemainDistinct)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd blend = cmd(OpBlendColor);
    plan.record(blend, {0, 0, 0, 0});
    draw(plan);
    plan.record(blend, {0x80000000, 0, 0, 0});
    draw(plan);
    const auto &first = plan.renderStates[plan.draw(1).state];
    const auto &second = plan.renderStates[plan.draw(2).state];
    EXPECT_NE(first.raster, second.raster);
    EXPECT_EQ(plan.raster[first.raster].blend[0], 0u);
    EXPECT_EQ(plan.raster[second.raster].blend[0], 0x80000000u);
}

TEST(SemanticPlan, EventsKeepTheirExactSemanticPositions)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 1), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    Cmd signal = binding(OpSignalEvent, 0, 0, 900, 12);
    plan.record(signal, {});
    Cmd wait = binding(OpWaitEvent, 0, 0, 901, 13);
    wait.a = 17;
    plan.record(wait, {});
    ASSERT_EQ(plan.events().size(), 2u);
    EXPECT_EQ(plan.events()[0].position, 3u);
    EXPECT_TRUE(plan.events()[0].signal);
    EXPECT_EQ(plan.events()[1].position, 4u);
    EXPECT_EQ(plan.events()[1].timeout, 17u);
    EXPECT_EQ(plan.legacy(3).c, 12u);
    EXPECT_EQ(plan.encoderEnd(0), 3u);
}

TEST(SemanticPlan, OpaqueCallsPreservePositionAndUnknownState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd cull = cmd(OpCull);
    cull.a = 1;
    plan.record(cull, {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    plan.record(binding(OpInvocation, 0, 0, 500), {});
    plan.record(binding(OpBufferOffset, 1, 0, 0, 128), {});
    draw(plan);
    ASSERT_EQ(plan.size(), 4u);
    const auto &call = plan.control(2);
    EXPECT_EQ(call.render, plan.draw(1).state);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyRenderState(plan, call.render, cursor, sink);
    sink.cull = 2;
    sink.bindings[{1, 0}].object = object(11);
    raw::applyRenderState(plan, plan.draw(3).state, cursor, sink);
    EXPECT_EQ(sink.cull, 2u);
    EXPECT_EQ(sink.bindings[std::make_pair(1u, 0u)].object, object(11));
    EXPECT_EQ(sink.bindings[std::make_pair(1u, 0u)].offset, 128u);
}

TEST(SemanticPlan, ExplicitStateAfterOpaqueCallOverridesUnknownState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd cull = cmd(OpCull);
    cull.a = 1;
    plan.record(cull, {});
    plan.record(binding(OpInvocation, 0, 0, 500), {});
    plan.record(cull, {});
    draw(plan);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyRenderState(plan, plan.control(1).render, cursor, sink);
    sink.cull = 2;
    raw::applyRenderState(plan, plan.draw(2).state, cursor, sink);
    EXPECT_EQ(sink.cull, 1u);
}

TEST(SemanticPlan, NewEncodersStartWithIndependentState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 20), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpRender), {});
    draw(plan);
    const auto &state = plan.renderStates[plan.draw(4).state];
    EXPECT_EQ(plan.buffers[0][state.buffers[0]][0].source, raw::BufferBinding::Source::Unset);
    EXPECT_EQ(plan.encoderEnd(0), 3u);
}

TEST(SemanticPlan, KernelDispatchHasTypedGridAndSharedState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 99, 16), {});
    plan.record(binding(OpKernelTexture, 3, 2, 100), {});
    Cmd dispatch = cmd(OpDispatch, 3, 1);
    dispatch.a = 1u << 2;
    dispatch.c = 8 | uint64_t(4) << 16 | uint64_t(2) << 32;
    dispatch.obj2 = object(999);
    plan.record(dispatch, {16, 12, 8, 2, 3, 4});
    plan.record(dispatch, {24, 12, 8, 3, 3, 4});
    ASSERT_EQ(plan.kernelStates.size(), 1u);
    EXPECT_EQ(plan.dispatch(1).groups, (std::array<uint64_t, 3>{2, 3, 4}));
    EXPECT_EQ(plan.dispatch(2).grid, (std::array<uint64_t, 3>{24, 12, 8}));
    EXPECT_EQ(plan.dispatch(1).threads, (std::array<uint32_t, 3>{8, 4, 2}));
    EXPECT_TRUE(plan.dispatch(1).perThread);
    EXPECT_EQ(plan.dispatch(1).surfaceSlots, 1u << 2);
    EXPECT_EQ(plan.legacy(1).a, 1u << 2);
    Sink sink;
    raw::StateCursor cursor;
    raw::applyKernelState(plan, plan.dispatch(1).state, cursor, sink);
    raw::applyKernelState(plan, plan.dispatch(2).state, cursor, sink);
    EXPECT_EQ(sink.bufferCalls, 1u);
    EXPECT_EQ(sink.textureCalls, 1u);
}

TEST(SemanticPlan, CachedRangeSummariesUnionAccessAndExcludeOtherRanges)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), raw::ResourceUse::Write}});
    plan.record(binding(OpBuffer, 1, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    plan.record(binding(OpKernelBuffer, 3, 0, 2), {});
    Cmd dispatch = cmd(OpDispatch);
    dispatch.c = 1 | uint64_t(1) << 16 | uint64_t(1) << 32;
    plan.record(dispatch, {1, 1, 1, 1, 1, 1});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, 3);
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, 3, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses.size(), 2u);
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read);
    uses.clear();
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read | raw::ResourceUse::Write);
    std::vector<void *> writes;
    plan.forEachWritten(0, 3, [&](void *object) {
        writes.push_back(object);
    });
    ASSERT_EQ(writes.size(), 1u);
    EXPECT_EQ(writes[0], object(1));
}

TEST(SemanticPlan, ReflectedUsesReplaceUnusedBindingResources)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpTexture, 2, 0, 1), {});
    plan.record(binding(OpTexture, 2, 1, 2), {});
    draw(plan);
    plan.setRenderUses(plan.draw(1).state, {{object(1), raw::ResourceUse::Read}});
    draw(plan);
    plan.prepareRange(0, plan.size());
    std::vector<void *> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t) {
        uses.push_back(object);
    });
    ASSERT_EQ(uses.size(), 1u);
    EXPECT_EQ(uses[0], object(1));
}

TEST(SemanticPlan, IndexedArgumentsPreserveSignedBaseVertex)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd indexed = binding(OpDrawIndexed, 3, 0, 21, 256);
    indexed.a = 6;
    indexed.b = 1;
    plan.record(indexed, {4, uint64_t(int64_t(-7)), 9});
    const auto &draw = plan.draw(1);
    EXPECT_EQ(draw.baseVertex, -7);
    EXPECT_EQ(draw.baseInstance, 9u);
    EXPECT_EQ(draw.instances, 4u);
    EXPECT_EQ(draw.indexOffset, 256u);
}

TEST(SemanticPlan, IndirectArgumentsAreReferencesNeverCpuReads)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    Cmd indirect = cmd(OpDrawIndexedIndirect, 3);
    indirect.obj = object(1);
    indirect.obj2 = object(3);
    indirect.a = 1;
    indirect.c = 64;
    indirect.x = 128;
    plan.record(indirect, {});
    const auto &draw = plan.draw(1);
    EXPECT_EQ(draw.arguments, object(3));
    EXPECT_EQ(draw.indices, object(1));
    EXPECT_EQ(draw.argumentOffset, 128u);
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(1)], raw::ResourceUse::Read);
    EXPECT_EQ(uses[object(3)], raw::ResourceUse::Read);
}

TEST(SemanticPlan, RecycledStorageDropsAllSemanticState)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    plan.prepareRange(0, plan.size());
    plan.clear();
    EXPECT_TRUE(plan.empty());
    EXPECT_TRUE(plan.events().empty());
    plan.record(cmd(OpRender), {});
    draw(plan);
    plan.prepareRange(0, plan.size());
    size_t uses = 0;
    plan.forEachUse(0, plan.size(), [&](void *, uint8_t) {
        ++uses;
    });
    EXPECT_EQ(uses, 0u);
    EXPECT_EQ(plan.renderStates.size(), 1u);
}

TEST(SemanticPlan, EligibilityIgnoresRasterChangesButTracksBindingsAndOpaqueCalls)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    const auto initial = plan.eligibilityGeneration();
    Cmd scissor = cmd(OpScissor);
    scissor.a = 4;
    scissor.b = 8;
    plan.record(scissor, {});
    plan.record(cmd(OpBlendColor), {0, 0, 0, 0});
    EXPECT_EQ(plan.eligibilityGeneration(), initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    const auto bound = plan.eligibilityGeneration();
    EXPECT_GT(bound, initial);
    plan.record(binding(OpTexture, 2, 0, 7), {});
    EXPECT_EQ(plan.eligibilityGeneration(), bound);
    plan.record(cmd(OpInvocation), {});
    EXPECT_GT(plan.eligibilityGeneration(), bound);
}

TEST(SemanticPlan, PreparedRangeCachesDistinctPipelinesAcrossEncoderSummaries)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 10), {});
    draw(plan);
    draw(plan);
    plan.record(binding(OpPipeline, 0, 0, 20), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    const size_t middle = plan.size();
    plan.prepareRange(0, middle);
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpPipeline, 0, 0, 10), {});
    draw(plan);
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(middle, plan.size());
    plan.prepareRange(0, plan.size());
    const auto &pipelines = plan.renderPipelines(0, plan.size());
    ASSERT_EQ(pipelines.size(), 2u);
    EXPECT_EQ(pipelines[0], object(10));
    EXPECT_EQ(pipelines[1], object(20));
    const auto *cached = pipelines.data();
    for (unsigned wake = 0; wake < 100; ++wake) {
        plan.prepareRange(0, plan.size());
        EXPECT_EQ(plan.renderPipelines(0, plan.size()).data(), cached);
    }
}

TEST(SemanticPlan, TextureSamplingAndAttachmentLoadsRemainDistinct)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.addUses(0, {{object(1), raw::ResourceUse::Read | raw::ResourceUse::Write}});
    plan.record(binding(OpTexture, 2, 0, 2), {});
    draw(plan);
    plan.record(cmd(OpInvocation), {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(1)], raw::ResourceUse::Read | raw::ResourceUse::Write);
    EXPECT_EQ(uses[object(2)], raw::ResourceUse::Read | raw::ResourceUse::Write | raw::ResourceUse::Sample);
}

TEST(SemanticPlan, RepeatedIndirectDrawsReuseResourceSummaryAndChangesKeepEarlierOnes)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    Cmd indirect = cmd(OpDrawIndexedIndirect, 3);
    indirect.obj = object(20);
    indirect.obj2 = object(30);
    plan.record(indirect, {});
    const auto first = plan.operation(1).uses;
    for (unsigned draw = 0; draw < 10000; ++draw) {
        plan.record(indirect, {});
        EXPECT_EQ(plan.operation(plan.size() - 1).uses, first);
    }
    indirect.obj2 = object(40);
    plan.record(indirect, {});
    EXPECT_NE(plan.operation(plan.size() - 1).uses, first);
    std::map<void *, uint8_t> uses;
    plan.forEachUse(1, 2, [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses.size(), 3u);
    EXPECT_EQ(uses.count(object(30)), 1u);
    EXPECT_EQ(uses.count(object(40)), 0u);
}

TEST(SemanticPlan, ExplicitResidencySurvivesRangeUnionWithBoundResources)
{
    RMCommandPlan plan;
    plan.record(cmd(OpRender), {});
    plan.record(binding(OpBuffer, 1, 0, 10), {});
    draw(plan);
    Cmd declared = binding(OpUse, 0, 0, 10);
    declared.a = 1;
    plan.record(declared, {});
    plan.record(cmd(OpEnd), {});
    plan.record(cmd(OpCompute), {});
    declared = binding(OpKernelUse, 3, 0, 20);
    declared.c = 2;
    plan.record(declared, {});
    plan.record(cmd(OpEnd), {});
    plan.prepareRange(0, plan.size());
    std::map<void *, uint8_t> uses;
    plan.forEachUse(0, plan.size(), [&](void *object, uint8_t access) {
        uses[object] = access;
    });
    EXPECT_EQ(uses[object(10)], raw::ResourceUse::Read | raw::ResourceUse::Explicit);
    EXPECT_EQ(uses[object(20)], raw::ResourceUse::Write | raw::ResourceUse::Explicit);
}

}  // namespace
