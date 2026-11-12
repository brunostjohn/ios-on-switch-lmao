// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCompletionFlipDisplay.h: a direct frame's completion writes window A's flip itself, through window A's own
// aperture and nothing the gate's reads switch, and the gate's ring, latch check and SC7 path see that flip at the
// boundary that took it, whatever order the completion, its own pass, the frame boundaries and the frame-end action
// run in (switch_completion_flip_harness.h has the model). Window A's aperture is taken as proven here, as it is
// after the first completion flip latched as written; switch_completion_flip_proof_test has the unit that refuses.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_completion_flip_harness.h"
#include "switch_completion_flip_walk.h"

namespace completion_flip_test {
namespace {

constexpr int kSlot = Display::kSlot0;
constexpr uint32_t kWinAStart = 0xbc0, kWinAKind = 0xbcb, kWinAHOffset = 0xbc6, kWinAStride = 0xb8a;

class CompletionFlip : public testing::Test {
protected:
    Display d;

    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
        d.completionFlipsProven = true;
    }

    void TearDown() override
    {
        gOffer = nullptr;
    }

    void frame()
    {
        d.boundary();
        d.tick();
    }
};

TEST_F(CompletionFlip, TheCompletionWritesWindowAThroughItsOwnApertureOnly)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    const unsigned selectors = d.model.selectorWrites;
    d.model.written.clear();
    d.complete(0);
    const std::vector<uint32_t> want = {
        kWinAKind, kWinAStride, kWinAHOffset, kWinAStart, Display::kDcStateControl, Display::kDcStateControl};
    EXPECT_EQ(d.model.written, want);
    EXPECT_EQ(d.model.selectorWrites, selectors);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_EQ(d.model.armed[Display::kWinLineStride], 2944u);
    EXPECT_EQ(d.model.requested, Display::kGeneralActReq | Display::kWinAActReq);
    EXPECT_EQ(d.completionFlip.requestedAt, d.frameEnds);
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);

    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.flipRequested, d.completionFlip.requestedAt);
    EXPECT_EQ(d.marks[kSlot].ready, d.completionFlip.writtenAt);
    EXPECT_EQ(d.entrySeq[kSlot], d.flipRequest);
    EXPECT_EQ(d.deadlineArms, 1u);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_FALSE(d.transferred);

    frame();
    ASSERT_EQ(d.shown.size(), 1u);
    EXPECT_EQ(d.shown[0].first, kSlot);
    EXPECT_EQ(d.ring.front, kSlot);
}

TEST_F(CompletionFlip, AFrameEndBeforeTheCompletionsPassRunsThePassFirst)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    d.boundary();
    EXPECT_EQ(d.activeEntry(), kSlot);
    const uint64_t stamp = d.feStamp;
    d.tick();
    ASSERT_EQ(d.shown.size(), 1u);
    EXPECT_EQ(d.shown[0], std::make_pair(kSlot, stamp));
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_FALSE(d.transferred);
    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.ring.front, kSlot);
}

// The completion lands between the frame-end action's first step and its latch read, and a boundary takes it before
// the read: the read sees the flip the ring has.
TEST_F(CompletionFlip, ACompletionInsideTheFrameEndActionIsInTheRingBeforeTheLatchRead)
{
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot + 1);
    ASSERT_EQ(d.offer(kSlot), 0);
    d.midTick = [this] {
        d.complete(0);
        d.boundary();
        d.midTick = nullptr;
    };
    d.boundary();
    d.tick();
    EXPECT_EQ(d.activeEntry(), kSlot);
    EXPECT_EQ(d.ring.front, kSlot);
    ASSERT_FALSE(d.shown.empty());
    EXPECT_EQ(d.shown.back().first, kSlot);
}

// A GPU or VIC copy into a scan buffer: its completion writes window A to the buffer it copied into, and the pass's
// flip() finds that buffer as the ring's target and puts it in the ring.
TEST_F(CompletionFlip, ACopyIntoAScanBufferIsFlippedByItsCompletion)
{
    ASSERT_EQ(d.offerCopy(), 0);
    const int target = d.ring.drawing;
    ASSERT_NE(target, Display::kNoBuffer);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    d.complete(0);
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(target));
    EXPECT_EQ(d.model.armed[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    d.donePass();
    EXPECT_EQ(d.ring.pending, target);
    EXPECT_EQ(d.ring.drawing, Display::kNoBuffer);
    frame();
    EXPECT_EQ(d.ring.front, target);
}

TEST_F(CompletionFlip, AFailedFrameWritesNothingAndFlipsNothing)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.model.written.clear();
    d.complete(-5);
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    d.donePass();
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);
    EXPECT_FALSE(d.transferred);
}

TEST_F(CompletionFlip, ARefusedOfferWithdrawsItsFlip)
{
    gOffer = [](const TegraScanoutCopy *) {
        return -16;
    };
    EXPECT_EQ(d.offer(kSlot), -16);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
}

// The completion may run on another CPU inside the offer call, before the offer's own bookkeeping.
TEST_F(CompletionFlip, ACompletionBeforeTheOfferReturnsStillFlips)
{
    gOffer = [this](const TegraScanoutCopy *) {
        d.complete(0);
        return 0;
    };
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.entrySeq[kSlot], d.flipRequest);
    frame();
    EXPECT_EQ(d.ring.front, kSlot);
}

TEST_F(CompletionFlip, AGateWriteOfWindowAWithdrawsAnArmedFlip)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.withdrawCompletionFlip();
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_GT(d.model.selectorWrites, 0u);
}

TEST_F(CompletionFlip, NotArmedWhenTheFrameWouldBeHeldOrCarriesWindowB)
{
    ASSERT_EQ(d.offer(kSlot + 3), 0);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot + 3);
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle) << "a pitch frame replacing a tiled one";
    d.complete(0);
    d.donePass();
    EXPECT_EQ(d.ring.held, kSlot);
    frame();
    EXPECT_EQ(d.ring.pending, kSlot);
    frame();
    EXPECT_EQ(d.ring.front, kSlot);

    d.planeNextSet = true;
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
}

// SC7: a written flip goes in the ring before the controller is saved, an armed one is withdrawn, and nothing writes
// the unclocked controller.
TEST_F(CompletionFlip, SuspendPutsAWrittenFlipInTheRingAndWithdrawsAnArmedOne)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    d.suspend();
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_FALSE(d.transferred);
    d.resume();

    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.suspend();
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_NO_THROW(d.complete(0));
    d.donePass();
    EXPECT_TRUE(d.deferred);
    d.resume();
    EXPECT_EQ(d.ring.pending, kSlot + 1);
    frame();
    EXPECT_EQ(d.ring.front, kSlot + 1);
}

// The ring sees each latch at the first frame end after the boundary that took it, whichever of a producer's filter
// and its completion wrote the flip.
TEST_F(CompletionFlip, RandomOrdersNeverLoseOrDelayALatch)
{
    Walk walk;
    randomWalk(d, 200000, walk);
    EXPECT_GT(walk.completed, 8000u) << walk.completed;
    EXPECT_GT(walk.fromCompletions, 2000u) << walk.fromCompletions;
    EXPECT_GT(walk.fromFilters, 2000u) << walk.fromFilters;
    EXPECT_GT(d.shown.size(), 5000u) << d.shown.size();
    EXPECT_FALSE(d.completionFilterOff);
    EXPECT_TRUE(gLog.empty());
}

// The display's own hook sites: the completion writes after gpuDonePending (so a gate that sees the flip written can
// run the pass), the frame-end action and SC7 take a written flip first, the latch read is the locked one, the offer
// is armed before the GPU driver sees it, and FRAME_END stays routed while a flip is out.
TEST(CompletionFlipSource, TheDisplayCallsItWhereTheOrderingNeedsIt)
{
    using switch_ios::test::Between;
    using switch_ios::test::SourceText;
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const std::string complete = Between(display, "    void gpuComplete(int status)", "\n    }\n");
    EXPECT_LT(complete.find("__atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);"),
              complete.find("writeCompletionFlip(status);"));
    EXPECT_LT(complete.find("writeCompletionFlip(status);"), complete.find("gpuDoneSource->interruptOccurred("));
    EXPECT_EQ(Between(display, "    void vblankTick()\n    {\n", ";").find("takeCompletionFlip()"), 8u);
    EXPECT_EQ(Between(display, "    void quiesceForSuspend()\n    {\n", ";").find("suspendCompletionFlip()"), 8u);
    EXPECT_EQ(Between(display, "    void stopRotation()\n    {\n", ";").find("withdrawCompletionFlip()"), 8u);
    EXPECT_EQ(Between(display, "    void settleStopped()\n    {\n", ";").find("flipUnverifiedFromGate()"), 8u);
    const std::string check = Between(display, "    void checkFlip(bool atFrameEnd)", "\n    }\n");
    EXPECT_NE(check.find("const auto [control, start] = latchState();"), std::string::npos);
    EXPECT_EQ(check.find("windowActive("), std::string::npos);
    EXPECT_NE(Between(display, "    bool frameEndNeeded()", "\n    }\n").find("completionFlipOut()"),
              std::string::npos);
    EXPECT_NE(Between(display, "    bool offerDirect(", "\n    }\n").find("offerWithCompletionFlip(copy, entry)"),
              std::string::npos);
    EXPECT_NE(Between(display, "    bool offerGpuCopy(", "\n    }\n").find("offerWithCompletionFlip(copy, target)"),
              std::string::npos);
    const std::string vic = Between(SourceText("../SwitchVICScanoutDisplay.h"), "bool offerVicCopy(", "\n}\n");
    EXPECT_LT(vic.find("ring.claim(target);"), vic.find("armCompletionFlip(target);"));
    EXPECT_LT(vic.find("armCompletionFlip(target);"), vic.find("copySource->interruptOccurred("));
    const std::string start = SourceText("../SwitchScanoutStart.h");
    EXPECT_EQ(Between(start, "    void presentEntry(int i)\n    {\n", ")").find("if (presentedByCompletion(i"), 8u);
    EXPECT_EQ(Between(start, "    void writeFlip(int i)\n    {\n", ";").find("flipFromGate()"), 8u);
    EXPECT_NE(Between(SourceText("../SwitchOverlayRegisters.h"), "void planeBlendA(", "\n}\n")
                  .find("withdrawCompletionFlip();"),
              std::string::npos);
    EXPECT_NE(Between(SourceText("../SwitchPaletteDisplay.h"), "void paletteFrameEnd(", "\n}\n")
                  .find("withdrawCompletionFlip();"),
              std::string::npos);
}

}  // namespace
}  // namespace completion_flip_test
