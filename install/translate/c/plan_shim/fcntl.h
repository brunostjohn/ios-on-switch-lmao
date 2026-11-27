// SPDX-License-Identifier: GPL-3.0-or-later
/* The planner's <fcntl.h>: open() of its in-memory files (plan.c). */
#ifndef SWITCH_PLAN_SHIM_FCNTL_H
#define SWITCH_PLAN_SHIM_FCNTL_H

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

#define open switch_plan_open

int switch_plan_open(const char *path, int flags, ...);

#endif
