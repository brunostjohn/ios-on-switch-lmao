<<<<<<< HEAD
// SPDX-License-Identifier: BSD-2-Clause-Patent
// An SD card for switch_sd_files_select: its sectors by LBA, the boot loader's reference page for it (the same-boot
// copy of sector 0 and the card's descriptor), and FAT32/exFAT boot sectors.
#pragma once
#include <array>
#include <cstring>
#include <map>
#include "../SwitchSdVolumeInternal.h"

namespace switch_sd_test {
using Sector = std::array<uint8_t, 512>;
constexpr uint64_t capacity = UINT64_C(1) << 32;
constexpr uint32_t fatFirst = 65536, fatSize = 524288, rootFirst = 2048, rootSize = 16384;
constexpr std::array<uint8_t, 16> basicType = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                               0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
constexpr std::array<uint8_t, 16> efiType = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                             0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
constexpr std::array<uint8_t, 16> apfsType = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                                              0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};

inline void put(uint8_t *p, uint64_t n, unsigned bytes = 4)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

inline void mbrEntry(uint8_t *p, uint8_t type, uint32_t first, uint32_t length)
{
    p[4] = type;
    put(p + 8, first);
    put(p + 12, length);
}

inline void writeFat32Vbr(Sector &sector, uint32_t length, uint8_t cluster = 4)
{
    sector.fill(0);
    auto *b = sector.data();
    const uint32_t fat = (length / cluster + 129) / 128;
    put(b + 11, 512, 2);
    b[13] = cluster;
    put(b + 14, 32, 2);
    b[16] = 2;
    put(b + 32, length);
    put(b + 36, fat > 1024 ? fat : 1024);
    put(b + 44, 2);
    put(b + 510, 0xaa55, 2);
}

inline void writeExfatVbr(Sector &sector, uint32_t length, unsigned shift = 9, unsigned clusterShift = 2)
{
    sector.fill(0);
    auto *b = sector.data();
    memcpy(b + 3, "EXFAT   ", 8);
    const uint32_t logical = length >> (shift - 9);
    const uint32_t entriesPerSector = 1u << (shift - 2);
    const uint32_t minFat = ((logical >> clusterShift) + 2 + entriesPerSector - 1) / entriesPerSector;
    const uint32_t fat = minFat > 1024 ? minFat : 1024;
    const uint32_t heap = 24 + fat > 2048 ? 24 + fat : 2048;
    put(b + 72, logical, 8);
    put(b + 80, 24);
    put(b + 84, fat);
    put(b + 88, heap);
    put(b + 92, (logical - heap) >> clusterShift);
    put(b + 96, 2);
    put(b + 104, 0x100, 2);
    b[108] = static_cast<uint8_t>(shift);
    b[109] = static_cast<uint8_t>(clusterShift);
    b[110] = 1;
    put(b + 510, 0xaa55, 2);
}

class Card {
public:
    std::array<uint8_t, SWITCH_SD_REFERENCE_BYTES> page{};
    std::map<uint64_t, Sector> sectors;
    switch_gpt_workspace work{};
    switch_sd_files_slice slice{};
    uint64_t cardCapacity = capacity;
    uint32_t entryCount = 2;
    uint32_t reads = 0;

    Card()
    {
        auto &mbr = sectors[0];
        mbrEntry(mbr.data() + 446, 0x0c, fatFirst, fatSize);
        mbrEntry(mbr.data() + 462, 0xee, 1, UINT32_MAX);
        put(mbr.data() + 510, 0xaa55, 2);
        setEntry(0, basicType, 2, fatFirst, fatSize, "hos_data");
        setEntry(1, apfsType, 3, rootFirst, rootSize, "iOS");
        syncTables();

        constexpr std::array<uint32_t, 10> header = {
            SWITCH_SD_REFERENCE_MAGIC, 2, 0x500, 512, 512, 0x700b0000, 0, 1, 1, 0};
        for (unsigned i = 0; i < header.size(); ++i)
            put(page.data() + 4 * i, header[i]);
        put(page.data() + 0x2c, 1);
        auto *r = page.data() + SWITCH_SD_CARD_OFFSET;
        constexpr std::array<uint32_t, 8> fixed = {SWITCH_SD_CARD_MAGIC, 1, 256, 1, 0x700b0000, 1, 512, 1};
        for (unsigned i = 0; i < fixed.size(); ++i)
            put(r + 4 * i, fixed[i]);
        put(r + 0x20, cardCapacity, 8);
        put(r + 0x28, cardCapacity * 512, 8);
        put(r + 0x30, 0xc0000000);
        put(r + 0x34, 1);
        put(r + 0x38, 0x80000000);
        put(r + 0x40, 1u << 30);
        put(r + 0x44, (9u << 16) | 0x3f);
        put(r + 0x48, 0xffff0000);
        put(r + 0x50, 0x1b534d31);
        put(r + 0x60, 0xf0100000, 8);
        put(r + 0x68, 0xf0100100, 8);
        put(r + 0x70, 0xf0100200, 8);
        constexpr std::array<uint8_t, 32> firmware = {0xb4, 0x92, 0xa4, 0x01, 0x75, 0x80, 0xca, 0x11, 0xc9, 0xcd, 0x49,
                                                      0x07, 0x5a, 0x89, 0x9a, 0x65, 0xf6, 0x20, 0x69, 0xd3, 0x75, 0x06,
                                                      0x15, 0x21, 0x7f, 0xc1, 0x03, 0x08, 0x95, 0x4c, 0x9f, 0xaa};
        memcpy(r + 0x80, firmware.data(), firmware.size());
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
        handoffMbr();
        writeFat32Vbr(sectors[fatFirst], fatSize);
    }

    void setCapacity(uint64_t cap)
    {
        cardCapacity = cap;
        const uint32_t csize = static_cast<uint32_t>(cap / 1024 - 1);
        auto *r = page.data() + SWITCH_SD_CARD_OFFSET;
        put(r + 0x20, cardCapacity, 8);
        put(r + 0x28, cardCapacity * 512, 8);
        put(r + 0x44, (9u << 16) | (csize >> 16));
        put(r + 0x48, (csize & 0xffffu) << 16);
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
    }

    void clearEntries(uint32_t count)
    {
        entryCount = count;
        const uint32_t entrySectors = (entryCount * 128 + 511) / 512;
        for (uint32_t s = 0; s < entrySectors; ++s)
            sectors[2 + s].fill(0);
    }

    void setEntry(uint32_t slot, const std::array<uint8_t, 16> &type, uint8_t guidTag, uint64_t first, uint64_t length,
                  const char *name, uint64_t attrs = 0)
    {
        if (slot >= entryCount)
            entryCount = slot + 1;
        uint8_t *e = sectors[2 + slot / 4].data() + (slot % 4) * 128;
        memset(e, 0, 128);
        memcpy(e, type.data(), 16);
        e[16] = guidTag;
        put(e + 32, first, 8);
        put(e + 40, first + length - 1, 8);
        put(e + 48, attrs, 8);
        for (unsigned i = 0; name[i] && i < 36; ++i)
            e[56 + i * 2] = static_cast<uint8_t>(name[i]);
    }

    void setProtectiveMbr(uint32_t count)
    {
        sectors[0].fill(0);
        mbrEntry(sectors[0].data() + 446, 0xee, 1, count);
        put(sectors[0].data() + 510, 0xaa55, 2);
        handoffMbr();
    }

    void syncTables()
    {
        const uint32_t bytes = entryCount * 128;
        const uint32_t entrySectors = (bytes + 511) / 512;
        std::array<uint8_t, 128 * 128> raw{};
        for (uint32_t s = 0; s < entrySectors; ++s) {
            const auto &sec = sectors[2 + s];
            sectors[cardCapacity - 33 + s] = sec;
            memcpy(raw.data() + s * 512, sec.data(), 512);
        }
        const uint32_t arrayCrc = switch_sd_crc32(raw.data(), bytes);
        for (bool backup : {false, true}) {
            auto &header = sectors[backup ? cardCapacity - 1 : 1];
            header.fill(0);
            auto *h = header.data();
            memcpy(h, "EFI PART", 8);
            put(h + 8, 0x10000);
            put(h + 12, 92);
            put(h + 24, backup ? cardCapacity - 1 : 1, 8);
            put(h + 32, backup ? 1 : cardCapacity - 1, 8);
            put(h + 40, 34, 8);
            put(h + 48, cardCapacity - 34, 8);
            h[56] = 1;
            put(h + 72, backup ? cardCapacity - 33 : 2, 8);
            put(h + 80, entryCount);
            put(h + 84, 128);
            put(h + 88, arrayCrc);
            put(h + 16, switch_sd_crc32(h, 92));
        }
    }

    void handoffMbr()
    {
        auto &mbr = sectors[0];
        memcpy(page.data() + 512, mbr.data(), 512);
        uint32_t c = switch_sd_crc32(mbr.data(), 512);
        for (unsigned i = 0; i < 4; ++i)
            page[0x28 + i] = static_cast<uint8_t>(c >> (24 - 8 * i));
    }

    void exfat(uint8_t type, unsigned shift = 9)
    {
        sectors[0][450] = type;
        handoffMbr();
        writeExfatVbr(sectors[fatFirst], fatSize, shift);
    }

    static int read(void *context, uint64_t lba, uint8_t out[512])
    {
        auto &card = *static_cast<Card *>(context);
        ++card.reads;
        auto it = card.sectors.find(lba);
        if (it == card.sectors.end())
            return -1;
        memcpy(out, it->second.data(), 512);
        return 0;
    }

    int select(uint64_t root = rootFirst)
    {
        return switch_sd_files_select(read, this, page.data(), root, rootSize, &work, &slice);
    }
};
}  // namespace switch_sd_test
||||||| 64a91a41a
=======
// A model of the SD card under the worker's pick (NativeSwitchBlock.cpp pickLocked), for sd_pick_test and
// sd_fence_test. The card takes ~0.58 ms a command plus ~10 us per KiB (r12-sdio's measured model) and 0.4 ms more for a
// write, and keeps two chains in its pipeline: the one on the card and the one prepared behind it, picked as the one
// before starts (worker()). Chains complete in pick order. A "fault" is SpringBoard's main thread taking a page fault: a
// 4 KiB APFS metadata read, then the 16 KiB data read, each issued 0.3 ms after the previous one completes.
//
// Synchronizes are modelled as SwitchSdWriteFence does them: a barrier of one window begins at a set time with the
// window's submitted writes as its target, waits while the window has completed fewer, and must not end before every
// write submitted ahead of it has completed.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "../SwitchSdPick.h"

namespace sdmodel {

constexpr uint32_t kSector = 512;
constexpr uint32_t kMaxSectors = 1024 * 1024 / kSector;
constexpr uint32_t kMaxChain = 256;
constexpr uint64_t kUs = 1000, kMs = 1000 * kUs;
constexpr uint64_t kThinkNs = 300 * kUs;
// switchSdPick's awaitedBeforeNs: no synchronize waiting, or one that began after every write.
constexpr uint64_t kNoSync[2] = {0, 0};
constexpr uint64_t kSyncAfterAll[2] = {~0ULL, ~0ULL};

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
    bool shared = false;  // the files window's, else the root window's
    bool done = false;
    bool replay = false;  // the kernel's boot replay read it (switchSdClassOf, switchSdAgeReplay)
};

enum class Rule {
    kOld,
    kNew
};

inline uint64_t serviceNs(uint32_t sectors, bool write)
{
    return 580 * kUs + static_cast<uint64_t>(sectors) * kSector / 1024 * 10 * kUs + (write ? 400 * kUs : 0);
}

// The rule pickLocked had before SwitchSdPick.h, as it was written there.
inline Request *oldFirst(Request *head, const uint32_t queuedReads[4], uint64_t now, Request **prev)
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
    bool shared = false;
    bool replay = false;
};

// A synchronize of one window, beginning at `at`.
struct Sync {
    uint64_t at;
    bool shared;
};

struct Result {
    uint64_t uiTotalNs = 0;     // first fault issued to last fault's data read done
    uint64_t uiWorstNs = 0;     // one UI request, queued to done
    uint64_t lowerWorstNs = 0;  // one request of the background, queued to done
    uint64_t worstByClass[4] = {};
    uint64_t replayWorstNs = 0;  // one boot replay read, queued to done
    uint64_t endNs = 0;
    size_t maxDepth = 0;
    bool ordered = true;      // no read started while an older overlapping write was still queued, and the reverse
    bool writesFirst = true;  // no write chain started while an older write of either window was still queued
    bool fenced = true;       // no synchronize ended before a write submitted ahead of it completed
    bool intact = true;       // after every pick the queue runs head to tail, depth requests, none of them chained,
                              // and queuedReplay counts its ageable replay reads
    unsigned syncsEnded = 0;
    unsigned outOfOrderWrites = 0;  // write chains that joined a write past an older one of its window still queued
    unsigned unfitChains = 0;       // gathered chains that did not fit, after which merges stay behind the first
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

    // Synchronizes at set times (each window's fence modelled), in place of a synchronize waiting the whole run.
    void syncAt(std::vector<Sync> syncs)
    {
        std::sort(syncs.begin(), syncs.end(), [](const Sync &a, const Sync &b) {
            return a.at < b.at;
        });
        planned = std::move(syncs);
        fencing = true;
    }

    // A synchronize's waiter leaves the fence (SwitchSdWriteFence::end) this long after its count is reached: the time
    // the woken thread takes to run.
    void wakeAfter(uint64_t ns)
    {
        wakeNs = ns;
    }

    // The rule before the synchronize's start time counted: while any synchronize waits, every write ranks with tier 0.
    void awaitEveryWrite()
    {
        awaitAll = true;
    }

    // pickLocked after a chain did not fit: requests merge only with the requests right behind them.
    void mergeOnlyBehind()
    {
        anywhere = false;
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

    struct Barrier {
        uint64_t target, beganNs;
        uint64_t endsAt = kNever;  // set once the count is reached
    };

    struct Fence {
        uint64_t submitted = 0, completed = 0;
        std::vector<Request *> writes;  // in submission order
        std::vector<Barrier> waiting;   // oldest first
    };

    Rule rule;
    std::vector<Arrival> pending;
    size_t pendingAt = 0;
    int faultsLeft;
    bool uiMeta = true;
    uint64_t uiNext = kNever, uiStart = 0;
    bool syncWaiting;
    bool fencing = false;
    bool anywhere = true;
    bool awaitAll = false;
    uint64_t wakeNs = 0;
    std::vector<Sync> planned;
    size_t plannedAt = 0;
    Fence fences[2];
    std::vector<Request> storage;
    Request *head = nullptr, *tail = nullptr;
    uint32_t queuedReads[4] = {};
    uint32_t queuedReplay = 0;
    uint32_t maxWriteSectors = kMaxSectors;  // the driver's maxWriteSectors: the card's own write limit
    SwitchSdPickState state = {};
    size_t depth = 0;
    std::vector<Request *> active, prepared;
    uint64_t activeEnd = 0, now = 0;
    std::mt19937_64 random{7};
    Result result;

    bool waiting(bool shared) const
    {
        if (!fencing)
            return syncWaiting;
        return std::any_of(fences[shared].waiting.begin(), fences[shared].waiting.end(), [&](const Barrier &b) {
            return b.endsAt > now;
        });
    }

    uint64_t newestBeganNs(bool shared) const
    {
        for (auto it = fences[shared].waiting.rbegin(); it != fences[shared].waiting.rend(); ++it) {
            if (it->endsAt > now)
                return it->beganNs;
        }
        return 0;
    }

    // SwitchSdWriteFence::awaitedBeforeNs() of each window.
    void awaited(uint64_t out[2]) const
    {
        for (int w = 0; w < 2; ++w) {
            if (!fencing)
                out[w] = syncWaiting ? ~0ULL : 0;
            else if (awaitAll)
                out[w] = waiting(false) || waiting(true) ? ~0ULL : 0;
            else
                out[w] = newestBeganNs(w == 1);
        }
    }

    uint64_t nextArrival() const
    {
        const uint64_t background = pendingAt < pending.size() ? pending[pendingAt].at : kNever;
        const uint64_t sync = plannedAt < planned.size() ? planned[plannedAt].at : kNever;
        return std::min({background, uiNext, sync});
    }

    void enqueue(const Arrival &a, bool ui)
    {
        storage.push_back({nullptr, a.lba, a.sectors, a.klass, a.write, false, now, static_cast<int>(storage.size()),
                           ui, 0, a.shared, false, a.replay});
        Request *r = &storage.back();
        (tail ? tail->next : head) = r;
        tail = r;
        result.maxDepth = std::max(result.maxDepth, ++depth);
        if (r->write) {
            ++fences[r->shared].submitted;
            fences[r->shared].writes.push_back(r);
        } else {
            ++queuedReads[r->klass];
        }
        if (switchSdAgeable(r))
            ++queuedReplay;
    }

    void admit(uint64_t at)
    {
        now = at;
        if (plannedAt < planned.size() && planned[plannedAt].at == at) {
            Fence &fence = fences[planned[plannedAt++].shared];
            fence.waiting.push_back({fence.submitted, at});
            endSyncs(fence);
        } else if (uiNext == at) {
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
        if (!done.empty() && done.front()->write) {
            Fence &fence = fences[done.front()->shared];
            fence.completed += done.size();
            endSyncs(fence);
        }
    }

    // Ends the barriers whose count is reached, checking each one's writes are all done.
    void endSyncs(Fence &fence)
    {
        for (Barrier &barrier : fence.waiting) {
            if (barrier.endsAt != kNever || fence.completed < barrier.target)
                continue;
            for (uint64_t i = 0; i < barrier.target; ++i)
                result.fenced = result.fenced && fence.writes[i]->done;
            ++result.syncsEnded;
            barrier.endsAt = now + wakeNs;
        }
    }

    void complete(Request *r)
    {
        r->doneNs = now;
        r->done = true;
        const uint64_t latency = now - r->queuedNs;
        if (!r->ui) {
            result.lowerWorstNs = std::max(result.lowerWorstNs, latency);
            result.worstByClass[r->klass] = std::max(result.worstByClass[r->klass], latency);
            if (r->replay)
                result.replayWorstNs = std::max(result.replayWorstNs, latency);
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

    // pickLocked's fallback: `first` and the requests right behind it that continue it on the card, in its window and
    // direction (a passing read's chain must not overlap anything it passes either), unlinked by `prev`, first's
    // predecessor, as the driver does. Returns the chain's count; `first` heads it.
    std::vector<SwitchLbaRange> inFlight() const
    {
        std::vector<SwitchLbaRange> ranges;
        for (const Request *r : active)
            ranges.push_back({r->lba, r->sectors});
        return ranges;
    }

    uint32_t unlinkBehind(Request *first, Request *prev)
    {
        const auto overlapsOlder = [&](const Request *later) {
            for (const Request *older = head; prev && older != first; older = older->next) {
                if (switchOverlaps(older, later))
                    return true;
            }
            return false;
        };
        const std::vector<SwitchLbaRange> flight = inFlight();
        const auto overlapsInFlight = [&](const Request *later) {
            for (const SwitchLbaRange &range : flight) {
                if (first->write && switchOverlapsRange(later, range))
                    return true;
            }
            return false;
        };
        Request *last = first;
        uint32_t count = 1;
        uint32_t sectors = first->sectors;
        while (count < kMaxChain && last->next && last->next->shared == first->shared &&
               last->next->write == first->write && last->next->lba == last->lba + last->sectors &&
               sectors + last->next->sectors <= (first->write ? maxWriteSectors : kMaxSectors) &&
               !overlapsOlder(last->next) && !overlapsInFlight(last->next)) {
            last = last->next;
            sectors += last->sectors;
            ++count;
        }
        for (const Request *r = first; r != last->next; r = r->next) {
            for (const Request *older = head; older != first; older = older->next) {
                if ((older->write || r->write) && switchOverlaps(older, r))
                    result.ordered = false;
            }
        }
        if (prev)
            prev->next = last->next;
        else
            head = last->next;
        if (tail == last)
            tail = prev;
        last->next = nullptr;
        return count;
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
            uint64_t awaitedBeforeNs[2];
            awaited(awaitedBeforeNs);
            switchSdAgeReplay(head, now, queuedReads, queuedReplay);
            const SwitchSdPicked<Request> picked = switchSdPick(head, awaitedBeforeNs, now, &state);
            first = picked.first;
            prev = picked.prev;
        }
        for (const Request *older = head; first->write && older != first; older = older->next)
            result.writesFirst = result.writesFirst && !older->write;
        uint64_t start = 0;
        uint32_t sectors = 0;
        uint32_t count = 0;
        Request *chain = nullptr;
        // pickLocked's dequeue with gapsOn false (no gap sink): a gathered chain that does not fit turns merging
        // anywhere off for good, and that pick takes the run behind its first request.
        bool gathered = false;
        if (anywhere) {
            const std::vector<SwitchLbaRange> flight = inFlight();
            bool fits = false;
            if (first->write) {
                count = switchGatherWrites(head, first, maxWriteSectors, kMaxChain, kMaxChain, &start, &sectors,
                                           flight.data(), static_cast<unsigned>(flight.size()),
                                           rule == Rule::kNew && waiting(first->shared));
                fits = switchWriteChainTiles(head, start, sectors, count);
            } else {
                count = switchGatherReads(head, first, kMaxSectors, kMaxChain, kMaxChain, &start, &sectors);
                fits = switchChainFits(head, start, sectors, count, 0, 0);
            }
            if (fits) {
                checkOrder();
                if (first->write)
                    countOutOfOrder(first);
                chain = switchUnlinkChain(head, tail, count);
                gathered = true;
            } else {
                switchClearChain(head);
                anywhere = false;
                ++result.unfitChains;
            }
        }
        if (!gathered) {
            count = unlinkBehind(first, prev);
            chain = first;
        }
        depth -= count;
        checkQueue();
        std::vector<Request *> out;
        out.reserve(count);
        for (Request *r = chain; r; r = r->next) {
            if (!r->write)
                --queuedReads[r->klass];
            if (switchSdAgeable(r))
                --queuedReplay;
            out.push_back(r);
        }
        checkReplayCount();
        return out;
    }

    void countOutOfOrder(const Request *first)
    {
        bool skipped = false;
        for (const Request *r = first; r; r = r->next) {
            if (!r->write || r->shared != first->shared)
                continue;
            if (!r->chained) {
                skipped = true;
            } else if (skipped) {
                ++result.outOfOrderWrites;
                return;
            }
        }
    }

    void checkQueue()
    {
        size_t n = 0;
        const Request *last = nullptr;
        for (const Request *r = head; r; r = r->next, ++n) {
            if (r->chained)
                result.intact = false;
            last = r;
        }
        if (n != depth || last != tail)
            result.intact = false;
    }

    void checkReplayCount()
    {
        uint32_t ageable = 0;
        for (const Request *r = head; r; r = r->next)
            ageable += switchSdAgeable(r);
        if (ageable != queuedReplay)
            result.intact = false;
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

inline std::vector<Arrival> scattered(int n, uint64_t spreadNs, uint32_t sectors, uint8_t klass, bool write,
                                      uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back({random() % (spreadNs + 1), random() % 30000000, sectors, klass, write});
    return out;
}

// One file read front to back in 512 KiB reads, one every `everyNs` (the GPU pack warm).
inline std::vector<Arrival> stream(int n, uint64_t everyNs, uint8_t klass)
{
    std::vector<Arrival> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out.push_back(
            {static_cast<uint64_t>(i) * everyNs, 70000000 + static_cast<uint64_t>(i) * 1024, 1024, klass, false});
    return out;
}

inline std::vector<Arrival> join(std::vector<Arrival> a, const std::vector<Arrival> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

inline void report(const char *name, const Result &before, const Result &after)
{
    std::printf("%-40s UI 10 faults %7.1f -> %7.1f ms, worst UI request %6.1f -> %6.1f ms, worst background %7.1f -> "
                "%7.1f ms\n",
                name, before.uiTotalNs / 1e6, after.uiTotalNs / 1e6, before.uiWorstNs / 1e6, after.uiWorstNs / 1e6,
                before.lowerWorstNs / 1e6, after.lowerWorstNs / 1e6);
}

}  // namespace sdmodel
>>>>>>> b6ce25505
