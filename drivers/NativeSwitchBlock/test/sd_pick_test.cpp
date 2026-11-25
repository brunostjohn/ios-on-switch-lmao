// Host test of drivers/NativeSwitchBlock/SwitchSdPick.h: the SD worker's choice of chain, run in a model of the card
// against the rule it replaced (r6-lows: a better-class read passes only a head of another class, and only while the
// head is under 100 ms old). The model's card takes ~0.58 ms a command plus ~10 us per KiB (r12-sdio's measured model)
// and 0.4 ms more for a write, and keeps two chains in its pipeline: the one on the card and the one prepared behind it,
// picked as the one before starts (NativeSwitchBlock.cpp worker()). A "fault" is SpringBoard's main thread taking a page
// fault: a 4 KiB APFS metadata read, then the 16 KiB data read, each issued 0.3 ms after the previous one completes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

#include "../SwitchSdPick.h"

namespace {

constexpr uint32_t kSector = 512;
constexpr uint32_t kMaxSectors = 1024 * 1024 / kSector;
constexpr uint32_t kMaxChain = 256;
constexpr uint64_t kUs = 1000, kMs = 1000 * kUs;
constexpr uint64_t kThinkNs = 300 * kUs;

struct Request {
    Request *next;
    uint64_t lba;
    uint32_t sectors;
    uint8_t klass;
    bool write;
    bool chained;
    uint64_t queuedNs;
    int id;
    bool ui;
    uint64_t doneNs;
};

enum class Rule {
    kOld,
    kNew
};

uint64_t serviceNs(uint32_t sectors, bool write)
{
    return 580 * kUs + static_cast<uint64_t>(sectors) * kSector / 1024 * 10 * kUs + (write ? 400 * kUs : 0);
}

// The rule pickLocked had before SwitchSdPick.h, as it was written there.
Request *oldFirst(Request *head, const uint32_t queuedReads[4], uint64_t now, Request **prev)
{
    *prev = nullptr;
    Request *first = head;
    if (first->next && first->klass && now - first->queuedNs < 100 * kMs) {
        unsigned better = 0;
        while (better < first->klass && !queuedReads[better])
            ++better;
        if (better < first->klass) {
            for (Request *before = first, *c = first->next; c; before = c, c = c->next) {
                if (c->write || c->klass != better)
                    continue;
                bool overlaps = false;
                for (const Request *older = head; older != c && !overlaps; older = older->next)
                    overlaps = switchOverlaps(older, c);
                if (overlaps)
                    continue;
                *prev = before;
                return c;
            }
        }
    }
    return first;
}

struct Arrival {
    uint64_t at;
    uint64_t lba;
    uint32_t sectors;
    uint8_t klass;
    bool write;
};

struct Result {
    uint64_t uiTotalNs = 0;     // first fault issued to last fault's data read done
    uint64_t uiWorstNs = 0;     // one UI request, queued to done
    uint64_t lowerWorstNs = 0;  // one request of the background, queued to done
    uint64_t worstByClass[4] = {};
    uint64_t endNs = 0;
    bool ordered = true;  // no read started while an older overlapping write was still queued, and the reverse
};

class Card {
public:
    Card(Rule rule, std::vector<Arrival> background, int faults, uint64_t faultsAt, bool syncWaiting = false)
        : rule(rule), pending(std::move(background)), faultsLeft(faults), syncWaiting(syncWaiting)
    {
        storage.reserve(pending.size() + 2 * static_cast<size_t>(faults) + 1);
        std::sort(pending.begin(), pending.end(), [](const Arrival &a, const Arrival &b) {
            return a.at < b.at;
        });
        if (faults)
            uiNext = faultsAt;
        uiStart = faultsAt;
    }

    Result run()
    {
        for (;;) {
            const uint64_t arrival = nextArrival();
            if (active.empty() && arrival == kNever)
                break;
            if (!active.empty() && activeEnd <= arrival) {
                finishActive();
                continue;
            }
            admit(arrival);
        }
        result.endNs = now;
        return result;
    }

private:
    static constexpr uint64_t kNever = ~0ULL;
    Rule rule;
    std::vector<Arrival> pending;
    size_t pendingAt = 0;
    int faultsLeft;
    bool uiMeta = true;
    uint64_t uiNext = kNever, uiStart = 0;
    bool syncWaiting;
    std::vector<Request> storage;
    Request *head = nullptr, *tail = nullptr;
    uint32_t queuedReads[4] = {};
    SwitchSdPickState state = {};
    std::vector<Request *> active, prepared;
    uint64_t activeEnd = 0, now = 0;
    std::mt19937_64 random{7};
    Result result;

    uint64_t nextArrival() const
    {
        const uint64_t background = pendingAt < pending.size() ? pending[pendingAt].at : kNever;
        return std::min(background, uiNext);
    }

    void enqueue(const Arrival &a, bool ui)
    {
        storage.push_back(
            {nullptr, a.lba, a.sectors, a.klass, a.write, false, now, static_cast<int>(storage.size()), ui, 0});
        Request *r = &storage.back();
        (tail ? tail->next : head) = r;
        tail = r;
        if (!r->write)
            ++queuedReads[r->klass];
    }

    void admit(uint64_t at)
    {
        now = at;
        if (uiNext == at) {
            const uint64_t lba = 40000000 + random() % 20000000;
            enqueue({at, lba, uiMeta ? 8u : 32u, 0, false}, true);
            uiNext = kNever;
        } else {
            enqueue(pending[pendingAt++], false);
        }
        if (active.empty()) {
            active = pick();
            activeEnd = now + chainNs(active);
        }
        if (prepared.empty())
            prepared = pick();
    }

    void finishActive()
    {
        now = activeEnd;
        const std::vector<Request *> done = active;
        active = prepared;
        prepared.clear();
        if (active.empty())
            active = pick();
        if (!active.empty())
            activeEnd = now + chainNs(active);
        prepared = pick();
        for (Request *r : done)
            complete(r);
    }

    void complete(Request *r)
    {
        r->doneNs = now;
        const uint64_t latency = now - r->queuedNs;
        if (!r->ui) {
            result.lowerWorstNs = std::max(result.lowerWorstNs, latency);
            result.worstByClass[r->klass] = std::max(result.worstByClass[r->klass], latency);
            return;
        }
        result.uiWorstNs = std::max(result.uiWorstNs, latency);
        if (uiMeta) {
            uiMeta = false;
            uiNext = now + kThinkNs;
        } else if (--faultsLeft > 0) {
            uiMeta = true;
            uiNext = now + kThinkNs;
        } else {
            result.uiTotalNs = now - uiStart;
        }
    }

    static uint64_t chainNs(const std::vector<Request *> &chain)
    {
        if (chain.empty())
            return 0;
        const uint64_t low = chain.front()->lba;
        const uint64_t high = chain.back()->lba + chain.back()->sectors;
        return serviceNs(static_cast<uint32_t>(high - low), chain.front()->write);
    }

    std::vector<Request *> pick()
    {
        if (!head)
            return {};
        Request *prev = nullptr;
        Request *first = head;
        if (rule == Rule::kOld) {
            first = oldFirst(head, queuedReads, now, &prev);
        } else {
            first = switchSdPick(head, queuedReads, syncWaiting, now, &state).first;
        }
        (void)prev;
        uint64_t start = 0;
        uint32_t sectors = 0;
        const uint32_t count =
            first->write ? switchGatherWrites(head, first, kMaxSectors, kMaxChain, kMaxChain, &start, &sectors)
                         : switchGatherReads(head, first, kMaxSectors, kMaxChain, kMaxChain, &start, &sectors);
        checkOrder();
        Request *chain = switchUnlinkChain(head, tail, count);
        std::vector<Request *> out;
        out.reserve(count);
        for (Request *r = chain; r; r = r->next) {
            if (!r->write)
                --queuedReads[r->klass];
            out.push_back(r);
        }
        return out;
    }

    // Every marked member: no older unmarked request still queued that it must not pass.
    void checkOrder()
    {
        for (const Request *r = head; r; r = r->next) {
            if (!r->chained)
                continue;
            for (const Request *older = head; older != r; older = older->next) {
                if (!older->chained && (older->write || r->write) && switchOverlaps(older, r))
                    result.ordered = false;
            }
        }
    }
};

std::vector<Arrival> scattered(int n, uint64_t spreadNs, uint32_t sectors, uint8_t klass, bool write, uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back({random() % (spreadNs + 1), random() % 30000000, sectors, klass, write});
    return out;
}

// One file read front to back in 512 KiB reads, one every `everyNs` (the GPU pack warm).
std::vector<Arrival> stream(int n, uint64_t everyNs, uint8_t klass)
{
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back(
            {static_cast<uint64_t>(i) * everyNs, 70000000 + static_cast<uint64_t>(i) * 1024, 1024, klass, false});
    return out;
}

std::vector<Arrival> join(std::vector<Arrival> a, const std::vector<Arrival> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void report(const char *name, const Result &before, const Result &after)
{
    std::printf("%-40s UI 10 faults %7.1f -> %7.1f ms, worst UI request %6.1f -> %6.1f ms, worst background %7.1f -> "
                "%7.1f ms\n",
                name, before.uiTotalNs / 1e6, after.uiTotalNs / 1e6, before.uiWorstNs / 1e6, after.uiWorstNs / 1e6,
                before.lowerWorstNs / 1e6, after.lowerWorstNs / 1e6);
}

Result run(Rule rule, const std::vector<Arrival> &background, bool syncWaiting = false, int faults = 10)
{
    Card card(rule, background, faults, 2 * kMs, syncWaiting);
    const Result result = card.run();
    EXPECT_TRUE(result.ordered);
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

// The same writes as IOSCHED marks them (metadata writes at tier 1, passive: spec_strategy).
TEST(SdPick, MetadataWritesAtTierOne)
{
    const auto background = join(scattered(73, 5 * kMs, 8, 1, true, 1), scattered(30, 20 * kMs, 64, 2, false, 2));
    const Result before = run(Rule::kOld, background);
    const Result after = run(Rule::kNew, background);
    report("dump 5, IOSCHED tiers", before, after);
    EXPECT_LE(after.uiTotalNs, before.uiTotalNs);
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
// tier-3 read still finishes: one passed-over chain every kSwitchSdShareNs once the head is kSwitchSdStarveNs old.
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

// A synchronize waiting for writes makes them rank as reads of their class: its writes are not passed.
TEST(SdPick, WritesASynchronizeWaitsForAreNotPassed)
{
    const auto background = scattered(40, kMs, 8, 0, true, 8);
    const Result waiting = run(Rule::kNew, background, true);
    const Result free = run(Rule::kNew, background, false);
    std::printf("writes with a synchronize waiting: worst write %.1f ms, without %.1f ms\n", waiting.lowerWorstNs / 1e6,
                free.lowerWorstNs / 1e6);
    EXPECT_LT(waiting.lowerWorstNs, free.lowerWorstNs);
}

TEST(SdPick, RankAndPass)
{
    std::vector<Request> q(4);
    q[0] = {&q[1], 100, 8, 0, true, false, 0, 0, false, 0};   // tier-0 write, head
    q[1] = {&q[2], 100, 8, 0, false, false, 0, 1, false, 0};  // overlaps it: must wait
    q[2] = {&q[3], 300, 8, 0, false, false, 0, 2, false, 0};  // passes
    q[3] = {nullptr, 500, 8, 2, false, false, 0, 3, false, 0};
    uint32_t reads[4] = {2, 0, 1, 0};
    SwitchSdPickState state = {};
    SwitchSdPicked<Request> picked = switchSdPick(q.data(), reads, false, kMs, &state);
    EXPECT_EQ(picked.first, &q[2]);
    EXPECT_EQ(picked.prev, &q[1]);
    EXPECT_TRUE(picked.passed);
    picked = switchSdPick(q.data(), reads, true, kMs, &state);
    EXPECT_EQ(picked.first, q.data());
    picked = switchSdPick(q.data(), reads, false, kSwitchSdStarveNs, &state);
    EXPECT_EQ(picked.first, q.data());
    EXPECT_TRUE(picked.starved);
    picked = switchSdPick(q.data(), reads, false, kSwitchSdStarveNs + 1, &state);
    EXPECT_EQ(picked.first, &q[2]);
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
