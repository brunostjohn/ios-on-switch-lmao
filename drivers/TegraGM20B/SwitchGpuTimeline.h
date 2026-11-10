/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif
struct switch_gpu_timeline;
int switch_gpu_timeline_init(void);
struct switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid, const volatile uint32_t *fence);
/* Failure publication is safe under NVKM event spin locks; close requires those callbacks to be unregistered. */
void switch_gpu_timeline_failed(struct switch_gpu_timeline *timeline, uint32_t completed);
void switch_gpu_timeline_close(struct switch_gpu_timeline *timeline);
void switch_gpu_timeline_token(struct switch_gpu_timeline *timeline, uint32_t value,
			       struct switch_gpu_backing_token *token);
#ifdef __cplusplus
}
#endif
