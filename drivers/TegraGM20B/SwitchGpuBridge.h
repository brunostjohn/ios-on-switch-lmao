// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif

struct switch_gpu_bridge;

enum {
	SWITCH_GPU_BRIDGE_MAX_TOKENS = 1024
};

/* Call outside backing/engine queue locks. The caller retains each token through this call; the returned bridge
 * retains them independently until its GPU work and the consumer have both retired. No producer is waited on by CPU. */
int switch_gpu_bridge_submit(const struct switch_gpu_backing_token *tokens, uint32_t count,
			     struct switch_gpu_bridge **bridge);
void switch_gpu_bridge_syncpoint(const struct switch_gpu_bridge *bridge, uint32_t *syncpoint,
				 uint32_t *threshold);
/* A transaction may use the bridge only if every pending GM20B dependency is covered. Completed tokens need no wait.
 * status must be checked before publishing the consumer's successful completion, including producer failure. */
int switch_gpu_bridge_covers(const struct switch_gpu_bridge *bridge,
			     const struct switch_gpu_backing_token *token);
void switch_gpu_bridge_completed(void);
/* 1 complete, 0 not yet, <0 failed. Before the slot retires, 1 waits for its syncpoint to reach the threshold too.
 * After, the stored 1 means the producers' writes are in DRAM, and an increment may still be on its way to host1x. */
int switch_gpu_bridge_status(const struct switch_gpu_bridge *bridge);
/* Exceptional inspection of retained original GM20B/promise producers; no bridge queue lock or retirement. */
int switch_gpu_bridge_source_failure(const struct switch_gpu_bridge *bridge);
/* Why the first of the bridge's producers that failed failed (switch_gpu_backing_tokens_failure); 0 when none did. */
int switch_gpu_bridge_why(const struct switch_gpu_bridge *bridge);
/* Retained consumer completion only; the handle remains owned by the caller. */
int switch_gpu_bridge_wait(const struct switch_gpu_bridge *bridge);
/* The same completion without a CPU wait: completed(context, 0 or the producers' failure) runs once, in retirement
 * thread context, never inside this call. It counts as a waiter, so the hang check covers a stalled producer as it does
 * for switch_gpu_bridge_wait. One per bridge (-16 for a second); the caller keeps its own reference. */
int switch_gpu_bridge_notify(const struct switch_gpu_bridge *bridge, void (*completed)(void *, int),
			     void *context);
/* A host1x stream waits on the bridge's syncpoint in hardware with no completion callback (the display's plane stream,
 * NVENC's direct input): the hang check covers its producers as it does a waiter's, from the submit until the bridge
 * retires. switch_gpu_bridge_submit_watched takes the watch under the submit's own lock, the per-frame route;
 * switch_gpu_bridge_watch watches a bridge already submitted, one more lock round trip (a second call does
 * nothing). */
int switch_gpu_bridge_submit_watched(const struct switch_gpu_backing_token *tokens, uint32_t count,
				     struct switch_gpu_bridge **bridge);
int switch_gpu_bridge_watch(const struct switch_gpu_bridge *bridge);
/* 1 when the bridge has no GPU job of its own: its syncpoint is its producer channel's, passed only by increments that
 * follow that channel's drain, and a channel that stops short of them has the host1x consumers queued on it cancelled
 * before the CPU makes the rest (switch_gpu_producer_release, tegra_host1x_syncpt_release_failed). A consumer queued
 * after that release is not cancelled: it must read switch_gpu_bridge_failed under host1x's channel lock before it
 * queues. 0 for a bridge with its own GPU job: a recovered producer's fence or a given-up promise word lets that job
 * pass and raise the syncpoint over unfinished writes, so only switch_gpu_bridge_status or a notify can report the
 * failure, after the fact. A failure dated after the writes reached DRAM (a kill between the increments and the fence)
 * leaves a jobless syncpoint passed over complete writes, and status reports it. Fixed for the bridge's life; no lock. */
int switch_gpu_bridge_jobless(const struct switch_gpu_bridge *bridge);
/* The bridge's stored failure (<0), else 0. Stored before any release of its syncpoint, so a consumer that reads it under
 * host1x's channel lock (gChannelLock, where queue.lock can't be taken) and queues only on 0 is either queued before a
 * release, which cancels it, or reads the failure. No lock. */
int switch_gpu_bridge_failed(const struct switch_gpu_bridge *bridge);
void switch_gpu_bridge_release(struct switch_gpu_bridge **bridge);
int switch_gpu_bridge_wait_room(void);
/* Explicit CPU acquisition only; waits on the bridge interrupt, never polls a producer. */
int switch_gpu_bridge_wait_external(const struct switch_gpu_backing_token *token);
/* Exact external completion for display/presentation. The callback runs in retirement thread context and receives
 * the retained producers' failure even if recovery released their abandoned hardware semaphores. */
int switch_gpu_bridge_fence(const struct switch_gpu_backing_token *tokens, uint32_t count,
			    void (*completed)(void *, int), void *context);
#ifdef __cplusplus
}
#endif
