// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window A flip that the producer's completion writes itself. The offer prepares it under the display's gate (the
// ring entry, its START_ADDR and layout); the completion writes it from whichever thread delivers it (the GPU's
// interrupt pass, a present work item); the gate puts it in the ring at the completion's own pass, which the next frame
// end runs first when it comes before. Plain state: every transition is made under the IOSimpleLock that also orders
// the gate's reads of window A's latch against the completion's write. Only the gate arms, so a flip it sees idle
// cannot be written behind its back.
#pragma once
#include <stdint.h>

struct SwitchCompletionFlip {
    enum : uint8_t {
        kIdle,
        kArmed,    // the completion may write it
        kWritten,  // requested by the completion; not in the ring until the completion's pass
    };

    uint8_t state = kIdle;
    int entry = -1;
    uint32_t start = 0, kind = 0, stride = 0, hOffset = 0;
    bool layout = false;
    uint64_t requestedAt = 0;  // the display's frame ends when the completion wrote it
    uint64_t writtenAt = 0;    // mach time of that write

    void arm(int i, uint32_t address)
    {
        entry = i;
        start = address;
        layout = false;
        state = kArmed;
    }

    void armLayout(uint32_t surfaceKind, uint32_t lineStride, uint32_t offset)
    {
        kind = surfaceKind;
        stride = lineStride;
        hOffset = offset;
        layout = true;
    }

    // The completion's verdict: true when it is to write the flip now. A failed frame is never flipped to.
    bool claim(int status)
    {
        if (state != kArmed)
            return false;
        state = status ? kIdle : kWritten;
        return status == 0;
    }

    // A refused offer, or a window A write of the gate's own that the prepared flip must not race.
    void withdraw()
    {
        if (state == kArmed)
            state = kIdle;
    }

    // presentEntry(i): true when the completion already wrote entry i (the caller puts it in the ring and writes
    // nothing). Anything still armed is withdrawn: the gate flips it itself.
    bool present(int i)
    {
        const bool written = state == kWritten && entry == i;
        if (state == kArmed || written)
            state = kIdle;
        return written;
    }
};

// Windows B and C's set, staged by the gate with every word but the surface addresses: the completion writes those
// (stores) and the STATE_CONTROL pair. Same states and lock as SwitchCompletionFlip.
struct SwitchPlaneCompletionFlip {
    static constexpr unsigned kMaxStores = 4;  // START_ADDR and START_ADDR_U of windows B and C

    uint8_t state = SwitchCompletionFlip::kIdle;
    unsigned stores = 0;
    uint32_t word[kMaxStores] = {}, value[kMaxStores] = {};
    uint32_t update = 0, request = 0;
    uint64_t requestedAt = 0, writtenAt = 0;

    void arm(uint32_t updateBits, uint32_t requestBits)
    {
        update = updateBits;
        request = requestBits;
        state = SwitchCompletionFlip::kArmed;
    }

    void clear()
    {
        stores = 0;
    }

    void store(uint32_t at, uint32_t to)
    {
        if (stores < kMaxStores) {
            word[stores] = at;
            value[stores] = to;
            ++stores;
        }
    }

    bool claim(int status)
    {
        if (state != SwitchCompletionFlip::kArmed)
            return false;
        state = status ? SwitchCompletionFlip::kIdle : SwitchCompletionFlip::kWritten;
        return status == 0;
    }

    void withdraw()
    {
        if (state == SwitchCompletionFlip::kArmed)
            state = SwitchCompletionFlip::kIdle;
    }

    // The gate's pass: true when the completion wrote it.
    bool take()
    {
        const bool written = state == SwitchCompletionFlip::kWritten;
        if (written)
            state = SwitchCompletionFlip::kIdle;
        return written;
    }
};
