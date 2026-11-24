// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchAudioAhub.h"
#ifndef SWITCH_MMIO_HOST_MODEL
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#endif

namespace switch_audio_ahub {
namespace {

constexpr uint32_t kBase = kMixer1Base;
constexpr uint32_t kTimeoutUs = 10000u;

// RX1 gain RAM entry: 9 linear ramp poly words, unity Q16 gain, instant duration {1,1,1,1}, reserved, trigger.
constexpr uint32_t kRx1UnityGainRam[16] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x01000000u, 0u, kMixerGainUnityQ16, 1u, 1u, 1u, 1u, 0u, 1u,
};

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

}  // namespace

bool mixerConfigure(volatile uint8_t *ahub, uint32_t channels)
{
    if (!ahub || channels < 1u || channels > 8u) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 0u);
    if (!waitReg(ahub, kBase + kMixerTx1Status, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 0u);
    switch_mmio_write32(ahub, kBase + kMixerRxSoftReset, 1u);
    if (!waitReg(ahub, kBase + kMixerRxSoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerTx1SoftReset, 1u);
    if (!waitReg(ahub, kBase + kMixerTx1SoftReset, 1u, 0u, kTimeoutUs)) {
        return false;
    }
    const uint32_t cif = switch_audio_ahub_cif(channels);
    switch_mmio_write32(ahub, kBase + kMixerRxCif, cif);
    switch_mmio_write32(ahub, kBase + kMixerTx1Cif, cif);
    switch_mmio_write32(ahub, kBase + kMixerTx1AdderCfg, 1u);
    const uint32_t rxCtrl = switch_mmio_read32(ahub, kBase + kMixerRxCtrl);
    switch_mmio_write32(ahub, kBase + kMixerRxCtrl, rxCtrl & ~kMixerRxSampleCountEn);
    switch_mmio_write32(ahub, kBase + kMixerRxCtrl, rxCtrl | kMixerRxSampleCountEn);
    if (!waitReg(ahub, kBase + kMixerGainRamCtrl, kRamCtrlReadBusy, 0u, kTimeoutUs)) {
        return false;
    }
    switch_mmio_write32(ahub, kBase + kMixerGainRamCtrl, kRamCtrlWriteSeq);
    for (const uint32_t word : kRx1UnityGainRam) {
        switch_mmio_write32(ahub, kBase + kMixerGainRamData, word);
    }
    return true;
}

void mixerEnable(volatile uint8_t *ahub)
{
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 1u);
    // MIXER_ENABLE resets to 0 in hardware and gates every RX/TX path.
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 1u);
}

bool mixerStop(volatile uint8_t *ahub)
{
    if (!ahub) {
        return false;
    }
    switch_mmio_rmw32(ahub, kBase + kMixerRxCtrl, kMixerRxSampleCountEn, 0u);
    switch_mmio_write32(ahub, kBase + kMixerTx1Enable, 0u);
    const bool stopped = waitReg(ahub, kBase + kMixerTx1Status, 1u, 0u, kTimeoutUs);
    switch_mmio_write32(ahub, kBase + kMixerGlobalEnable, 0u);
    return stopped;
}

// NOLINTNEXTLINE(misc-const-correctness): signature fixed in SwitchAudioAhub.h
uint32_t mixerSampleCount(volatile uint8_t *ahub)
{
    return switch_mmio_read32(ahub, kBase + kMixerRxSampleCount);
}

}  // namespace switch_audio_ahub
