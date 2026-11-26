// SPDX-License-Identifier: GPL-3.0-or-later
/* The planner's <sys/mman.h>: mmap() hands out an in-memory file's bytes (plan.c). */
#ifndef SWITCH_PLAN_SHIM_SYS_MMAN_H
#define SWITCH_PLAN_SHIM_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>

#ifndef PROT_READ
#define PROT_READ 1
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 2
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define mmap   switch_plan_mmap
#define munmap switch_plan_munmap

void *switch_plan_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t offset);
int switch_plan_munmap(void *addr, size_t len);

#endif
