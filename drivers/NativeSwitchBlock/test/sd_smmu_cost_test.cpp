// SPDX-License-Identifier: BSD-2-Clause-Patent
// What confining SDMMC1 costs the CPU per command: SwitchSdSmmu.h's hold and release of a chain over the RAM view
// (TegraSMMUIdentity.h), with the MC's flush registers modelled by SwitchSmmuRegisters.h's own sequences. A boot's page-in
// storm is mostly 4 KiB reads of scattered pages, two in flight; the rest is longer reads of a few runs. Per command:
// the thread's instructions (thread_selfcounts, the fewest of several rounds: preemption only adds), the view's lock
// taken (an os_unfair_lock stands in for its IOLock: both take an uncontended lock with one compare-and-swap), and the
// register writes, register reads (each a round trip to the MC on the device) and cache-clean calls the commits make.
#include <gtest/gtest.h>
#include <os/lock.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

typedef uint64_t IOByteCount;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "drivers/NativeSwitchBlock/SwitchSdSmmu.h"
#include "drivers/TegraSMMU/SwitchSmmuRegisters.h"
#include "drivers/TegraSMMU/TegraSMMUIdentity.h"

extern "C" int thread_selfcounts(int type, void *buf, size_t nbytes);

namespace {
struct Registers {
    uint64_t writes = 0, reads = 0;

    uint32_t read(uint32_t)
    {
        ++reads;
        return 0;
    }

    void write(uint32_t, uint32_t)
    {
        ++writes;
    }
};

struct Env {
    Registers io;
    uint64_t cleans = 0;
    std::vector<std::unique_ptr<uint32_t[]>> pages;
    std::vector<std::unique_ptr<uint16_t[]>> users;
    std::vector<std::unique_ptr<TegraSMMUIdentityTable>> tables;
    uint64_t nextPa = 0x90000000ULL;

    TegraSMMUIdentityTable *makeTable()
    {
        pages.emplace_back(new uint32_t[1024]());
        users.emplace_back(new uint16_t[1024]());
        tables.emplace_back(new TegraSMMUIdentityTable{});
        *tables.back() = {pages.back().get(), nextPa, 0, users.back().get(), 0, 0, nullptr, 0, 0, 0, {}};
        nextPa += 4096;
        return tables.back().get();
    }

    void clean(const void *, size_t)
    {
        ++cleans;
    }

    void sync()
    {}

    void flushAsid(uint32_t asid)
    {
        switch_smmu::flush(io, asid);
    }

    void flush(uint32_t asid, const switch_smmu_table_flush *tables, uint32_t count)
    {
        switch_smmu::flushTables(io, asid, tables, count);
    }
};

// TegraSMMURamView's: a batch under one lock.
struct View {
    TegraSMMUIdentityCore<Env> *core;
    bool commits = true;
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    uint64_t locks = 0;

    bool hold(uint64_t pa, uint64_t bytes)
    {
        return core->hold(pa, bytes);
    }

    bool release(uint64_t pa, uint64_t bytes)
    {
        return core->release(pa, bytes);
    }

    void commit()
    {
        if (commits)
            core->commit();
    }

    template <class Body> void locked(Body &&body)
    {
        os_unfair_lock_lock(&lock);
        ++locks;
        body(*this);
        os_unfair_lock_unlock(&lock);
    }
};

uint64_t instructions()
{
    uint64_t words[2] = {};
    return thread_selfcounts(1, words, sizeof words) == 0 ? words[0] : 0;
}

struct Chain {
    std::vector<SwitchAdmaDescriptor> entries;
    SwitchAdmaTable table;
};

// A command's table: `runs` physically contiguous runs of `pages` 4 KiB pages each, at pages spread over RAM.
Chain chain(std::mt19937_64 &random, uint32_t runs, uint32_t pages)
{
    Chain c;
    c.entries.resize(runs * (pages * 4096 / 0xf000 + 1));
    c.table = {c.entries.data(), static_cast<uint32_t>(c.entries.size()), 1ULL << 34, 0, 0, 0, 0};
    for (uint32_t r = 0; r < runs; ++r) {
        const uint64_t page = 0x80000000ULL + (random() % (0x100000000ULL / 4096 - pages)) * 4096;
        EXPECT_EQ(c.table.addChecked(page, uint64_t(pages) * 4096), kAdmaOk);
    }
    return c;
}

struct Cost {
    double instructions, writes, reads, cleans, locks, uncommitted;
};

// `commands` chains, two in flight as the driver keeps them: each held before its command, released after it.
Cost measure(uint32_t runs, uint32_t pages, uint32_t commands)
{
    std::mt19937_64 random(7);
    std::vector<Chain> chains;
    chains.reserve(commands);
    for (uint32_t i = 0; i < commands; ++i)
        chains.push_back(chain(random, runs, pages));
    double fewest = 1e300;
    Cost cost = {};
    for (int round = 0; round < 5; ++round) {
        Env env;
        uint32_t low[1024] = {}, high[1024] = {};
        TegraSMMUIdentityCore<Env> core;
        EXPECT_TRUE(core.init(&env, 113, low, 0x88000000ULL, 112, high, 0x88001000ULL));
        EXPECT_TRUE(core.makeTables());
        core.commit();
        env.io = {};
        env.cleans = 0;
        View view{&core};
        const SwitchSdOwnedRange owned[1] = {{0xf0000000ULL, 1u << 20}};
        const auto storm = [&] {
            EXPECT_TRUE(switchSdHoldChain(view, chains[0].table, owned, 1));
            for (uint32_t i = 1; i < commands; ++i) {
                EXPECT_TRUE(switchSdHoldChain(view, chains[i].table, owned, 1));
                EXPECT_TRUE(switchSdReleaseChain(view, chains[i - 1].table, owned, 1));
            }
            EXPECT_TRUE(switchSdReleaseChain(view, chains[commands - 1].table, owned, 1));
        };
        storm();  // the tables' first touches (page faults the thread's count would include) out of the measurement
        env.io = {};
        env.cleans = 0;
        view.locks = 0;
        const uint64_t before = instructions();
        storm();
        const double spent = double(instructions() - before);
        const double locks = double(view.locks) / commands;
        view.commits = false;
        const uint64_t bare = instructions();
        storm();
        const double holds = double(instructions() - bare);
        if (spent < fewest) {
            fewest = spent;
            cost = {spent / commands, double(env.io.writes) / commands, double(env.io.reads) / commands,
                    double(env.cleans) / commands, locks, holds / commands};
        }
    }
    return cost;
}

void print(const char *name, const Cost &c)
{
    printf("SDSMMU %s: %.0f instructions (%.0f of them outside the commits), %.2f locks, %.2f MC writes, %.2f MC "
           "reads, %.2f cleans a command\n",
           name, c.instructions, c.uncommitted, c.locks, c.writes, c.reads, c.cleans);
}
}  // namespace

TEST(SdSmmuCost, APageInStormOfScatteredSmallReads)
{
    const Cost c = measure(1, 1, 20000);
    print("4 KiB scattered", c);
    EXPECT_GT(c.writes, 0.0);
}

TEST(SdSmmuCost, LongerReadsOfAFewRuns)
{
    const Cost c = measure(4, 32, 2000);
    print("4 x 128 KiB", c);
    EXPECT_GT(c.writes, 0.0);
}
