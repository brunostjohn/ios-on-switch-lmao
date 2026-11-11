// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_vic_jobs_stubs.h"
#include <algorithm>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchVICJobs.cpp"

struct switch_gpu_bridge {
    std::vector<switch_gpu_backing_token> tokens;
};

namespace {
std::vector<switch_gpu_backing_token> sourceTokens;
std::vector<uint32_t> publishedWords;
uint32_t begins, aborts, commits, retained, bridges, released, roomWaits, submits, powerUses, powerDones;
int bridgeError, submitError, retainError, statusError, powerError;
int bridgeResult = 1;
uint64_t nextTestSequence = 0;
bool transactionLocked, changeProducer, busyOnce;
volatile uint64_t *changeGeneration;
}

extern "C" {
int switch_gpu_backing_register_engine(uint32_t, const switch_gpu_backing_engine *)
{
    return 0;
}

void switch_gpu_backing_completed()
{}

int switch_gpu_backing_token_status(const switch_gpu_backing_token *)
{
    return 1;
}

int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *)
{
    return statusError;
}

int switch_gpu_backing_token_retain(const switch_gpu_backing_token *)
{
    EXPECT_TRUE(transactionLocked);
    if (retainError && retained == 1)
        return retainError;
    ++retained;
    return 0;
}

void switch_gpu_backing_token_release(const switch_gpu_backing_token *)
{
    EXPECT_GT(retained, 0u);
    --retained;
}

int switch_gpu_backing_begin_checked(switch_gpu_backing_use *uses, uint32_t count,
                                     const switch_gpu_backing_guard *guards, uint32_t guardCount,
                                     const switch_gpu_backing_token *completion,
                                     switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(switch_test_queue_locked);
    EXPECT_FALSE(transactionLocked);
    ++begins;
    for (uint32_t i = 0; i < guardCount; ++i)
        if (*guards[i].generation != guards[i].expected)
            return SWITCH_GPU_BACKING_STALE;
    *transaction = {};
    transaction->uses = uses;
    transaction->count = count;
    transaction->locked = 1;
    transaction->completion = *completion;
    transactionLocked = true;
    return 0;
}

int switch_gpu_backing_begin_conversion_checked(switch_gpu_backing_use *uses, uint32_t count,
                                                switch_gpu_cpu_lease *const *, uint32_t,
                                                const switch_gpu_backing_guard *guards, uint32_t guardCount,
                                                const switch_gpu_backing_token *completion,
                                                switch_gpu_backing_transaction *transaction)
{
    return switch_gpu_backing_begin_checked(uses, count, guards, guardCount, completion, transaction);
}

int switch_gpu_backing_dependencies(const switch_gpu_backing_transaction *, switch_gpu_backing_token *out,
                                    uint32_t capacity, uint32_t *needed)
{
    EXPECT_TRUE(transactionLocked);
    *needed = static_cast<uint32_t>(sourceTokens.size());
    if (capacity < *needed)
        return -28;
    std::copy(sourceTokens.begin(), sourceTokens.end(), out);
    return 0;
}

void switch_gpu_backing_abort(switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(transactionLocked);
    transactionLocked = false;
    transaction->locked = 0;
    ++aborts;
}

void switch_gpu_backing_commit(switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(transactionLocked);
    transactionLocked = false;
    transaction->locked = 0;
    ++commits;
}

int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count, switch_gpu_bridge **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    EXPECT_EQ(retained, count);
    if (busyOnce) {
        busyOnce = false;
        return -16;
    }
    if (bridgeError)
        return bridgeError;
    *out = new switch_gpu_bridge{{tokens, tokens + count}};
    ++bridges;
    if (changeGeneration) {
        __atomic_add_fetch(changeGeneration, 1, __ATOMIC_RELEASE);
        changeGeneration = nullptr;
    }
    if (changeProducer) {
        ++sourceTokens[0].value;
        changeProducer = false;
    }
    return 0;
}

void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *, uint32_t *id, uint32_t *threshold)
{
    *id = 170;
    *threshold = 0x1000001;
}

int switch_gpu_bridge_covers(const switch_gpu_bridge *bridge, const switch_gpu_backing_token *token)
{
    for (const auto &held : bridge->tokens)
        if (held.queue == token->queue && held.reset_generation == token->reset_generation &&
            held.value >= token->value)
            return 1;
    return 0;
}

int switch_gpu_bridge_status(const switch_gpu_bridge *)
{
    return 1;
}

int switch_gpu_bridge_wait(const switch_gpu_bridge *)
{
    return bridgeResult;
}

void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
    if (*bridge) {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        delete *bridge;
        *bridge = nullptr;
        ++released;
    }
}

int switch_gpu_bridge_wait_room()
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    ++roomWaits;
    return 0;
}

int tegra_host1x_submit_on(uint32_t, const uint32_t *words, uint32_t count)
{
    ++submits;
    publishedWords.assign(words, words + count);
    return submitError;
}

int tegra_host1x_syncpt_wait(uint32_t, uint32_t, uint32_t)
{
    throw std::runtime_error("unexpected producer wait");
}

void tegra_vic_describe(char *, size_t)
{}

int switch_vic_power_use()
{
    ++powerUses;
    return powerError;
}

void switch_vic_power_done()
{
    ++powerDones;
}

void switch_vic_power_idle()
{}

void tegra_host1x_describe_channel(uint32_t, char *, size_t)
{}
}

bool switch_vic_semantics_start(uint32_t)
{
    return true;
}

bool switch_vic_semantics_pending()
{
    return false;
}

int switch_vic_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchVICSemantic **out)
{
    *out = nullptr;
    return 0;
}

void switch_vic_semantics_commit(SwitchVICSemantic *)
{}

void switch_vic_semantics_physical(uint64_t)
{}

void switch_vic_semantics_retire(switch_gpu_event_waits *)
{}

switch_gpu_event_waits *switch_vic_events_batch(SwitchVICEvents *)
{
    return nullptr;
}

uint32_t switch_vic_events_tokens(SwitchVICEvents *, const switch_gpu_backing_token **out)
{
    *out = nullptr;
    return 0;
}

int switch_vic_events_bind(SwitchVICEvents *, const switch_gpu_backing_token *)
{
    return 0;
}

void switch_vic_events_commit(SwitchVICEvents *)
{}

extern "C" {
void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{}

void switch_gpu_event_waits_release(switch_gpu_event_waits **)
{}

void switch_gpu_event_waits_abort(switch_gpu_event_waits **)
{}

void switch_gpu_event_waits_commit(switch_gpu_event_waits **)
{}

int switch_gpu_event_waits_wait(switch_gpu_event_waits *, uint64_t)
{
    return 1;
}

int switch_gpu_event_waits_covers_token(switch_gpu_event_waits *, const switch_gpu_backing_token *)
{
    return 1;
}

int switch_gpu_event_waits_prepare_tokens(const switch_gpu_backing_token *, uint32_t, switch_gpu_event_waits **)
{
    return 0;
}

int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *, uint32_t, switch_gpu_event_waits **)
{
    return 0;
}

int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *, switch_gpu_event_waits *)
{
    return 0;
}
}

namespace {
class SwitchVICJobsTest : public testing::Test {
protected:
    uint8_t configurations[kSwitchVICConfigBytes] = {};
    TegraVICJob jobs[3] = {};
    switch_gpu_backing_use uses[1] = {{reinterpret_cast<switch_gpu_backing *>(1), SWITCH_GPU_BACKING_WRITE, 0}};

    void SetUp() override
    {
        begins = aborts = commits = retained = bridges = released = roomWaits = submits = powerUses = powerDones = 0;
        bridgeError = submitError = retainError = statusError = powerError = 0;
        bridgeResult = 1;
        transactionLocked = changeProducer = busyOnce = false;
        changeGeneration = nullptr;
        sourceTokens.clear();
        publishedWords.clear();
        gQueue = {};
        nextTestSequence += 4096;
        gQueue.submitted = gQueue.completed = nextTestSequence;
        gCompleted = nextTestSequence;
        gFailed = gSuspended = false;
        gLock = IOLockAlloc();
        switch_test_queue_lock = gLock;
        gSyncpt = 161;
        ASSERT_TRUE(Results::start(gSyncpt));
        gConfigs = {0x90000000, sizeof(configurations), configurations};
        for (auto &job : jobs) {
            job.count = 1;
            job.method[0] = TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET;
        }
    }

    void TearDown() override
    {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        EXPECT_EQ(retained, 0u);
        for (auto &job : gQueue.jobs) {
            switch_gpu_bridge_release(&job.bridge);
            Results::cancel(&job.outcome);
        }
        EXPECT_EQ(bridges, released);
        IOLockFree(gLock);
        gLock = nullptr;
        switch_test_queue_lock = nullptr;
    }

    int submit(uint32_t count = 1, const switch_gpu_backing_guard *guard = nullptr)
    {
        const SwitchVICOwnership ownership = {uses, 1, guard, guard ? 1u : 0u, nullptr, 0};
        return switch_vic_submit_checked(jobs, count, nullptr, ownership, nullptr, nullptr, nullptr);
    }

    void gpu(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i)
            sourceTokens.push_back({10, 1, SWITCH_GPU_ENGINE_GM20B, i + 1});
    }
};

TEST_F(SwitchVICJobsTest, PendingGpuProducerUsesHardwareBridgeOutsideOwnershipLocks)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(bridges, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    ASSERT_GE(publishedWords.size(), 3u);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
    EXPECT_EQ(publishedWords[1], 0x1000001u);
    EXPECT_EQ(publishedWords[2], 170u);
}

TEST_F(SwitchVICJobsTest, ExecutedGpuWriterStillBridgesUntilItsPixelsAreExternallyVisible)
{
    gpu();
    ASSERT_EQ(switch_gpu_backing_token_status(&sourceTokens[0]), 1);
    ASSERT_EQ(switch_gpu_backing_token_external_status(&sourceTokens[0]), 0);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 1u);
    EXPECT_EQ(commits, 1u);
}

TEST_F(SwitchVICJobsTest, PendingNvdecUsesFullWidthWaitWithoutGpuBridge)
{
    sourceTokens = {{0x181234567ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(begins, 1u);
    EXPECT_EQ(publishedWords[1], 0x81234567u);
    EXPECT_EQ(publishedWords[2], 162u);
}

TEST_F(SwitchVICJobsTest, MoreThanInlineDependenciesGrowAndPublishWithoutCpuProducerWait)
{
    gpu(65);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(aborts, 2u);
    EXPECT_EQ(commits, 1u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens.size(), 65u);
}

TEST_F(SwitchVICJobsTest, TooManyDependenciesRefuseBeforeHardwarePublication)
{
    gpu(SWITCH_GPU_BRIDGE_MAX_TOKENS + 1);
    EXPECT_EQ(submit(), -28);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(bridges, 0u);
}

TEST_F(SwitchVICJobsTest, ChangedGenerationAfterBridgeSubmissionRefusesStaleRepresentation)
{
    gpu();
    volatile uint64_t generation = 1;
    changeGeneration = &generation;
    const switch_gpu_backing_guard guard = {uses[0].backing, &generation, 1};
    EXPECT_EQ(submit(1, &guard), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(released, 1u);
}

TEST_F(SwitchVICJobsTest, NewProducerDuringBridgeSetupRequiresReplacement)
{
    gpu();
    changeProducer = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 2u);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens[0].value, 11u);
}

TEST_F(SwitchVICJobsTest, BridgeRoomWaitRunsWithoutBackingOrQueueLocks)
{
    gpu();
    busyOnce = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(roomWaits, 1u);
    EXPECT_EQ(commits, 1u);
}

TEST_F(SwitchVICJobsTest, BridgeAndTokenFailuresUnwindBeforePublication)
{
    gpu(2);
    retainError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(bridges, 0u);
    retainError = 0;
    bridgeError = -19;
    EXPECT_EQ(submit(), -19);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(retained, 0u);
}

TEST_F(SwitchVICJobsTest, FailedHostSubmissionDropsBridgeAndAbortsOwnership)
{
    gpu();
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(gQueue.count, 0u);
}

TEST_F(SwitchVICJobsTest, BatchRetainsBridgeThroughItsFinalCompletion)
{
    gpu();
    ASSERT_EQ(submit(3), 0);
    EXPECT_EQ(gQueue.count, 3u);
    EXPECT_EQ(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[1].bridge, nullptr);
    EXPECT_NE(gQueue.jobs[2].bridge, nullptr);
}

TEST_F(SwitchVICJobsTest, FailedProducerDoesNotPublishConsumer)
{
    gpu();
    statusError = -125;
    EXPECT_EQ(submit(), -125);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
}
}  // namespace

TEST_F(SwitchVICJobsTest, CompletedHardwareWithFailedProducerRetiresOnlyItsOwnJob)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    bridgeResult = -5;
    auto failed = gQueue.jobs[gQueue.head];
    EXPECT_EQ(completeJob(failed), -5);
    ASSERT_TRUE(gQueue.retire(static_cast<uint32_t>(failed.fence)));
    EXPECT_EQ(waitSequence(failed.fence), -5);
    sourceTokens.clear();
    bridgeResult = 1;
    ASSERT_EQ(submit(), 0);
    auto independent = gQueue.jobs[gQueue.head];
    EXPECT_EQ(completeJob(independent), 0);
    ASSERT_TRUE(gQueue.retire(static_cast<uint32_t>(independent.fence)));
    EXPECT_EQ(waitSequence(independent.fence), 0);
    EXPECT_EQ(waitSequence(failed.fence), -5);
}

// A submission records its output as a write, or VIC refuses it: an unrecorded write would leave the GPU's L2 lines of the
// allocation valid for its next reader.
TEST_F(SwitchVICJobsTest, ASubmissionWithoutAWriteUseIsRefused)
{
    switch_gpu_backing_use readOnly[1] = {{uses[0].backing, SWITCH_GPU_BACKING_READ, 0}};
    EXPECT_EQ(switch_vic_submit_checked(jobs, 1, nullptr, {nullptr, 0, nullptr, 0, nullptr, 0}, nullptr, nullptr,
                                        nullptr), -22);
    EXPECT_EQ(switch_vic_submit_checked(jobs, 1, nullptr, {readOnly, 1, nullptr, 0, nullptr, 0}, nullptr, nullptr,
                                        nullptr), -22);
    EXPECT_EQ(gQueue.submitted, gQueue.completed);
}

// Idle power gating (TegraVIC.cpp, SwitchEngineGate.h): a submission holds the powered engine for the whole call, an
// idle power-off parks only an empty queue, and the power-on's proof stream moves the syncpoint the queue rebases on.
TEST_F(SwitchVICJobsTest, EverySubmissionHoldsThePowerOnceAndReleasesIt)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(powerUses, 1u);
    EXPECT_EQ(powerDones, 1u);
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(powerUses, 2u);
    EXPECT_EQ(powerDones, 2u);
    submitError = 0;
    powerError = -19;
    const uint32_t before = submits;
    EXPECT_EQ(submit(), -19);
    EXPECT_EQ(submits, before);
    EXPECT_EQ(powerDones, 2u);
}

TEST_F(SwitchVICJobsTest, AnIdlePowerOffParksOnlyAnEmptyQueue)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_FALSE(switch_vic_jobs_park(false));
    EXPECT_FALSE(gSuspended);
    auto job = gQueue.jobs[gQueue.head];
    EXPECT_EQ(completeJob(job), 0);
    ASSERT_TRUE(gQueue.retire(static_cast<uint32_t>(job.fence)));
    EXPECT_TRUE(switch_vic_jobs_park(false));
    EXPECT_TRUE(gSuspended);
}

TEST_F(SwitchVICJobsTest, AParkedQueueHoldsTheNextSubmissionUntilAResumeRebasesIt)
{
    ASSERT_TRUE(switch_vic_jobs_park(false));
    EXPECT_THROW(submit(), std::runtime_error) << "the submission sleeps until resume";
    IOLockUnlock(gLock);
    const uint64_t base = gQueue.submitted;
    switch_vic_jobs_resume(true, static_cast<uint32_t>(base) + 2);
    EXPECT_FALSE(gSuspended);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(gQueue.submitted, base + 3);
    EXPECT_EQ(powerUses, powerDones);
}
