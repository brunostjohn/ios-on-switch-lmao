// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "drivers/TegraGM20B/test/switch_gpu_event_fixture.h"
#include "drivers/TegraNVDEC/SwitchNVDECSemantics.h"
#include "drivers/TegraHost1x/SwitchHost1xResults.h"

namespace {
switch_gpu_backing_semantics adapter;
}

extern "C" int switch_gpu_backing_register_semantics(uint32_t engine, const switch_gpu_backing_semantics *operations)
{
    EXPECT_EQ(engine, SWITCH_GPU_ENGINE_NVDEC);
    adapter = *operations;
    return 0;
}

TEST_F(Events, DecodeFailuresRemainExactAcrossHardwareCompletedConsumers)
{
    using Results = SwitchHost1xResults<SWITCH_GPU_ENGINE_NVDEC>;
    ASSERT_TRUE(switch_nvdec_semantics_start(162));
    statusEngine = SWITCH_GPU_ENGINE_NVDEC;
    statusOverride = Results::status;
    physicalOverride = Results::physicalStatus;
    auto source = token();
    signal(19, source);
    auto *use = resolve(19);
    switch_gpu_backing_token first{}, independent{}, dependent{};
    auto *ra = Results::allocate(), *rb = Results::allocate(), *rc = Results::allocate();
    ASSERT_EQ(Results::prepare(ra, 1, 1, &first), 0);
    ASSERT_EQ(Results::prepare(rb, 2, 2, &independent), 0);
    ASSERT_EQ(Results::prepare(rc, 3, 3, &dependent), 0);
    tokens[key(first)] = {};
    tokens[key(independent)] = {};
    tokens[key(dependent)] = {};
    auto *a = batch(use, first), *b = batch(nullptr, independent);
    switch_gpu_event_use_release(&use);
    switch_gpu_event_waits *c = nullptr;
    ASSERT_EQ(switch_gpu_event_waits_prepare_tokens(&first, 1, &c), 0);
    ASSERT_EQ(switch_gpu_event_waits_bind(c, &dependent), 0);
    SwitchNVDECSemantic *node = nullptr;
    ASSERT_EQ(switch_nvdec_semantics_bind(a, &first, &node), 0);
    commit(a);
    ASSERT_EQ(switch_nvdec_semantics_bind(b, &independent, &node), 0);
    commit(b);
    ASSERT_EQ(switch_nvdec_semantics_bind(c, &dependent, &node), 0);
    commit(c);
    Results::physical(ra, 1);
    Results::physical(rb, 1);
    Results::physical(rc, 1);
    switch_nvdec_semantics_physical(3);
    pump();
    EXPECT_EQ(switch_gpu_event_waits_status(a), 0);
    EXPECT_EQ(switch_gpu_event_waits_status(b), 1);
    EXPECT_EQ(switch_gpu_event_waits_status(c), 0);
    EXPECT_EQ(Results::status(&independent), 1);
    finish(source, -5);
    EXPECT_EQ(switch_gpu_event_waits_status(a), -5);
    EXPECT_EQ(switch_gpu_event_waits_status(c), -5);
    EXPECT_EQ(Results::status(&first), -5);
    EXPECT_EQ(Results::status(&dependent), -5);
    EXPECT_EQ(Results::status(&independent), 1);
    EXPECT_EQ(adapter.physical_status(&first, 0), 1);
    switch_gpu_event_waits *captured = nullptr;
    EXPECT_EQ(adapter.snapshot(&first, &captured), -5);
    EXPECT_EQ(adapter.snapshot(&independent, &captured), 0);
    EXPECT_EQ(native->value, 0u);
    switch_nvdec_semantics_retire(a);
    switch_nvdec_semantics_retire(b);
    switch_nvdec_semantics_retire(c);
    Results::complete(&ra, -5);
    Results::complete(&rb, 0);
    Results::complete(&rc, -5);
    EXPECT_EQ(Results::waitLegacy(1), -5);
    EXPECT_EQ(Results::waitLegacy(2), 0);
    switch_gpu_event_waits_release(&a);
    switch_gpu_event_waits_release(&b);
    switch_gpu_event_waits_release(&c);
    pump();
}
