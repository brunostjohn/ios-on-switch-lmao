// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window A as SwitchCompletionFlipDisplay.h and SwitchScanoutStart.h drive it, against a model
// of DC A's window A: an assembly copy (written through WINDOW_HEADER or window A's own aperture), the ARM copy
// WIN_A_UPDATE takes, and the active copy a frame boundary takes while WIN_A_ACT_REQ is out. The gate's side is the real code; the
// offer, the completion's pass and the frame-end action are NativeLegacyDisplay's as far as window A sees them.
#pragma once
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../../TegraDisplayFlip/TegraScanoutRing.h"
#include "../SwitchCompletionFlip.h"

namespace completion_flip_test {

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    throw std::runtime_error(text);
}

inline void IOLog(const char *, ...)
{}

struct IOSimpleLock {
    bool held = false;
};

using IOInterruptState = int;

inline IOSimpleLock *IOSimpleLockAlloc()
{
    static IOSimpleLock lock;
    return &lock;
}

inline IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock)
{
    if (lock->held)
        throw std::runtime_error("the completion flip's lock taken while held");
    lock->held = true;
    return 0;
}

inline void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState)
{
    lock->held = false;
}

inline uint64_t gClock = 1000;

inline uint64_t mach_absolute_time()
{
    return ++gClock;
}

struct PanicRestore {
    bool armed = false;
    bool panicking = false;
};

inline PanicRestore gPanicRestore;

inline std::function<int(const TegraScanoutCopy *)> gOffer;

struct DcModel {
    static constexpr uint32_t kStateAccess = 0x040, kStateControl = 0x041, kHeader = 0x042, kWindowASelect = 1u << 4;
    static constexpr uint32_t kStart = 0x800, kActReqs = 3u, kWinAUpdate = 1u << 9, kWinAActReq = 1u << 1;
    std::map<uint32_t, uint32_t> general, assembly, armed, active;
    uint32_t requested = 0;
    bool clocked = true;
    unsigned selectorWrites = 0, boundaries = 0;
    std::vector<uint32_t> written;

    static int aperture(uint32_t word)
    {
        if (word >= 0xb80 && word <= 0xb99)
            return static_cast<int>(0x700 + (word - 0xb80));
        if (word >= 0xbc0 && word <= 0xbf9)
            return static_cast<int>(0x800 + (word - 0xbc0));
        return -1;
    }

    static bool windowWord(uint32_t word)
    {
        return word >= 0x700 && word <= 0x839;
    }

    void write(uint32_t word, uint32_t value)
    {
        if (!clocked)
            throw std::runtime_error("a write to DC A while it is unclocked");
        written.push_back(word);
        if (const int a = aperture(word); a >= 0) {
            assembly[static_cast<uint32_t>(a)] = value;
        } else if (windowWord(word)) {
            if (general[kHeader] != kWindowASelect)
                throw std::runtime_error("a window A word written with another window selected");
            assembly[word] = value;
        } else if (word == kStateControl) {
            if (value & kWinAUpdate)
                armed = assembly;
            requested |= value & kActReqs;
        } else {
            if (word == kStateAccess || word == kHeader)
                ++selectorWrites;
            general[word] = value;
        }
    }

    uint32_t read(uint32_t word)
    {
        if (word == kStateControl)
            return requested;
        if (const int a = aperture(word); a >= 0)
            return assembly[static_cast<uint32_t>(a)];
        if (windowWord(word))
            return (general[kStateAccess] & 1u) ? active[word] : assembly[word];
        return general[word];
    }

    void boundary()
    {
        ++boundaries;
        if (requested & kWinAActReq)
            active = armed;
        requested = 0;
    }
};

struct Words {
    DcModel *dc = nullptr;

    struct Ref {
        DcModel *dc;
        uint32_t word;

        operator uint32_t() const
        {
            return dc->read(word);
        }

        Ref &operator=(uint32_t value)
        {
            dc->write(word, value);
            return *this;
        }

        Ref &operator=(const Ref &other)
        {
            return *this = static_cast<uint32_t>(other);
        }
    };

    Ref operator[](uint32_t word) const
    {
        return {dc, word};
    }
};

struct Display {
    static constexpr int kNoBuffer = TegraScanoutRing::kNone, kSlot0 = TegraScanoutRing::kMaxBuffers;
    static constexpr const char *kBlockScanName = "row scan";
    static constexpr uint32_t kDcIntStatus = 0x037, kDcIntEnable = 0x039, kDcStateAccess = 0x040,
                              kDcStateControl = 0x041, kDcWindowHeader = 0x042, kDcRegActControl = 0x043;
    static constexpr uint32_t kWinOptions = 0x700, kWinLineStride = 0x70a, kWinbufStartAddr = 0x800,
                              kWinbufHOffset = 0x806, kWinbufUnderflow = 0x80a, kWinbufSurfaceKind = 0x80b,
                              kWinABase = 0xb80;
    static constexpr uint32_t kReadMuxActive = 1, kWriteMuxActive = 4, kWindowASelect = 1u << 4,
                              kWinAActHcntrSel = 1u << 2;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kGeneralUpdate = 1u << 8, kWinAUpdate = 1u << 9;
    static constexpr uint32_t kWinAUnderflowInt = 1u << 8, kBlockScan = 0;
    static constexpr uint32_t kBlockSurfaceKind = 0x42, kBlockLineStride = 0x1800, kBlockHOffset = 0;
    static constexpr unsigned kLatchFrames = 4, kDirectUnderflowFrames = 3, kUnderflowFrames = 30;

    struct Plane {
        uint64_t seq = 0;
    };

    struct Mark {
        uint64_t ready = 0;
    };

    struct Probe {
        bool active() const
        {
            return false;
        }
    } probe;

    DcModel model;
    Words dc{&model};
    TegraScanoutRing ring;
    std::array<uint64_t, TegraScanoutRing::kMaxEntries> entrySeq{}, entryPendingSeq{};
    std::array<bool, TegraScanoutRing::kMaxEntries> entryPlaneSet{};
    std::array<Plane, TegraScanoutRing::kMaxEntries> entryPlane{};
    std::array<Mark, TegraScanoutRing::kMaxEntries> marks{};
    Plane planeNext, planeTarget;
    bool planeNextSet = false, rotated = true, rotationLatched = true, rotationAnnounced = true, doubleBuffer = true;
    bool directAttached = true, directOn = true, stats = false, probeUnderflow = false, probeRequest = false;
    bool hwSuspended = false, gpuDonePending = false, gpuScanout = false, transferred = false, deferred = false;
    bool copying = false, deferredCopy = false;
    int gpuStatus = 0, gpuDirect = kNoBuffer;
    uint32_t pitchActControl = 0, pitchOptions = 0x40000000;
    uint64_t frameEnds = 10, feStamp = 0, rotationRequested = 0, flipRequested = 0, flipRequest = 0, nextSwap = 40;
    uint64_t stReplaced = 0, stUnderflows = 0;
    unsigned underflowRun = 0, deadlineArms = 0, signals = 0;
    std::vector<std::pair<int, uint64_t>> shown;  // (entry, stamp) for every latch the ring saw
    std::function<void()> midTick;

    Display()
    {
        ring.reset(3);
        model.general[DcModel::kHeader] = kWindowASelect;
        model.assembly[kWinbufStartAddr] = surfaceAddress(0);
        model.armed = model.active = model.assembly;
    }

    // Slots 0-2 are pitch surfaces of one stride; slot 3 is tiled.
    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const
    {
        const bool tiled = i < kSlot0 || i == kSlot0 + 3;
        kind = tiled ? kBlockSurfaceKind : 0;
        stride = tiled ? kBlockLineStride : 2944;
        hOffset = 0;
    }

    bool sameLayout(int a, int b) const
    {
        uint32_t ka = 0, sa = 0, ha = 0, kb = 0, sb = 0, hb = 0;
        layoutOf(a, ka, sa, ha);
        layoutOf(b, kb, sb, hb);
        return ka == kb && sa == sb && ha == hb;
    }

    uint32_t surfaceAddress(int i) const
    {
        return i >= kSlot0 ? 0x10000000u + static_cast<uint32_t>(i - kSlot0) * 0x1000000u
                           : 0xc0000000u + static_cast<uint32_t>(i) * 0x1000000u;
    }

    int surfaceIndex(uint32_t address) const
    {
        for (int i = 0; i < static_cast<int>(TegraScanoutRing::kMaxEntries); ++i)
            if (surfaceAddress(i) == address)
                return i;
        return kNoBuffer;
    }

    int activeEntry()
    {
        return surfaceIndex(model.active[kWinbufStartAddr]);
    }

    uint32_t expectedStart() const
    {
        return surfaceAddress(ring.pending != kNoBuffer ? ring.pending : ring.front);
    }

    uint64_t seqOf(uint64_t request) const
    {
        return request;
    }

    void markFlip(int i)
    {
        marks.at(static_cast<size_t>(i)).ready = mach_absolute_time();
    }

    void checkPitchWindow()
    {}

    void chainPanicHook()
    {}

    void scanoutMemoryBarrier()
    {}

    void armFlipDeadline()
    {
        ++deadlineArms;
    }

    void planeStage(const Plane &, uint32_t &, uint32_t &)
    {
        throw std::runtime_error("no planes here");
    }

    uint32_t windowActive(uint32_t word)
    {
        return model.active[word];
    }

    void stopConsoleTape()
    {}

    void releaseIdleDirectReads()
    {}

    void notePresent(uint64_t, uint64_t, const Mark &)
    {}

    void retireThrough(uint64_t, uint64_t)
    {}

    static bool columnScanBusy()
    {
        return false;
    }

    void flipLatched()
    {
        if (const int held = ring.flipHeld(); held != kNoBuffer)
            writeFlip(held);
    }

    // NativeLegacyDisplay's checkFlip, with every reading the ring cannot explain a failure.
    void checkFlip(bool atFrameEnd)
    {
        const auto [control, start] = latchState();
        const int asked = ring.pending;
        const int replaced = ring.superseded;
        switch (ring.latch(surfaceIndex(start), (control & (kGeneralActReq | kWinAActReq)) != 0u)) {
        case TegraScanoutRing::kWaiting:
            if (atFrameEnd && frameEnds - flipRequested >= kLatchFrames)
                throw std::runtime_error("window A never latched the flip");
            return;
        case TegraScanoutRing::kInconsistent:
            throw std::runtime_error("window A scans what the ring cannot explain");
        case TegraScanoutRing::kTaken:
            shown.emplace_back(asked, feStamp);
            break;
        case TegraScanoutRing::kTakenReplaced:
            shown.emplace_back(replaced, feStamp);
            break;
        }
        flipLatched();
    }

    // NativeLegacyDisplay's offerDirect for slot entry, as far as the ring and window A see it.
    int offer(int entry)
    {
        TegraScanoutCopy copy = {};
        copy.display = this;
        const int offered = offerWithCompletionFlip(copy, entry);
        if (offered)
            return offered;
        gpuScanout = transferred = true;
        gpuDirect = entry;
        flipRequest = ++nextSwap;
        return 0;
    }

    // offerGpuCopy (and offerVicCopy, armed the same way): the frame is copied into the scan buffer ring.target().
    int offerCopy()
    {
        const int target = ring.target();
        if (target == kNoBuffer)
            return -1;
        TegraScanoutCopy copy = {};
        copy.display = this;
        const int offered = offerWithCompletionFlip(copy, target);
        if (offered)
            return offered;
        ring.claim(target);
        gpuScanout = transferred = copying = true;
        flipRequest = ++nextSwap;
        return 0;
    }

    // gpuComplete, on the completion's thread.
    void complete(int status)
    {
        gpuStatus = status;
        gpuDonePending = true;
        writeCompletionFlip(status);
        ++signals;
    }

    // gpuDoneFired, under the gate.
    void donePass()
    {
        if (gpuDonePending) {
            gpuDonePending = false;
            finishGpuSwap(gpuStatus);
        }
    }

    // finishGpuCopy and presentGpuCopy: a direct slot is presented as it is, a copy through flip()'s ring.target().
    void finishGpuSwap(int status)
    {
        gpuScanout = false;
        if (copying) {
            copying = false;
            if (!status && hwSuspended) {
                deferredCopy = true;
                return;
            }
            if (status)
                ring.release();
            else
                presentEntry(ring.target());
            transferred = false;
            return;
        }
        const int entry = gpuDirect;
        gpuDirect = kNoBuffer;
        if (status) {
            transferred = false;
            return;
        }
        if (hwSuspended) {
            deferred = true;
            gpuDirect = entry;
            return;
        }
        presentEntry(entry);
        transferred = false;
    }

    // vblankTick's window A steps: the filter stamped the boundary; this runs at some later point.
    void tick()
    {
        takeCompletionFlip();
        if (midTick)
            midTick();
        frameEnds = frameEnds + 1;
        if (rotated)
            checkRotation();
    }

    void boundary()
    {
        model.boundary();
        feStamp = mach_absolute_time();
    }

    // quiesceForSuspend and finishResume as far as window A sees them.
    void suspend()
    {
        suspendCompletionFlip();
        ring.settle();
        hwSuspended = true;
        model.clocked = false;
    }

    void resume()
    {
        model.clocked = true;
        hwSuspended = false;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        if (deferred) {
            deferred = false;
            const int entry = gpuDirect;
            gpuDirect = kNoBuffer;
            presentEntry(entry);
            transferred = false;
        }
        if (deferredCopy) {
            deferredCopy = false;
            presentEntry(ring.target());
            transferred = false;
        }
        if (const int held = ring.flipHeld(); held != kNoBuffer)
            writeFlip(held);
    }

#define SWITCH_FRAME_TIMELINE 0
#define KDBG(...)
#include "../SwitchScanoutStart.h"
#include "../SwitchCompletionFlipDisplay.h"
#undef KDBG
#undef SWITCH_FRAME_TIMELINE
};

}  // namespace completion_flip_test

extern "C" inline int tegra_gm20b_scanout_copy(const TegraScanoutCopy *copy)
{
    return completion_flip_test::gOffer ? completion_flip_test::gOffer(copy) : 0;
}
