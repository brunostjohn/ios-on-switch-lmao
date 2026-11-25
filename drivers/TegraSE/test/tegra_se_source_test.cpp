// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the device run showed and a host model cannot: over the AHB both engines ran at about 120 MB/s, a tenth of the
// CPU, and boot sent XNU's large hashes to them anyway. The engines move data through their own memory-controller
// clients, nothing goes to them below a measured crossover, and bring-up's failures turn an engine off rather than
// panic.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraSESource, EveryOperationMovesDataThroughTheEnginesMemoryClients)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_EQ(Count(source, "CRYPTO_CONFIG,"), 1u);
    EXPECT_THAT(Body(source, "void crypto(const Engine &e, uint32_t config)\n"),
                HasSubstr("wr(e, CRYPTO_CONFIG, config | kCryptoMccif);"));
    EXPECT_THAT(source, HasSubstr("kCryptoMccif = 1u << 31"));
}

TEST(TegraSESource, NothingGoesToAnEngineUntilItIsMeasuredNoSlower)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(source, HasSubstr("volatile size_t gCutover[ALGS] = {kNever, kNever, kNever, kNever, kNever, kNever, "
                                  "kNever, kNever, kNever, kNever};"));
    const std::string timing = Body(source, "void timing()\n");
    EXPECT_THAT(timing, HasSubstr("gCutover[alg] = cutover[alg] ? cutover[alg] : kNever;"));
    EXPECT_THAT(timing, Not(HasSubstr("2 * static_cast<size_t>(sizes[4])")));
}

TEST(TegraSESource, OnlyBringUpsEndProvesAnEngine)
{
    const std::string source = SourceText("../TegraSE.cpp");
    EXPECT_THAT(Body(source, "bool run(Engine &e, uint32_t in, uint32_t out)\n"), Not(HasSubstr("e.proven = true")));
    const std::string bringUp = Body(source, "void seThread(void *, wait_result_t)\n");
    const size_t proven = bringUp.find("e.proven = e.up;");
    ASSERT_NE(proven, std::string::npos);
    EXPECT_LT(bringUp.find("rng(*got, seed)"), proven);
    EXPECT_LT(proven, bringUp.find("gReady = true;"));
}
