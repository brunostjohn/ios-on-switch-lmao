// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICOddProbe.h"
#include "vic_model.h"

namespace {
using namespace switch_vic_odd_probe;

constexpr uint64_t kSrcIova = 0x10000000, kDstIova = 0x18000000, kConfig = 0x80001000, kFilter = 0x80002000;

std::vector<uint8_t> onTheModel(const Case &c, const TegraVICJob &job)
{
    std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
    fillSource(source.data(), c);
    VicModel model = {};
    model.region[0] = {kSrcIova, source.data(), source.size()};
    model.region[1] = {kDstIova, destination.data(), destination.size()};
    model.regions = 2;
    model.flips = VIC_MODEL_FLIP_SURFACE;
    vic_model_run(&model, &job);
    EXPECT_EQ(model.faults + model.clipped, 0u) << c.width << "x" << c.height;
    return destination;
}

SwitchVICOddProbeReply run()
{
    SwitchVICOddProbeReply reply = {};
    for (uint32_t i = 0; i < SWITCH_VIC_ODD_PROBE_CASES; i++, reply.count++) {
        const Case &c = kCases[i];
        SwitchVICOddResult &r = reply.results[i];
        TegraVICJob job;
        if (build(c, kSrcIova, kDstIova, kConfig, kFilter, &job)) {
            r.status = -95;
            continue;
        }
        const std::vector<uint8_t> bytes = onTheModel(c, job);
        compare(bytes.data(), c, &r);
    }
    summarise(&reply);
    return reply;
}

TEST(VicOddProbe, EveryCaseBuildsAndIsExactOnTheModel)
{
    const SwitchVICOddProbeReply reply = run();
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, (1u << SWITCH_VIC_ODD_PROBE_CASES) - 1);
    for (uint32_t i = 0; i < SWITCH_VIC_ODD_PROBE_CASES; i++)
        EXPECT_EQ(reply.results[i].status, 0) << i;
}

TEST(VicOddProbe, ALostLastPairOrRowIsCountedAsTheLast)
{
    const Case &c = kCases[4];  // '420v' 33x9
    TegraVICJob job;
    ASSERT_EQ(build(c, kSrcIova, kDstIova, kConfig, kFilter, &job), nullptr);
    std::vector<uint8_t> bytes = onTheModel(c, job);
    bytes[kChromaOffset + 4 * kDestinationPitch + 32] = kPoison;
    bytes[8 * kDestinationPitch + 3] ^= 1;
    bytes[2 * kDestinationPitch + 40] = 0;
    SwitchVICOddResult r = {};
    compare(bytes.data(), c, &r);
    EXPECT_EQ(r.wrong, 2u);
    EXPECT_EQ(r.lastWrong, 2u);
    EXPECT_EQ(r.outsideWritten, 1u);
    EXPECT_EQ(r.firstPlane, 0u);
    EXPECT_EQ(r.firstY, 8u);
    EXPECT_FALSE(clean(r));
}

TEST(VicOddProbe, OddSizesAreDescribedInWholeChromaPairs)
{
    TegraVICJob job;
    ASSERT_EQ(build(kCases[4], kSrcIova, kDstIova, kConfig, kFilter, &job), nullptr);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH), 33u);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT), 9u);
    EXPECT_EQ(TEGRA_VIC_GET(job.config, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT), 32u);
    EXPECT_EQ(rowBytes(kCases[4], 1), 34u);
    EXPECT_EQ(planeRows(kCases[4], 1), 5u);
}

TEST(VicOddProbe, ReplyLayout)
{
    EXPECT_EQ(sizeof(SwitchVICOddResult), 44u);
    EXPECT_EQ(sizeof(SwitchVICOddProbeReply), 16u + 44u * SWITCH_VIC_ODD_PROBE_CASES);
}
}  // namespace
