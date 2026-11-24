// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018 naehrwert
// Copyright (c) 2018-2025 CTCaer
// Copyright (C) 2012 Avionic Design GmbH
// Copyright (C) 2012 NVIDIA CORPORATION
// Display controller B; see TegraDisplayB.h. kInit is display controller A's base configuration from hekate
// (github.com/CTCaer/hekate, GPL-2.0-only): bdk/display/di.inl _di_dc_init_config, as TegraDisplayPower.cpp has it,
// without the DSI parts. The rest is Linux drivers/gpu/drm/tegra/dc.c and dc.h: tegra_crtc_atomic_enable,
// tegra_dc_set_timings (reference-to-sync 1/1), tegra_dc_commit_state (shift clock 1:1), tegra_dc_setup_window for a
// 1:1 pitch BGRA window, SOR_ENABLE(1) = DC_DISP_DISP_WIN_OPTIONS bit 26. Power: the DISB partition (19), DISP2 clock
// and reset 26, MC client hot reset DCB (MC 0x200 bit 3, Linux memory/tegra/tegra210.c), the MBIST workaround
// (clk-tegra210.c tegra210_generic_mbist_war, LVL2_CLK_GATE_OVRA bit 2).
#include "TegraDisplayB.h"

#include <IOKit/IOLib.h>
#include "../TegraDisplayFlip/SwitchDisplayPriority.h"
#include "../TegraSMMU/TegraSMMU.h"

#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kDisplayBPA = 0x54240000;
constexpr unsigned kClockDisp2 = 26, kPartitionDisb = 19;
constexpr uint32_t kMcHotResetControl = 0x200, kMcHotResetStatus = 0x204, kMcHotResetDcb = 1u << 3;
constexpr uint32_t kLvl2ClockGateOverrideA = 0xf8, kOverrideDisb = 1u << 2;
constexpr uint32_t kStateAccessReadActive = 1u << 0, kStateAccessWriteActive = 1u << 2;

enum : uint32_t {
    DC_CMD_DISPLAY_COMMAND_OPTION0 = 0x031,
    DC_CMD_DISPLAY_COMMAND = 0x032,
    DC_CMD_DISPLAY_POWER_CONTROL = 0x036,
    DC_CMD_INT_STATUS = 0x037,
    DC_CMD_INT_MASK = 0x038,
    DC_CMD_INT_ENABLE = 0x039,
    DC_CMD_STATE_ACCESS = 0x040,
    DC_CMD_STATE_CONTROL = 0x041,
    DC_CMD_DISPLAY_WINDOW_HEADER = 0x042,
    DC_CMD_REG_ACT_CONTROL = 0x043,
    DC_COM_CRC_CONTROL = 0x300,
    DC_COM_PIN_OUTPUT_POLARITY1 = 0x307,
    DC_COM_PIN_OUTPUT_POLARITY3 = 0x309,
    DC_DISP_DISP_WIN_OPTIONS = 0x402,
    DC_DISP_DISP_MEM_HIGH_PRIORITY = 0x403,
    DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER = 0x404,
    DC_DISP_DISP_TIMING_OPTIONS = 0x405,
    DC_DISP_REF_TO_SYNC = 0x406,
    DC_DISP_SYNC_WIDTH = 0x407,
    DC_DISP_BACK_PORCH = 0x408,
    DC_DISP_ACTIVE = 0x409,
    DC_DISP_FRONT_PORCH = 0x40a,
    DC_DISP_DISP_CLOCK_CONTROL = 0x42e,
    DC_DISP_DISP_INTERFACE_CONTROL = 0x42f,
    DC_DISP_DISP_COLOR_CONTROL = 0x430,
    DC_DISP_DC_MCCIF_FIFOCTRL = 0x480,
    DC_DISP_BLEND_BACKGROUND_COLOR = 0x4e4,
    DC_WIN_WIN_OPTIONS = 0x700,
    DC_WIN_BYTE_SWAP = 0x701,
    DC_WIN_BUFFER_CONTROL = 0x702,
    DC_WIN_COLOR_DEPTH = 0x703,
    DC_WIN_POSITION = 0x704,
    DC_WIN_SIZE = 0x705,
    DC_WIN_PRESCALED_SIZE = 0x706,
    DC_WIN_H_INITIAL_DDA = 0x707,
    DC_WIN_V_INITIAL_DDA = 0x708,
    DC_WIN_DDA_INC = 0x709,
    DC_WIN_LINE_STRIDE = 0x70a,
    DC_WIN_BUF_STRIDE = 0x70b,
    DC_WIN_UV_BUF_STRIDE = 0x70c,
    DC_WIN_BUFFER_ADDR_MODE = 0x70d,
    DC_WIN_DV_CONTROL = 0x70e,
    DC_WINBUF_BLEND_LAYER_CONTROL = 0x716,
    DC_WINBUF_START_ADDR = 0x800,
    DC_WINBUF_ADDR_H_OFFSET = 0x806,
    DC_WINBUF_ADDR_V_OFFSET = 0x808,
    DC_WINBUF_SURFACE_KIND = 0x80b,
    DC_WINBUF_START_ADDR_HI = 0x80d,
    DC_WINBUF_CDE_CONTROL = 0x82f,
    DC_WINBUF_AD_UFLOW_STATUS = 0xbca,
};

constexpr uint32_t kGeneralActReq = 1u << 0, kWinAActReq = 1u << 1, kGeneralUpdate = 1u << 8;
constexpr uint32_t kFrameEnd = 1u << 1, kContinuousDisplay = 1u << 5, kSor1Enable = 1u << 26;
constexpr uint32_t kPowerAll = 0x50155;  // PW0..PW4, PM0, PM1
constexpr uint32_t kWindowA = 1u << 4, kWinEnable = 1u << 30, kB8G8R8A8 = 12, kDdaOne = 0x1000;

struct Reg {
    uint32_t index, value;
};

constexpr Reg kInit[] = {
    {DC_CMD_STATE_ACCESS, 1},  // Read active, write assembly. DCB has a single window writer.
    {DC_CMD_REG_ACT_CONTROL, 0},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_DISP_DC_MCCIF_FIFOCTRL, 0},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY, SwitchDisplayPriority::kThresholds},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER, SwitchDisplayPriority::kDelays},
    {DC_CMD_DISPLAY_WINDOW_HEADER, 0xf0},
    {DC_WIN_WIN_OPTIONS, 0},
    {DC_WIN_DV_CONTROL, 0},
    {DC_DISP_DISP_COLOR_CONTROL, 8},
    {DC_DISP_DISP_INTERFACE_CONTROL, 0},
    {DC_COM_PIN_OUTPUT_POLARITY1, 0},
    {DC_COM_PIN_OUTPUT_POLARITY3, 0},
    {DC_DISP_BLEND_BACKGROUND_COLOR, 0},
    {DC_COM_CRC_CONTROL, 0},
    {DC_WINBUF_BLEND_LAYER_CONTROL, (1u << 24) | 255},
    {DC_CMD_DISPLAY_COMMAND_OPTION0, 0},
    {DC_DISP_DISP_WIN_OPTIONS, 0},
    {DC_CMD_DISPLAY_COMMAND, 0},
    {DC_CMD_STATE_CONTROL, 0x1f00},
    {DC_CMD_STATE_CONTROL, 0x1f},
};
}  // namespace

uint32_t TegraDisplayB::read(uint32_t word) const
{
    return switch_mmio_read32(base, word * 4u);
}

void TegraDisplayB::write(uint32_t word, uint32_t value)
{
    switch_mmio_write32(base, word * 4u, value);
}

void TegraDisplayB::commit(uint32_t update)
{
#if defined(__aarch64__) && !defined(SWITCH_MMIO_HOST_MODEL)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    write(DC_CMD_STATE_CONTROL, update << 8);
    write(DC_CMD_STATE_CONTROL, update);
}

bool TegraDisplayB::powerUp(char *why, size_t size)
{
    if (base == nullptr) {
        base = switch_mmio_map(kDisplayBPA, 0x40000);
    }
    if (!switch_pmc_power_up(kPartitionDisb, kClockDisp2, kClockDisp2, nullptr)) {
        snprintf(why, size, "the DISB power partition did not come up");
        return false;
    }
    // tegra210_generic_mbist_war for DISB: the partition's RAMs initialise only with their second-level clock gate
    // overridden for a moment after the partition comes up.
    switch_car_rmw(kLvl2ClockGateOverrideA, 0, kOverrideDisb);
    IODelay(1);
    switch_car_rmw(kLvl2ClockGateOverrideA, kOverrideDisb, 0);
    IODelay(1);
    tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, false);
    emcRefusedLogged = false;
    for (const Reg &reg : kInit) {
        write(reg.index, reg.value);
    }
    return true;
}

void TegraDisplayB::setMode(const struct switch_dp_mode &mode)
{
    memory = {1, mode.pixel_khz * 1000u, mode.hactive, mode.vactive,
              switch_dp_mode_htotal(&mode), switch_dp_mode_vtotal(&mode), 0, 0, {}};
    active = {};
    windowEnabled = false;
    emcRefusedLogged = false;
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    write(DC_WIN_BUFFER_CONTROL, 0);
    write(DC_WIN_COLOR_DEPTH, kB8G8R8A8);
    write(DC_WIN_BYTE_SWAP, 0);
    write(DC_WIN_POSITION, 0);
    write(DC_WIN_SIZE, (uint32_t(mode.vactive) << 16) | mode.hactive);
    write(DC_WIN_PRESCALED_SIZE, (uint32_t(mode.vactive) << 16) | (mode.hactive * 4u));
    write(DC_WIN_H_INITIAL_DDA, 0);
    write(DC_WIN_V_INITIAL_DDA, 0);
    write(DC_WIN_DDA_INC, (kDdaOne << 16) | kDdaOne);
    write(DC_WIN_BUF_STRIDE, 0);
    write(DC_WIN_UV_BUF_STRIDE, 0);
    write(DC_WIN_BUFFER_ADDR_MODE, 0);
    write(DC_WINBUF_ADDR_H_OFFSET, 0);
    write(DC_WINBUF_ADDR_V_OFFSET, 0);
    write(DC_WINBUF_START_ADDR_HI, 0);
    write(DC_WINBUF_CDE_CONTROL, 0);
    write(DC_DISP_DISP_CLOCK_CONTROL, 0);
    write(DC_DISP_DISP_TIMING_OPTIONS, 1u);
    write(DC_DISP_REF_TO_SYNC, (1u << 16) | 1u);
    write(DC_DISP_SYNC_WIDTH, (static_cast<uint32_t>(mode.vsync) << 16) | mode.hsync);
    write(DC_DISP_BACK_PORCH, (static_cast<uint32_t>(mode.vback) << 16) | mode.hback);
    write(DC_DISP_FRONT_PORCH, (static_cast<uint32_t>(mode.vfront) << 16) | mode.hfront);
    write(DC_DISP_ACTIVE, (static_cast<uint32_t>(mode.vactive) << 16) | mode.hactive);
    write(DC_CMD_INT_STATUS, ~0u);
    write(DC_CMD_INT_ENABLE, 0);
    frameInterruptEnabled = false;
    write(DC_CMD_INT_MASK, kFrameEnd);
    write(DC_CMD_DISPLAY_COMMAND, kContinuousDisplay);
    write(DC_CMD_DISPLAY_POWER_CONTROL, kPowerAll);
#if DEVELOPMENT || DEBUG
    write(DC_WINBUF_AD_UFLOW_STATUS, 0);
#endif
    commit(kGeneralActReq | kWinAActReq);
}

void TegraDisplayB::enableSor(bool on)
{
    const uint32_t value = on ? kSor1Enable : 0u;
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
    write(DC_DISP_DISP_WIN_OPTIONS, value);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive | kStateAccessWriteActive);
    write(DC_DISP_DISP_WIN_OPTIONS, value);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
}

bool TegraDisplayB::scanout(const Frame &frame)
{
    if (!windowEnabled) {
        auto next = memory;
        next.peak_bytes_per_second = uint64_t(next.pixel_clock_hz) * 4;
        if (!switch_display::memoryWindow(next.windows[0], next.peak_bytes_per_second, next.active_width,
                                          next.active_height, next.active_width, next.active_height, false,
                                          frame.kind == 0, false))
            return false;
        if (const int err = switch_emc_display_memory_reserve(&next); err == 0) {
            memory = next;
        } else if (!emcRefusedLogged) {
            emcRefusedLogged = true;
            IOLog("SwitchDisplayPort: DCB memory plan refused (%d); continuing without EMC reservation\n", err);
        }
    }
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    if (!windowEnabled) {
        write(DC_WINBUF_BLEND_LAYER_CONTROL, (1u << 24) | 255);
        write(DC_WIN_WIN_OPTIONS, kWinEnable);
    }
    if (!windowEnabled || active.colorDepth != frame.colorDepth) {
        active.colorDepth = frame.colorDepth;
        write(DC_WIN_COLOR_DEPTH, active.colorDepth);
    }
    if (!windowEnabled || active.position != frame.position) {
        active.position = frame.position;
        write(DC_WIN_POSITION, active.position);
    }
    if (!windowEnabled || active.size != frame.size) {
        active.size = frame.size;
        write(DC_WIN_SIZE, active.size);
    }
    if (!windowEnabled || active.prescaled != frame.prescaled) {
        active.prescaled = frame.prescaled;
        write(DC_WIN_PRESCALED_SIZE, active.prescaled);
    }
    if (!windowEnabled || active.stride != frame.stride) {
        active.stride = frame.stride;
        write(DC_WIN_LINE_STRIDE, active.stride);
    }
    if (!windowEnabled || active.hOffset != frame.hOffset) {
        active.hOffset = frame.hOffset;
        write(DC_WINBUF_ADDR_H_OFFSET, active.hOffset);
    }
    if (!windowEnabled || active.vOffset != frame.vOffset) {
        active.vOffset = frame.vOffset;
        write(DC_WINBUF_ADDR_V_OFFSET, active.vOffset);
    }
    if (!windowEnabled || active.kind != frame.kind) {
        active.kind = frame.kind;
        write(DC_WINBUF_SURFACE_KIND, active.kind);
    }
    windowEnabled = true;
    active.iova = frame.iova;
    write(DC_WINBUF_START_ADDR, frame.iova);
    commit(kWinAActReq);
    frameInterrupt(true);
    return true;
}

void TegraDisplayB::resetWindowCache()
{
    windowEnabled = false;
    active = {};
}

void TegraDisplayB::windowOff()
{
    windowEnabled = false;
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    write(DC_WIN_WIN_OPTIONS, 0);
    commit(kWinAActReq);
}

void TegraDisplayB::releaseBandwidth()
{
    if (!memory.peak_bytes_per_second)
        return;
    auto idle = memory;
    idle.peak_bytes_per_second = 0;
    idle.windows[0] = {};
    if (!switch_emc_display_memory_reserve(&idle))
        memory = idle;
}

TegraDisplayB::Latch TegraDisplayB::windowLatch(uint32_t address)
{
    if (read(DC_CMD_STATE_CONTROL) & kWinAActReq)
        return Latch::Pending;
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    if (windowEnabled) {
        if (read(DC_WINBUF_START_ADDR) != address)
            return Latch::Missed;
    } else {
        if (read(DC_WIN_WIN_OPTIONS) & kWinEnable)
            return Latch::Missed;
        releaseBandwidth();
    }
    return Latch::Active;
}

void TegraDisplayB::frameInterrupt(bool enable)
{
    if (enable != frameInterruptEnabled) {
        frameInterruptEnabled = enable;
        write(DC_CMD_INT_ENABLE, enable ? kFrameEnd : 0);
    }
}

uint32_t TegraDisplayB::takeInterrupt()
{
    const uint32_t cause = read(DC_CMD_INT_STATUS);
    write(DC_CMD_INT_STATUS, cause);
    (void)read(DC_CMD_INT_STATUS);
    return cause;
}

void TegraDisplayB::powerDown()
{
    if (base == nullptr || !switch_car_clock_enabled(kClockDisp2)) {
        return;
    }
    write(DC_CMD_INT_ENABLE, 0);
    frameInterruptEnabled = false;
    windowEnabled = false;
    emcRefusedLogged = false;
    active = {};
    write(DC_CMD_INT_MASK, 0);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive | kStateAccessWriteActive);
    write(DC_DISP_DISP_WIN_OPTIONS, 0);
    write(DC_CMD_DISPLAY_COMMAND, 0);
    write(DC_CMD_DISPLAY_POWER_CONTROL, 0);
    write(DC_CMD_STATE_ACCESS, kStateAccessReadActive);
#if DEVELOPMENT || DEBUG
    IOLog("SwitchDisplayPort: DCB window A UFLOW_STATUS %#x at stop\n", read(DC_WINBUF_AD_UFLOW_STATUS));
#endif
    tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, true);
    uint32_t settled = 0;
    for (uint32_t i = 0; i < 1000 && settled < 5; ++i) {
        if (!(tegra_smmu_read(kMcHotResetControl) & kMcHotResetDcb)) {
            IOLog("SwitchDisplayPort: DCB MC hot reset request cleared during flush\n");
            break;
        }
        settled = (tegra_smmu_read(kMcHotResetStatus) & kMcHotResetDcb) ? settled + 1 : 0;
        if (settled < 5)
            IODelay(10);
    }
    if (settled < 5)
        IOLog("SwitchDisplayPort: DCB MC hot reset flush timed out (STATUS %#x)\n",
              tegra_smmu_read(kMcHotResetStatus));
    switch_car_reset_assert(kClockDisp2);
    releaseBandwidth();
    switch_car_clock_disable(kClockDisp2);
    (void)switch_pmc_partition_off(kPartitionDisb);
}
