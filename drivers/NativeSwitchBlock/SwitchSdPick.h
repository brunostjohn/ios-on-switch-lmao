// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which queued request the SD worker starts its next chain with (NativeSwitchBlock.cpp pickLocked), and the raise-only
// reprioritisation behind doSetPriority. Free of kernel types: test/sd_pick_test.cpp runs this code in a model of the
// card.
//
// A request's rank is its IOStoragePriority class (0 = I/O tier 0, the UI's page-ins) doubled, plus one for a write:
// a read goes before a write of its own class, because a write's completion blocks no thread unless a synchronize
// waits for it. While one does, every write ranks with tier-0 reads (IOSCHED puts metadata writes at tier 1, and the
// waiter may be the UI). The oldest read of the best class queued goes before a head of worse rank, past every older
// request it does not overlap a write of (reads may overlap reads).
//
// Nothing ages back into FIFO order: a head passed over for kSwitchSdStarveNs goes first once, then again after
// kSwitchSdShareNs or kSwitchSdShareChains passes, whichever comes first, so lower classes keep at least one chain in
// kSwitchSdShareChains under a tier-0 flood. A tier-0 read waits for the chain on the card, the chain prepared behind
// it, older tier-0 requests, and those passed-over chains.
#pragma once

#include <stdint.h>

#include "SwitchQueue.h"

constexpr unsigned kSwitchSdPickClasses = 4;
constexpr uint64_t kSwitchSdStarveNs = 100000000;  // 100 ms
constexpr uint64_t kSwitchSdShareNs = 50000000;    // 50 ms
constexpr uint32_t kSwitchSdShareChains = 8;

struct SwitchSdPickState {
    uint64_t lowerDueNs;  // a passed-over head may go first again from here
    uint32_t passes;      // chains taken ahead of the head since it last went first
};

template <typename Request> struct SwitchSdPicked {
    Request *first;
    Request *prev;  // first's predecessor in the queue, nullptr when first is the head
    bool passed;    // first was taken before older requests
    bool starved;   // the head went first because it had been passed over for too long
};

template <typename Request> inline unsigned switchSdRank(const Request *r, bool syncWaiting)
{
    if (r->write)
        return syncWaiting ? 0u : 2u * r->klass + 1u;
    return 2u * r->klass;
}

template <typename Request>
inline SwitchSdPicked<Request> switchSdPick(Request *head, const uint32_t queuedReads[kSwitchSdPickClasses],
                                            bool syncWaiting, uint64_t now, SwitchSdPickState *state)
{
    unsigned best = 0;
    while (best < kSwitchSdPickClasses && !queuedReads[best])
        ++best;
    if (best == kSwitchSdPickClasses || 2u * best >= switchSdRank(head, syncWaiting))
        return {head, nullptr, false, false};
    if (now - head->queuedNs >= kSwitchSdStarveNs &&
        (now >= state->lowerDueNs || state->passes >= kSwitchSdShareChains)) {
        state->lowerDueNs = now + kSwitchSdShareNs;
        state->passes = 0;
        return {head, nullptr, false, true};
    }
    for (Request *before = head, *candidate = head->next; candidate; before = candidate, candidate = candidate->next) {
        if (candidate->write || candidate->klass != best)
            continue;
        bool blocked = false;
        for (const Request *older = head; older != candidate && !blocked; older = older->next)
            blocked = older->write && switchOverlaps(older, candidate);
        if (!blocked) {
            ++state->passes;
            return {candidate, before, true, false};
        }
    }
    return {head, nullptr, false, false};
}

// Raises every queued request overlapping [lba, lba + sectors) to `klass` (never lowers one), keeping queuedReads in
// step. Returns how many it raised. Requests already taken into a chain are not in the queue and keep their class.
template <typename Request>
inline uint32_t switchSdPromote(Request *head, uint64_t lba, uint64_t sectors, uint8_t klass,
                                uint32_t queuedReads[kSwitchSdPickClasses])
{
    uint32_t raised = 0;
    for (Request *r = head; r; r = r->next) {
        if (r->klass <= klass || !(r->lba < lba + sectors && lba < r->lba + r->sectors))
            continue;
        if (!r->write) {
            --queuedReads[r->klass];
            ++queuedReads[klass];
        }
        r->klass = klass;
        ++raised;
    }
    return raised;
}
