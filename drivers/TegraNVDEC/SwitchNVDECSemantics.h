// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../TegraGM20B/SwitchGpuSharedEvent.h"

struct SwitchNVDECSemantic;
bool switch_nvdec_semantics_start(uint32_t syncpoint);
bool switch_nvdec_semantics_pending();
int switch_nvdec_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchNVDECSemantic **);
void switch_nvdec_semantics_commit(SwitchNVDECSemantic *);
void switch_nvdec_semantics_physical(uint64_t sequence);

void switch_nvdec_semantics_retire(switch_gpu_event_waits *);
