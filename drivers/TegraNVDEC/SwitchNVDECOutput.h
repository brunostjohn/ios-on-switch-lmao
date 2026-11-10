// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "TegraNVDEC.h"
#include "../TegraVIC/SwitchVICPicture.h"

struct SwitchNVDECBuffer {
    uint32_t handle;
    TegraNVDECBlock block;
    IOMemoryMap *map;
};

bool switch_nvdec_native_output_ready();
int switch_nvdec_output_recycle(SwitchNVDECBuffer *buffer, task_t task);
int switch_nvdec_output_picture(const TegraNVDECBlock *block, const TegraNVDECPresent *request,
                                 TegraVICPicture *picture);
int switch_nvdec_output_export(const TegraNVDECBlock *block, const TegraNVDECPresent *request, task_t task);
