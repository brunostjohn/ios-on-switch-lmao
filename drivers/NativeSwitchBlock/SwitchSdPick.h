// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which queued request the SD worker starts its next chain with (NativeSwitchBlock.cpp pickLocked), and the raise-only
// reprioritisation behind doSetPriority. Free of kernel types: test/sd_pick_test.cpp runs this code in a model of the
// card.
//
// A request's rank is its IOStoragePriority class (0 = I/O tier 0, the UI's page-ins) doubled, plus one for a write:
// a read goes before a write of its own class, because a write's completion blocks no thread unless a synchronize
// waits for it, and then the writes rank as reads of their class. The oldest read of the best class queued goes before
// a head of worse rank, past every older request it does not overlap a write of (reads may overlap reads).
//
// Nothing ages back into FIFO order: a head passed over for kSwitchSdStarveNs goes first once, and then not again for
// kSwitchSdShareNs. A tier-0 read therefore waits for the chain on the card, the chain prepared behind it, older tier-0
// requests, and at most one passed-over chain per kSwitchSdShareNs.
#pragma once

#include <stdint.h>

#include "SwitchQueue.h"

constexpr unsigned kSwitchSdPickClasses = 4;
constexpr uint64_t kSwitchSdStarveNs = 100000000;  // 100 ms
constexpr uint64_t kSwitchSdShareNs = 50000000;    // 50 ms

struct SwitchSdPickState {
    uint64_t lowerDueNs;  // a passed-over head may go first again from here
};

template <typename Request> struct SwitchSdPicked {
    Request *first;
    Request *prev;  // first's predecessor in the queue, nullptr when first is the head
    bool passed;    // first was taken before older requests
    bool starved;   // the head went first because it had been passed over for too long
};

template <typename Request> inline unsigned switchSdRank(const Request *r, bool syncWaiting)
{
    return 2u * r->klass + (r->write && !syncWaiting ? 1u : 0u);
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
    if (now - head->queuedNs >= kSwitchSdStarveNs && now >= state->lowerDueNs) {
        state->lowerDueNs = now + kSwitchSdShareNs;
        return {head, nullptr, false, true};
    }
    for (Request *before = head, *candidate = head->next; candidate; before = candidate, candidate = candidate->next) {
        if (candidate->write || candidate->klass != best)
            continue;
        bool blocked = false;
        for (const Request *older = head; older != candidate && !blocked; older = older->next)
            blocked = older->write && switchOverlaps(older, candidate);
        if (!blocked)
            return {candidate, before, true, false};
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
