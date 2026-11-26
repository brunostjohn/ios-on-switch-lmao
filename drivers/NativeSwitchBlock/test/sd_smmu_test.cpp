// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchSdSmmu.h on the host: a chain holds exactly its request pages (not the driver's bounce runs or gap sink) and
// releases the same; two chains sharing a page each hold it; a hold the view refuses holds nothing.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

typedef uint64_t IOByteCount;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "../SwitchSdSmmu.h"

namespace {
struct View {
    std::map<uint64_t, int> pages;
    std::vector<std::pair<uint64_t, uint64_t>> held;
    unsigned commits = 0, locks = 0;
    uint64_t refuse = ~0ULL;

    template <class Body> void locked(Body &&body)
    {
        ++locks;
        body(*this);
    }

    bool hold(uint64_t pa, uint64_t bytes)
    {
        if (pa == refuse)
            return false;
        held.emplace_back(pa, bytes);
        for (uint64_t page = pa & ~4095ULL; page < pa + bytes; page += 4096)
            ++pages[page];
        return true;
    }

    bool release(uint64_t pa, uint64_t bytes)
    {
        for (uint64_t page = pa & ~4095ULL; page < pa + bytes; page += 4096) {
            if (pages.count(page) == 0)
                return false;
            if (--pages[page] == 0)
                pages.erase(page);
        }
        return true;
    }

    void commit()
    {
        ++commits;
    }
};

constexpr uint64_t kBounce = 0xf0000000ULL, kSink = 0xf0200000ULL;
const SwitchSdOwnedRange kOwned[] = {{kBounce, 1u << 20}, {kSink, 32u << 10}};

struct Table {
    SwitchAdmaDescriptor entries[16] = {};
    SwitchAdmaTable table = {entries, 16, 1ULL << 34, 0, 0, 0, 0};

    Table &add(uint64_t pa, uint64_t length)
    {
        EXPECT_EQ(table.addChecked(pa, length), kAdmaOk);
        return *this;
    }

    Table &sealed(uint64_t pa, uint64_t length)
    {
        EXPECT_EQ(table.addSealed(pa, length), kAdmaOk);
        return *this;
    }
};
}  // namespace

TEST(SdSmmu, AChainHoldsItsRequestPagesButNotTheDriversBuffersAndReleasesTheSame)
{
    View view;
    Table t;
    t.add(0x180000000ULL - 0x2000, 0x2000)
        .sealed(kBounce + 0x4000, 0x1000)
        .add(0x91234040ULL, 0x40)
        .sealed(kSink, 0x8000);
    ASSERT_TRUE(switchSdHoldChain(view, t.table, kOwned, 2));
    EXPECT_EQ(view.held, (std::vector<std::pair<uint64_t, uint64_t>>{{0x17fffe000ULL, 0x2000}, {0x91234040ULL, 0x40}}));
    EXPECT_EQ(view.commits, 1u) << "one commit for the chain";
    EXPECT_EQ(view.locks, 1u) << "its holds and commit under one lock";
    EXPECT_EQ(view.pages.count(kBounce + 0x4000), 0u);
    EXPECT_TRUE(switchSdReleaseChain(view, t.table, kOwned, 2));
    EXPECT_TRUE(view.pages.empty());
    EXPECT_EQ(view.commits, 2u);
    EXPECT_EQ(view.locks, 2u);
}

TEST(SdSmmu, TwoChainsSharingAPageEachHoldItAndTheFirstToFinishLeavesItHeld)
{
    View view;
    Table a;
    Table b;
    a.add(0x90000000ULL, 0x40);
    b.add(0x90000040ULL, 0x40);
    ASSERT_TRUE(switchSdHoldChain(view, a.table, kOwned, 2));
    ASSERT_TRUE(switchSdHoldChain(view, b.table, kOwned, 2));
    EXPECT_EQ(view.pages[0x90000000ULL], 2);
    EXPECT_TRUE(switchSdReleaseChain(view, a.table, kOwned, 2));
    EXPECT_EQ(view.pages[0x90000000ULL], 1) << "the second chain's command may still be reading it";
    EXPECT_TRUE(switchSdReleaseChain(view, b.table, kOwned, 2));
    EXPECT_TRUE(view.pages.empty());
    EXPECT_FALSE(switchSdReleaseChain(view, b.table, kOwned, 2)) << "a second release has no hold to end";
}

TEST(SdSmmu, ARefusedHoldReleasesWhatTheChainHadHeld)
{
    View view;
    Table t;
    t.add(0x90000000ULL, 0x1000).add(0x95000000ULL, 0x1000).add(0x96000000ULL, 0x1000);
    view.refuse = 0x96000000ULL;
    EXPECT_FALSE(switchSdHoldChain(view, t.table, kOwned, 2));
    EXPECT_TRUE(view.pages.empty());
    EXPECT_EQ(view.commits, 1u) << "the releases are published too";
}
