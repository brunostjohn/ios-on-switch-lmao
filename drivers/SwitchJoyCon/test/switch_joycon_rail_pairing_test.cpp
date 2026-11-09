// Every rail attach pairs the Joy-Con over the rail (SwitchJoyConRail.cpp, "Pairing"), whatever its pairing log holds:
// a Joy-Con whose slot in use already named the console, left as it was, never paged after a detach. Reading the log is
// a debug build's diagnostic and ends in the pairing either way, and joyconpaird hands every rail record's key to
// bluetoothd.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchJoyConRailPairing, EveryAttachPairsOverTheRail)
{
    const std::string rail = SourceText("../SwitchJoyConRail.cpp");
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::startPairing("),
                HasSubstr("s.step = kReportSlot ? kReadSlot : kSendHost;"));
    EXPECT_THAT(rail, HasSubstr("#if DEVELOPMENT || DEBUG\nconstexpr bool kReportSlot = true;\n#else\nconstexpr bool "
                                "kReportSlot = false;"));
    const std::string walk = Body(rail, "void SwitchJoyConRail::pairingSlotReply(");
    EXPECT_THAT(walk, Not(HasSubstr("finishPairing(s, true")));
    EXPECT_THAT(walk, HasSubstr("s.step = kSendHost;\n    sendPairStep(s);"));
    // The one successful finish is the save's acknowledgement.
    EXPECT_EQ(Count(rail, "finishPairing(s, true,"), 1u);
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::pairingReply("), HasSubstr("finishPairing(s, true, \"rail\");"));
}

TEST(SwitchJoyConRailPairing, JoyconpairdHandsEveryRailKeyToBluetoothd)
{
    const std::string helper = SourceText("../helper/joyconpaird.c");
    EXPECT_THAT(helper, Not(HasSubstr("HostCapability")));
    EXPECT_THAT(Body(helper, "static void import("), HasSubstr("pair(address, name, classOfDevice,"));
}

}  // namespace
