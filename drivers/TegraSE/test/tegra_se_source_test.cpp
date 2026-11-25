// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the device run showed and a host model cannot: over the AHB both engines ran at about 120 MB/s, a tenth of the
// CPU, and boot sent XNU's large hashes to them anyway. SE1 moves data through its own memory-controller clients, work
// reaches the engines from the bulk size down to whatever a timing run measures no slower, and an engine is proven
// (its failures panic) once bring-up has run every kind of operation on it into the driver's own buffers.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraSESource, EveryOperationTakesItsEnginesMemoryInterface)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_EQ(Count(source, "CRYPTO_CONFIG,"), 1u);
    EXPECT_THAT(Body(source, "void crypto(const Engine &e, uint32_t config)\n"),
                HasSubstr("wr(e, CRYPTO_CONFIG, config | kMemif[&e - gEngines]);"));
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kMemif[2] = {kCryptoMccif, 0};"));
}

TEST(TegraSESource, BulkWorkGoesToTheEnginesUntilATimingRunMeasuresLower)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(source, HasSubstr("constexpr size_t kBulk = 2u << 20;"));
    EXPECT_THAT(source, HasSubstr("volatile size_t gCutover[ALGS] = {kBulk, kBulk, kBulk, kBulk, kBulk, kBulk, kBulk, "
                                  "kBulk, kBulk, kBulk};"));
    const std::string timing = Body(source, "void timing()\n");
    EXPECT_THAT(timing, HasSubstr("gCutover[alg] = cutover[alg] && cutover[alg] < kBulk ? cutover[alg] : kBulk;"));
}

// Proven before gReady lets callers in, in every kernel: from then on an operation may be writing a caller's pages.
TEST(TegraSESource, AnEngineIsProvenBeforeCallersPagesReachIt)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(Body(source, "bool run(Engine &e, uint32_t in, uint32_t out)\n"), Not(HasSubstr("e.proven = true")));
    const std::string bringUp = Body(source, "void seThread(void *, wait_result_t)\n");
    const size_t proven = bringUp.find("    prove();\n    gReady = true;");
    ASSERT_NE(proven, std::string::npos);
    EXPECT_LT(bringUp.find("rng(*got, seed)"), proven);
    EXPECT_EQ(Count(source, "prove();"), 1u);
}
