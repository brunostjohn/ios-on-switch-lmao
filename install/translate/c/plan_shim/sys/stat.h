// SPDX-License-Identifier: GPL-3.0-or-later
/* The planner's <sys/stat.h>: the size of an in-memory file (plan.c). */
#ifndef SWITCH_PLAN_SHIM_SYS_STAT_H
#define SWITCH_PLAN_SHIM_SYS_STAT_H

#include <stdint.h>

#define stat  switch_plan_stat
#define fstat switch_plan_fstat

struct switch_plan_stat {
    uint64_t st_size;
};

int switch_plan_fstat(int fd, struct switch_plan_stat *st);

#endif
