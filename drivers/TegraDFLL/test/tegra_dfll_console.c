// SPDX-License-Identifier: GPL-2.0
// tegra_dfll_fake.h's console: the register blocks in memory, VDD_CPU's MAX77812 on TegraI2C, and what TegraDFLL takes
// from kits/SwitchLinux's XNU side.
#include <stdlib.h>
#include <string.h>

#include "../../../kits/SwitchLinux/SwitchLinux.h"
#include "../../../kits/SwitchLinux/SwitchLinuxLock.h"
#include "../../TegraI2C/TegraI2C.h"
#include "../../../kits/SwitchCAR/SwitchCAR.h"
#include "tegra_dfll_fake.h"

#define BIT(n) (1u << (n))

struct tegra_dfll_fake tegra_dfll_fake;
bool tegra_dfll_fake_refuse_map[TEGRA_DFLL_FAKE_BLOCKS];

static uint32_t blocks[TEGRA_DFLL_FAKE_BLOCKS][0x1000 / 4];
static const uint64_t block_pa[TEGRA_DFLL_FAKE_BLOCKS] = {0x60006000, 0x7000f800, 0x70000800,
							  0x7000d000, 0x70110000};

volatile uint32_t *tegra_dfll_fake_reg(enum tegra_dfll_fake_block block, uint32_t offset)
{
	return &blocks[block][offset / 4];
}

void tegra_dfll_fake_console(void)
{
	memset(blocks, 0, sizeof(blocks));
	memset(tegra_dfll_fake_refuse_map, 0, sizeof(tegra_dfll_fake_refuse_map));
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_APBMISC, 0x004) =
		0x12120;  // HIDREV: T210B01 (0x21), major 2, minor 1
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x014) =
		BIT(15);  // CLK_OUT_ENB_H: I2C5 clocked, out of reset
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x128) = 0x13;  // CLK_SOURCE_I2C5: PLLP / 20
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x0e0) =
		BIT(30) | BIT(27);  // PLLX_BASE: enabled, locked
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x020) =
		BIT(29) | (8u << 4);  // CCLK_BURST_POLICY: RUN on PLLX
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x368) =
		BIT(29) | (8u << 4);  // CCLKG_BURST_POLICY: the same
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_I2C5, 0x000) = BIT(17);  // I2C_CNFG: multi-master
	tegra_dfll_fake.speedo_id = 0;
	tegra_dfll_fake.speedo_value = 1659;
	tegra_dfll_fake.process_id = 0;
}

void switch_car_fuse_readable(void)
{
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x014) |= BIT(7);
	*tegra_dfll_fake_reg(TEGRA_DFLL_FAKE_CAR, 0x048) |= BIT(28);
}

volatile void *switch_linux_ioremap(uint64_t pa, uint64_t size, int write_combined)
{
	unsigned int i = 0;

	tegra_dfll_fake.maps++;
	for (i = 0; i < TEGRA_DFLL_FAKE_BLOCKS; i++) {
		if (block_pa[i] == pa && size <= sizeof(blocks[i]))
			return tegra_dfll_fake_refuse_map[i] ? NULL : (volatile void *)blocks[i];
	}
	return NULL;
}

void *switch_linux_malloc(uint64_t size, int zero)
{
	return zero ? calloc(1, size) : malloc(size);
}

void switch_linux_free(const void *block)
{
	free((void *)block);
}

static int api_mutex;

void *switch_linux_mutex_of(void **slot)
{
	if (!*slot)
		*slot = &api_mutex;
	return *slot;
}

void switch_linux_mutex_lock(void *mutex)
{
}

void switch_linux_mutex_unlock(void *mutex)
{
}

unsigned long switch_linux_irq_save(void)
{
	return 0;
}

void switch_linux_irq_restore(unsigned long flags)
{
}

void switch_linux_spin_contended(switch_linux_spinlock *lock, uintptr_t self)
{
}

uintptr_t switch_linux_model_thread(void)
{
	return 1;
}

uint64_t switch_linux_nanotime(void)
{
	return 0;
}

void switch_linux_udelay(uint64_t us)
{
}

void switch_linux_usleep_range(uint64_t min_us, uint64_t max_us)
{
}

void switch_linux_log(const char *fmt, ...)
{
	tegra_dfll_fake.lines++;
}

void *switch_linux_call_alloc(void (*fn)(void *arg), void *arg)
{
	return switch_linux_malloc(1, 1);
}

bool switch_linux_call_cancel(void *call)
{
	return false;
}

bool switch_linux_call_cancel_wait(void *call)
{
	return false;
}

bool switch_linux_call_enter_after(void *call, uint64_t delay_ns)
{
	return true;
}

int tegra_i2c_init(unsigned bus)
{
	return 0;
}

int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len)
{
	tegra_dfll_fake.pmic_reads++;
	if (!tegra_dfll_fake.pmic_answer)
		memset(buf, 0x4a, len);
	return tegra_dfll_fake.pmic_answer;
}

int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len)
{
	return 0;
}
