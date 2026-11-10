/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif
struct switch_gpu_timeline;
struct switch_gpu_timeline_semantic;
int switch_gpu_timeline_has_semantics(void);
int switch_gpu_timeline_semantic_snapshot(const struct switch_gpu_backing_token *,
					  struct switch_gpu_event_waits **);
int switch_gpu_timeline_physical_status(const struct switch_gpu_backing_token *, int external);
int switch_gpu_timeline_semantic_allocate(struct switch_gpu_timeline_semantic **);
int switch_gpu_timeline_semantic_reserve(const struct switch_gpu_backing_token *,
					 struct switch_gpu_event_waits *,
					 struct switch_gpu_timeline_semantic **);
void switch_gpu_timeline_semantic_finish(struct switch_gpu_timeline_semantic **, int result);
int switch_gpu_timeline_init(void);
struct switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid,
						       const volatile uint32_t *fence);
struct TegraGM20BAbiFenceSemantic;
void switch_gpu_timeline_semantic_visibility(struct switch_gpu_timeline *,
					     volatile struct TegraGM20BAbiFenceSemantic *);
void switch_gpu_timeline_visibility(struct switch_gpu_timeline *timeline,
				    const volatile uint32_t *visible);
int switch_gpu_timeline_promote(const struct switch_gpu_backing_token *token);
/* Failure publication is safe under NVKM event spin locks; close requires those callbacks to be unregistered. */
void switch_gpu_timeline_failed(struct switch_gpu_timeline *timeline, uint32_t completed);
void switch_gpu_timeline_close(struct switch_gpu_timeline *timeline);
void switch_gpu_timeline_token(struct switch_gpu_timeline *timeline, uint32_t value,
			       struct switch_gpu_backing_token *token);
#ifdef __cplusplus
}
#endif
