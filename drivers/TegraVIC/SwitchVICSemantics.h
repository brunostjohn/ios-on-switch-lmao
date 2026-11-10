// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../TegraGM20B/SwitchGpuSharedEvent.h"

struct SwitchVICSemantic;
bool switch_vic_semantics_start(uint32_t syncpoint);
bool switch_vic_semantics_pending();
int switch_vic_semantics_bind(switch_gpu_event_waits *, const switch_gpu_backing_token *, SwitchVICSemantic **);
void switch_vic_semantics_commit(SwitchVICSemantic *);
void switch_vic_semantics_physical(uint64_t sequence);

void switch_vic_semantics_retire(switch_gpu_event_waits *);
