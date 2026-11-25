// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSMMUIdentity.h on the host: RAM at its own addresses through two directories split at 4 GiB, a page translating
// only while held, and each commit's cleans and flushes.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../TegraSMMUIdentity.h"

namespace {
struct ModelEnv {
    std::vector<std::unique_ptr<uint32_t[]>> pages;
    std::vector<std::unique_ptr<uint16_t[]>> users;
    std::vector<std::unique_ptr<TegraSMMUIdentityTable>> tables;
    std::vector<std::string> events;
    uint64_t nextPa = 0x90000000ULL;
    bool outOfMemory = false;

    TegraSMMUIdentityTable *makeTable()
    {
        if (outOfMemory)
            return nullptr;
        pages.emplace_back(new uint32_t[1024]());
        users.emplace_back(new uint16_t[1024]());
        tables.emplace_back(
            new TegraSMMUIdentityTable{pages.back().get(), nextPa, 0, users.back().get(), 0, 0, nullptr});
        nextPa += 4096;
        events.emplace_back("table");
        return tables.back().get();
    }

    void clean(const void *, size_t bytes)
    {
        events.emplace_back("clean " + std::to_string(bytes));
    }

    void sync()
    {
        events.emplace_back("sync");
    }

    void flushAsid(uint32_t asid)
    {
        events.emplace_back("flush asid " + std::to_string(asid));
    }

    void flushEntries(uint32_t asid, uint64_t pa, uint64_t bytes, uint64_t iova, uint64_t iovaBytes)
    {
        char line[160];
        snprintf(line, sizeof(line), "flush asid %u table %#llx+%llu iova %#llx+%#llx", asid,
                 static_cast<unsigned long long>(pa), static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(iova), static_cast<unsigned long long>(iovaBytes));
        events.emplace_back(line);
    }
};

class SmmuIdentity : public testing::Test {
protected:
    ModelEnv env;
    uint32_t low[1024] = {}, high[1024] = {};
    TegraSMMUIdentityCore<ModelEnv> core;

    void SetUp() override
    {
        ASSERT_TRUE(core.init(&env, 113, low, 0x88000000ULL, 112, high, 0x88001000ULL));
    }

    std::vector<std::string> take()
    {
        std::vector<std::string> events;
        events.swap(env.events);
        return events;
    }
};

TEST_F(SmmuIdentity, AHeldPageTranslatesToItselfOnlyWhileHeld)
{
    EXPECT_EQ(core.translate(0x91234000ULL), UINT64_MAX) << "nothing is mapped until held";
    ASSERT_TRUE(core.hold(0x91234040ULL, 0x40));
    core.commit();
    EXPECT_EQ(core.translate(0x91234abcULL), 0x91234abcULL);
    EXPECT_EQ(core.translate(0x91235000ULL), UINT64_MAX) << "only the page holding the bytes";
    ASSERT_TRUE(core.release(0x91234040ULL, 0x40));
    core.commit();
    EXPECT_EQ(core.translate(0x91234abcULL), UINT64_MAX);
}

TEST_F(SmmuIdentity, RamAbove4GiBGoesThroughTheHighDirectoryLess4GiB)
{
    ASSERT_TRUE(core.hold(0x123456000ULL, 0x2000));
    core.commit();
    EXPECT_EQ(core.translate(0x123457010ULL), 0x123457010ULL);
    EXPECT_EQ(high[(0x23456000ULL >> 22) & 1023] & TegraSMMUDomain::kNext, TegraSMMUDomain::kNext);
    EXPECT_EQ(low[(0x23456000ULL >> 22) & 1023], 0u) << "the low directory's same index stays empty";
    EXPECT_EQ(core.ptb(1), (0x88001000u >> 12) | (7u << 29));
}

TEST_F(SmmuIdentity, APageTwoHoldersShareStaysMappedUntilTheLastLetsGo)
{
    ASSERT_TRUE(core.hold(0xa0000000ULL, 0x1000));
    ASSERT_TRUE(core.hold(0xa0000800ULL, 0x40));
    core.commit();
    EXPECT_EQ(core.holders(0xa0000000ULL), 2u);
    ASSERT_TRUE(core.release(0xa0000000ULL, 0x1000));
    core.commit();
    EXPECT_EQ(core.translate(0xa0000800ULL), 0xa0000800ULL);
    ASSERT_TRUE(core.release(0xa0000800ULL, 0x40));
    core.commit();
    EXPECT_EQ(core.translate(0xa0000800ULL), UINT64_MAX);
}

TEST_F(SmmuIdentity, AHoldReachingPastRamHoldsNothing)
{
    EXPECT_FALSE(core.hold(0x17ffff000ULL, 0x2000));
    EXPECT_FALSE(core.hold(0x7ffff000ULL, 0x2000));
    EXPECT_FALSE(core.hold(0x90000000ULL, 0));
    EXPECT_EQ(core.holders(0x17ffff000ULL), 0u);
    core.commit();
    for (const std::string &event : take())
        EXPECT_EQ(event.rfind("flush", 0), std::string::npos) << event;
}

TEST_F(SmmuIdentity, NoTableMeansNoHold)
{
    env.outOfMemory = true;
    EXPECT_FALSE(core.hold(0x90000000ULL, 0x1000));
    env.outOfMemory = false;
    EXPECT_EQ(core.holders(0x90000000ULL), 0u);
}

TEST_F(SmmuIdentity, AReleaseWithoutAHoldChangesNothing)
{
    ASSERT_TRUE(core.hold(0x90000000ULL, 0x1000));
    EXPECT_FALSE(core.release(0x90000000ULL, 0x2000)) << "the second page has no holder";
    EXPECT_EQ(core.holders(0x90000000ULL), 1u);
}

TEST_F(SmmuIdentity, APageAtItsHolderLimitTakesNoMore)
{
    for (uint32_t i = 0; i < UINT16_MAX; ++i)
        ASSERT_TRUE(core.hold(0x90000000ULL, 0x1000));
    EXPECT_FALSE(core.hold(0x8ffff000ULL, 0x2000)) << "all or nothing: the first page is not held either";
    EXPECT_EQ(core.holders(0x8ffff000ULL), 0u);
}

TEST_F(SmmuIdentity, ANewTableFlushesItsAsidWholeAndAPageChangeOnlyItsEntriesAndSection)
{
    ASSERT_TRUE(core.hold(0x91000000ULL, 0x3000));
    core.commit();
    EXPECT_EQ(take(), (std::vector<std::string>{"table", "clean 4096", "clean 12", "sync", "flush asid 113"}))
        << "the directory gained a table: clean it and the entries, then flush the ASID";
    ASSERT_TRUE(core.hold(0x91010000ULL, 0x1000));
    ASSERT_TRUE(core.release(0x91001000ULL, 0x1000));
    core.commit();
    EXPECT_EQ(take(), (std::vector<std::string>{"clean 64", "sync",
                                                "flush asid 113 table 0x90000004+64 iova 0x91001000+0x10000"}));
    ASSERT_TRUE(core.hold(0x91000000ULL, 0x1000));
    core.commit();
    EXPECT_EQ(take(), std::vector<std::string>{"sync"}) << "a second holder writes nothing, flushes nothing";
}

TEST_F(SmmuIdentity, TheHighHalfFlushesItsOwnAsidAndIovas)
{
    ASSERT_TRUE(core.hold(0x100400000ULL, 0x1000));
    core.commit();
    take();
    ASSERT_TRUE(core.release(0x100400000ULL, 0x1000));
    core.commit();
    EXPECT_EQ(take(),
              (std::vector<std::string>{"clean 4", "sync", "flush asid 112 table 0x90000000+4 iova 0x400000+0x1000"}));
}

// The SD card's view: every table made at setup, so a hold on the I/O path never allocates.
TEST_F(SmmuIdentity, MadeTablesLetEveryHoldInRamGoWithoutAnAllocation)
{
    ASSERT_TRUE(core.makeTables());
    core.commit();
    take();
    env.outOfMemory = true;
    EXPECT_TRUE(core.hold(0x80000000ULL, 0x1000));
    EXPECT_TRUE(core.hold(0x17ffff000ULL, 0x1000));
    core.commit();
    for (const std::string &event : take())
        EXPECT_NE(event, "table");
    EXPECT_EQ(env.tables.size(), 1024u) << "4 GiB of RAM in 4 MiB tables";
}
TEST_F(SmmuIdentity, TablesMadeBelow4GiBLeaveTheHighHalfToBeMadeOnDemand)
{
    ASSERT_TRUE(core.makeTables(1ULL << 32));
    EXPECT_EQ(env.tables.size(), 512u) << "0x8000_0000-0xffff_ffff in 4 MiB tables";
    env.outOfMemory = true;
    EXPECT_TRUE(core.hold(0xfffff000ULL, 0x1000));
    EXPECT_FALSE(core.hold(0x100000000ULL, 0x1000)) << "above 4 GiB a table is still made on demand";
}
}  // namespace
