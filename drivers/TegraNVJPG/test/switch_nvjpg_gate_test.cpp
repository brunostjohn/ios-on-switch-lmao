// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVJPG.cpp's idle gate and stall cancellation against the engine model (switch_nvjpg_engine_model.h).
#include "switch_nvjpg_engine_model.h"

TEST_F(NVJPGGate, AJobAfterTheIdlePowerOffPowersThePartitionOnBootsTheFalconAndRuns)
{
    EXPECT_EQ(gEngine.syncpt, 369427731u);  // J2's IMMEDIATE and OP_DONE
    size_t from = gEngine.events.size();
    idle();
    EXPECT_FALSE(gEngine.partitionOn);
    EXPECT_TRUE(gEngine.flushed) << "L4T keeps the client in hot reset while the partition is off";
    using Steps = std::vector<std::string>;
    EXPECT_EQ(gEngine.steps(from, {"clock on", "flush", "reset", "clock off", "partition off"}),
              (Steps{"clock on", "flush", "reset", "clock off", "partition off"}));
    from = gEngine.events.size();
    const int powerOns = gEngine.partitionPowerOns, boots = gEngine.boots;
    const size_t streams = gEngine.streams.size();
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    // powergate-t21x.c's order, then nvhost's module reset; no MBIST pulse on T210B01.
    EXPECT_EQ(gEngine.steps(from, {"partition on", "clock on", "reset", "unclamp", "unreset", "release", "lvl2"}),
              (Steps{"partition on", "clock on", "reset", "unclamp", "unreset", "release", "reset", "unreset"}));
    EXPECT_FALSE(gEngine.flushed);
    EXPECT_EQ(switch_mmio_read32(gNvjpgRegisters.data(), 0x117c), 0x30008u);  // FALCON_CG2's prod value
    EXPECT_EQ(gEngine.partitionPowerOns, powerOns + 1);
    EXPECT_FALSE(gEngine.clamped);
    EXPECT_EQ(gEngine.boots, boots + 1);
    ASSERT_EQ(gEngine.streams.size(), streams + 2);  // the power-on's proof of life, then the job
    EXPECT_EQ(gEngine.streams[streams].size(), 3u);
    const std::vector<uint32_t> &decode = gEngine.streams.back();
    EXPECT_EQ(decode.front(), tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVJPG, 0, 0));
    EXPECT_EQ(decode.back(), tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, kSyncpt));
    EXPECT_EQ(gEngine.syncpt, 369427734u);  // the proof's two increments, then the job's fence
}

TEST_F(NVJPGGate, AJobPastItsBoundIsCancelledAndTheNextJobRuns)
{
    idle();
    gEngine.stallDecodes = true;
    const int boots = gEngine.boots, teardowns = gEngine.teardowns, restarts = gEngine.restarts;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(logged("not reached"));
    EXPECT_TRUE(logged("job selector 7, 64x64"));
    EXPECT_TRUE(logged("engine reset: running again"));
    EXPECT_EQ(gEngine.teardowns, teardowns + 1);
    EXPECT_EQ(gEngine.restarts, restarts + 1);
    EXPECT_EQ(gEngine.boots, boots + 2);  // the gate's power-on, then the cancellation's reboot
    EXPECT_TRUE(gEngine.partitionOn);
    EXPECT_FALSE(gEngine.flushed);
    EXPECT_TRUE(logged("memory client drained"));
    EXPECT_TRUE(tegra_nvjpg_ready());

    gEngine.stallDecodes = false;
    const uint32_t before = gEngine.syncpt;
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.syncpt, before + 1);
}

// Channel 3 is NVJPG's alone and one job runs at a time, so a job waits only on itself: 50 ms plus a microsecond per 16
// pixels of its MCU grid. SpringBoard's 205x83 file that hung the engine is cancelled in 51 ms (it took 2 s).
TEST_F(NVJPGGate, AJobIsBoundedByItsOwnPixelsAndASmallMargin)
{
    TegraNVJPGJob job = decodeJob();
    job.setup.frame_width = 205;
    job.setup.frame_height = 83;
    job.pixels = 208 * 88;
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 50000u + 208 * 88 / 16);
    job.pixels = 4032ull * 3024;
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 50000u + 4032 * 3024 / 16);
    job.pixels = 16384ull * 16384 * 2;
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(gEngine.lastBoundUs, 30000000u) << "at most 30 s";
}

// powergate-t21x.c powers NVJPG's partition before NVDEC's and keeps it on while NVDEC's is: an idle NVJPG is then
// only flushed, reset and unclocked.
TEST_F(NVJPGGate, AnIdlePowerOffKeepsThePartitionOnWhileNVDECsIsOn)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.nvdecPartitionOn = true;
    idle();
    EXPECT_TRUE(gEngine.partitionOn);
    EXPECT_TRUE(gEngine.inReset);
    EXPECT_FALSE(gEngine.clockOn);
    EXPECT_TRUE(gEngine.flushed);
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    gEngine.nvdecPartitionOn = false;
}

// The stream is the job's: the selector the request came through only names it in a stall line. Two decodes of one
// picture differ in their PICTURE_INDEX value alone.
TEST_F(NVJPGGate, TheStreamIsTheSameWhicheverSelectorTheJobCameThrough)
{
    TegraNVJPGJob first = decodeJob(), seventh = decodeJob();
    first.selector = 1;
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&first, &status, nullptr), 0);
    const std::vector<uint32_t> one = gEngine.streams.back();
    ASSERT_EQ(tegra_nvjpg_run(&seventh, &status, nullptr), 0);
    const std::vector<uint32_t> seven = gEngine.streams.back();
    ASSERT_EQ(one.size(), seven.size());
    ASSERT_EQ(one.size(), 29u);  // SETCLASS, 9 methods of 3 words for two planes, OP_DONE: T-v3a's 0x74 bytes
    for (size_t i = 0; i < one.size(); i++) {
        if (i > 0 && one[i - 1] == NVE7D0_SET_PICTURE_INDEX / 4) {
            EXPECT_EQ(seven[i], one[i] + 1);
            continue;
        }
        EXPECT_EQ(one[i], seven[i]) << "word " << i;
    }
}

TEST_F(NVJPGGate, AnEngineTheResetDoesNotBringBackRefusesTheNextJob)
{
    gEngine.stallDecodes = true;
    gEngine.deadAfterReset = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    EXPECT_TRUE(logged("it did not come back, NVJPG refuses work"));
    EXPECT_FALSE(tegra_nvjpg_ready());
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -19);
}
