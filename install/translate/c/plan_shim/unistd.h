// SPDX-License-Identifier: GPL-3.0-or-later
/* The planner's <unistd.h>: its file calls and getopt, served from memory by plan.c on every platform. */
#ifndef SWITCH_PLAN_SHIM_UNISTD_H
#define SWITCH_PLAN_SHIM_UNISTD_H

#define close  switch_plan_close
#define getopt switch_plan_getopt
#define optind switch_plan_optind
#define optarg switch_plan_optarg

int switch_plan_close(int fd);
int switch_plan_getopt(int argc, char *const argv[], const char *options);
extern int switch_plan_optind;
extern char *switch_plan_optarg;

#endif
