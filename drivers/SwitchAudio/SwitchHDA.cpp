// SPDX-License-Identifier: GPL-2.0-only
// Tegra wrapper: Linux v4.9 sound/pci/hda/hda_tegra.c; rings and streams: Intel HD Audio 1.0a chapters 3 and 4.
#include "SwitchHDA.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchDMAOrder.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../TegraSMMU/TegraSMMU.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>

namespace {
constexpr uint64_t kHdaPA = 0x70030000;
constexpr uint32_t kBar = 0x8000, kRingBytes = SWITCH_AUDIO_RING_FRAMES * 8;
constexpr uint32_t kRirbOffset = 1024, kBdlOffset = 3072;

constexpr unsigned kClockHda = 125, kClockHda2Codec2x = 111, kClockHda2Hdmi = 128, kClockMaud = 202;
constexpr uint32_t CLK_SOURCE_HDA2CODEC_2X = 0x3e4, CLK_SOURCE_HDA = 0x428, CLK_SOURCE_MAUD = 0x6d4;

constexpr uint32_t HDA_FPCI_BAR0 = 0x80, kFpciBar0Start = 0x40;
constexpr uint32_t HDA_IPFS_CONFIG = 0x180, HDA_IPFS_INTR_MASK = 0x188;
constexpr uint32_t kIpfsEnFpci = 1u << 0, kIpfsCl2FastIo = 1u << 2, kIpfsEnSnoop = 1u << 6, kIpfsEnIntr = 1u << 16;
constexpr uint32_t HDA_CFG_CMD = 0x1004, HDA_CFG_BAR0 = 0x1010;
constexpr uint32_t kCfgCmdEnable = 0x107u, kCfgCmdIntDisable = 1u << 10, kCfgBar0Start = 1u << 14;

constexpr uint32_t GCAP = 0x00, GCTL = 0x08, INTCTL = 0x20, INTSTS = 0x24;
constexpr uint32_t CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4a;
constexpr uint32_t CORBCTL = 0x4c, CORBSTS = 0x4d, CORBSIZE = 0x4e;
constexpr uint32_t RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5a;
constexpr uint32_t RIRBCTL = 0x5c, RIRBSTS = 0x5d, RIRBSIZE = 0x5e;
constexpr uint32_t SD_BASE = 0x80, SD_STRIDE = 0x20;
constexpr uint32_t SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0c, SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1c;

constexpr uint32_t kGctlCrst = 1u << 0;
constexpr uint32_t kIntGie = 1u << 31, kIntCie = 1u << 30;
constexpr uint32_t kCorbRpReset = 0x8000u, kRirbWpReset = 0x8000u;
constexpr uint32_t kCorbStsError = 1u << 0, kRirbStsInt = 1u << 0, kRirbStsOverflow = 1u << 2;
constexpr uint32_t kRirbResponseUnsol = 1u << 4;
constexpr uint32_t kCorbCtlEnable = 0x03u, kRirbCtlEnable = 0x07u;
constexpr uint32_t kStreamReset = 1u << 0, kRun = 1u << 1, kStreamInterrupts = 0x1cu, kStreamErrors = 0x18u;
constexpr uint32_t kStreamStripe = 1u << 16, kStreamTag1 = 1u << 20;
constexpr uint32_t kPeriods = SWITCH_AUDIO_RING_FRAMES / SWITCH_HDA_PERIOD_FRAMES;
static_assert(kPeriods * 16 <= 1024);

uint32_t ringSize(uint32_t capabilities)
{
    if (capabilities & 0x40)
        return 2;
    if (capabilities & 0x20)
        return 1;
    return 0;
}
}  // namespace

uint32_t SwitchHDA::read(uint32_t offset) const
{
    return switch_mmio_read32(registers, offset);
}

void SwitchHDA::write(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(registers, offset, value);
}

uint32_t SwitchHDA::core(uint32_t offset) const
{
    const uint32_t value = read(kBar + (offset & ~3u));
    return value >> (8 * (offset & 3));
}

void SwitchHDA::coreWrite(uint32_t offset, uint32_t value)
{
    write(kBar + offset, value);
}

// Tegra's wrapper accepts DWORD accesses only. Control writes keep adjacent RW1C status bits zero.
void SwitchHDA::field(uint32_t offset, uint32_t mask, uint32_t value)
{
    const uint32_t shift = 8 * (offset & 3), aligned = offset & ~3u;
    uint32_t word = core(aligned);
    if (aligned == CORBCTL || aligned == RIRBCTL)
        word &= ~0xff00u;
    if (aligned == stream)
        word &= 0xffffffu;
    coreWrite(aligned, (word & ~(mask << shift)) | ((value & mask) << shift));
}

bool SwitchHDA::initialize(IOCommandGate *commandGate, SwitchAudioControl *page, uint64_t hostHz)
{
    gate = commandGate;
    control = page;
    control->magic = SWITCH_AUDIO_MAGIC;
    control->version = SWITCH_AUDIO_VERSION;
    control->rate = 48000;
    control->rates[0] = 48000;
    control->out_channels = 2;
    control->ring_frames = SWITCH_AUDIO_RING_FRAMES;
    control->period_frames = SWITCH_HDA_PERIOD_FRAMES;
    control->lead_frames = 2 * SWITCH_AUDIO_PERIOD_FRAMES;
    control->backend = SwitchAudioBackendTegraHDA;
    control->ticks_per_frame_q32 = (hostHz << 32) / 48000;
    return true;
}

bool SwitchHDA::prepareHardware()
{
    registers = switch_mmio_map(kHdaPA, 0x10000);
    if (!registers || !switch_dma_alloc(&commands, 4096, kSwitchDMABelow4G, kIOMapWriteCombineCache) ||
        !switch_dma_alloc(&tables, 4 * 4096))
        return false;
    const uint64_t outputPA = output->getPhysicalSegment(0, nullptr);
    if (!domain.init(tables.va, tables.pa, 4) || !domain.identity(commands.pa, commands.size, true) ||
        !domain.identity(outputPA, kRingBytes, false))
        return false;
    switch_dcache_clean(tables.va, tables.size);
    char why[160];
    if (!(tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_1) & (1u << 21)) ||
        tegra_smmu_attach_wide(TEGRA_SMMU_ASID_HDA, domain.ptb(), TEGRA_SMMU_HDA_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0,
                               1u << 21, why, sizeof(why))) {
        IOLog("SwitchAudio: HDA SMMU attachment failed\n");
        return false;
    }
    mapped = true;
    power(true);
    prepared = true;
    return true;
}

void SwitchHDA::power(bool on)
{
    if (powered == on)
        return;
    constexpr unsigned clocks[] = {kClockHda, kClockHda2Codec2x, kClockHda2Hdmi};
    if (!on) {
        coreWrite(GCTL, 0);
        IOSleep(1);
        switch_car_clock_disable(kClockMaud);
        for (const unsigned id : clocks)
            switch_car_clock_disable(id);
        powered = false;
        return;
    }
    // MAUD must track the 48 MHz link at exactly 2.125x (T210 TRM 27.7.3).
    switch_car_write(CLK_SOURCE_HDA, 6);
    switch_car_write(CLK_SOURCE_HDA2CODEC_2X, 15);
    switch_car_write(CLK_SOURCE_MAUD, 3);
    for (const unsigned id : clocks) {
        switch_car_clock_enable(id);
        switch_car_reset_deassert(id);
    }
    switch_car_clock_enable(kClockMaud);
    write(HDA_IPFS_CONFIG, (read(HDA_IPFS_CONFIG) & ~kIpfsCl2FastIo) | kIpfsEnFpci | kIpfsEnSnoop);
    write(HDA_CFG_CMD, (read(HDA_CFG_CMD) & ~kCfgCmdIntDisable) | kCfgCmdEnable);
    write(HDA_CFG_BAR0, 0xffffffffu);
    write(HDA_CFG_BAR0, kCfgBar0Start);
    write(HDA_FPCI_BAR0, kFpciBar0Start);
    write(HDA_IPFS_INTR_MASK, read(HDA_IPFS_INTR_MASK) | kIpfsEnIntr);
    powered = true;
}

bool SwitchHDA::reset()
{
    if (!stop() || !stopCommands())
        return false;
    coreWrite(GCTL, 0);
    // CRST and stream reset have no completion interrupt (Intel HD Audio 1.0a 4.2.2, 4.5.1.2).
    IOSleep(1);
    if (core(GCTL) & kGctlCrst)
        return false;
    coreWrite(GCTL, kGctlCrst);
    IOSleep(1);
    if (!(core(GCTL) & kGctlCrst))
        return false;
    IOSleep(1);  // Codecs have 521 us after CRST to register (Intel 4.3).
    const uint32_t capabilities = core(GCAP) & 0xffff;
    if (!((capabilities >> 12) & 15))
        return false;
    const uint32_t inputs = (capabilities >> 8) & 15;
    stream = SD_BASE + inputs * SD_STRIDE;
    streamBit = 1u << inputs;
    const uint32_t corbSize = ringSize(core(CORBSIZE) & 0xff);
    const uint32_t rirbSize = ringSize(core(RIRBSIZE) & 0xff);
    corbMask = corbSize == 2 ? 255 : corbSize == 1 ? 15 : 1;
    rirbMask = rirbSize == 2 ? 255 : rirbSize == 1 ? 15 : 1;
    field(CORBCTL, 0xff, 0);
    field(RIRBCTL, 0xff, 0);
    field(CORBSIZE, 3, corbSize);
    field(RIRBSIZE, 3, rirbSize);
    bzero(commands.va, commands.size);
    switch_dma_publish();
    coreWrite(CORBLBASE, static_cast<uint32_t>(commands.pa));
    coreWrite(CORBUBASE, 0);
    field(CORBWP, 0xffff, 0);
    field(CORBRP, 0xffff, kCorbRpReset);
    IOSleep(1);  // Tegra's CORBRP reset is self-clearing (Linux AZX_DCAPS_CORBRP_SELF_CLEAR).
    if (core(CORBRP) & 0xffff)
        return false;
    coreWrite(RIRBLBASE, static_cast<uint32_t>(commands.pa + kRirbOffset));
    coreWrite(RIRBUBASE, 0);
    field(RIRBWP, 0xffff, kRirbWpReset);
    field(RINTCNT, 0xffff, 1);
    corbWrite = rirbRead = 0;
    field(RIRBSTS, 0xff, kRirbStsInt | kRirbStsOverflow);
    field(RIRBCTL, 0xff, kRirbCtlEnable);
    field(CORBCTL, 0xff, kCorbCtlEnable);
    coreWrite(INTCTL, kIntGie | kIntCie);
    return true;
}

bool SwitchHDA::verb(uint32_t node, uint32_t operation, uint32_t payload, uint32_t *value)
{
    if (!attached || pending)
        return false;
    pending = true;
    responseValid = false;
    corbWrite = (corbWrite + 1) & corbMask;
    auto *corb = reinterpret_cast<volatile uint32_t *>(commands.va);
    corb[corbWrite] = switch_hda_verb(codec, node, operation, payload);
    switch_dma_publish();
    field(CORBWP, 0xffff, corbWrite);
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    gate->commandSleep(&pending, deadline, THREAD_UNINT);
    if (pending) {
        pending = false;
        responseValid = false;
        IOLog("SwitchAudio: HDA response interrupt lost, INTID %u INTSTS %#x RIRB %#x\n", kIntid, core(INTSTS),
              core(RIRBCTL));
        return false;
    }
    if (!responseValid)
        return false;
    if (value)
        *value = response;
    return true;
}

void SwitchHDA::interrupt()
{
    if (!powered)
        return;
    const uint32_t status = core(INTSTS);  // Flushes preceding HDA DMA writes (T210 TRM 27.7.8.5).
    if (status & kIntCie) {
        const uint32_t rirbStatus = core(RIRBSTS) & 0xff;
        if (rirbStatus & kRirbStsOverflow) {
            IOLog("SwitchAudio: HDA RIRB overflow, INTID %u\n", kIntid);
            field(RIRBSTS, 0xff, rirbStatus);
            disconnect();
            return;
        }
        const uint32_t written = core(RIRBWP) & rirbMask;
        switch_dma_consume();
        const auto *rirb = reinterpret_cast<volatile uint32_t *>(commands.va + kRirbOffset);
        for (; rirbRead != written;) {
            rirbRead = (rirbRead + 1) & rirbMask;
            const uint32_t answer = rirb[rirbRead * 2], flags = rirb[rirbRead * 2 + 1];
            if (!(flags & kRirbResponseUnsol) && (flags & 15) == codec && pending) {
                response = answer;
                responseValid = true;
                pending = false;
                gate->commandWakeup(&pending, false);
            }
        }
        field(RIRBSTS, 0xff, rirbStatus);
        if (core(CORBSTS) & kCorbStsError) {
            IOLog("SwitchAudio: HDA CORB DMA error, INTID %u\n", kIntid);
            field(CORBSTS, 0xff, kCorbStsError);
            disconnect();
            return;
        }
    }
    if (!(status & streamBit) || !stream)
        return;
    const uint32_t streamStatus = core(stream) >> 24;
    coreWrite(stream, (core(stream) & 0xffffffu) | streamStatus << 24);
    if (streamStatus & kStreamErrors) {
        stop();
        IOLog("SwitchAudio: HDA stream DMA error %#x\n", streamStatus);
        return;
    }
    if (!(control->state & SwitchAudioOutputRunning))
        return;
    const uint32_t frameBytes = control->sample_bits == 16 ? 4 : 8;
    const uint32_t periodBytes = SWITCH_HDA_PERIOD_FRAMES * frameBytes;
    const uint32_t at = core(stream + SD_LPIB) / periodBytes;
    const uint32_t done = (at - lastPeriod) & (kPeriods - 1);
    auto *samples = static_cast<uint8_t *>(output->getBytesNoCopy());
    for (uint32_t n = 0; n < done; ++n) {
        uint8_t *period = samples + lastPeriod * periodBytes;
        bzero(period, periodBytes);
        switch_dcache_clean(period, periodBytes);
        lastPeriod = (lastPeriod + 1) & (kPeriods - 1);
        control->out_frames += SWITCH_HDA_PERIOD_FRAMES;
    }
}

bool SwitchHDA::stop()
{
    bool stopped = true;
    if (stream && powered) {
        coreWrite(stream, core(stream) & 0xffffe1u);
        // SD RUN clears after pending DMA is idle, within 40 us (Intel HD Audio 1.0a 4.5.4).
        if (!switch_mmio_wait32(registers, kBar + stream, kRun, 0, 40)) {
            IOLog("SwitchAudio: HDA stream DMA did not stop\n");
            stopped = false;
        }
    }
    if (control)
        control->state = 0;
    return stopped;
}

bool SwitchHDA::stopCommands()
{
    if (!powered)
        return true;
    coreWrite(INTCTL, 0);
    field(CORBCTL, 0xff, 0);
    field(RIRBCTL, 0xff, 0);
    // Command engines have no stop interrupt; both RUN bits must read zero before CRST (Intel 3.3.7).
    if (!switch_mmio_wait32(registers, kBar + CORBCTL, kRun, 0, 1000) ||
        !switch_mmio_wait32(registers, kBar + RIRBCTL, kRun, 0, 1000)) {
        IOLog("SwitchAudio: HDA command DMA did not stop\n");
        return false;
    }
    return true;
}

void SwitchHDA::disconnect()
{
    attached = ready = false;
    stop();
    stopCommands();
    if (control)
        __atomic_store_n(&control->jack, 0, __ATOMIC_RELEASE);
    if (pending) {
        pending = responseValid = false;
        gate->commandWakeup(&pending, false);
    }
}

bool SwitchHDA::prepare(IOBufferMemoryDescriptor *ring)
{
    output = ring;
    if (!prepared && !prepareHardware()) {
        release();
        IOLog("SwitchAudio: HDA initialization failed\n");
        return false;
    }
    power(true);
    attached = reset();
    return attached;
}

bool SwitchHDA::connect(uint32_t formats)
{
    if (!attached)
        return false;
    ready = discover(formats);
    if (!ready && attached)
        IOLog("SwitchAudio: HDA display codec not ready\n");
    return ready;
}

IOReturn SwitchHDA::start()
{
    if (!ready || !attached)
        return kIOReturnNotReady;
    if (control->state)
        return kIOReturnSuccess;
    if (!stop())
        return kIOReturnTimeout;
    const uint32_t frameBytes = control->sample_bits == 16 ? 4 : 8;
    const uint32_t bytes = SWITCH_AUDIO_RING_FRAMES * frameBytes;
    const uint32_t periodBytes = SWITCH_HDA_PERIOD_FRAMES * frameBytes;
    if (!ringEmpty) {
        bzero(output->getBytesNoCopy(), bytes);
        switch_dcache_clean(output->getBytesNoCopy(), bytes);
    }
    coreWrite(stream, kStreamReset);
    IOSleep(1);
    if (!(core(stream) & kStreamReset))
        return kIOReturnTimeout;
    coreWrite(stream, 0);
    IOSleep(1);
    if (core(stream) & kStreamReset)
        return kIOReturnTimeout;
    auto *bdl = reinterpret_cast<volatile uint32_t *>(commands.va + kBdlOffset);
    const uint64_t outputPA = output->getPhysicalSegment(0, nullptr);
    for (uint32_t i = 0; i < kPeriods; ++i) {
        bdl[i * 4] = static_cast<uint32_t>(outputPA + i * periodBytes);
        bdl[i * 4 + 1] = 0;
        bdl[i * 4 + 2] = periodBytes;
        bdl[i * 4 + 3] = 1;
    }
    switch_dma_publish();
    coreWrite(stream + SD_CBL, bytes);
    field(stream + SD_LVI, 0xffff, kPeriods - 1);
    field(stream + SD_FMT, 0xffff, switch_hda_stream_format(control->sample_bits));
    coreWrite(stream + SD_BDPL, static_cast<uint32_t>(commands.pa + kBdlOffset));
    coreWrite(stream + SD_BDPU, 0);
    lastPeriod = 0;
    control->out_frames = 0;
    control->out_written = 0;
    control->anchor_host = mach_absolute_time();
    control->state = SwitchAudioOutputRunning;
    coreWrite(INTCTL, core(INTCTL) | streamBit);
    const uint32_t stripe = (core(GCAP) >> 1) & 3;
    ringEmpty = false;
    coreWrite(stream, kStreamTag1 | (stripe ? kStreamStripe : 0) | kStreamInterrupts | kRun);
    return kIOReturnSuccess;
}

void SwitchHDA::sleep(bool entering)
{
    disconnect();
    if (entering && powered)
        power(false);
}

void SwitchHDA::release()
{
    disconnect();
    if (powered)
        power(false);
    if (mapped)
        tegra_smmu_detach(TEGRA_SMMU_ASID_HDA, TEGRA_SMMU_HDA_ASID);
    mapped = false;
    switch_dma_free(&commands);
    switch_dma_free(&tables);
    prepared = false;
    registers = nullptr;
}
