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
int bridgeError, submitError, retainError, statusError, powerError, semanticResult = 1;
std::vector<switch_host1x_fence *> fences;
std::vector<uint32_t> fenceBounds, watchedBounds;

struct Notification {
    void (*completed)(void *, int);
    void *context;
};

std::vector<Notification> notifications;
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

int switch_gpu_bridge_notify(const switch_gpu_bridge *, void (*completed)(void *, int), void *context)
{
    EXPECT_FALSE(switch_test_queue_locked);
    notifications.push_back({completed, context});
    return 0;
}

void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                              void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t boundUs)
{
    EXPECT_TRUE(switch_test_queue_locked);
    fence->state = SWITCH_HOST1X_FENCE_QUEUED;
    fences.push_back(fence);
    fenceBounds.push_back(boundUs);
    return 0;
}

void switch_host1x_fence_watch(switch_host1x_fence *, uint32_t boundUs)
{
    watchedBounds.push_back(boundUs);
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

bool switch_nvdec_semantics_start(uint32_t, void (*)(switch_gpu_event_waits *))
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

int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *)
{
    return semanticResult;
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
        semanticResult = 1;
        fences.clear();
        fenceBounds.clear();
        watchedBounds.clear();
        notifications.clear();
        for (auto &stage : Stages::stages)
            stage = {};
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

    int submit(const SwitchNVDECCompletion &completion = {})
    {
        waitedForRoom = false;
        return publish(request, work, completion, &fence, &waitedForRoom);
    }

    bool waitedForRoom = false;

    void gpu(uint32_t count = 1)
    {
        for (uint32_t i = 0; i < count; ++i)
            sourceTokens.push_back({10, 1, SWITCH_GPU_ENGINE_GM20B, i + 1});
    }

    static void interrupt(size_t which)
    {
        auto *fence = fences.at(which);
        fence->state = SWITCH_HOST1X_FENCE_IDLE;
        fence->signaled(fence, 0);
    }

    static void bridged(size_t which, int status)
    {
        const auto notification = notifications.at(which);
        notification.completed(notification.context, status);
    }

    // The events' retirement reporting the job's semantic outcome.
    static void semantic(uint64_t sequence)
    {
        switch_gpu_event_waits *batch = nullptr;
        for (const auto &job : gQueue.jobs)
            if (job.sequence == sequence)
                batch = job.semantics;
        ASSERT_NE(batch, nullptr);
        semanticRetired(batch);
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
    const uint64_t failed = gQueue.submitted;
    bridged(0, -5);
    interrupt(0);
    semantic(failed);
    EXPECT_EQ(waitSequence(failed), -5);
    sourceTokens.clear();
    ASSERT_EQ(submit(), 0);
    const uint64_t independent = gQueue.submitted;
    interrupt(1);
    semantic(independent);
    EXPECT_EQ(waitSequence(independent), 0);
    EXPECT_EQ(waitSequence(failed), -5);
    EXPECT_EQ(gQueue.count, 0u);
}

// A decode's bound covers the whole queue ahead of it, from its submission or, behind a GPU producer, from that
// producer's completion (D8); it ends on the interrupt, the bridge and the events' retirement, in any order, on no
// thread of its own.
TEST_F(SwitchNVDECSubmitTest, ADecodeEndsWhenItsFenceBridgeAndSemanticOutcomeAreAllIn)
{
    ASSERT_EQ(submit(), 0);
    EXPECT_EQ(fenceBounds.back(), kWaitBoundUs);
    EXPECT_EQ(fences.back()->threshold, fence);
    const uint64_t plain = gQueue.submitted;
    gpu();
    ASSERT_EQ(submit(), 0);
    const uint64_t bridgedDecode = gQueue.submitted;
    EXPECT_EQ(fenceBounds.back(), 0u);
    semantic(plain);
    EXPECT_EQ(gQueue.count, 2u);
    interrupt(0);
    EXPECT_EQ(gQueue.count, 1u);
    interrupt(1);
    semantic(bridgedDecode);
    EXPECT_EQ(gQueue.count, 1u);
    bridged(0, 0);
    EXPECT_EQ(watchedBounds, std::vector<uint32_t>{kWaitBoundUs});
    EXPECT_EQ(gQueue.count, 0u);
    EXPECT_EQ(gQueue.completed, bridgedDecode);
    EXPECT_EQ(released, 1u);
}

TEST_F(SwitchNVDECSubmitTest, ADecodeWhoseFenceFailsFaults)
{
    ASSERT_EQ(submit(), 0);
    auto *failing = fences.at(0);
    EXPECT_THROW(failing->signaled(failing, TEGRA_HOST1X_ETIMEDOUT), std::runtime_error);
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
    ASSERT_EQ(run(&request, resolve, nullptr, check, nullptr, &fence, &waitedForRoom, &why), 0);
    EXPECT_EQ(powerUses, 1u);
    EXPECT_EQ(powerDones, 1u);
    powerError = -19;
    const uint32_t before = submits;
    EXPECT_EQ(run(&request, resolve, nullptr, check, nullptr, &fence, &waitedForRoom, &why), -19);
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

namespace {
struct Completed {
    uint32_t fence;
    int result, recordStatus;
    bool queueLocked;
};

std::vector<Completed> completed;

void recordCompletion(void *context, uint32_t fence, int result)
{
    const switch_gpu_backing_token token = {*static_cast<uint64_t *>(context), switch_nvdec_result_state.generation,
                                            SWITCH_GPU_ENGINE_NVDEC, gSyncpt};
    completed.push_back({fence, result, status(&token), switch_test_queue_locked});
}
}  // namespace

// The AVD plug-in's emission hangs off this: a job's completion runs once from its fence-driven retirement, off the
// queue lock, while a Wait on the job would still sleep (its Results record not yet complete).
TEST_F(SwitchNVDECSubmitTest, ACompletionRunsOnceFromRetirementBeforeItsRecordCompletes)
{
    completed.clear();
    uint64_t sequence = nextTestSequence + 1;
    ASSERT_EQ(submit({recordCompletion, &sequence}), 0);
    interrupt(0);
    EXPECT_TRUE(completed.empty()) << "the semantic outcome is not in yet";
    semantic(sequence);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].fence, fence);
    EXPECT_EQ(completed[0].result, 0);
    EXPECT_EQ(completed[0].recordStatus, 0) << "the record completes after the completion";
    EXPECT_FALSE(completed[0].queueLocked);
    EXPECT_EQ(waitSequence(sequence), 0);
    EXPECT_EQ(gQueue.jobs[0].completion.done, nullptr);
}

TEST_F(SwitchNVDECSubmitTest, AFailedProducersJobCompletesWithItsError)
{
    completed.clear();
    gpu();
    uint64_t sequence = nextTestSequence + 1;
    ASSERT_EQ(submit({recordCompletion, &sequence}), 0);
    bridged(0, -5);
    interrupt(0);
    semantic(sequence);
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].result, -5);
}

// A client whose ring of input buffers is one deeper than the queue never writes one a job still reads: the submit that
// finds the queue full sleeps until the oldest job retires, and says so.
TEST_F(SwitchNVDECSubmitTest, ASubmitIntoAFullQueueSleepsUntilTheOldestRetiresAndSaysSo)
{
    for (uint32_t i = 0; i < SwitchNVDECQueue::kSlots; ++i) {
        ASSERT_EQ(submit(), 0);
        EXPECT_FALSE(waitedForRoom);
    }
    ASSERT_TRUE(gQueue.full());
    switch_test_while_asleep = [] {
        interrupt(0);
        semantic(nextTestSequence + 1);
    };
    ASSERT_EQ(submit(), 0);
    EXPECT_TRUE(waitedForRoom);
    EXPECT_EQ(gQueue.completed, nextTestSequence + 1);
    EXPECT_EQ(fence, nextTestSequence + SwitchNVDECQueue::kSlots + 1);
}
