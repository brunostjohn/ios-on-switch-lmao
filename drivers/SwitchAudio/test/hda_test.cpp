// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>
#include "../SwitchHDAFormat.h"
#include "../hal/SwitchAudioConvert.h"

namespace {
using Edid = std::array<uint8_t, 384>;

Edid sink()
{
    Edid edid{};
    edid[126] = 1;
    edid[128] = 2;
    edid[129] = 3;
    edid[130] = 8;
    edid[132] = 0x23;
    edid[133] = 0x09;
    edid[134] = 4;
    edid[135] = 5;
    return edid;
}

TEST(HdaSink, RequiresStereo48kLpcmOrBasicAudio)
{
    auto edid = sink();
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 5u);
    edid[134] = 2;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
    edid[131] = 0x40;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 1u);
    edid[131] = 0;
    edid[134] = 4;
    edid[133] = 0x08;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
    edid[133] = 0x11;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
}

TEST(HdaSink, RespectsProvidedExtensionAndDataBlockBounds)
{
    auto edid = sink();
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 1), 0u);
    EXPECT_EQ(switch_hda_sink_formats(nullptr, 2), 0u);
    edid[130] = 7;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
    edid[130] = 255;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
    edid[130] = 8;
    edid[126] = 0;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 2), 0u);
}

TEST(HdaEld, CarriesMonitorIdentityAndActualSadsOnDisplayPort)
{
    auto edid = sink();
    edid[8] = 0x12;
    edid[9] = 0x34;
    edid[10] = 0x56;
    edid[11] = 0x78;
    edid[57] = 0xfc;
    std::memcpy(edid.data() + 59, "Monitor\n", 8);
    std::array<uint8_t, 130> buffer;
    buffer.fill(0xa5);
    uint8_t *eld = buffer.data() + 1;
    EXPECT_EQ(switch_hda_eld(edid.data(), 2, eld), 32u);
    EXPECT_EQ(buffer.front(), 0xa5);
    EXPECT_EQ(buffer.back(), 0xa5);
    EXPECT_EQ(eld[0], 16);
    EXPECT_EQ(eld[2], 7);
    EXPECT_EQ(eld[4], 0x67);
    EXPECT_EQ(eld[5], 0x14);
    EXPECT_EQ(eld[7], 1);
    EXPECT_EQ(std::memcmp(eld + 16, edid.data() + 8, 4), 0);
    EXPECT_EQ(std::memcmp(eld + 20, "Monitor", 7), 0);
    EXPECT_EQ(std::memcmp(eld + 27, edid.data() + 133, 3), 0);
    EXPECT_EQ(eld[31], 0);
}

TEST(HdaEld, BasicAudioHasItsMandatory16BitLpcmDescriptor)
{
    auto edid = sink();
    edid[130] = 0;
    edid[131] = 0x40;
    uint8_t eld[128];
    EXPECT_EQ(switch_hda_eld(edid.data(), 2, eld), 24u);
    EXPECT_EQ(eld[5], 0x14);
    EXPECT_EQ(eld[20], 9);
    EXPECT_EQ(eld[21], 7);
    EXPECT_EQ(eld[22], 1);
    EXPECT_EQ(eld[23], 0);
}

TEST(HdaEld, LimitsDescriptorsAcrossExtensions)
{
    auto edid = sink();
    edid[126] = 2;
    for (unsigned block = 1; block <= 2; ++block) {
        const unsigned base = block * 128;
        edid[base] = 2;
        edid[base + 1] = 3;
        edid[base + 2] = 35;
        edid[base + 4] = 0x3e;
        for (unsigned sad = 0; sad < 10; ++sad) {
            edid[base + 5 + sad * 3] = 9;
            edid[base + 6 + sad * 3] = 4;
            edid[base + 7 + sad * 3] = 5;
        }
    }
    uint8_t eld[128];
    EXPECT_EQ(switch_hda_eld(edid.data(), 3, eld), 68u);
    EXPECT_EQ(eld[5], 0xf4);
    EXPECT_EQ(eld[64], 5);
    EXPECT_EQ(eld[65], 0);
}

TEST(HdaProtocol, Encodes48kStereoAnd16BitVerbPayloads)
{
    EXPECT_EQ(switch_hda_stream_format(16), 0x11);
    EXPECT_EQ(switch_hda_stream_format(24), 0x31);
    EXPECT_EQ(switch_hda_verb(2, 3, 0x200, 0x31), 0x20320031u);
    EXPECT_EQ(switch_hda_verb(0, 7, 0xf00, 9), 0x007f0009u);
}

TEST(HdaMix, Saturates16BitFloatSamplesIncludingTailAndNonFiniteValues)
{
    int16_t ring[] = {100, -100, 0, 0, 0, 10, -10, 32760, -32760};
    const float samples[] = {0.5f, -0.5f, INFINITY, -INFINITY, NAN, 0, 0, 0.5f, -0.5f};
    switch_audio_mix_s16(ring, samples, 9);
    const int16_t expected[] = {16484, -16484, 32767, -32768, 0, 10, -10, 32767, -32768};
    EXPECT_EQ(std::memcmp(ring, expected, sizeof(ring)), 0);
}
}  // namespace
