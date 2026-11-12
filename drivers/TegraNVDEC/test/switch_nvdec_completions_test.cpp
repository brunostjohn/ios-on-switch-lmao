// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvdec_completions_stubs.h"
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../SwitchNVDECCompletions.cpp"

struct switch_gpu_backing {
    int references = 1;
};

extern "C" {
void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->references;
}

void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    EXPECT_GT(backing->references, 0);
    --backing->references;
}
}

namespace {
constexpr uint32_t kHandle = 5, kOffset = 256;
constexpr mach_port_t kPort = 0x1103, kOtherPort = 0x2207;

class SwitchNVDECCompletionsTest : public testing::Test {
protected:
    alignas(64) uint8_t input[4096] = {};
    switch_gpu_backing backing;
    TegraNVDECSubmit submit = {};
    SwitchNVDECCompletions *completions = nullptr;

    void SetUp() override
    {
        switch_test_sent.clear();
        switch_test_released_ports.clear();
        submit.entry_count = 2;
        submit.entries[0] = {NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kHandle, 512};
        submit.entries[1] = {NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kHandle, kOffset};
        completions = switch_nvdec_completions_create({switch_test_send, switch_test_refer});
        ASSERT_NE(completions, nullptr);
    }

    void TearDown() override
    {
        switch_nvdec_completions_release(completions);
        EXPECT_EQ(backing.references, 1) << "every armed job gives its status buffer back";
    }

    static bool resolve(void *context, uint32_t handle, TegraNVDECBlock *block)
    {
        auto *test = static_cast<SwitchNVDECCompletionsTest *>(context);
        if (handle != kHandle)
            return false;
        *block = {};
        block->va = test->input;
        block->size = sizeof(test->input);
        block->backing = &test->backing;
        return true;
    }

    SwitchNVDECCompletion arm()
    {
        SwitchNVDECCompletion completion = {};
        EXPECT_EQ(switch_nvdec_completions_arm(completions, &submit, resolve, this, &completion), 0);
        return completion;
    }

    void status(uint32_t error, uint32_t correct)
    {
        nvdec_status_s record = {};
        record.error_status = error;
        record.mbs_correctly_decoded = correct;
        std::memcpy(input + kOffset, &record, sizeof(record));
    }
};

TEST_F(SwitchNVDECCompletionsTest, NothingListeningArmsNothing)
{
    const auto completion = arm();
    EXPECT_EQ(completion.done, nullptr);
    EXPECT_EQ(backing.references, 1);
}

// The AVD plug-in's emission: the job's status, read by the kernel after the job's fence, goes to the registered port
// with the job's fence; the status buffer is held until then.
TEST_F(SwitchNVDECCompletionsTest, AJobsStatusReachesTheRegisteredPortWithItsFence)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    const auto completion = arm();
    ASSERT_NE(completion.done, nullptr);
    EXPECT_EQ(backing.references, 2);
    status(0x2c, 0);
    completion.done(completion.context, 77, 0);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kPort);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnSuccess);
    EXPECT_EQ(switch_test_sent[0].arguments, (std::vector<io_user_reference_t>{77, 0x2c, 0}));
    EXPECT_EQ(backing.references, 1);
}

TEST_F(SwitchNVDECCompletionsTest, AFailedJobCompletesWithAnError)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    const auto completion = arm();
    completion.done(completion.context, 78, -5);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].result, kIOReturnIOError);
}

// A connection closed with jobs in flight: their completions still read and give back the status buffer, send nothing,
// and the record set goes with the last of them.
TEST_F(SwitchNVDECCompletionsTest, AJobOutlivingItsConnectionSendsNothingAndFreesTheRecords)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvdec_completions_release(completions);
    completions = nullptr;
    EXPECT_EQ(switch_test_released_ports, std::vector<mach_port_t>{kPort});
    completion.done(completion.context, 79, 0);
    EXPECT_TRUE(switch_test_sent.empty());
}

TEST_F(SwitchNVDECCompletionsTest, AFailedSubmitsCompletionIsGivenBackUnsent)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    const auto completion = arm();
    switch_nvdec_completions_disarm(&completion);
    EXPECT_TRUE(switch_test_sent.empty());
    EXPECT_EQ(backing.references, 1);
}

TEST_F(SwitchNVDECCompletionsTest, ARecordPerJobInFlightAndThenNoMore)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    std::vector<SwitchNVDECCompletion> armed;
    for (uint32_t i = 0; i < 2 * SwitchNVDECQueue::kSlots; ++i)
        armed.push_back(arm());
    SwitchNVDECCompletion refused = {};
    EXPECT_EQ(switch_nvdec_completions_arm(completions, &submit, resolve, this, &refused), -12);
    EXPECT_EQ(refused.done, nullptr);
    for (const auto &completion : armed)
        completion.done(completion.context, 1, 0);
    EXPECT_EQ(switch_test_sent.size(), armed.size());
}

TEST_F(SwitchNVDECCompletionsTest, ANewPortReplacesTheOldOne)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kOtherPort, nullptr), kIOReturnSuccess);
    EXPECT_EQ(switch_test_released_ports, std::vector<mach_port_t>{kPort});
    const auto completion = arm();
    completion.done(completion.context, 80, 0);
    ASSERT_EQ(switch_test_sent.size(), 1u);
    EXPECT_EQ(switch_test_sent[0].port, kOtherPort);
}

TEST_F(SwitchNVDECCompletionsTest, AStatusRecordOutsideItsBufferArmsNothing)
{
    ASSERT_EQ(switch_nvdec_completions_listen(completions, kPort, nullptr), kIOReturnSuccess);
    submit.entries[1].offset = sizeof(input) - sizeof(nvdec_status_s) + 4;
    EXPECT_EQ(arm().done, nullptr);
    submit.entries[1].value = kHandle + 1;
    EXPECT_EQ(arm().done, nullptr);
}
}  // namespace
