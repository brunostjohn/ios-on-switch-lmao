// SPDX-License-Identifier: MIT
// raw/runtime/raw_device_answers.h, every kind: a feature the device has no form of is answered NO wherever Metal lets a
// caller ask for it, and its answers agree with each other (RawMetalUnsupported.mm refuses the creations past them);
// counter sampling is answered as the identity's driver answers it.
#include "../raw/runtime/raw_device_answers.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>

namespace {

std::optional<uint64_t> count(const raw::answers::Kind &k, const char *selector)
{
    for (size_t i = 0; i < k.countCount; i++)
        if (!strcmp(k.counts[i].selector, selector))
            return k.counts[i].value;
    return std::nullopt;
}

uint64_t argument(const raw::answers::Kind &k, const char *selector, uint64_t a)
{
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, selector) && a >= k.args[i].first && a <= k.args[i].last)
            return k.args[i].value;
    return 0;
}

TEST(RawDeviceAnswers, AmplificationIsOneViewAndEveryQuerySaysSo)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(count(k, "maxVertexAmplificationCount"), 1u) << k.kind;
        EXPECT_EQ(count(k, "maxVertexAmplificationFactor"), 1u) << k.kind;
        EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", 1), 1u) << k.kind;
        for (uint64_t n = 2; n <= 8; n++)
            EXPECT_EQ(argument(k, "supportsVertexAmplificationCount:", n), 0u) << k.kind << " count " << n;
    }
}

// The identity's AGX driver samples counters at stage boundaries (MTLCounterSamplingPointAtStageBoundary, 0) alone
TEST(RawDeviceAnswers, CountersAreSampledAtStageBoundariesAlone)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds) {
        EXPECT_EQ(argument(k, "supportsCounterSampling:", 0), 1u) << k.kind;
        for (uint64_t point = 1; point <= 4; point++)
            EXPECT_EQ(argument(k, "supportsCounterSampling:", point), 0u) << k.kind << " point " << point;
    }
}

TEST(RawDeviceAnswers, FeaturesWithNoFormHereAreNo)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        for (const char *selector : {"maxPredicatedNestingDepth", "supportsDynamicAttributeStride",
                                     "supportsSharedTextureHandles", "supportsTessellation",
                                     "supportsIndirectTessellation", "maxTessellationFactor"})
            EXPECT_EQ(count(k, selector), 0u) << k.kind << " " << selector;
}

}  // namespace
