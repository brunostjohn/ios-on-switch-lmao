// SPDX-License-Identifier: BSD-2-Clause-Patent
// The syncpoints TegraHost1x hands out (tegra_host1x_syncpt_alloc, _free): host1x's 192 (T210 TRM 14.2), less the ones
// reserved and the ones the boot chain left counting. Allocation takes one compare-and-swap and never sleeps or locks,
// so any context may allocate. The search starts where the last one ended (round robin): a freed id comes back only
// after every other free id has been handed out since, which keeps it from an owner whose fence a late reader still
// holds. The value is never reset: each owner reads its base fresh.
//
// The cursor starts at kFirstHandedOut, so the first 32 ids are the ones every boot used before the pool grew, and
// nothing below them is handed out before TegraHost1x's bring-up has reserved what the boot chain left counting.
#pragma once
#include <stdint.h>

struct SwitchHost1xSyncptPool {
    static constexpr uint32_t kSyncpts = 192, kWords = kSyncpts / 64, kFirstHandedOut = 160;

    uint64_t taken[kWords] = {};  // handed out now, or never to be (reserved)
    uint64_t owned[kWords] = {};  // handed out now
    uint64_t ever[kWords] = {};   // handed out since boot: SC7 saves and restores these
    uint32_t cursor = kFirstHandedOut;
    uint32_t inUse = 0, reserved = 0;

    static uint64_t bit(uint32_t id)
    {
        return 1ULL << (id % 64);
    }

    // Never handed out. False when it already is: the caller is too late to keep it out.
    bool reserve(uint32_t id)
    {
        if (id >= kSyncpts)
            return false;
        const uint64_t was = __atomic_fetch_or(&taken[id / 64], bit(id), __ATOMIC_ACQ_REL);
        if ((was & bit(id)) != 0)
            return (__atomic_load_n(&owned[id / 64], __ATOMIC_ACQUIRE) & bit(id)) == 0;
        __atomic_fetch_add(&reserved, 1, __ATOMIC_RELAXED);
        return true;
    }

    bool alloc(uint32_t *id)
    {
        const uint32_t start = __atomic_load_n(&cursor, __ATOMIC_RELAXED) % kSyncpts;
        for (uint32_t step = 0; step < kSyncpts; ++step) {
            const uint32_t at = (start + step) % kSyncpts;
            uint64_t word = __atomic_load_n(&taken[at / 64], __ATOMIC_ACQUIRE);
            while ((word & bit(at)) == 0) {
                if (__atomic_compare_exchange_n(&taken[at / 64], &word, word | bit(at), false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
                    __atomic_fetch_or(&owned[at / 64], bit(at), __ATOMIC_RELEASE);
                    __atomic_fetch_or(&ever[at / 64], bit(at), __ATOMIC_RELEASE);
                    __atomic_store_n(&cursor, at + 1, __ATOMIC_RELAXED);
                    __atomic_add_fetch(&inUse, 1, __ATOMIC_RELAXED);
                    *id = at;
                    return true;
                }
            }
        }
        return false;
    }

    // False (nothing changed) when `id` is not handed out now: never handed out, reserved, or freed already.
    bool free(uint32_t id)
    {
        if (id >= kSyncpts)
            return false;
        const uint64_t was = __atomic_fetch_and(&owned[id / 64], ~bit(id), __ATOMIC_ACQ_REL);
        if ((was & bit(id)) == 0)
            return false;
        __atomic_fetch_sub(&inUse, 1, __ATOMIC_RELAXED);
        __atomic_fetch_and(&taken[id / 64], ~bit(id), __ATOMIC_RELEASE);
        return true;
    }

    bool owns(uint32_t id) const
    {
        return id < kSyncpts && (__atomic_load_n(&owned[id / 64], __ATOMIC_ACQUIRE) & bit(id)) != 0;
    }

    bool everOwned(uint32_t id) const
    {
        return id < kSyncpts && (__atomic_load_n(&ever[id / 64], __ATOMIC_ACQUIRE) & bit(id)) != 0;
    }

    uint32_t capacity() const
    {
        return kSyncpts - __atomic_load_n(&reserved, __ATOMIC_RELAXED);
    }
};
