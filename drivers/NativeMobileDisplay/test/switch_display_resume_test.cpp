// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <vector>
#include "../../TegraSMMU/TegraSMMU.h"
#include "../SwitchDisplayMemoryTiming.h"

struct {
    bool armed = false;
} gPanicRestore;

int restoreResult = 0;
unsigned restores = 0;

extern "C" int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t group, char *, size_t)
{
    EXPECT_EQ(asid, TEGRA_SMMU_ASID_DISPLAY);
    EXPECT_EQ(ptb, 0x12345u);
    EXPECT_EQ(group, TEGRA_SMMU_DC_ASID);
    ++restores;
    return restoreResult;
}

namespace {
[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("restore refused");
}

struct Display {
    enum Reg {
        kDcStateAccess,
        kDcWindowHeader,
        kWinOptions,
        kWinStart,
        kDcDispWinOptions,
        kDcRegActControl,
        kDcScratchB,
        kDcStateControl,
        kDcIntStatus,
        kDcIntEnable,
        kCount
    };

    static constexpr uint32_t kWindowAState[] = {kWinStart, kWinOptions};
    static constexpr uint32_t kWindowASelect = 0x10, kGeneralUpdate = 1 << 8, kWinAUpdate = 1 << 9;
    static constexpr uint32_t kGeneralActReq = 1, kWinAActReq = 2, kVblankInt = 2, kFrameEndInt = 4;
    static constexpr uint32_t kWinAUnderflowInt = 256;
    static constexpr int kNoBuffer = -1;

    struct Loop {
        bool gated = true;

        bool inGate() const
        {
            return gated;
        }
    } loop;

    struct Domain {
        uint32_t ptb() const
        {
            return 0x12345;
        }
    } probeDomain;

    struct Color {
        void restoreAfterPowerUp()
        {}
    } colorObject;

    Color *color = &colorObject;

    struct Source {
        bool enabled = false;

        void enable()
        {
            enabled = true;
        }
    } source;

    Source *vblankSource = &source;

    struct Ring {
        int held = 7;

        int flipHeld()
        {
            const auto result = held;
            held = -1;
            return result;
        }
    } ring;

    std::array<uint32_t, kCount> dc{};
    uint32_t savedWindow[2] = {0x10000000, 0x40000000};
    uint32_t savedDispWinOptions = 1, savedActControl = 2, savedScratchB = 3, savedIntEnable = 7, savedIntMask = 6;
    uint32_t dcPending = 9, underflowRun = 3;
    uint64_t frameEnds = 100, rotationRequested = 0;
    bool hwSuspended = true, directAttached = true, planesAttached = true, vpulseProgrammed = true;
    bool scanoutStopped = true, rotated = true, rotationLatched = true, resumeLit = true;
    bool presentDeferred = true, gpuPresentDeferred = true, kickDeferred = true;
    bool timingValid = true, planesValid = true, planesRestored = false;
    unsigned started = 0, callbacks = 0;
    std::vector<int> order;

    Display()
    {
        restoreResult = 0;
        restores = 0;
        gPanicRestore = {};
    }

    Loop *getWorkLoop()
    {
        return &loop;
    }

    bool captureDisplayMemory(switch_display::TimingBank bank)
    {
        EXPECT_EQ(bank, switch_display::TimingBank::Programmed);
        return timingValid;
    }

    void writePhaseRegisters()
    {
        order.push_back(1);
    }

    void writeIntMask(uint32_t, uint32_t)
    {}

    bool planesResume()
    {
        EXPECT_TRUE(hwSuspended);
        EXPECT_TRUE(scanoutStopped);
        EXPECT_FALSE(source.enabled);
        EXPECT_EQ(dc[kWinStart], savedWindow[0]);
        order.push_back(2);
        planesRestored = planesValid;
        return planesValid;
    }

    void setScanout(bool on)
    {
        EXPECT_TRUE(planesRestored);
        EXPECT_FALSE(hwSuspended);
        scanoutStopped = !on;
        ++started;
        order.push_back(3);
    }

    void callback()
    {
        EXPECT_TRUE(planesRestored);
        EXPECT_FALSE(hwSuspended);
        ++callbacks;
        order.push_back(4);
    }

    void presentCopied()
    {
        callback();
    }

    void presentGpuCopy()
    {
        callback();
    }

    void writeFlip(int slot)
    {
        EXPECT_EQ(slot, 7);
        callback();
    }

    void kickSwapQueue()
    {
        callback();
    }

#include "../SwitchDisplayResume.h"
};

TEST(DisplayResume, RestoresCompleteImageBeforeAnyDeferredPresentationOrStart)
{
    Display d;
    ASSERT_TRUE(d.finishResume());
    EXPECT_EQ(restores, 1u);
    EXPECT_EQ(d.dc[Display::kWinStart], 0x10000000u);
    EXPECT_EQ(d.started, 1u);
    EXPECT_EQ(d.callbacks, 4u);
    EXPECT_EQ(d.order, (std::vector<int>{1, 2, 3, 4, 4, 4, 4}));
    EXPECT_TRUE(d.directAttached && d.planesAttached && gPanicRestore.armed);
}

TEST(DisplayResume, FailedDomainKeepsAllAcceptedStateStoppedAndRetained)
{
    Display d;
    restoreResult = -1;
    EXPECT_THROW(d.finishResume(), std::runtime_error);
    EXPECT_TRUE(d.hwSuspended && d.scanoutStopped && d.directAttached && d.planesAttached);
    EXPECT_EQ(d.savedWindow[0], 0x10000000u);
    EXPECT_EQ(d.ring.held, 7);
    EXPECT_EQ(d.callbacks + d.started, 0u);
    EXPECT_FALSE(d.source.enabled);
}

TEST(DisplayResume, FailedBandwidthCannotIlluminateOnlyTheUi)
{
    Display d;
    d.planesValid = false;
    EXPECT_THROW(d.finishResume(), std::runtime_error);
    EXPECT_TRUE(d.hwSuspended && d.scanoutStopped);
    EXPECT_EQ(d.callbacks + d.started, 0u);
    EXPECT_FALSE(d.source.enabled);
    EXPECT_EQ(d.ring.held, 7);
}

TEST(DisplayResume, SleepingPanelStillRestoresItsAcceptedState)
{
    Display d;
    d.resumeLit = false;
    EXPECT_FALSE(d.finishResume());
    EXPECT_TRUE(d.planesRestored && d.scanoutStopped);
    EXPECT_EQ(d.started, 0u);
    EXPECT_EQ(d.callbacks, 4u);
}

TEST(DisplayResume, UngatedOrInvalidTimingCannotPublishRestoredRegisters)
{
    Display d;
    d.loop.gated = false;
    EXPECT_THROW(d.finishResume(), std::runtime_error);
    d.loop.gated = true;
    d.timingValid = false;
    EXPECT_THROW(d.finishResume(), std::runtime_error);
    EXPECT_EQ(restores, 0u);
    EXPECT_EQ(d.callbacks + d.started, 0u);
}
}
