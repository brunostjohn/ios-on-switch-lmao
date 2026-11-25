// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/source_text.h"

namespace {
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// A GPU sampler whose first ask of a surface nothing has written comes while NVJPG decodes into it would tile in place
// the rows NVJPG is writing (TegraGM20BTiled.cpp's seed-0 rule): the decode moves the seed before it reads the layout,
// so such an ask finds a written surface and keeps it pitch, and a surface decided block linear before is held, its
// texels unmoved, until the decode and any copy-engine re-pack have ended. Every route but VIC's asks.
TEST(NVJPGLayoutWiring, TheSeedMovesBeforeTheLayoutIsReadAndTheHoldOutlastsTheDecodeAndItsCopy)
{
    const std::string source = SourceText("../TegraNVJPGUserClient.cpp");
    const std::string run = Body(source, "IOReturn runJob(Job *job)\n{");
    const auto vic = run.find("if (job->plan.repack == TEGRA_NVJPG_REPACK_VIC)\n        return runRepack(job);");
    const auto seed = run.find("tegra_vic_surface_written(&job->dst);");
    const auto layout = run.find("routeJob(job, target, &hold, &route)");
    const auto decode = run.find("ret = job->plan.repack ? runRepack(job) : runDirect(job, target, extent);");
    const auto unhold = run.find("tegra_gm20b_surface_unhold(hold);");
    ASSERT_NE(vic, std::string::npos);
    ASSERT_NE(seed, std::string::npos);
    ASSERT_NE(layout, std::string::npos);
    ASSERT_NE(decode, std::string::npos);
    ASSERT_NE(unhold, std::string::npos);
    EXPECT_LT(vic, seed);
    EXPECT_LT(seed, layout);
    EXPECT_LT(layout, decode);
    EXPECT_LT(decode, unhold);
    EXPECT_GT(run.find("runRepack(job)", seed), layout) << "no copy-engine re-pack before the ask";
    const std::string route = Body(
        source, "const char *routeJob(Job *job, const SwitchNVJPGBacking &target, void **hold, uint32_t *route)\n{");
    EXPECT_LT(route.find("tegra_gm20b_surface_hold(target.pages[0], &tiled)"),
              route.find("tegra_gm20b_surface_layout("));
    EXPECT_THAT(route, HasSubstr("tegra_nvjpg_route("));
    const std::string direct =
        Body(source, "IOReturn runDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)\n{");
    EXPECT_THAT(direct, Not(HasSubstr("unhold")));
}

// The destination is mapped for the plan the route leaves, and only the run itself holds the job lock.
TEST(NVJPGLayoutWiring, TheDestinationIsMappedForThePlanTheRouteLeavesAndOnlyTheRunHoldsTheJobLock)
{
    const std::string direct =
        Body(SourceText("../TegraNVJPGUserClient.cpp"),
             "IOReturn runDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)\n{");
    const auto map = direct.find("switch_nvjpg_mapping(target, extent, p.writeEnd, &why, job->pid)");
    const auto lock = direct.find("tegra_nvjpg_lock();");
    const auto unlock = direct.find("tegra_nvjpg_unlock();");
    ASSERT_NE(map, std::string::npos);
    ASSERT_NE(lock, std::string::npos);
    ASSERT_NE(unlock, std::string::npos);
    EXPECT_LT(map, lock);
    EXPECT_EQ(direct.substr(lock, unlock - lock),
              "tegra_nvjpg_lock();\n    const int error = tegra_nvjpg_run(&job->engine, &status, &busy);\n    ");
}

// A '422f' or '444f' decode: VIC's re-pack is queued while NVJPG's job is in flight, to wait for its fence on the
// hardware (no CPU between the engines), and the decode ends when VIC's completion arrives.
TEST(NVJPGLayoutWiring, TheRepackWaitsForTheDecodeOnTheHardwareAndTheDecodeEndsOnVIC)
{
    const std::string run =
        Body(SourceText("../TegraNVJPG.cpp"),
             "extern \"C\" int tegra_nvjpg_run(const TegraNVJPGJob *job, nvjpg_dec_status *status, "
             "uint64_t *busyUs,\n                               const TegraNVJPGFollow *follow)\n{");
    const auto submit = run.find("switch_host1x_job_submit(");
    const auto follow = run.find("follow->queue(follow->context, gSyncpt, fence);");
    const auto wait = run.find("tegra_host1x_syncpt_wait(gSyncpt, fence");
    ASSERT_NE(follow, std::string::npos);
    EXPECT_LT(submit, follow);
    EXPECT_LT(follow, wait);
    const std::string repack = SourceText("../TegraNVJPGRepack.cpp");
    EXPECT_THAT(repack, HasSubstr("const TegraVICAcquire after = {syncpt, fence};"));
    EXPECT_THAT(repack, HasSubstr("ret = tegra_nvjpg_run(engine, status, busyUs, &follow);"));
    EXPECT_THAT(repack, HasSubstr("while (!wait.done)\n            IOLockSleep(gWaitLock, &wait, THREAD_UNINT);"));
}
}  // namespace
