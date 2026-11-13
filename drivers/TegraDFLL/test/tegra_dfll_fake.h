// SPDX-License-Identifier: GPL-2.0
// The console as tegra_dfll_cpu_prepare() meets it on the host: the CAR, fuse, APB_MISC, I2C5 and DFLL registers in
// memory, VDD_CPU's MAX77812 on TegraI2C and the L4T probe (tegra_dfll_fakes.c), each with the answer the test sets.
#ifndef TEGRA_DFLL_FAKE_H
#define TEGRA_DFLL_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tegra_dfll_fake {
	int speedo_id, speedo_value, process_id;
	int pmic_answer;   // TegraI2C's result for each MAX77812 read
	int probe_answer;  // the L4T probe's
	unsigned pmic_reads, probes, maps, lines;
};

extern struct tegra_dfll_fake tegra_dfll_fake;

enum tegra_dfll_fake_block {
	TEGRA_DFLL_FAKE_CAR,
	TEGRA_DFLL_FAKE_FUSE,
	TEGRA_DFLL_FAKE_APBMISC,
	TEGRA_DFLL_FAKE_I2C5,
	TEGRA_DFLL_FAKE_DFLL,
	TEGRA_DFLL_FAKE_BLOCKS
};

// A register of one block, as the driver reads and writes it. Blocks map on first use; `refuse` makes a block's
// mapping fail until cleared.
volatile uint32_t *tegra_dfll_fake_reg(enum tegra_dfll_fake_block block, uint32_t offset);
extern bool tegra_dfll_fake_refuse_map[TEGRA_DFLL_FAKE_BLOCKS];

// A T210B01 A01 with I2C5 clocked, the CPU on PLLX in RUN, the DFLL disabled, speedo fused and the PMIC answering.
void tegra_dfll_fake_console(void);

#ifdef __cplusplus
}
#endif

#endif
