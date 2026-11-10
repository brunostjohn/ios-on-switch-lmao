// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <kern/task.h>
#include "SwitchVICPicture.h"
struct switch_gpu_cpu_lease;
struct TegraVICSurface;

#ifdef __cplusplus
extern "C" {
#endif
bool tegra_vic_present_ready(void);
void tegra_vic_present_start(void);
int tegra_vic_present(const TegraVICPicture *picture, task_t task, const char **why);
// Materializes equivalent rows under destination ownership without changing the IOSurface seed.
int switch_vic_present_owned(const TegraVICPicture *picture, task_t task,
                              struct switch_gpu_cpu_lease *destination, const char **why);
int switch_vic_present_surface_owned(const TegraVICPicture *picture, const struct TegraVICSurface *surface,
                                      struct switch_gpu_cpu_lease *destination, const char **why);
// Publishes the VIC producer before returning; mappings stay retained until completion.
int switch_vic_present_surface_queue(const TegraVICPicture *picture, const struct TegraVICSurface *surface,
                                      struct switch_gpu_cpu_lease *destination, const char **why);
#ifdef __cplusplus
}
#endif
