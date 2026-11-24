// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"
#ifndef SWITCH_MMIO_HOST_MODEL
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#endif

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kOpeBase = kOpe1Base;
constexpr uint32_t kPeqBase = kPeq1Base;
constexpr uint32_t kMbdrcBase = kMbdrc1Base;
constexpr uint32_t kTimeoutUs = 10000u;

// Switch OLED speaker PEQ: TRM Ch. 23.2.12 negates a1/a2. Stage 0 (40 Hz Butterworth HPF) uses Q2.30, stage 1
// (360 Hz +1.0 dB shelf) uses Q3.29 to avoid int32_t overflow, and post-gain is -1.90 dB (0x066e / 0x0800) in Q30.
constexpr int32_t kPeqPreGainUnity = 1;
constexpr int32_t kPeqPostGainOledQ30 = 862978048;
constexpr uint32_t kPeqTotalStages = 12u;
constexpr uint32_t kPeqActiveStages = 2u;

constexpr int32_t kPeqStage0_48k[5] = {1069773750, -2139547500, 1069773750, 2139532835, -1065820340};
constexpr int32_t kPeqStage1_48k[5] = {601021859, -1156269477, 556460727, 1030460294, -494802491};

constexpr int32_t kPeqStage0_44k[5] = {1069423538, -2138847077, 1069423538, 2138829710, -1065122620};
constexpr int32_t kPeqStage1_44k[5] = {600903488, -1152043835, 552572491, 1026682276, -491243509};

constexpr int32_t kPeqStageBypass[5] = {1, 0, 0, 0, 0};

constexpr uint32_t kPeqShiftTable[kPeqShiftWordsPerCh] = {
    0u, 30u, 29u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 30u,
};

// 3-band all-pass crossover biquads (shift 30; band 2 places stages 3..5 at word offset 15 per TRM p. 1859).
constexpr int32_t kMbdrcCrossover[kMbdrcBands][kMbdrcWordsPerBand] = {
    {
        961046798,   -2030431983, 1073741824,  2030431983,  -961046798, 1030244425,  -2099481453, 1073741824,
        2099481453,  -1030244425, 1067169294,  -2136327263, 1073741824, 2136327263,  -1067169294, 434951949,
        -1306567134, 1073741824,  1306567134,  -434951949,  780656019,  -1605955641, 1073741824,  1605955641,
        -780656019,  1024497031,  -1817128152, 1073741824,  1817128152, -1024497031, 1073741824,  0,
        0,           0,           0,           1073741824,  0,          0,           0,           0,
    },
    {
        -1005668963, 1073741824, 0,           1005668963,  0,          998437058,   -2067742187, 1073741824,
        2067742187,  -998437058, 1051963422,  -2121153948, 1073741824, 2121153948,  -1051963422, 434951949,
        -1306567134, 1073741824, 1306567134,  -434951949,  780656019,  -1605955641, 1073741824,  1605955641,
        -780656019,  1024497031, -1817128152, 1073741824,  1817128152, -1024497031, 1073741824,  0,
        0,           0,          0,           1073741824,  0,          0,           0,           0,
    },
    {
        1073741824, 0,         0,           0,          0,          1073741824,  0,          0,
        0,          0,         1073741824,  0,          0,          0,           0,          -619925131,
        1073741824, 0,         619925131,   0,          606839335,  -1455425976, 1073741824, 1455425976,
        -606839335, 917759617, -1724690840, 1073741824, 1724690840, -917759617,  1073741824, 0,
        0,          0,         0,           1073741824, 0,          0,           0,          0,
    },
};

constexpr uint32_t kMbdrcIirStagesMinus1 = 5u;
constexpr uint32_t kMbdrcInitGainUnityQ22 = 1u << 22;
constexpr uint32_t kMbdrcInAttackVal = 1044928780u;
constexpr uint32_t kMbdrcInReleaseVal = 138497695u;
constexpr uint32_t kMbdrcFastAttackVal = 2147483647u;
constexpr uint32_t kMbdrcGainAttackVal = 14268942u;
constexpr uint32_t kMbdrcGainReleaseVal = 1044928780u;
constexpr uint32_t kMbdrcFastReleaseVal = 2147480170u;

// RMS_OFFSET=0, unbias=1, PEAK mode=0, ALL_PASS_TREE=0, shift=30, frame_size=32 (5), MULTIBAND=3.
constexpr uint32_t kMbdrcCfgSpeaker = (1u << 15) | (30u << 8) | (5u << 4) | 3u;

bool waitReg(const volatile uint8_t *ahub, uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeoutUs)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    const uint32_t limit = timeoutUs < 1024u ? timeoutUs : 1024u;
    for (uint32_t i = 0; i < limit; ++i) {
        if ((switch_mmio_read32(ahub, offset) & mask) == value) {
            return true;
        }
    }
    return (switch_mmio_read32(ahub, offset) & mask) == value;
#else
    return switch_mmio_wait32(ahub, offset, mask, value, timeoutUs);
#endif
}

void writePeqStage(volatile uint8_t *ahub, const int32_t stage[5])
{
    for (uint32_t i = 0; i < 5u; ++i) {
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(stage[i]));
    }
}

bool configurePeqSpeaker(volatile uint8_t *ahub, uint32_t linkRate, uint32_t channels)
{
    const bool rate44k = switch_audio_is_44k_family(linkRate) != 0;
    const int32_t *const stage0 = rate44k ? kPeqStage0_44k : kPeqStage0_48k;
    const int32_t *const stage1 = rate44k ? kPeqStage1_44k : kPeqStage1_48k;

    for (uint32_t ch = 0; ch < channels; ++ch) {
        if (!waitReg(ahub, kPeqBase + kPeqRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqRamCtrl, kRamCtrlWriteSeq | (ch * kPeqGainWordsPerCh));
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(kPeqPreGainUnity));
        writePeqStage(ahub, stage0);
        writePeqStage(ahub, stage1);
        for (uint32_t s = kPeqActiveStages; s < kPeqTotalStages; ++s) {
            writePeqStage(ahub, kPeqStageBypass);
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqRamData, static_cast<uint32_t>(kPeqPostGainOledQ30));

        if (!waitReg(ahub, kPeqBase + kPeqShiftCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, kPeqBase + kPeqShiftCtrl, kRamCtrlWriteSeq | (ch * kPeqShiftWordsPerCh));
        for (const uint32_t shift : kPeqShiftTable) {
            switch_mmio_write32(ahub, kPeqBase + kPeqShiftData, shift);
        }
    }

    const uint32_t cfg = ((kPeqActiveStages - 1u) << kPeqCfgStagesShift) | kPeqCfgUnbias | kPeqCfgModeActive;
    switch_mmio_write32(ahub, kPeqBase + kPeqCfg, cfg);
    return true;
}

bool configureMbdrcSpeaker(volatile uint8_t *ahub, uint32_t channels)
{
    switch_mmio_write32(ahub, kMbdrcBase + kMbdrcChanMask, (1u << channels) - 1u);
    switch_mmio_write32(ahub, kMbdrcBase + kMbdrcMasterVol, 0u);
    switch_mmio_write32(ahub, kMbdrcBase + kMbdrcFastFactor, switch_audio_mbdrc_fast_factor(0x3000u, 0x0800u));

    const uint32_t inThresh = switch_audio_mbdrc_pack_thresh(180u, 120u, 24u, 12u);
    const uint32_t outThresh = switch_audio_mbdrc_pack_thresh(180u, 120u, 24u, 15u);

    for (uint32_t band = 0; band < kMbdrcBands; ++band) {
        const uint32_t off = 4u * band;
        if (!waitReg(ahub, kMbdrcBase + kMbdrcRamCtrl + off, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
            return false;
        }
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRamCtrl + off, kRamCtrlWriteSeq);
        for (const int32_t coeff : kMbdrcCrossover[band]) {
            switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRamData + off, static_cast<uint32_t>(coeff));
        }

        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcIirCfg + off, kMbdrcIirStagesMinus1);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcInThresh + off, inThresh);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcOutThresh + off, outThresh);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRatio1st + off, 4096u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRatio2nd + off, 4096u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRatio3rd + off, 4096u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRatio4th + off, 2048u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcRatio5th + off, 512u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcMakeupGain + off, 0u);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcInitGain + off, kMbdrcInitGainUnityQ22);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcInAttack + off, kMbdrcInAttackVal);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcInRelease + off, kMbdrcInReleaseVal);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcFastAttack + off, kMbdrcFastAttackVal);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcGainAttack + off, kMbdrcGainAttackVal);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcGainRelease + off, kMbdrcGainReleaseVal);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcFastRelease + off, kMbdrcFastReleaseVal);
    }

    switch_mmio_write32(ahub, kMbdrcBase + kMbdrcCfg, kMbdrcCfgSpeaker);
    return true;
}

}  // namespace

bool opeConfigure(volatile uint8_t *ahub, uint32_t linkRate, bool speakerActive, uint32_t channels)
{
    if (!ahub || channels < 1u || channels > 8u) {
        return false;
    }
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 0u);
    if (!waitReg(ahub, kOpeBase + kOpeStatus, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    // Reset OPE1 as a whole before programming (TRM Ch. 23.2.12 forbids standalone PEQ_SOFT_RESET).
    switch_mmio_write32(ahub, kOpeBase + kOpeSoftReset, 1u);
    if (!waitReg(ahub, kOpeBase + kOpeSoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, kOpeBase + kOpeRxCif, cif);
    switch_mmio_write32(ahub, kOpeBase + kOpeTxCif, cif);
    switch_mmio_write32(ahub, kOpeBase + kOpeDir, kOpeDirPeqToMbdrc);

    if (!speakerActive) {
        switch_mmio_write32(ahub, kPeqBase + kPeqCfg, kPeqCfgUnbias);
        switch_mmio_write32(ahub, kMbdrcBase + kMbdrcCfg, 0u);
        return true;
    }
    if (!configurePeqSpeaker(ahub, linkRate, channels)) {
        return false;
    }
    return configureMbdrcSpeaker(ahub, channels);
}

void opeEnable(volatile uint8_t *ahub)
{
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 1u);
}

bool opeStop(volatile uint8_t *ahub)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_write32(ahub, kOpeBase + kOpeEnable, 0u);
    return waitReg(ahub, kOpeBase + kOpeStatus, 1u, 0u, kTimeoutUs);
}

}  // namespace switch_audio_ahub
