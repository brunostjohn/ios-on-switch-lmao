// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <kern/task.h>
#include "SwitchVICPicture.h"

#ifdef __cplusplus
extern "C" {
#endif
bool tegra_vic_present_ready(void);
void tegra_vic_present_start(void);
int tegra_vic_present(const TegraVICPicture *picture, task_t task, const char **why);
#ifdef __cplusplus
}
#endif
