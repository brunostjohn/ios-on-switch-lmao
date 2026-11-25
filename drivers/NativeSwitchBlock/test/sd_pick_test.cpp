// Host test of drivers/NativeSwitchBlock/SwitchSdPick.h: the SD worker's choice of chain, run in the card model
// (sd_card_model.h) against the rule it replaced (r6-lows: a better-class read passes only a head of another class, and
// only while the head is under 100 ms old).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "sd_card_model.h"

using namespace sdmodel;

namespace {

Result run(Rule rule, const std::vector<Arrival> &background, bool syncWaiting = false, int faults = 10)
{
    Card card(rule, background, faults, 2 * kMs, syncWaiting);
    const Result result = card.run();
    EXPECT_TRUE(result.ordered);
    EXPECT_TRUE(result.writesFirst);
    return result;
}

}  // namespace

// Dump 5: 73 APFS metadata writes at tier 0 (passive) behind which SpringBoard's reads queued. Under the old rule a
// tier-0 write at the head stops every read; now the reads go first, and the writes all still finish.
TEST(SdPick, MetadataWritesNoLongerHoldTheUi)
{
    const auto background = join(scattered(73, 5 * kMs, 8, 0, true, 1), scattered(30, 20 * kMs, 64, 2, false, 2));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 5: 73 tier-0 metadata writes", before, after);
    EXPECT_LT(after.uiTotalNs * 2, before.uiTotalNs);
    EXPECT_LT(after.uiWorstNs, 5 * kMs);
    EXPECT_LT(after.lowerWorstNs, 1000 * kMs);
}

// The same writes as IOSCHED marks them (metadata writes at tier 1, passive: spec_strategy). They now also pass the
// older tier-2 reads, which costs the UI a command or so at most.
TEST(SdPick, MetadataWritesAtTierOne)
{
    const auto background = join(scattered(73, 5 * kMs, 8, 1, true, 1), scattered(30, 20 * kMs, 64, 2, false, 2));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 5, IOSCHED tiers", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
    EXPECT_LT(after.uiWorstNs, 5 * kMs);
}

// Dump 4 once the pack warm reads at tier 3 (the state lane's fix). The old rule already let tier 0 pass it. What is
// left is a 512 KiB read prepared behind the UI's; the software throttle XNU keeps for this device holds tier-3 reads
// back while tier 0 is busy.
TEST(SdPick, SingleLargeBackgroundReadsAreNoWorse)
{
    const auto background = join(stream(16, 10 * kMs, 3), scattered(21, 50 * kMs, 64, 2, false, 3));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 4, pack warm at tier 3", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs);
}

// Dump 4 as it ran: the warm's 512 KiB reads at tier 0, the UI's own class. No class rule can separate them, which
// is why the warm's tier has to be fixed at its source; this rule must not make it worse.
TEST(SdPick, SameClassBulkReadsStayFirstComeFirstServed)
{
    const auto background = join(stream(16, 10 * kMs, 0), scattered(21, 50 * kMs, 64, 2, false, 3));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 4 as it ran, pack warm at tier 0", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
}

// Dump 6: 148 other processes' tier-0 page-ins and 75 tier-2 ones. The tier-2 part no longer delays the UI; the
// tier-0 part is the UI's own class and keeps its order.
TEST(SdPick, OtherTierZeroPageInsKeepTheirOrder)
{
    const auto background = join(scattered(148, 30 * kMs, 32, 0, false, 5), scattered(75, 30 * kMs, 64, 2, false, 6));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 6: 148 tier-0 + 75 tier-2 page-ins", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs + kMs);
}

// Tier-0 reads arriving faster than the card serves them (every 0.5 ms, 16 KiB each) never let the queue empty. A
// tier-3 read still finishes: once the head is kSwitchSdStarveNs old it goes first after kSwitchSdShareNs or
// kSwitchSdShareChains passes.
TEST(SdPick, BackgroundIsNeverStarved)
{
    std::vector<Arrival> background = scattered(20, kMs, 64, 3, false, 7);
    background.reserve(background.size() + 4000);
    for (int i = 0; i < 4000; ++i)
        background.push_back(
            {static_cast<uint64_t>(i) * 500 * kUs, 30000000 + static_cast<uint64_t>(i) * 977, 32, 0, false});
    const Result before = run(Rule::kOld, background, false, 0);
    const Result after = run(Rule::kNew, background, false, 0);
    std::printf("starvation: worst tier-3 read %.1f -> %.1f ms, worst tier-0 read %.1f -> %.1f ms\n",
                before.worstByClass[3] / 1e6, after.worstByClass[3] / 1e6, before.worstByClass[0] / 1e6,
                after.worstByClass[0] / 1e6);
    EXPECT_GT(after.worstByClass[3], 0u);
    EXPECT_LT(after.worstByClass[3], kSwitchSdStarveNs + 20 * kSwitchSdShareNs + 50 * kMs);
}

// A synchronize waiting for writes makes them rank with tier-0 reads: its writes are not passed, at tier 0 or at
// IOSCHED's tier 1.
TEST(SdPick, WritesASynchronizeWaitsForAreNotPassed)
{
    for (uint8_t klass : {uint8_t{0}, uint8_t{1}}) {
        const auto background = scattered(40, kMs, 8, klass, true, 8);
        const Result waiting = run(Rule::kNew, background, true);
        const Result free = run(Rule::kNew, background, false);
        std::printf("tier-%u writes with a synchronize waiting: worst write %.1f ms, without %.1f ms\n", klass,
                    waiting.lowerWorstNs / 1e6, free.lowerWorstNs / 1e6);
        EXPECT_LT(waiting.lowerWorstNs, free.lowerWorstNs);
    }
}

// A synchronize waits for writes queued behind older tier-2 reads while tier-0 reads keep arriving: the oldest write
// passes the reads, so the synchronize is not held to the one-in-eight share of a passed-over head.
TEST(SdPick, AwaitedWritesPassOlderLowerReads)
{
    std::vector<Arrival> background = scattered(60, kMs, 128, 2, false, 10);
    const auto writes = scattered(20, kMs, 8, 1, true, 11);
    background.insert(background.end(), writes.begin(), writes.end());
    for (Arrival &write : background) {
        if (write.write)
            write.at += kMs;
    }
    const Result before = run(Rule::kOld, background, true, 40);
    const Result after = run(Rule::kNew, background, true, 40);
    std::printf("awaited writes behind tier-2 reads: worst write %.1f -> %.1f ms, UI %.1f -> %.1f ms\n",
                before.worstByClass[1] / 1e6, after.worstByClass[1] / 1e6, before.uiTotalNs / 1e6,
                after.uiTotalNs / 1e6);
    EXPECT_LE(after.worstByClass[1], before.worstByClass[1]);
}

// APFS writing steadily (200 scattered 4 KiB writes a second, tier 1) beside tier-0 reads that keep the card ~75% busy
// on their own: the writes keep up and the queue stays far below the request pool (1024).
TEST(SdPick, SteadyWritesKeepUpUnderATierZeroFlood)
{
    std::vector<Arrival> background = scattered(400, 2000 * kMs, 8, 1, true, 9);
    background.reserve(background.size() + 2000);
    for (int i = 0; i < 2000; ++i)
        background.push_back({static_cast<uint64_t>(i) * kMs, 30000000 + static_cast<uint64_t>(i) * 977, 32, 0, false});
    const Result before = run(Rule::kOld, background, false, 0);
    const Result after = run(Rule::kNew, background, false, 0);
    std::printf("steady writes under a flood: worst write %.1f -> %.1f ms, worst tier-0 read %.1f -> %.1f ms, deepest "
                "queue %zu -> %zu\n",
                before.worstByClass[1] / 1e6, after.worstByClass[1] / 1e6, before.worstByClass[0] / 1e6,
                after.worstByClass[0] / 1e6, before.maxDepth, after.maxDepth);
    EXPECT_LT(after.maxDepth, 256u);
    EXPECT_LT(after.worstByClass[1], 1000 * kMs);
}

TEST(SdPick, RankAndPass)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 0, true, false, 0, 0, false, 0};   // tier-0 write, head
    q[1] = {&q[2], 100, 8, 0, false, false, 0, 1, false, 0};  // overlaps it: must wait
    q[2] = {&q[3], 300, 8, 0, false, false, 0, 2, false, 0};  // passes
    q[3] = {nullptr, 500, 8, 2, false, false, 0, 3, false, 0};
    SwitchSdPickState state = {};
    SwitchSdPicked<Request> picked = switchSdPick(q.data(), false, kMs, &state);
    EXPECT_EQ(picked.first, &q[2]);
    EXPECT_EQ(picked.prev, &q[1]);
    EXPECT_TRUE(picked.passed);
    picked = switchSdPick(q.data(), true, kMs, &state);
    EXPECT_EQ(picked.first, q.data());
    picked = switchSdPick(q.data(), false, kSwitchSdStarveNs, &state);
    EXPECT_EQ(picked.first, q.data());
    EXPECT_TRUE(picked.starved);
    picked = switchSdPick(q.data(), false, kSwitchSdStarveNs + 1, &state);
    EXPECT_EQ(picked.first, &q[2]);
}

// Writes and reads pass by the same rank: a tier-0 write goes ahead of an older tier-2 read even when no read could
// pass that head, but never past an older write or a request it overlaps.
TEST(SdPick, AWritePassesByRankAlone)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 2, false, false, 0, 0, false, 0};  // tier-2 read, head
    q[1] = {&q[2], 200, 8, 2, false, false, 0, 1, false, 0};
    q[2] = {&q[3], 300, 8, 0, true, false, 0, 2, false, 0};  // tier-0 write: passes both reads
    q[3] = {nullptr, 400, 8, 0, true, false, 0, 3, false, 0};
    SwitchSdPickState state = {};
    EXPECT_EQ(switchSdPick(q.data(), false, kMs, &state).first, &q[2]);
    q[2].lba = 104;  // now overlaps the head read: it waits, and the write behind it may not pass it
    EXPECT_EQ(switchSdPick(q.data(), false, kMs, &state).first, q.data());
}

TEST(SdPick, PromoteRaisesOnly)
{
    std::vector<Request> q(3);
    q[0] = {&q[1], 0, 64, 3, false, false, 0, 0, false, 0};
    q[1] = {&q[2], 64, 64, 1, true, false, 0, 1, false, 0};
    q[2] = {nullptr, 200, 8, 0, false, false, 0, 2, false, 0};
    uint32_t reads[4] = {1, 0, 0, 1};
    EXPECT_EQ(switchSdPromote(q.data(), 32, 300, 1, reads), 1u);
    EXPECT_EQ(q[0].klass, 1);
    EXPECT_EQ(q[1].klass, 1);
    EXPECT_EQ(q[2].klass, 0);
    EXPECT_EQ(reads[0], 1u);
    EXPECT_EQ(reads[1], 1u);
    EXPECT_EQ(reads[3], 0u);
    EXPECT_EQ(switchSdPromote(q.data(), 0, 1000, 0, reads), 2u);
    EXPECT_EQ(reads[0], 2u);
    EXPECT_EQ(reads[1], 0u);
}
