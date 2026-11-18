// SPDX-License-Identifier: GPL-2.0-only
// EDID source limits and the native IOAV wire layout.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "../SwitchDPLink.h"
#include "../SwitchDPModes.h"
#include "../SwitchDPAppleTiming.h"
#include <cstring>

namespace {

using Block = std::array<uint8_t, 128>;

void checksum(Block &block)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < 127; ++i) {
        sum = static_cast<uint8_t>(sum + block[i]);
    }
    block[127] = static_cast<uint8_t>(0x100 - sum);
}

// An 18-byte detailed timing descriptor, digital separate sync with both polarities positive.
std::array<uint8_t, 18> dtd(uint32_t pixel_khz, unsigned ha, unsigned hf, unsigned hs, unsigned hb, unsigned va,
                            unsigned vf, unsigned vs, unsigned vb, bool interlaced = false)
{
    const unsigned hblank = hf + hs + hb, vblank = vf + vs + vb, clock = pixel_khz / 10;
    std::array<uint8_t, 18> d{};
    d[0] = clock & 0xFF;
    d[1] = clock >> 8;
    d[2] = ha & 0xFF;
    d[3] = hblank & 0xFF;
    d[4] = static_cast<uint8_t>(((ha >> 8) << 4) | (hblank >> 8));
    d[5] = va & 0xFF;
    d[6] = vblank & 0xFF;
    d[7] = static_cast<uint8_t>(((va >> 8) << 4) | (vblank >> 8));
    d[8] = hf & 0xFF;
    d[9] = hs & 0xFF;
    d[10] = static_cast<uint8_t>(((vf & 0xF) << 4) | (vs & 0xF));
    d[11] = static_cast<uint8_t>(((hf >> 8) << 6) | ((hs >> 8) << 4) | ((vf >> 4) << 2) | (vs >> 4));
    d[17] = static_cast<uint8_t>(0x1E | (interlaced ? 0x80 : 0));
    return d;
}

Block base(const std::vector<std::array<uint8_t, 18>> &dtds, uint8_t extensions, bool vga = true)
{
    Block b{};
    const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(header, header + 8, b.begin());
    b[18] = 1;
    b[19] = 4;
    b[35] = vga ? 0x20 : 0;
    for (size_t i = 0; i < dtds.size() && i < 4; ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + 54 + 18 * i);
    }
    b[126] = extensions;
    checksum(b);
    return b;
}

Block cta(const std::vector<uint8_t> &vics, const std::vector<std::array<uint8_t, 18>> &dtds = {})
{
    Block b{};
    b[0] = 0x02;
    b[1] = 0x03;
    b[4] = static_cast<uint8_t>((2 << 5) | vics.size());
    std::copy(vics.begin(), vics.end(), b.begin() + 5);
    b[2] = static_cast<uint8_t>(5 + vics.size());
    for (size_t i = 0; i < dtds.size(); ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + b[2] + 18 * i);
    }
    checksum(b);
    return b;
}

switch_dp_mode_list parse(const std::vector<Block> &blocks)
{
    std::vector<uint8_t> edid;
    for (const Block &b : blocks) {
        edid.insert(edid.end(), b.begin(), b.end());
    }
    switch_dp_mode_list list{};
    switch_dp_edid_modes(edid.data(), static_cast<unsigned>(blocks.size()), &list);
    return list;
}

const auto k2160p60 = dtd(594000, 3840, 176, 88, 296, 2160, 8, 10, 72);
const auto k1440p144 = dtd(586590, 2560, 48, 32, 80, 1440, 3, 5, 62);
const auto k1080p60 = dtd(148500, 1920, 88, 44, 148, 1080, 4, 5, 36);
const uint64_t kHbr2Four = switch_dp_payload_kbps(SWITCH_DP_RATE_HBR2, 4);

TEST(Edid, StandardDetailedTimingParses)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_edid_dtd(k1080p60.data(), &mode));
    EXPECT_EQ(mode.pixel_khz, 148500u);
    EXPECT_EQ(mode.hactive, 1920);
    EXPECT_EQ(mode.hfront, 88);
    EXPECT_EQ(mode.hsync, 44);
    EXPECT_EQ(mode.hback, 148);
    EXPECT_EQ(mode.vactive, 1080);
    EXPECT_EQ(mode.vfront, 4);
    EXPECT_EQ(mode.vsync, 5);
    EXPECT_EQ(mode.vback, 36);
    EXPECT_TRUE(mode.hsync_high);
    EXPECT_TRUE(mode.vsync_high);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 60000u);
    EXPECT_EQ(switch_dp_refresh_period(&mode), 0x04444444u);
}

TEST(Edid, InterlacedAndDescriptorsAreSkipped)
{
    switch_dp_mode mode{};
    const auto interlaced = dtd(74250, 1920, 88, 44, 148, 540, 2, 5, 15, true);
    EXPECT_FALSE(switch_dp_edid_dtd(interlaced.data(), &mode));
    const std::array<uint8_t, 18> name{0, 0, 0, 0xFC, 0, 'T', 'V'};
    EXPECT_FALSE(switch_dp_edid_dtd(name.data(), &mode));
}

TEST(SourceLimits, UltrawideAndFastModesLeaveRoomForSupportedEdidTimings)
{
    const auto ultrawide = dtd(319750, 3440, 48, 32, 80, 1440, 3, 10, 28);
    const auto list = parse({base({ultrawide, k2160p60, k1440p144, k1080p60}, 0)});
    ASSERT_EQ(list.count, 2u);
    EXPECT_EQ(list.modes[0].hactive, 1920);
    EXPECT_TRUE(switch_dp_prefer(&list.modes[0], &list.modes[1]));
    auto timing = list.modes[0];
    EXPECT_TRUE(switch_dp_mode_allowed(&timing, switch_dp_payload_kbps(SWITCH_DP_RATE_HBR, 2)));
    ++timing.pixel_khz;
    EXPECT_FALSE(switch_dp_mode_allowed(&timing, kHbr2Four));
    timing.pixel_khz = 148352;
    EXPECT_TRUE(switch_dp_mode_allowed(&timing, kHbr2Four));
    EXPECT_EQ(switch_dp_refresh_period(&timing), (uint64_t{2200} * 1125 << 32) / 148352000u);
}

TEST(Edid, PublishesBoth1080RatesWhenFourKIsExcluded)
{
    const auto list = parse({base({}, 1, false), cta({97, 95, 0x80 | 16})});
    ASSERT_EQ(list.count, 2u);
    EXPECT_EQ(list.modes[0].hactive, 1920);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[0]), 60000u);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[1]), 59940u);
    EXPECT_TRUE(list.modes[0].preferred);
    EXPECT_EQ(parse({base({}, 1, false), cta({97, 95})}).count, 0u);
}

TEST(Edid, RepeatedDetailedAndCtaTimingsShareOneModeId)
{
    const auto list = parse({base({k1080p60}, 1, false), cta({16, 0x80 | 16}, {k1080p60})});
    ASSERT_EQ(list.count, 2u);
    EXPECT_TRUE(list.modes[0].preferred);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[1]), 59940u);
}

TEST(Edid, BrokenBaseAndExtensionAreIgnored)
{
    Block broken = base({k1080p60}, 0);
    broken[60] ^= 0xff;
    const auto fallback = parse({broken});
    EXPECT_EQ(fallback.count, 0u);
    auto extension = cta({97});
    extension[127] ^= 1;
    const auto list = parse({base({k1080p60}, 1, false), extension});
    ASSERT_EQ(list.count, 1u);
    EXPECT_EQ(list.modes[0].hactive, 1920);
}

TEST(SourceLimits, RejectsTimingsTheControllerCannotEncode)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    mode.hactive = 8192;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, kHbr2Four));
    mode.hactive = 1920;
    mode.vfront = 1;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, kHbr2Four));
    mode.vfront = 4;
    mode.pixel_khz = 750001;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, UINT64_MAX));
}

TEST(NativeTiming, MatchesBothPinnedNative1080p60Tables)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    const auto timing = switch_dp_apple_timing(&mode);
    const uint32_t native[] = {0, 2200, 1920, 44, 148, 88, 0, 1, 0,
                              1125, 1080, 5, 36, 4, 0x3c0000, 1, 0, 148500000, 0, 0};
    static_assert(sizeof(timing) == 80);
    EXPECT_EQ(std::memcmp(&timing, native, sizeof(native)), 0);
    const auto color = switch_dp_apple_rgb8();
    const uint32_t rgb8[] = {8, 0, 0, 10, 0, 0, 0, 0};
    static_assert(sizeof(color) == 32);
    EXPECT_EQ(std::memcmp(&color, rgb8, sizeof(rgb8)), 0);
}

}  // namespace
