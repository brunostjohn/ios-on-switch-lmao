// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvdec_submit_stubs.h"
#include <algorithm>
#include <vector>
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchNVDECJobs.cpp"

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
bool semanticCapture;
uint32_t preparedBatches;

struct FakeBatch {};
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

int switch_gpu_backing_begin(switch_gpu_backing_use *uses, uint32_t count, const switch_gpu_backing_token *completion,
                             switch_gpu_backing_transaction *transaction)
{
    EXPECT_TRUE(switch_test_queue_locked);
    EXPECT_FALSE(transactionLocked);
    ++begins;
    *transaction = {};
    transaction->uses = uses;
    transaction->count = count;
    transaction->locked = 1;
    transaction->completion = *completion;
    transactionLocked = true;
    return 0;
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

int tegra_host1x_submit(const uint32_t *words, uint32_t count)
{
    ++submits;
    publishedWords.assign(words, words + count);
    return submitError;
}

int tegra_host1x_syncpt_wait(uint32_t, uint32_t, uint32_t)
{
    throw std::runtime_error("unexpected producer wait");
}

bool tegra_nvdec_ready()
{
    return true;
}

int switch_gpu_backing_cpu_acquire(switch_gpu_backing *, uint64_t, uint64_t, uint32_t)
{
    return 0;
}

int switch_gpu_backing_cpu_publish(switch_gpu_backing *, uint64_t, uint64_t)
{
    return 0;
}

int switch_gpu_backing_cpu_release(switch_gpu_backing *, uint64_t, uint64_t, uint32_t)
{
    return 0;
}

void tegra_host1x_describe_channel(uint32_t, char *, size_t)
{}
}

[[noreturn]] void switch_nvdec_job_fault(uint32_t, int, uint64_t)
{
    throw std::runtime_error("decode fault");
}

int switch_nvdec_power_use()
{
    ++powerUses;
    return powerError;
}

void switch_nvdec_power_done()
{
    ++powerDones;
}

void switch_nvdec_power_idle()
{}

bool switch_nvdec_semantics_start(uint32_t)
{
    return true;
}

bool switch_nvdec_semantics_pending()
{
    return false;
}

int switch_nvdec_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchNVDECSemantic **out)
{
    *out = nullptr;
    return 0;
}

void switch_nvdec_semantics_commit(SwitchNVDECSemantic *)
{}

void switch_nvdec_semantics_physical(uint64_t)
{}

void switch_nvdec_semantics_retire(switch_gpu_event_waits *)
{}

extern "C" {
void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{}

void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
    delete reinterpret_cast<FakeBatch *>(*batch);
    *batch = nullptr;
}

void switch_gpu_event_waits_abort(switch_gpu_event_waits **batch)
{
    switch_gpu_event_waits_release(batch);
}

void switch_gpu_event_waits_commit(switch_gpu_event_waits **batch)
{
    *batch = nullptr;
}

int switch_gpu_event_waits_wait(switch_gpu_event_waits *, uint64_t)
{
    return 1;
}

int switch_gpu_event_waits_covers_token(switch_gpu_event_waits *, const switch_gpu_backing_token *)
{
    return !semanticCapture || preparedBatches ? 1 : 0;
}

int switch_gpu_event_waits_prepare_tokens(const switch_gpu_backing_token *, uint32_t, switch_gpu_event_waits **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    *out = nullptr;
    ++preparedBatches;
    return 0;
}

int switch_gpu_event_waits_prepare(switch_gpu_event_use *const *, uint32_t, switch_gpu_event_waits **out)
{
    EXPECT_FALSE(transactionLocked);
    EXPECT_FALSE(switch_test_queue_locked);
    *out = reinterpret_cast<switch_gpu_event_waits *>(new FakeBatch);
    return 0;
}

int switch_gpu_event_waits_merge_uncommitted(switch_gpu_event_waits *, switch_gpu_event_waits *)
{
    return 0;
}
}

namespace {
class SwitchNVDECSubmitTest : public testing::Test {
protected:
    uint8_t setup[SwitchNVDECQueue::kSlots * kSetupStride] = {};
    Submission work = {};
    TegraNVDECSubmit request = {};
    uint32_t fence = 0;

    void SetUp() override
    {
        begins = aborts = commits = retained = bridges = released = roomWaits = submits = preparedBatches = 0;
        powerUses = powerDones = 0;
        bridgeError = submitError = retainError = statusError = powerError = 0;
        bridgeResult = 1;
        transactionLocked = changeProducer = busyOnce = semanticCapture = false;
        sourceTokens.clear();
        publishedWords.clear();
        gQueue = {};
        nextTestSequence += 4096;
        gQueue.submitted = gQueue.completed = nextTestSequence;
        gCompleted = nextTestSequence;
        gFailed = gSuspended = false;
        gLock = IOLockAlloc();
        switch_test_queue_lock = gLock;
        gSyncpt = 162;
        ASSERT_TRUE(Results::start(gSyncpt));
        gSetup = {nullptr, setup, 0x90000000, sizeof(setup)};
        request.entry_count = 1;
        request.entries[0] = {NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 0, 0};
        request.setup_bytes = 4;
        work.uses[0] = {reinterpret_cast<switch_gpu_backing *>(1), SWITCH_GPU_BACKING_WRITE, 0};
        work.useCount = 1;
    }

    void TearDown() override
    {
        EXPECT_FALSE(transactionLocked);
        EXPECT_FALSE(switch_test_queue_locked);
        EXPECT_EQ(retained, 0u);
        for (auto &job : gQueue.jobs) {
            switch_gpu_bridge_release(&job.bridge);
            switch_gpu_event_waits_release(&job.semantics);
            Results::cancel(&job.outcome);
        }
        EXPECT_EQ(bridges, released);
        if (work.words)
            IODelete(work.words, uint32_t, work.wordCapacity);
        IOLockFree(gLock);
        gLock = nullptr;
        switch_test_queue_lock = nullptr;
    }

    int submit()
    {
        return publish(request, work, &fence);
    }

    void gpu(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i)
            sourceTokens.push_back({10, 1, SWITCH_GPU_ENGINE_GM20B, i + 1});
    }
};

TEST_F(SwitchNVDECSubmitTest, PendingGpuUsesHardwareBridgeOutsideOwnershipLocks)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(aborts, 1u);
    EXPECT_EQ(commits, 1u);
    EXPECT_EQ(fence, nextTestSequence + 1);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5));
    EXPECT_EQ(publishedWords[1], 0x1000001u);
    EXPECT_EQ(publishedWords[2], 170u);
}

TEST_F(SwitchNVDECSubmitTest, ExecutedGpuStillBridgesUntilExternalVisibility)
{
    gpu();
    ASSERT_EQ(switch_gpu_backing_token_status(&sourceTokens[0]), 1);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 1u);
}

TEST_F(SwitchNVDECSubmitTest, VicWaitKeepsFullLow32Threshold)
{
    sourceTokens = {{0x181234567ULL, 1, SWITCH_GPU_ENGINE_VIC, 161}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(publishedWords[1], 0x81234567u);
    EXPECT_EQ(publishedWords[2], 161u);
}

TEST_F(SwitchNVDECSubmitTest, OwnQueueDependencyUsesEngineOrderWithoutCpuWait)
{
    sourceTokens = {{10, 1, SWITCH_GPU_ENGINE_NVDEC, 162}};
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 0u);
    EXPECT_EQ(publishedWords[0], tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0));
}

TEST_F(SwitchNVDECSubmitTest, LargeDependencySetGrowsOutsideOwnershipAndPublishes)
{
    gpu(70);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(begins, 3u);
    EXPECT_EQ(aborts, 2u);
    ASSERT_NE(gQueue.jobs[0].bridge, nullptr);
    EXPECT_EQ(gQueue.jobs[0].bridge->tokens.size(), 70u);
}

TEST_F(SwitchNVDECSubmitTest, LargeVicDependencySetHasEnoughCommandStorage)
{
    for (uint32_t i = 0; i < 100; ++i)
        sourceTokens.push_back({0x181234567ULL + i, 1, SWITCH_GPU_ENGINE_VIC, 161});
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(publishedWords.size(), 308u);
    EXPECT_EQ(publishedWords[298], 0x812345cau);
}

TEST_F(SwitchNVDECSubmitTest, TooManyDependenciesRejectBeforeHardware)
{
    gpu(SWITCH_GPU_BRIDGE_MAX_TOKENS + 1);
    EXPECT_EQ(submit(), -28);
    EXPECT_EQ(submits, 0u);
    EXPECT_EQ(commits, 0u);
}

TEST_F(SwitchNVDECSubmitTest, ChangedProducerDuringBridgeSetupRequiresFreshBridge)
{
    gpu();
    changeProducer = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(bridges, 2u);
    EXPECT_EQ(released, 1u);
    EXPECT_EQ(begins, 3u);
}

TEST_F(SwitchNVDECSubmitTest, BridgePressureSleepsOnlyOutsideOwnership)
{
    gpu();
    busyOnce = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(roomWaits, 1u);
}

TEST_F(SwitchNVDECSubmitTest, FailedProducerNeverPublishes)
{
    gpu();
    statusError = -125;
    EXPECT_EQ(submit(), -125);
    EXPECT_EQ(submits, 0u);
}

TEST_F(SwitchNVDECSubmitTest, FailedKickUnwindsBridgeAndOwnership)
{
    gpu();
    submitError = -5;
    EXPECT_EQ(submit(), -5);
    EXPECT_EQ(commits, 0u);
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(released, 1u);
}

TEST_F(SwitchNVDECSubmitTest, SemanticCaptureDropsBackingAndQueueLocksBeforeAllocation)
{
    gpu();
    semanticCapture = true;
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(preparedBatches, 1u);
    EXPECT_EQ(begins, 3u);
    EXPECT_NE(gQueue.jobs[0].semantics, nullptr);
}
}

TEST_F(SwitchNVDECSubmitTest, CompletedHardwareWithFailedProducerRetiresOnlyItsOwnJob)
{
    gpu();
    ASSERT_EQ(submit(), 0);
    bridgeResult = -5;
    auto failed = gQueue.jobs[gQueue.head];
    EXPECT_EQ(completeJob(failed), -5);
    ASSERT_TRUE(gQueue.retire(static_cast<uint32_t>(failed.sequence)));
    EXPECT_EQ(waitSequence(failed.sequence), -5);
    sourceTokens.clear();
    bridgeResult = 1;
    ASSERT_EQ(submit(), 0);
    auto independent = gQueue.jobs[gQueue.head];
    EXPECT_EQ(completeJob(independent), 0);
    ASSERT_TRUE(gQueue.retire(static_cast<uint32_t>(independent.sequence)));
    EXPECT_EQ(waitSequence(independent.sequence), 0);
    EXPECT_EQ(waitSequence(failed.sequence), -5);
}

// Idle power gating (TegraNVDEC.cpp, SwitchEngineGate.h): a run holds the powered engine across its publication, an
// idle power-off parks only an empty queue, and the power-on's proof stream moves the syncpoint the queue rebases on.
TEST_F(SwitchNVDECSubmitTest, EveryRunHoldsThePowerOnceAndReleasesIt)
{
    const TegraNVDECResolve resolve = [](void *, uint32_t, TegraNVDECBlock *) {
        return false;
    };
    const Check check = [](const TegraNVDECSubmit *, const uint64_t *) -> const char * {
        return nullptr;
    };
    const char *why = nullptr;
    ASSERT_EQ(run(&request, resolve, nullptr, check, &fence, &why), 0);
    EXPECT_EQ(powerUses, 1u);
    EXPECT_EQ(powerDones, 1u);
    powerError = -19;
    const uint32_t before = submits;
    EXPECT_EQ(run(&request, resolve, nullptr, check, &fence, &why), -19);
    EXPECT_EQ(submits, before);
    EXPECT_EQ(powerDones, 1u);
}

TEST_F(SwitchNVDECSubmitTest, AnIdlePowerOffParksOnlyAnEmptyQueue)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_FALSE(switch_nvdec_jobs_park(false));
    EXPECT_FALSE(gSuspended);
}

TEST_F(SwitchNVDECSubmitTest, AParkedQueueHoldsTheNextJobUntilAResumeRebasesIt)
{
    ASSERT_TRUE(switch_nvdec_jobs_park(false));
    EXPECT_THROW(submit(), std::runtime_error) << "the job sleeps until resume";
    IOLockUnlock(gLock);
    switch_nvdec_jobs_resume(true, static_cast<uint32_t>(nextTestSequence) + 2);
    EXPECT_FALSE(gSuspended);
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(fence, static_cast<uint32_t>(nextTestSequence + 3));
}
