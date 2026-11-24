// SPDX-License-Identifier: BSD-2-Clause-Patent
// Behavioral host-model tests for Tegra210 AHUB hardware offload engines (AXBAR, SFC, MIXER1, MVC1, OPE1 [PEQ1 + MBDRC1]).
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../SwitchAudioAhub.h"

namespace {

using namespace switch_audio_ahub;

struct RamCapture {
    bool active = false;
    uint32_t addr = 0;
    std::vector<uint32_t> words;
};

struct AhubModel {
    static constexpr uintptr_t kBase = 0x10000000u;
    std::unordered_map<uint32_t, uint32_t> regs;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    bool freezeSoftReset = false;
    bool freezeStatus = false;
    bool freezeMvcSwitch = false;

    RamCapture sfc1Ram;
    RamCapture sfc2Ram;
    RamCapture mixerGainRam;
    RamCapture mvcRam;
    RamCapture peqGainRam;
    RamCapture peqShiftRam;
    std::array<RamCapture, kMbdrcBands> mbdrcRam;

    void reset()
    {
        regs.clear();
        writes.clear();
        freezeSoftReset = false;
        freezeStatus = false;
        freezeMvcSwitch = false;
        sfc1Ram = {};
        sfc2Ram = {};
        mixerGainRam = {};
        mvcRam = {};
        peqGainRam = {};
        peqShiftRam = {};
        mbdrcRam = {};
    }

    static volatile uint8_t *basePtr()
    {
        return reinterpret_cast<volatile uint8_t *>(kBase);
    }

    static bool isSoftResetReg(uint32_t off)
    {
        return off == kSfc1Base + kSfcSoftReset || off == kSfc2Base + kSfcSoftReset ||
               off == kMixer1Base + kMixerRxSoftReset || off == kMixer1Base + kMixerTx1SoftReset ||
               off == kMvc1Base + kMvcSoftReset || off == kOpe1Base + kOpeSoftReset;
    }

    static void startRam(RamCapture &ram, uint32_t ctrl, uint32_t capacity)
    {
        if (ram.words.size() < capacity) {
            ram.words.assign(capacity, 0u);
        }
        ram.active = (ctrl & kRamCtrlRwWrite) != 0u;
        ram.addr = ctrl & 0xfffu;
    }

    static void writeRam(RamCapture &ram, uint32_t value)
    {
        if (!ram.active) {
            return;
        }
        if (ram.addr < ram.words.size()) {
            ram.words[ram.addr] = value;
        }
        ++ram.addr;
    }

    uint32_t read32(uint32_t off)
    {
        if (off == kSfc1Base + kSfcStatus) {
            return freezeStatus ? 1u : regs[kSfc1Base + kSfcEnable];
        }
        if (off == kSfc2Base + kSfcStatus) {
            return freezeStatus ? 1u : regs[kSfc2Base + kSfcEnable];
        }
        if (off == kMixer1Base + kMixerTx1Status) {
            return freezeStatus ? 1u : regs[kMixer1Base + kMixerTx1Enable];
        }
        if (off == kMvc1Base + kMvcStatus) {
            return freezeStatus ? 1u : regs[kMvc1Base + kMvcEnable];
        }
        if (off == kOpe1Base + kOpeStatus) {
            return freezeStatus ? 1u : regs[kOpe1Base + kOpeEnable];
        }
        if (isSoftResetReg(off)) {
            const uint32_t v = regs[off];
            if (!freezeSoftReset && (v & 1u)) {
                regs[off] = 0u;
            }
            return v;
        }
        if (off == kMvc1Base + kMvcSwitch) {
            const uint32_t v = regs[off];
            if (!freezeMvcSwitch && v) {
                regs[off] = 0u;
            }
            return v;
        }
        return regs[off];
    }

    void write32(uint32_t off, uint32_t value)
    {
        writes.emplace_back(off, value);
        regs[off] = value;

        if (off == kSfc1Base + kSfcCfgRamCtrl) {
            startRam(sfc1Ram, value, kSfcCoefDepth);
        } else if (off == kSfc1Base + kSfcCfgRamData) {
            writeRam(sfc1Ram, value);
        } else if (off == kSfc2Base + kSfcCfgRamCtrl) {
            startRam(sfc2Ram, value, kSfcCoefDepth);
        } else if (off == kSfc2Base + kSfcCfgRamData) {
            writeRam(sfc2Ram, value);
        } else if (off == kMixer1Base + kMixerGainRamCtrl) {
            startRam(mixerGainRam, value, 16u);
        } else if (off == kMixer1Base + kMixerGainRamData) {
            writeRam(mixerGainRam, value);
        } else if (off == kMvc1Base + kMvcCfgRamCtrl) {
            startRam(mvcRam, value, 9u);
        } else if (off == kMvc1Base + kMvcCfgRamData) {
            writeRam(mvcRam, value);
        } else if (off == kPeq1Base + kPeqRamCtrl) {
            startRam(peqGainRam, value, 2u * kPeqGainWordsPerCh);
        } else if (off == kPeq1Base + kPeqRamData) {
            writeRam(peqGainRam, value);
        } else if (off == kPeq1Base + kPeqShiftCtrl) {
            startRam(peqShiftRam, value, 2u * kPeqShiftWordsPerCh);
        } else if (off == kPeq1Base + kPeqShiftData) {
            writeRam(peqShiftRam, value);
        } else {
            for (uint32_t b = 0; b < kMbdrcBands; ++b) {
                if (off == kMbdrc1Base + kMbdrcRamCtrl + 4u * b) {
                    startRam(mbdrcRam[b], value, kMbdrcWordsPerBand);
                } else if (off == kMbdrc1Base + kMbdrcRamData + 4u * b) {
                    writeRam(mbdrcRam[b], value);
                }
            }
        }
    }

    size_t countWrites(uint32_t off) const
    {
        size_t n = 0;
        for (const auto &[reg, val] : writes) {
            (void)val;
            if (reg == off) {
                ++n;
            }
        }
        return n;
    }
};

AhubModel gModel;

class AhubTest : public testing::Test {
protected:
    void SetUp() override
    {
        gModel.reset();
    }
};

}  // namespace

extern "C" {

uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gModel.read32(static_cast<uint32_t>(address - AhubModel::kBase));
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gModel.write32(static_cast<uint32_t>(address - AhubModel::kBase), value);
}

uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write8(uintptr_t, uint8_t)
{}

}  // extern "C"

namespace {

TEST_F(AhubTest, XbarPlaybackAndCaptureRoutingAreOneHot)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    auto expectConn = [](uint32_t dst, XbarSource expected) {
        for (uint32_t part = 0; part < 3u; ++part) {
            const uint32_t off = kXbarBase + dst + part * kXbarPartStride;
            EXPECT_EQ(gModel.regs[off], part == expected.part ? expected.mask : 0u)
                << "dst=" << dst << " part=" << part;
        }
    };

    xbarRoutePlayback(ahub);
    expectConn(kXbarDstMixer1Rx1, kSrcAdmaif1);
    expectConn(kXbarDstSfc1, kSrcMixer1Tx1);
    expectConn(kXbarDstMvc1, kSrcSfc1);
    expectConn(kXbarDstOpe1, kSrcMvc1);
    expectConn(kXbarDstI2s1, kSrcOpe1);

    xbarRouteCapture(ahub, true);
    expectConn(kXbarDstSfc2, kSrcI2s1);
    expectConn(kXbarDstAdmaif1, kSrcSfc2);

    xbarRouteCapture(ahub, false);
    expectConn(kXbarDstSfc2, kSrcNone);
    expectConn(kXbarDstAdmaif1, kSrcI2s1);
}

TEST_F(AhubTest, SfcSupportsAllAdvertisedRatesAndProgramsBypassVsRam)
{
    volatile uint8_t *ahub = AhubModel::basePtr();

    constexpr uint32_t kRates48[] = {48000u, 96000u, 32000u, 24000u, 16000u, 8000u};
    for (uint32_t rate : kRates48) {
        EXPECT_TRUE(sfcSupportsPair(rate, 48000u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(48000u, rate)) << rate;
    }
    constexpr uint32_t kRates44[] = {44100u, 88200u, 22050u, 11025u};
    for (uint32_t rate : kRates44) {
        EXPECT_TRUE(sfcSupportsPair(rate, 44100u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(44100u, rate)) << rate;
        EXPECT_TRUE(sfcSupportsPair(rate, 48000u)) << rate;
        EXPECT_TRUE(sfcSupportsPair(48000u, rate)) << rate;
    }
    EXPECT_FALSE(sfcSupportsPair(64000u, 48000u));
    EXPECT_FALSE(sfcSupportsPair(48000u, 64000u));
    EXPECT_FALSE(sfcSupportsPair(0u, 48000u));
    EXPECT_EQ(sfcCoefficients(48000u, 48000u), nullptr);
    EXPECT_NE(sfcCoefficients(96000u, 48000u), nullptr);

    ASSERT_TRUE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 0u);
    EXPECT_EQ(gModel.countWrites(kSfc1Base + kSfcCfgRamData), 0u);

    gModel.reset();
    ASSERT_TRUE(sfcConfigure(ahub, kSfc1Base, 96000u, 48000u, 2u));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcRxFreq], 10u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcTxFreq], 7u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 1u);
    EXPECT_EQ(gModel.countWrites(kSfc1Base + kSfcCfgRamData), kSfcCoefDepth);
    ASSERT_EQ(gModel.sfc1Ram.words.size(), kSfcCoefDepth);
    const uint32_t *const expected = sfcCoefficients(96000u, 48000u);
    ASSERT_NE(expected, nullptr);
    for (uint32_t i = 0; i < kSfcCoefDepth; ++i) {
        EXPECT_EQ(gModel.sfc1Ram.words[i], expected[i]) << "word " << i;
    }

    sfcEnable(ahub, kSfc1Base);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcEnable], 1u);
    ASSERT_TRUE(sfcStop(ahub, kSfc1Base));
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcEnable], 0u);
    EXPECT_EQ(gModel.regs[kSfc1Base + kSfcCoefRam], 0u);
}

TEST_F(AhubTest, MixerUnityGainRamAndGlobalEnable)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    ASSERT_TRUE(mixerConfigure(ahub, 2u));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Cif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1AdderCfg], 1u);
    EXPECT_NE(gModel.regs[kMixer1Base + kMixerRxCtrl] & kMixerRxSampleCountEn, 0u);
    ASSERT_EQ(gModel.mixerGainRam.words.size(), 16u);
    EXPECT_EQ(gModel.mixerGainRam.words[7], 0x01000000u);
    EXPECT_EQ(gModel.mixerGainRam.words[9], kMixerGainUnityQ16);
    EXPECT_EQ(gModel.mixerGainRam.words[15], 1u);

    mixerEnable(ahub);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Enable], 1u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerGlobalEnable], 1u);

    gModel.regs[kMixer1Base + kMixerRxSampleCount] = 0x12345678u;
    EXPECT_EQ(mixerSampleCount(ahub), 0x12345678u);

    ASSERT_TRUE(mixerStop(ahub));
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerTx1Enable], 0u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerGlobalEnable], 0u);
    EXPECT_EQ(gModel.regs[kMixer1Base + kMixerRxCtrl] & kMixerRxSampleCountEn, 0u);
}

TEST_F(AhubTest, MvcLinearRampVolumeMuteAndPeak)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    ASSERT_TRUE(mvcConfigure(ahub, 2u, -6000, false));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcRxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTxCif], switch_audio_ahub_cif(2u));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcDuration], 720u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcDurationInv], (1u << 31) / 720u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPolyN1], 16u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPolyN2], 63u);
    EXPECT_EQ(gModel.countWrites(kMvc1Base + kMvcCfgRamData), 9u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kMvcCtrlCurveLinear | kMvcCtrlRoundingUnbias);
    const uint32_t minus6dB = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(-6000));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcInitVol], minus6dB);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol], minus6dB);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcPeakCtrl], 0x12c0u);

    mvcEnable(ahub);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcEnable], 1u);

    // Smooth ramp updates TARGET_VOL only, leaving INIT_VOL untouched.
    const size_t initWritesBefore = gModel.countWrites(kMvc1Base + kMvcInitVol);
    ASSERT_TRUE(mvcSetVolume(ahub, -12000, true, false));
    EXPECT_EQ(gModel.countWrites(kMvc1Base + kMvcInitVol), initWritesBefore);
    const uint32_t minus12dB = static_cast<uint32_t>(switch_audio_mvc_linear_db_q8(-12000));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol], minus12dB);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol + 4u], minus12dB);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kMvcCtrlCurveLinear | kMvcCtrlRoundingUnbias | kMvcCtrlMuteMask);

    // Immediate update writes both INIT_VOL and TARGET_VOL.
    ASSERT_TRUE(mvcSetVolume(ahub, 0, false, true));
    EXPECT_GT(gModel.countWrites(kMvc1Base + kMvcInitVol), initWritesBefore);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcInitVol], 0u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcTargetVol], 0u);
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcCtrl], kMvcCtrlCurveLinear | kMvcCtrlRoundingUnbias);

    gModel.regs[kMvc1Base + kMvcPeakValue] = 0x00100000u;
    gModel.regs[kMvc1Base + kMvcPeakValue + 4u] = 0x00250000u;
    EXPECT_EQ(mvcReadPeak(ahub), 0x00250000u);

    ASSERT_TRUE(mvcStop(ahub));
    EXPECT_EQ(gModel.regs[kMvc1Base + kMvcEnable], 0u);
}

TEST_F(AhubTest, OpeSpeakerDspAndHeadphoneBypass)
{
    volatile uint8_t *ahub = AhubModel::basePtr();

    // Headphone mode: clean bypass with zero PEQ/MBDRC RAM writes.
    ASSERT_TRUE(opeConfigure(ahub, 48000u, false, 2u));
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeDir], kOpeDirPeqToMbdrc);
    EXPECT_EQ(gModel.regs[kPeq1Base + kPeqCfg], kPeqCfgUnbias);
    EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcCfg], 0u);
    EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqRamData), 0u);
    EXPECT_EQ(gModel.countWrites(kMbdrc1Base + kMbdrcRamData), 0u);
    // Standalone PEQ_SOFT_RESET (+0x04) must never be written.
    EXPECT_EQ(gModel.countWrites(kPeq1Base + 0x04u), 0u);

    // Speaker mode at 48 kHz and 44.1 kHz.
    for (uint32_t linkRate : {48000u, 44100u}) {
        gModel.reset();
        ASSERT_TRUE(opeConfigure(ahub, linkRate, true, 2u)) << linkRate;
        EXPECT_EQ(gModel.countWrites(kPeq1Base + 0x04u), 0u);
        EXPECT_EQ(gModel.regs[kPeq1Base + kPeqCfg], (1u << kPeqCfgStagesShift) | kPeqCfgUnbias | kPeqCfgModeActive);
        EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqRamData), 2u * kPeqGainWordsPerCh);
        EXPECT_EQ(gModel.countWrites(kPeq1Base + kPeqShiftData), 2u * kPeqShiftWordsPerCh);
        ASSERT_EQ(gModel.peqGainRam.words.size(), 2u * kPeqGainWordsPerCh);
        ASSERT_EQ(gModel.peqShiftRam.words.size(), 2u * kPeqShiftWordsPerCh);

        // Verify per-channel shift table: pre=0, stage0=30 (Q2.30), stage1=29 (Q3.29), post=30.
        for (uint32_t ch = 0; ch < 2u; ++ch) {
            const uint32_t sBase = ch * kPeqShiftWordsPerCh;
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 0], 0u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 1], 30u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 2], 29u);
            EXPECT_EQ(gModel.peqShiftRam.words[sBase + 13], 30u);

            const uint32_t gBase = ch * kPeqGainWordsPerCh;
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 0]), 1);
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 61]), 862978048);
            const int32_t expectedB0 = linkRate == 44100u ? 1069423538 : 1069773750;
            EXPECT_EQ(static_cast<int32_t>(gModel.peqGainRam.words[gBase + 1]), expectedB0);
        }

        // Verify MBDRC: fixed FAST_FACTOR, little-endian thresholds, Band 2 offset-15 layout, active 3-band config.
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcChanMask], 3u);
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcFastFactor], switch_audio_mbdrc_fast_factor(0x3000u, 0x0800u));
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcInThresh], switch_audio_mbdrc_pack_thresh(180u, 120u, 24u, 12u));
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcOutThresh], switch_audio_mbdrc_pack_thresh(180u, 120u, 24u, 15u));
        EXPECT_EQ(gModel.regs[kMbdrc1Base + kMbdrcCfg], (1u << 15) | (30u << 8) | (5u << 4) | 3u);
        for (uint32_t band = 0; band < kMbdrcBands; ++band) {
            ASSERT_EQ(gModel.mbdrcRam[band].words.size(), kMbdrcWordsPerBand);
        }
        // Band 2 stages 0..2 are bypass {1<<30, 0, 0, 0, 0}, stage 3 begins at word 15.
        EXPECT_EQ(static_cast<int32_t>(gModel.mbdrcRam[2].words[0]), 1073741824);
        EXPECT_EQ(static_cast<int32_t>(gModel.mbdrcRam[2].words[15]), -619925131);
    }

    opeEnable(ahub);
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeEnable], 1u);
    ASSERT_TRUE(opeStop(ahub));
    EXPECT_EQ(gModel.regs[kOpe1Base + kOpeEnable], 0u);
}

TEST_F(AhubTest, NullAndTimeoutGuards)
{
    volatile uint8_t *ahub = AhubModel::basePtr();
    EXPECT_FALSE(sfcConfigure(nullptr, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_FALSE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 0u));
    EXPECT_FALSE(mixerConfigure(nullptr, 2u));
    EXPECT_FALSE(mvcConfigure(nullptr, 2u, 0, false));
    EXPECT_FALSE(opeConfigure(nullptr, 48000u, true, 2u));

    gModel.freezeSoftReset = true;
    EXPECT_FALSE(sfcConfigure(ahub, kSfc1Base, 48000u, 48000u, 2u));
    EXPECT_FALSE(mixerConfigure(ahub, 2u));
    EXPECT_FALSE(mvcConfigure(ahub, 2u, 0, false));
    EXPECT_FALSE(opeConfigure(ahub, 48000u, true, 2u));

    gModel.reset();
    gModel.freezeMvcSwitch = true;
    gModel.regs[kMvc1Base + kMvcSwitch] = kMvcSwitchVolume;
    EXPECT_FALSE(mvcSetVolume(ahub, -3000, false, false));
}

}  // namespace
