// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDMMC1's DMA through the SMMU (swgroup SDMMC1A, drivers/TegraSMMU/TegraSMMURamView.h). The controller reaches the
// driver's own buffers (the descriptor tables, the bounce buffer, the gap sink), held for good, and a chain's request
// pages, held from before its command can be issued until the controller is done with them; any other address faults at
// the MC, which panics naming the client and address. Descriptors keep the pages' own addresses, so building a table
// is unchanged.
//
// A page may be in both slots' tables (64-byte segments of two requests): holds are counted per page, so each chain
// holds and releases its own descriptors and neither waits for the other. Only the worker thread calls these.
#pragma once
#include <stdint.h>
#include "SwitchAdma.h"

struct SwitchSdOwnedRange {
    uint64_t pa, bytes;
};

inline bool switchSdOwned(uint64_t pa, const SwitchSdOwnedRange *owned, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (pa - owned[i].pa < owned[i].bytes)
            return true;
    }
    return false;
}

// The request pages of a built table, held and committed under one lock of the view before the command may be
// issued. False (nothing held, the chain goes through the bounce buffer) when the view could not take one.
template <class View>
bool switchSdHoldChain(View &view, const SwitchAdmaTable &table, const SwitchSdOwnedRange *owned, unsigned ownedCount)
{
    uint32_t held = 0;
    view.locked([&](auto &core) {
        for (; held < table.count; ++held) {
            const SwitchAdmaDescriptor &d = table.entries[held];
            if (!switchSdOwned(switchAdmaAddress(d), owned, ownedCount) && !core.hold(switchAdmaAddress(d), d.length))
                break;
        }
        if (held < table.count) {
            for (uint32_t i = 0; i < held; ++i) {
                const SwitchAdmaDescriptor &d = table.entries[i];
                if (!switchSdOwned(switchAdmaAddress(d), owned, ownedCount))
                    (void)core.release(switchAdmaAddress(d), d.length);
            }
        }
        core.commit();
    });
    return held == table.count;
}

// The same table's holds end once the controller can no longer reach them: after its transfer completed, or after a
// failed command's reset, and before its requests complete (their pages may go back to the VM). False when a page had
// no hold to end: the chain's accounting is wrong.
template <class View>
bool switchSdReleaseChain(View &view, const SwitchAdmaTable &table, const SwitchSdOwnedRange *owned,
                          unsigned ownedCount)
{
    bool released = true;
    view.locked([&](auto &core) {
        for (uint32_t i = 0; i < table.count; ++i) {
            const SwitchAdmaDescriptor &d = table.entries[i];
            if (!switchSdOwned(switchAdmaAddress(d), owned, ownedCount))
                released &= core.release(switchAdmaAddress(d), d.length);
        }
        core.commit();
    });
    return released;
}
