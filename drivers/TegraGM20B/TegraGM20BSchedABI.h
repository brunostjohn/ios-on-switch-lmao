/* SPDX-License-Identifier: MIT */
/* Scheduler and submission extensions kept out of TegraGM20BABI.h and TegraGM20BJobABI.h
 * so //gpu:stack_key does not move. */
#ifndef TEGRA_GM20B_SCHED_ABI_H
#define TEGRA_GM20B_SCHED_ABI_H
#include <stdint.h>

enum {
	/* GetInfo.flags: MethodPriority accepts an optional TegraGM20BAbiPriorityReply */
	TegraGM20BInfoPriorityReply = 1u << 30,
	/* TegraGM20BAbiPriorityReply.flags: connection is the UI-critical compositor */
	TegraGM20BPriorityCritical = 1u << 0,
};

struct TegraGM20BAbiPriorityReply {
	uint32_t flags;
	uint32_t reserved;
};

#endif
