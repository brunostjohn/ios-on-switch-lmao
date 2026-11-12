// SPDX-License-Identifier: MIT
#include "drivers/TegraGM20BMetal/raw/runtime/raw_loop_ranges.h"
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using Records = raw::PredicateRecords;

// A tape as the facade records it: 'C' a compute encoder (serial), 'P' a compute encoder (concurrent), 'R' other work,
// and the records: i (StartIf), e (StartElse), f (EndIf), w/W (StartWhile/EndWhile), d/D (StartDoWhile/EndDoWhile),
// '.' a dispatch. Answers each split as (loop start, loop end + 1).
std::vector<std::pair<uint32_t, uint32_t>> splits(const std::string &tape)
{
    raw::LoopRanges loops;
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t at = 0; at < tape.size(); ++at) {
        const char c = tape[at];
        const Records::Kind kinds[] = {Records::StartIf,    Records::StartElse, Records::EndIf,
                                       Records::StartWhile, Records::EndWhile,  Records::StartDoWhile,
                                       Records::EndDoWhile};
        const std::string letters = "iefwWdD";
        if (c == 'C' || c == 'P')
            loops.computeEncoder(c == 'C' ? 0 : 1);
        else if (c == 'R')
            loops.otherWork();
        else if (const size_t k = letters.find(c); k != std::string::npos && loops.record(kinds[k], at))
            out.push_back({loops.loopFrom(), at + 1});
    }
    return out;
}

using Splits = std::vector<std::pair<uint32_t, uint32_t>>;

TEST(RawLoopRanges, EachOutermostLoopEndsARange)
{
    EXPECT_EQ(splits("C.w.W.d.D."), (Splits{{2, 5}, {6, 9}}));
}

TEST(RawLoopRanges, ALoopInsideAnotherSplitsOnlyAfterTheOuter)
{
    EXPECT_EQ(splits("Cw.d.D.W."), (Splits{{1, 8}}));
    EXPECT_EQ(splits("Cd.w.W.D"), (Splits{{1, 8}}));
}

TEST(RawLoopRanges, ALoopInsideABlockDoesNotSplit)
{
    EXPECT_EQ(splits("Ci.w.W.f"), Splits{});
    EXPECT_EQ(splits("Ci.e.d.D.f"), Splits{});
}

// A block stays open across compute encoders of one dispatch type, and closes at any other encoder or type
TEST(RawLoopRanges, BlocksLastAsTheRecordsUnitsKeepThem)
{
    EXPECT_EQ(splits("Ci.Cw.W.f"), Splits{});
    EXPECT_EQ(splits("Ci.Rw.W"), (Splits{{4, 7}}));
    EXPECT_EQ(splits("Ci.Pw.W"), (Splits{{4, 7}}));
}

TEST(RawLoopRanges, AMisusedEndSplitsNothing)
{
    EXPECT_EQ(splits("C.W.D"), Splits{});
    EXPECT_EQ(splits("Ci.W"), Splits{});
}

}  // namespace
