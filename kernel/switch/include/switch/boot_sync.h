// SPDX-License-Identifier: GPL-2.0-only
/*
 * Which changes the kernel makes on Data before launchd (bsd/kern/kern_exec.c: launchd's overrides, the setup's
 * preferences, cache drops, markers, a panic record) reach the card before launchd starts. A REDONE change is made
 * again by the next boot's same step from the kernel's own state before anything reads it, so a power cut that loses
 * it costs one redo. Every other change is DURABLE: something may read it before the next step could redo it
 * (launchd's overrides, managed and setup preferences), or nothing redoes it (seeds, edits of the user's own plists,
 * a panic record). A boot syncs from its first durable change on: that change and every later write go out
 * synchronously, each directory sync after it runs, and so does the volume's at the end. Free of kernel types so
 * kernel/tests can drive it.
 */
#ifndef SWITCH_BOOT_SYNC_H
#define SWITCH_BOOT_SYNC_H

#include <stdbool.h>
#include <stddef.h>

/* DURABLE is 0: a change nobody classed is synced. */
enum switch_boot_change {
    SWITCH_BOOT_DURABLE,
    SWITCH_BOOT_REDONE,
};

struct switch_boot_sync {
    unsigned durable; /* durable changes made */
    char first[128];  /* the first one's path, cut to fit */
};

/* A change about to be made to `path` (a file written or removed, a directory made): whether it goes out
 * synchronously. A durable change counts before it is made, so it is synced itself. */
static inline bool switch_boot_sync_change(struct switch_boot_sync *sync, enum switch_boot_change change,
                                           const char *path)
{
    if (change == SWITCH_BOOT_DURABLE && !sync->durable++) {
        size_t i = 0;
        for (; path[i] && i < sizeof(sync->first) - 1; ++i) {
            sync->first[i] = path[i];
        }
        sync->first[i] = '\0';
    }
    return sync->durable != 0;
}

/* Whether a directory sync, or the volume's, runs now. */
static inline bool switch_boot_sync_due(const struct switch_boot_sync *sync)
{
    return sync->durable != 0;
}

#endif /* SWITCH_BOOT_SYNC_H */
