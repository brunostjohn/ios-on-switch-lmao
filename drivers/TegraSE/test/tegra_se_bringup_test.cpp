// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSE's bring-up against a register model of both engines: an operation the model never ends turns its engine off
// for the boot, and nothing after it waits for that engine; once bring-up is over, a caller's operation that never ends
// is a panic. Each scenario runs in a process of its own (the driver's state is boot-wide) and is given 5 s; one still
// waiting then is let go (callers refused as at system sleep) and fails.
#include "tegra_se_model.h"
#include "bazel/testing/fresh_process.h"

namespace se_model {
uint32_t initial[16] = {1, 2, 3, 4, 5, 6, 7, 8};

void compress(ccdigest_state_t state, size_t nblocks, const void *)
{
    reinterpret_cast<uint32_t *>(state)[0] += static_cast<uint32_t>(nblocks);
}

const ccdigest_info kSha256 = {32, 32, 64, 0, nullptr, initial, compress};
const ccdigest_info kSha1 = {20, 20, 64, 0, nullptr, initial, compress};
}  // namespace se_model

namespace {
void bringUpWithSe2LostOnItsSecondOperation()
{
    se_model::engines[1].hangAt = 2;
    const bool ended = se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    });
    EXPECT_TRUE(ended) << "bring-up kept waiting for SE2 after it went off:\n" << se_model::log();
    EXPECT_TRUE(gReady);
    EXPECT_TRUE(gEngines[0].up);
    EXPECT_FALSE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[1].started, 2u) << "an operation reached SE2 after it was off";
    EXPECT_TRUE(gEngines[0].proven) << "callers reach SE1 from here";
    EXPECT_NE(se_model::log().find("SE2 off for this boot: operation never ended"), std::string::npos)
        << se_model::log();
    ASSERT_FALSE(se_model::engines[0].crypto.empty());
    ASSERT_FALSE(se_model::engines[1].crypto.empty());
    for (const uint32_t config : se_model::engines[0].crypto)
        EXPECT_NE(config & kCryptoMccif, 0u) << "SE1 moves its data through its memory clients";
    for (const uint32_t config : se_model::engines[1].crypto)
        EXPECT_EQ(config & kCryptoMccif, 0u) << "SE2 stays on the AHB";
}

void stateLoadAfterItsEngineWentOff()
{
    gAhb = switch_mmio_map(kAhbArbPA, 0x1000);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gLoop = IOWorkLoop::workLoop();
    gQueue = IOLockAlloc();
    ASSERT_TRUE(engineUp(gEngines[0]));
    ASSERT_TRUE(engineUp(gEngines[1]));
    se_model::engines[0].hangAt = 1;
    bool sha256 = true;
    bool sha1 = true;
    const bool ended = se_model::finishes([&] {
        sha256 = stateLoadWorks(gEngines[0], SHA256, &se_model::kSha256);
        sha1 = stateLoadWorks(gEngines[0], SHA1, &se_model::kSha1);
    });
    EXPECT_TRUE(ended) << "the SHA-1 state load kept waiting for SE1 after it went off:\n" << se_model::log();
    EXPECT_FALSE(sha256);
    EXPECT_FALSE(sha1);
    EXPECT_FALSE(gEngines[0].up);
    EXPECT_TRUE(gEngines[1].up);
    EXPECT_EQ(se_model::engines[0].started, 1u);
    EXPECT_EQ(se_model::engines[1].started, 0u) << "SE2 was not the engine under test";
}

// A caller's operation on a proven engine may have DMA in flight into the caller's pages: never an engine turned off.
void callerOperationThatNeverEnds()
{
    ASSERT_TRUE(se_model::finishes([] {
        seThread(nullptr, THREAD_AWAKENED);
    }));
    ASSERT_TRUE(gReady);
    ASSERT_TRUE(gEngines[0].proven);
    gAlg[SHA256].on = true;  // the model's engines give no real digests, so no known answer passed
    switch_dma_buffer caller;
    ASSERT_TRUE(switch_dma_alloc(&caller, kBulk, 0, 0));
    se_model::engines[0].hangFromBytes = 1;
    uint8_t digest[32];
    (void)tegra_se_digest(256, caller.va, kBulk, digest);
    ADD_FAILURE() << "a caller's operation that never ended returned:\n" << se_model::log();
}
}  // namespace

TEST(TegraSEBringUp, AnEngineThatNeverEndsAnOperationIsOffAndBringUpFinishesWithoutIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(bringUpWithSe2LostOnItsSecondOperation);
}

TEST(TegraSEBringUp, AStateLoadCheckOfAnEngineThatWentOffEndsAtOnce)
{
    switch_ios::test::ExpectPassesInFreshProcess(stateLoadAfterItsEngineWentOff);
}

TEST(TegraSEBringUp, ACallersOperationThatNeverEndsOnAProvenEngineIsAPanic)
{
    switch_ios::test::ExpectExitInFreshProcess(callerOperationThatNeverEnds, 3,
                                               "panic: TegraSE: SE1 operation never ended");
}
