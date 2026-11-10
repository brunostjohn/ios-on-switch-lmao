// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "TegraVICSurface.h"

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination,
                         const TegraVICPlan *plan, uint32_t chromaSite,
                         void (*completion)(void *, int), void *context);
