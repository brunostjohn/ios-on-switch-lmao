// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_PREFETCH_INTERNAL_H
#define SWITCH_PREFETCH_INTERNAL_H

#include <stdint.h>
#include <sys/mount.h>
#include <switch/prefetch_vm.h>
#include "prefetch_list.h"

#define SWITCH_PREFETCH_LOG_ENTRIES (1u << 18) /* 262144 x 32 B = 8 MiB (4 GiB / 512) */
#define SWITCH_PREFETCH_MAX_RUNS    (1u << 18)
#define SWITCH_PREFETCH_CAP_PAGES   ((2ULL << 30) / 4096ULL)

void switch_prefetch_list_path(char *out, size_t size);
void switch_cryptex_mounted(unsigned index, mount_t mount);
void switch_boot_io_report(void);
void switch_boot_io_complete(void);
void switch_prefetch_save_next(struct switch_record_entry *log, uint32_t appended,
                               const struct switch_prefetch_run *old_runs, uint32_t replayed_runs,
                               const struct switch_prefetch_header *expected_key, mount_t *mounts,
                               uint32_t mount_count);

#endif
