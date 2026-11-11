// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_glass_mem.h"

namespace {

constexpr uint64_t kPyramid = 0x7f0004000, kCapture = 0x7f0010000;
constexpr uint32_t kBgra8 = 80, kRgba16f = 115;

raw::GlassMemItem item(uint64_t key, uint32_t width, uint32_t height, uint32_t format, uint32_t bpp, uint32_t level = 0)
{
    raw::GlassMemItem it;
    it.key = key;
    it.width = width;
    it.height = height;
    it.format = format;
    it.bpp = bpp;
    it.level = level;
    it.texels = std::min(raw::kGlassMemTexels, width);
    return it;
}

// A slot whose item i's row repeats texels[i], ending in command buffer `serial`'s cookie
std::vector<uint8_t> rows(uint64_t serial, std::initializer_list<std::vector<uint8_t>> texels)
{
    std::vector<uint8_t> out(raw::kGlassMemSlotBytes);
    const uint32_t cookie = raw::glassMemCookie(serial);
    memcpy(out.data() + uint64_t(raw::kGlassMemItems) * raw::kGlassMemItemBytes, &cookie, sizeof cookie);
    size_t at = 0;
    for (const std::vector<uint8_t> &texel : texels) {
        for (uint32_t t = 0; t < raw::kGlassMemTexels; t++)
            memcpy(out.data() + at + t * texel.size(), texel.data(), texel.size());
        at += raw::kGlassMemItemBytes;
    }
    return out;
}

const std::vector<uint8_t> kGrey = {90, 90, 90, 255}, kOpaqueBlack = {0, 0, 0, 255}, kWhite = {255, 255, 255, 255};

}  // namespace

// Black is every texel's colour zero whatever its alpha (BGRA8's fourth byte, RGBA16Float's fourth half), -0 included
TEST(RawGlassMem, RowBlackLeavesAlphaOut)
{
    const uint8_t opaque[4] = {0, 0, 0, 255}, grey[4] = {1, 0, 0, 255};
    EXPECT_TRUE(raw::glassMemRow(opaque, 1, 4, kBgra8).black);
    EXPECT_FALSE(raw::glassMemRow(grey, 1, 4, kBgra8).black);
    const uint8_t half[8] = {0, 0x80, 0, 0, 0, 0x80, 0x00, 0x3c};  // -0, 0, -0, 1.0
    EXPECT_TRUE(raw::glassMemRow(half, 1, 8, kRgba16f).black);
    const uint8_t lit[8] = {0, 0x38, 0, 0, 0, 0, 0x00, 0x3c};  // 0.5 red
    EXPECT_FALSE(raw::glassMemRow(lit, 1, 8, kRgba16f).black);
}

TEST(RawGlassMem, RowFindsNonFiniteHalvesAndFloats)
{
    const uint8_t nan[8] = {0, 0x7e, 0, 0, 0, 0, 0, 0x3c}, inf[16] = {0, 0, 0x80, 0x7f};
    EXPECT_TRUE(raw::glassMemRow(nan, 1, 8, kRgba16f).nonFinite);
    EXPECT_TRUE(raw::glassMemRow(inf, 1, 16, 125).nonFinite);
    const uint8_t bytes[4] = {0xff, 0xff, 0xff, 0xff};
    EXPECT_FALSE(raw::glassMemRow(bytes, 1, 4, kBgra8).nonFinite);  // unorm: no NaN
}

TEST(RawGlassMem, StateIsTheTermAfterUnderscoreT)
{
    char s[5];
    raw::glassMemState("PBGRAXm_TvrdA2Xhf_Isrc", s);
    EXPECT_STREQ(s, "Tvrd");
    raw::glassMemState("PBGRABsovXmw_TgsgA3Xhf_Isrc", s);
    EXPECT_STREQ(s, "Tgsg");
    raw::glassMemState("PBGRAXm_Tds4A2Xhf_Isrc", s);
    EXPECT_STREQ(s, "Tds4");
    raw::glassMemState("PBGRAXmw_A3Xhfc", s);
    EXPECT_STREQ(s, "-");
    raw::glassMemState(nullptr, s);
    EXPECT_STREQ(s, "-");
}

// A texture this process never wrote (an icon, a surface) is not one of the probe's
TEST(RawGlassMem, OnlyTexturesThisProcessWroteAreSampled)
{
    raw::GlassMem probe;
    EXPECT_EQ(probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRAXm_TvrdA2Xhf_Isrc", 7, 1, 10), -1);
    probe.wrote(kPyramid, 10, 0);
    EXPECT_EQ(probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRAXm_TvrdA2Xhf_Isrc", 7, 1, 10), 0);
    EXPECT_EQ(probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 10), -1);
    EXPECT_EQ(probe.sampled(item(kPyramid, 96, 32, kBgra8, 4, 2), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 10), 1);
}

// The frame that builds the pyramid sets the reference; a later frame that only samples it is reused, aged by command
// buffers, and judged against it: the same row is ok, any other with no write between is changed
TEST(RawGlassMem, ReusedRowChangedWithNoWriteIsCaught)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 10, 3);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 10);
    raw::GlassMemCb built = probe.close(10, 1000, 3);
    ASSERT_EQ(built.count, 1u);
    EXPECT_TRUE(built.items[0].rebuilt);
    std::string line = probe.finish(built, rows(10, {kGrey}).data(), true);
    EXPECT_NE(line.find("/Tgsg/R0:ok"), std::string::npos) << line;

    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 11);
    raw::GlassMemCb same = probe.close(11, 1100, 5);
    EXPECT_FALSE(same.items[0].rebuilt);
    EXPECT_EQ(same.items[0].age, 1u);
    EXPECT_EQ(same.items[0].events, 2u);
    EXPECT_TRUE(probe.finish(same, rows(11, {kGrey}).data(), false).empty());
    line = probe.finish(same, rows(11, {kGrey}).data(), true);
    EXPECT_NE(line.find("/U1/e2:ok"), std::string::npos) << line;

    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 14);
    raw::GlassMemCb blackened = probe.close(14, 1400, 9);
    line = probe.finish(blackened, rows(14, {kOpaqueBlack}).data(), false);
    EXPECT_NE(line.find("/U4/e6:zchg"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.changed.load(), 1u);
    EXPECT_EQ(probe.counts.black.load(), 1u);
}

// A rebuilt pyramid that reads black or non-finite says so; the rebuild becomes the next reference
TEST(RawGlassMem, RebuiltRowsAreJudgedAndBecomeTheReference)
{
    raw::GlassMem probe;
    probe.wrote(kCapture, 20, 0);
    probe.sampled(item(kCapture, 320, 128, kRgba16f, 8), "PBGRAXm_Tds4A2Xhf_Isrc", 9, 2, 20);
    const std::vector<uint8_t> nan = {0, 0x7e, 0, 0, 0, 0, 0, 0x3c};
    std::string line = probe.finish(probe.close(20, 2000, 0), rows(20, {nan}).data(), false);
    EXPECT_NE(line.find("/R0:nf"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.nonFinite.load(), 1u);

    probe.wrote(kCapture, 21, 0);
    probe.sampled(item(kCapture, 320, 128, kRgba16f, 8), "PBGRAXm_Tds4A2Xhf_Isrc", 9, 2, 21);
    line = probe.finish(probe.close(21, 2100, 0), rows(21, {std::vector<uint8_t>(8, 0)}).data(), true);
    EXPECT_NE(line.find("/R0:zero"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.black.load(), 0u);  // black counts reused rows only
}

// A reused texture whose reference was never read (its last write's command buffer took no row of it) is marked unsure
TEST(RawGlassMem, ReusedWithoutReferenceIsUnsure)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 30, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 32);
    const std::string line = probe.finish(probe.close(32, 3200, 0), rows(32, {kWhite}).data(), true);
    EXPECT_NE(line.find("/U2:ok?"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.changed.load(), 0u);
}

// A texture header whose bytes change under the same index while its texture lives is moved
TEST(RawGlassMem, HeaderChangedUnderTheSameIndexIsMoved)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 40, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 0xaaaa, 40);
    probe.finish(probe.close(40, 4000, 0), rows(40, {kGrey}).data(), false);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 0xbbbb, 41);
    const std::string line = probe.finish(probe.close(41, 4100, 0), rows(41, {kGrey}).data(), false);
    EXPECT_NE(line.find("/mv:ok"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.moved.load(), 1u);
    probe.forget(kPyramid, 7);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 0xcccc, 42);
    EXPECT_EQ(probe.close(42, 4200, 0).items[0].moved, false);  // a view's header made again
}

// A destroyed texture's history goes: memory reused by a new texture starts unwritten
TEST(RawGlassMem, ForgetDropsTheTexturesHistory)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 50, 0);
    probe.forget(kPyramid);
    EXPECT_EQ(probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 51), -1);
}

TEST(RawGlassMem, FullLineFitsOsLog)
{
    raw::GlassMem probe;
    for (uint32_t i = 0; i < raw::kGlassMemItems + 2; i++) {
        probe.wrote(kPyramid + i * 0x40000, 60, 0);
        raw::GlassMemItem it = item(kPyramid + i * 0x40000, 16384, 16384, kRgba16f, 8, 7);
        it.levels = 14;
        it.flags = raw::kGlassMemHeap;
        probe.sampled(it, "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 60);
    }
    const raw::GlassMemCb cb = probe.close(60, ~0ull, ~0ull);
    EXPECT_EQ(cb.count, raw::kGlassMemItems);
    const std::string line = probe.finish(cb, rows(60, {}).data(), true);
    EXPECT_LT(line.size(), 1000u);
    EXPECT_EQ(line.find("cb 60 mach "), 0u) << line;
}

// A slot whose cookie never landed (the fence passed without the copies' release) is no read: '?' and nothing counted
TEST(RawGlassMem, SlotWithoutItsCookieIsNotJudged)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 70, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 70);
    const std::string line = probe.finish(probe.close(70, 7000, 0), rows(69, {kGrey}).data(), false);
    EXPECT_NE(line.find("/R0:?"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.cbs.load(), 0u);
    EXPECT_EQ(probe.counts.rebuilt.load(), 0u);
}

// A row the copy engine never wrote still holds the CPU's poison: '?', nothing counted for it
TEST(RawGlassMem, PoisonedRowIsNotJudged)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 75, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 75);
    std::vector<uint8_t> slot = rows(75, {});
    memset(slot.data(), raw::kGlassMemPoison, raw::kGlassMemItemBytes);
    const std::string line = probe.finish(probe.close(75, 7500, 0), slot.data(), true);
    EXPECT_NE(line.find("/R0:?"), std::string::npos) << line;
    EXPECT_EQ(probe.counts.black.load(), 0u);
}

// Past the item cap a glass draw's texture takes the place of the latest item no glass draw sampled; the line counts drops
TEST(RawGlassMem, GlassDrawsKeepTheirItemsPastTheCap)
{
    raw::GlassMem probe;
    for (uint32_t i = 0; i <= raw::kGlassMemItems; i++) {
        probe.wrote(kCapture + i * 0x40000, 80, 0);
        probe.sampled(item(kCapture + i * 0x40000, 320, 128, kBgra8, 4), "PBGRAXm_TvrdA2Xhf_Isrc", 9, 1, 80);
    }
    probe.wrote(kPyramid, 80, 0);
    const int index = probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 80);
    EXPECT_EQ(index, int(raw::kGlassMemItems) - 1);
    const raw::GlassMemCb cb = probe.close(80, 8000, 0);
    EXPECT_EQ(cb.count, raw::kGlassMemItems);
    EXPECT_EQ(cb.dropped, 2u);
    EXPECT_EQ(cb.items[raw::kGlassMemItems - 1].key, kPyramid);
    EXPECT_NE(probe.finish(cb, rows(80, {}).data(), true).find(" drop 2 |"), std::string::npos);
}

// A command buffer begun after an abandoned one starts with no items; a texture destroyed while listed is not read
TEST(RawGlassMem, BeginDropsAnAbandonedRecordAndForgetStopsItsRead)
{
    raw::GlassMem probe;
    probe.wrote(kPyramid, 90, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 90);
    probe.begin();
    EXPECT_EQ(probe.close(90, 9000, 0).count, 0u);
    probe.wrote(kPyramid, 91, 0);
    probe.sampled(item(kPyramid, 384, 128, kBgra8, 4), "PBGRABsovXmw_TgsgA3Xhf_Isrc", 7, 1, 91);
    probe.forget(kPyramid);
    const raw::GlassMemCb cb = probe.close(91, 9100, 0);
    ASSERT_EQ(cb.count, 1u);
    EXPECT_TRUE(cb.items[0].flags & raw::kGlassMemUnsampled);
}

// The notify words HW Test unpacks: fields where ui.glass_sidebar reads them, saturating
TEST(RawGlassMem, NotifyWordsPackTheirFields)
{
    raw::GlassMemCounts c;
    c.cbs = (1u << 20) + 5;
    c.changed = 7;
    c.nonFinite = 5000;
    c.moved = 2;
    c.black = 300;
    const uint64_t w = raw::glassMemWord(c);
    EXPECT_EQ(w & 0xfffff, 5u);
    EXPECT_EQ(w >> 20 & 0xfff, 7u);
    EXPECT_EQ(w >> 32 & 0xfff, 4095u);
    EXPECT_EQ(w >> 44 & 0xfff, 2u);
    EXPECT_EQ(w >> 56, 255u);
    const uint64_t p = raw::glassMemPoolWord(88, 4, 1, 24, 2, 65541);
    EXPECT_EQ(p & 0xfff, 88u);
    EXPECT_EQ(p >> 12 & 0xff, 4u);
    EXPECT_EQ(p >> 20 & 0xff, 1u);
    EXPECT_EQ(p >> 28 & 0xfff, 24u);
    EXPECT_EQ(p >> 40 & 0xff, 2u);
    EXPECT_EQ(p >> 48, 5u);
}
