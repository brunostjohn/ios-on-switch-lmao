// SPDX-License-Identifier: MIT
// raw_xr_planes.h: the copy-engine launches a merge and a split emit, read back as methods and run over a byte memory with the
// copy engine's remap, put every byte of an RGB10A8_2P_XR10 surface's planes where raw's form of the format holds it.
#include "../raw/runtime/raw_xr_planes.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <vector>

#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

using Side = raw::XrPlaneSide;

struct Memory {
    std::map<uint64_t, std::vector<uint8_t>> regions;

    std::vector<uint8_t> &add(uint64_t va, uint64_t bytes, uint8_t fill)
    {
        return regions[va] = std::vector<uint8_t>(bytes, fill);
    }

    uint8_t *at(uint64_t va, uint32_t bytes)
    {
        auto it = regions.upper_bound(va);
        if (it == regions.begin())
            return nullptr;
        --it;
        if (va + bytes > it->first + it->second.size())
            return nullptr;
        return it->second.data() + (va - it->first);
    }
};

// MAXWELL_DMA_COPY_A's remapped multi-line copy (clb0b5.h), the methods the kit's copy writes
struct CopyEngine {
    explicit CopyEngine(Memory &m) : memory(m)
    {}

    Memory &memory;
    std::map<uint32_t, uint32_t> state;
    std::vector<uint32_t> launches;

    uint32_t get(uint32_t method)
    {
        return state[method];
    }

    uint64_t offset(bool source, uint32_t launch, uint32_t x, uint32_t y, uint32_t bpp)
    {
        const uint64_t base = uint64_t(get(source ? NVB0B5_OFFSET_IN_UPPER : NVB0B5_OFFSET_OUT_UPPER)) << 32 |
                              get(source ? NVB0B5_OFFSET_IN_LOWER : NVB0B5_OFFSET_OUT_LOWER);
        if (launch >> (source ? 7 : 8) & 1)
            return base + uint64_t(y) * get(source ? NVB0B5_PITCH_IN : NVB0B5_PITCH_OUT) + uint64_t(x) * bpp;
        const uint32_t block = get(source ? NVB0B5_SET_SRC_BLOCK_SIZE : NVB0B5_SET_DST_BLOCK_SIZE);
        const uint32_t width = get(source ? NVB0B5_SET_SRC_WIDTH : NVB0B5_SET_DST_WIDTH);
        const uint32_t origin = get(source ? NVB0B5_SET_SRC_ORIGIN : NVB0B5_SET_DST_ORIGIN);
        EXPECT_EQ(get(source ? NVB0B5_SET_SRC_DEPTH : NVB0B5_SET_DST_DEPTH), 1u);
        EXPECT_EQ(get(source ? NVB0B5_SET_SRC_LAYER : NVB0B5_SET_DST_LAYER), 0u);
        return base + tgm_layout_offset_of(width * bpp, uint8_t(block >> 4 & 15), ((origin & 0xffff) + x) * bpp,
                                           (origin >> 16) + y);
    }

    void launch(uint32_t value)
    {
        launches.push_back(value);
        ASSERT_TRUE(value >> 10 & 1) << "remap enabled";
        ASSERT_TRUE(value >> 9 & 1) << "multi-line";
        const uint32_t remap = get(NVB0B5_SET_REMAP_COMPONENTS);
        const uint32_t size = (remap >> 16 & 3) + 1, from = (remap >> 20 & 3) + 1, to = (remap >> 24 & 3) + 1;
        const uint32_t constA = get(NVB0B5_SET_REMAP_CONST_A), constB = get(NVB0B5_SET_REMAP_CONST_B);
        for (uint32_t y = 0; y < get(NVB0B5_LINE_COUNT); y++)
            for (uint32_t x = 0; x < get(NVB0B5_LINE_LENGTH_IN); x++) {
                const uint8_t *s = memory.at(offset(true, value, x, y, size * from), size * from);
                uint8_t *d = memory.at(offset(false, value, x, y, size * to), size * to);
                ASSERT_TRUE(s && d) << "texel (" << x << ", " << y << ") inside its memory";
                for (uint32_t c = 0; c < to; c++) {
                    const uint32_t select = remap >> 4 * c & 7;
                    if (select == NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE)
                        continue;
                    if (select == NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A ||
                        select == NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_B) {
                        const uint32_t k = select == NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A ? constA : constB;
                        std::memcpy(d + c * size, &k, size);
                        continue;
                    }
                    ASSERT_LT(select, from);
                    std::memcpy(d + c * size, s + select * size, size);
                }
            }
    }

    void run(const std::vector<uint32_t> &words)
    {
        std::vector<switch_gm20b_method> methods(words.size());
        const long n = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(n, 0);
        for (long i = 0; i < n; i++) {
            ASSERT_EQ(methods[i].subchannel, raw::SUBC_COPY);
            state[methods[i].method] = methods[i].value;
            if (methods[i].method == NVB0B5_LAUNCH_DMA)
                launch(methods[i].value);
        }
    }
};

struct Surface {
    uint32_t width, height, wordPitch, alphaPitch;
    tgm_layout image{};
    uint32_t tempPitch;
    static constexpr uint64_t kWord = 0x1'0000'0000, kAlpha = 0x1'2000'0000, kImage = 0x2'0000'0000,
                              kTemp = 0x3'0000'0000;

    Surface(uint32_t w, uint32_t h, uint32_t wordPad, uint32_t alphaPad)
        : width(w), height(h), wordPitch(w * 4 + wordPad), alphaPitch(w + alphaPad), tempPitch((w * 2 + 255) & ~255u)
    {
        tgm_layout_tiled(w, h, 8, &image);
    }

    std::array<raw::CopySurface, 4> sides() const
    {
        return {raw::CopySurface{kImage, image.row_bytes, 0, 0, height, true, image.y_log2},
                raw::CopySurface{kWord, wordPitch}, raw::CopySurface{kAlpha, alphaPitch},
                raw::CopySurface{kTemp, tempPitch}};
    }

    std::vector<uint32_t> push(bool split) const
    {
        std::vector<uint32_t> words(raw::kXrPlaneWords);
        raw::Push p{words.data(), words.data() + words.size()};
        raw::emitXrPlanes(p, sides(), width, height, split);
        words.resize(size_t(p.at - words.data()));
        return words;
    }

    uint8_t *texel(Memory &m, uint32_t x, uint32_t y) const
    {
        return m.at(kImage + tgm_layout_offset(&image, x * 8, y), 8);
    }
};

uint32_t wordAt(uint32_t x, uint32_t y)
{
    return (x * 37 + y * 1009) % 1024 << 20 | (x * 11 + y * 7 + 5) % 1024 << 10 | (x ^ y * 3) % 1024;
}

uint8_t alphaAt(uint32_t x, uint32_t y)
{
    return uint8_t(x * 29 + y * 13 + 1);
}

void fillPlanes(Memory &m, const Surface &s, uint8_t pad)
{
    auto &word = m.add(Surface::kWord, uint64_t(s.wordPitch) * s.height, pad);
    auto &alpha = m.add(Surface::kAlpha, uint64_t(s.alphaPitch) * s.height, pad);
    for (uint32_t y = 0; y < s.height; y++)
        for (uint32_t x = 0; x < s.width; x++) {
            const uint32_t w = wordAt(x, y);
            std::memcpy(&word[uint64_t(y) * s.wordPitch + x * 4], &w, 4);
            alpha[uint64_t(y) * s.alphaPitch + x] = alphaAt(x, y);
        }
}

const Surface kSurfaces[] = {Surface(256, 256, 0, 0), Surface(37, 19, 12, 11), Surface(160, 72, 64, 48),
                             Surface(1, 1, 0, 15)};

TEST(XrPlanes, MergeWritesRawsFormOfThePlanesOverAnyOldImage)
{
    for (const Surface &s : kSurfaces) {
        SCOPED_TRACE(::testing::Message() << s.width << "x" << s.height);
        Memory m;
        fillPlanes(m, s, 0xee);
        m.add(Surface::kImage, s.image.bytes, 0xa5);
        m.add(Surface::kTemp, uint64_t(s.tempPitch) * s.height, 0x5a);
        CopyEngine ce(m);
        ce.run(s.push(false));
        ASSERT_EQ(ce.launches.size(), 3u);
        for (uint32_t y = 0; y < s.height; y++)
            for (uint32_t x = 0; x < s.width; x++) {
                const uint8_t *t = s.texel(m, x, y);
                uint32_t word, alpha;
                std::memcpy(&word, t, 4);
                std::memcpy(&alpha, t + 4, 4);
                ASSERT_EQ(word, wordAt(x, y)) << x << "," << y;
                ASSERT_EQ(alpha, alphaAt(x, y)) << x << "," << y;
            }
    }
}

TEST(XrPlanes, SplitWritesTheImageIntoThePlanesAndNothingElse)
{
    for (const Surface &s : kSurfaces) {
        SCOPED_TRACE(::testing::Message() << s.width << "x" << s.height);
        Memory m;
        fillPlanes(m, s, 0xee);
        m.add(Surface::kImage, s.image.bytes, 0);
        m.add(Surface::kTemp, uint64_t(s.tempPitch) * s.height, 0x5a);
        for (uint32_t y = 0; y < s.height; y++)
            for (uint32_t x = 0; x < s.width; x++) {
                uint8_t *t = s.texel(m, x, y);
                const uint32_t word = wordAt(y, x + 3) & 0x3fffffff, alpha = uint8_t(alphaAt(y, x) ^ 0x5c);
                std::memcpy(t, &word, 4);
                std::memcpy(t + 4, &alpha, 4);
            }
        CopyEngine ce(m);
        ce.run(s.push(true));
        ASSERT_EQ(ce.launches.size(), 3u);
        const auto &word = m.regions[Surface::kWord], &alpha = m.regions[Surface::kAlpha];
        for (uint32_t y = 0; y < s.height; y++) {
            for (uint32_t x = 0; x < s.width; x++) {
                uint32_t w;
                std::memcpy(&w, &word[uint64_t(y) * s.wordPitch + x * 4], 4);
                ASSERT_EQ(w, wordAt(y, x + 3) & 0x3fffffff) << x << "," << y;
                ASSERT_EQ(alpha[uint64_t(y) * s.alphaPitch + x], uint8_t(alphaAt(y, x) ^ 0x5c)) << x << "," << y;
            }
            for (uint32_t b = s.width * 4; b < s.wordPitch; b++)
                ASSERT_EQ(word[uint64_t(y) * s.wordPitch + b], 0xee) << "word row padding " << y;
            for (uint32_t b = s.width; b < s.alphaPitch; b++)
                ASSERT_EQ(alpha[uint64_t(y) * s.alphaPitch + b], 0xee) << "alpha row padding " << y;
        }
    }
}

TEST(XrPlanes, MergeThenSplitGivesThePlanesBack)
{
    const Surface s(96, 40, 32, 32);
    Memory m;
    fillPlanes(m, s, 0x11);
    const auto word = m.regions[Surface::kWord], alpha = m.regions[Surface::kAlpha];
    m.add(Surface::kImage, s.image.bytes, 0xc3);
    m.add(Surface::kTemp, uint64_t(s.tempPitch) * s.height, 0x3c);
    CopyEngine ce(m);
    ce.run(s.push(false));
    m.regions[Surface::kWord].assign(word.size(), 0x11);
    m.regions[Surface::kAlpha].assign(alpha.size(), 0x11);
    ce.run(s.push(true));
    EXPECT_EQ(m.regions[Surface::kWord], word);
    EXPECT_EQ(m.regions[Surface::kAlpha], alpha);
}

// Each launch is NON_PIPELINED (the temp is read after a launch wrote it; the image's halves come from two launches), the
// merge's constant is zero, and the word moves with the remap a 4-byte plane needs
TEST(XrPlanes, LaunchesAreOrderedRemappedCopies)
{
    for (const bool split : {false, true}) {
        const Surface s(64, 64, 0, 0);
        Memory m;
        fillPlanes(m, s, 0);
        m.add(Surface::kImage, s.image.bytes, 0);
        m.add(Surface::kTemp, uint64_t(s.tempPitch) * s.height, 0);
        CopyEngine ce(m);
        ce.state[NVB0B5_SET_REMAP_CONST_A] = 0xff;
        ce.run(s.push(split));
        EXPECT_EQ(ce.get(NVB0B5_SET_REMAP_CONST_A), 0u);
        for (const uint32_t launch : ce.launches)
            EXPECT_EQ(launch & 3, uint32_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
    }
    using raw::kXrRemapNoWrite;
    EXPECT_EQ(raw::xrPlaneTransfers(false)[1].remap,
              raw::xrRemap(0, kXrRemapNoWrite, kXrRemapNoWrite, kXrRemapNoWrite, 4, 1, 2));
    EXPECT_EQ(raw::xrPlaneTransfers(false)[1].source, Side::word);
    EXPECT_EQ(raw::xrPlaneTransfers(true)[0].destination, Side::word);
}

TEST(XrPlanes, BytesATexel)
{
    EXPECT_EQ(raw::xrPlaneBytes(false), 3u + 12u + 10u);
    EXPECT_EQ(raw::xrPlaneBytes(true), 12u + 10u + 3u);
}

uint32_t outputs(std::initializer_list<std::pair<unsigned, uint32_t>> targets)
{
    uint32_t word = 0;
    for (const auto &[slot, bits] : targets)
        word |= bits << 4 * slot;
    return word;
}

TEST(XrPlanes, AlphaOutputIsTheLowestFreeSlotsRed)
{
    uint32_t sph[20] = {};
    sph[18] = outputs({{0, 0x1}, {1, 0x1}});
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x01, 0), 1);
    sph[18] = outputs({{0, 0x3}});
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x01, 0), -1) << "today's form: the word and the alpha in one target";
    sph[18] = outputs({{0, 0x1}, {1, 0xf}, {2, 0x1}});
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x03, 0), 2) << "slot 1 bound: the alpha goes to slot 2";
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x03, 1), -1) << "slot 1's word is not red alone";
    sph[18] = outputs({{2, 0x1}, {0, 0x1}});
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x04, 2), 0);
    sph[18] = outputs({{0, 0x1}});
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x01, 0), -1) << "no alpha output";
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0x00, 0), -1) << "no attachment at the slot";
    sph[18] = 0x11111111;
    EXPECT_EQ(raw::xrAlphaOutput(sph, 0xff, 0), -1) << "every slot bound";
}

}  // namespace
