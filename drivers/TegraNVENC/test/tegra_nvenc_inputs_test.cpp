// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "tegra_nvenc_inputs_stubs.h"
#include <gtest/gtest.h>
#include <map>
#include <tuple>
#include "drivers/TegraNVENC/TegraNVENCInputs.h"

namespace {
using Key = std::tuple<uint32_t, uint64_t>;
std::map<Key, int> gPhysical, gStatus;  // absent: ran, done

Key key(const switch_gpu_backing_token &token)
{
    return {token.engine, token.value};
}

int lookup(const std::map<Key, int> &map, const switch_gpu_backing_token &token)
{
    const auto at = map.find(key(token));
    return at == map.end() ? 1 : at->second;
}
}  // namespace

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int external)
{
    EXPECT_EQ(external, 0);
    return lookup(gPhysical, *token);
}

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
    return lookup(gStatus, *token);
}

namespace {
const switch_gpu_backing_token kVicInput = {0x700000101ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};
const switch_gpu_backing_token kDecode = {0x400000033ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162};
const switch_gpu_backing_token kGpu = {9, 1, SWITCH_GPU_ENGINE_GM20B, 3};

struct NVENCInputs : testing::Test {
    TegraNVENCInputs inputs;

    void SetUp() override
    {
        gPhysical.clear();
        gStatus.clear();
        ASSERT_TRUE(inputs.start());
    }
};

// VIC's job that wrote the encoder's input was cancelled: recorded failed before the CPU released VIC's syncpoint (so
// NVENC's stream went on), its result not yet filed. The encode's Wait answers with that failure.
TEST_F(NVENCInputs, AnEncodeOfACancelledVicInputFailsWithItsFailure)
{
    inputs.record(500, &kVicInput, 1);
    EXPECT_EQ(inputs.failure(500), 0);
    gPhysical[key(kVicInput)] = -5;
    EXPECT_EQ(inputs.failure(500), -5);
}

// A native picture read in place: a producer whose decode failed in its semantic outcome (its hardware ran) fails the
// encode once that outcome is filed.
TEST_F(NVENCInputs, ANativePicturesFailedProducerFailsTheEncode)
{
    const switch_gpu_backing_token producers[] = {kGpu, kDecode, kVicInput};
    inputs.record(501, producers, 3);
    gStatus[key(kDecode)] = -125;
    EXPECT_EQ(inputs.failure(501), -125);
}

TEST_F(NVENCInputs, OnlyTheEnginesThatWroteTheInputAreAsked)
{
    const switch_gpu_backing_token nvenc = {499, 1, SWITCH_GPU_ENGINE_NVENC, 170};
    const switch_gpu_backing_token producers[] = {kGpu, nvenc, {}};
    gPhysical[key(kGpu)] = gPhysical[key(nvenc)] = -5;
    inputs.record(502, producers, 3);
    EXPECT_EQ(inputs.failure(502), 0) << "the GPU's go through VIC, NVENC's own last job is no producer";
}

TEST_F(NVENCInputs, AProducerWhoseRecordIsGoneIsNoFailure)
{
    inputs.record(503, &kDecode, 1);
    gPhysical[key(kDecode)] = gStatus[key(kDecode)] = SWITCH_GPU_BACKING_STALE;
    EXPECT_EQ(inputs.failure(503), 0);
}

TEST_F(NVENCInputs, EachFenceHasItsOwnProducersUntilItsSlotIsReused)
{
    inputs.record(504, &kVicInput, 1);
    inputs.record(505, &kDecode, 1);
    gPhysical[key(kVicInput)] = -5;
    EXPECT_EQ(inputs.failure(505), 0);
    EXPECT_EQ(inputs.failure(504), -5);
    inputs.record(504 + TegraNVENCInputs::kJobs, &kDecode, 1);
    EXPECT_EQ(inputs.failure(504), 0) << "no longer known";
    EXPECT_EQ(inputs.failure(504 + TegraNVENCInputs::kJobs), 0);
}
}  // namespace
