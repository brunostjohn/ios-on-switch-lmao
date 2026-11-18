// SPDX-License-Identifier: GPL-2.0-only
// Display controller B (0x54240000, SPI 74 level-high) driving SOR1: its power partition and clock, the mode's timing,
// and window A scanning a pitch-linear BGRA surface by DMA through the SMMU (swgroup dcb), flipped at frame end.
#ifndef TEGRA_DISPLAY_B_H
#define TEGRA_DISPLAY_B_H

#include <stddef.h>
#include <stdint.h>

#include "SwitchDPModes.h"
#include "../SwitchPower/SwitchDisplayMemory.h"

#define TEGRA_DISPLAY_B_INTID (32u + 74u)

class TegraDisplayB {
public:
    // The DISB partition with DISP2 (already sourced from pll_d2_out0) and the controller's base configuration.
    bool powerUp(char *why, size_t size);
    void setMode(const struct switch_dp_mode &mode);
    void enableSor(bool on);
    bool scanout(uint32_t iova, uint32_t stride, uint32_t kind);
    enum class Latch { Pending, Active, Missed };
    Latch windowLatch(uint32_t address);
    void windowOff();
    void frameInterrupt(bool enable);
    uint32_t takeInterrupt();
    void powerDown();

private:
    volatile uint8_t *base = nullptr;
    switch_display_memory_head memory = {};
    uint32_t stride = 0, kind = 0;
    bool windowEnabled = false, frameInterruptEnabled = false;
    void releaseBandwidth();
    uint32_t read(uint32_t word) const;
    void write(uint32_t word, uint32_t value);
    void commit(uint32_t update);
};

#endif
