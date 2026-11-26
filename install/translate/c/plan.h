// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * `arm64e plan` (translate/arm64e/tools/a64plan2.c) on buffers: the dyld shared cache's translation plan. The planner
 * keeps its state in file-scope statics, so one plan runs at a time; the caller serialises them.
 */
#ifndef SWITCH_TRANSLATE_PLAN_H
#define SWITCH_TRANSLATE_PLAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One file of the cache: the main file has suffix "", each subcache the suffix its main header names. */
struct switch_plan_file {
    const char *suffix;
    const uint8_t *data; /* 4-byte aligned */
    size_t size;
};

/*
 * The plan of the cache whose main file is files[0], in *plan (*plan_size bytes). The planner's report goes to *log
 * (NUL-terminated) either way. Returns 0, or 1 when the planner refuses: a subcache it names is missing, or a site's
 * stub doesn't reach its return. Free both with switch_plan_release.
 */
int switch_plan_make(const struct switch_plan_file *files, size_t nfiles, uint8_t **plan, size_t *plan_size,
                     char **log);
void switch_plan_release(void *buffer);

#ifdef __cplusplus
}
#endif

#endif
