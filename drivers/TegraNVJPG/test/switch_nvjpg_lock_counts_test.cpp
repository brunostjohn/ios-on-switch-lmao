// SPDX-License-Identifier: BSD-2-Clause-Patent
// A debug kernel's lock counts on NVJPG's periodic decode line (tegra_nvjpg_lock_counts): each job whose engine wait
// ran with gStateLock free, and the longest a cancellation or an idle gate call waited to take gStateLock. A wait is the
// model's clock moved on while the lock is being taken.
#include "switch_nvjpg_engine_model.h"
#include "../TegraNVJPGStats.h"

namespace {
uint64_t gStateHeldUs;

void stateLockHeldElsewhere(IOLock *lock)
{
    if (lock == gStateLock)
        switch_test_now() += gStateHeldUs * NSEC_PER_USEC;
}

struct LockCounts {
    uint64_t waitsUnlocked = 0, stateWaitMaxUs = 0;
};

LockCounts taken()
{
    LockCounts counts;
    tegra_nvjpg_lock_counts(&counts.waitsUnlocked, &counts.stateWaitMaxUs);
    return counts;
}

class NVJPGLockCounts : public NVJPGGate {
protected:
    void SetUp() override
    {
        NVJPGGate::SetUp();
        TegraNVJPGJob job = decodeJob();
        nvjpg_dec_status status = {};
        ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);  // the gate powered on, before any count is read
        taken();
    }

    void TearDown() override
    {
        switch_test_lock_taken = nullptr;
    }
};
}  // namespace

TEST_F(NVJPGLockCounts, EachJobsEngineWaitIsCountedAndTakingTheCountsZeroesThem)
{
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    EXPECT_EQ(taken().waitsUnlocked, 2u);
    EXPECT_EQ(taken().waitsUnlocked, 0u);
}

TEST_F(NVJPGLockCounts, AnIdleGateCallsWaitForTheStateLockIsKeptAtItsLongest)
{
    gStateHeldUs = 700;
    switch_test_lock_taken = stateLockHeldElsewhere;
    idle();
    gStateHeldUs = 200;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);  // powered on again: gateOn waited 200 us
    switch_test_lock_taken = nullptr;
    const LockCounts counts = taken();
    EXPECT_EQ(counts.stateWaitMaxUs, 700u);
    EXPECT_EQ(counts.waitsUnlocked, 1u);
    EXPECT_EQ(taken().stateWaitMaxUs, 0u);
}

TEST_F(NVJPGLockCounts, ACancellationsWaitForTheStateLockIsCounted)
{
    gStateHeldUs = 300;
    switch_test_lock_taken = stateLockHeldElsewhere;
    gEngine.stallDecodes = true;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    EXPECT_EQ(tegra_nvjpg_run(&job, &status, nullptr), -5);
    switch_test_lock_taken = nullptr;
    ASSERT_TRUE(logged("engine reset: running again"));
    // the job's own check of the engine's state moved the clock too, and is not a gate call's wait
    EXPECT_EQ(taken().stateWaitMaxUs, 300u);
}

TEST_F(NVJPGLockCounts, AJobsOwnStateCheckIsNotCountedAsAWait)
{
    gStateHeldUs = 900;
    switch_test_lock_taken = stateLockHeldElsewhere;
    TegraNVJPGJob job = decodeJob();
    nvjpg_dec_status status = {};
    ASSERT_EQ(tegra_nvjpg_run(&job, &status, nullptr), 0);
    switch_test_lock_taken = nullptr;
    EXPECT_EQ(taken().stateWaitMaxUs, 0u);
}
