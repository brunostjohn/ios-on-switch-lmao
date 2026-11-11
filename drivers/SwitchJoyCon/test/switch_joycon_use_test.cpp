// drivers/SwitchJoyCon/SwitchJoyConUse.h: which rail reports are the user playing, and when the pair goes idle.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>

#include "../SwitchJoyConUse.h"

namespace {

using Report = std::array<uint8_t, 49>;

void putStick(Report &r, unsigned at, unsigned x, unsigned y)
{
    r[at] = static_cast<uint8_t>(x & 0xff);
    r[at + 1] = static_cast<uint8_t>((x >> 8) | ((y & 0x0f) << 4));
    r[at + 2] = static_cast<uint8_t>(y >> 4);
}

Report resting(uint8_t id = 0x30, unsigned x = 2048, unsigned y = 2048)
{
    Report r{};
    r[0] = id;
    r[2] = 0x8e;
    putStick(r, 6, x, y);
    putStick(r, 9, 2100, 1990);
    return r;
}

struct Use : ::testing::Test {
    switch_joycon_use u{};

    void SetUp() override
    {
        switch_joycon_use_reset(&u);
        ASSERT_FALSE(take(resting()));
    }

    bool take(const Report &r)
    {
        return switch_joycon_use_report(&u, r.data(), r.size());
    }
};

TEST_F(Use, RestingSticksAndNoiseAreNotUse)
{
    EXPECT_FALSE(take(resting()));
    EXPECT_FALSE(take(resting(0x30, 2048 + SWITCH_JOYCON_USE_TRAVEL, 2048 - SWITCH_JOYCON_USE_TRAVEL)));
}

TEST_F(Use, AStickPushIsUse)
{
    EXPECT_TRUE(take(resting(0x30, 2048 + SWITCH_JOYCON_USE_TRAVEL + 1, 2048)));
    EXPECT_TRUE(take(resting(0x30, 2048, 400)));
}

TEST_F(Use, EveryGamepadButtonIsUse)
{
    const struct {
        unsigned byte;
        uint8_t bit;
    } buttons[] = {{3, 0x01}, {3, 0x02}, {3, 0x04}, {3, 0x08}, {3, 0x40}, {3, 0x80}, {4, 0x01}, {4, 0x02},
                   {4, 0x04}, {4, 0x08}, {5, 0x01}, {5, 0x02}, {5, 0x04}, {5, 0x08}, {5, 0x40}, {5, 0x80}};
    for (const auto &button : buttons) {
        Report r = resting();
        r[button.byte] = button.bit;
        EXPECT_TRUE(take(r)) << "byte " << button.byte << " bit " << unsigned{button.bit};
    }
}

TEST_F(Use, HomeCaptureGripAndRailButtonsAreNotUse)
{
    for (const auto &[byte, bit] : {std::pair<unsigned, uint8_t>{4, 0x10}, {4, 0x20}, {4, 0x80}, {3, 0x10}, {3, 0x20},
                                    {5, 0x10}, {5, 0x20}}) {
        Report r = resting();
        r[byte] = bit;
        EXPECT_FALSE(take(r)) << "byte " << byte << " bit " << unsigned{bit};
    }
}

TEST_F(Use, SubcommandRepliesAndNfcReportsCarryTheSameButtons)
{
    Report reply = resting(0x21);
    reply[3] = 0x08;
    EXPECT_TRUE(take(reply));
    Report nfc = resting(0x31);
    nfc[5] = 0x02;
    EXPECT_TRUE(take(nfc));
}

TEST_F(Use, SimpleReportsAndShortPacketsAreIgnored)
{
    Report simple = resting(0x3f);
    simple[3] = 0xff;
    EXPECT_FALSE(take(simple));
    Report r = resting();
    r[3] = 0x08;
    EXPECT_FALSE(switch_joycon_use_report(&u, r.data(), 11));
}

TEST(SwitchJoyConUse, TheRestIsTakenAgainAfterAReset)
{
    switch_joycon_use u{};
    switch_joycon_use_reset(&u);
    const Report pushed = resting(0x30, 3800, 2048);
    EXPECT_FALSE(switch_joycon_use_report(&u, pushed.data(), pushed.size()));
    EXPECT_FALSE(switch_joycon_use_report(&u, pushed.data(), pushed.size()));
    const Report centred = resting();
    EXPECT_TRUE(switch_joycon_use_report(&u, centred.data(), centred.size()));
    switch_joycon_use_reset(&u);
    EXPECT_FALSE(switch_joycon_use_report(&u, centred.data(), centred.size()));
    EXPECT_FALSE(switch_joycon_use_report(&u, centred.data(), centred.size()));
}

TEST(SwitchJoyConUse, IdleAfterGameControllersOwnTimeout)
{
    EXPECT_EQ(SWITCH_JOYCON_USE_IDLE_MS, 900000u);
    EXPECT_FALSE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS - 1));
    EXPECT_TRUE(switch_joycon_use_idle(1000, 1000 + SWITCH_JOYCON_USE_IDLE_MS));
}

}  // namespace
