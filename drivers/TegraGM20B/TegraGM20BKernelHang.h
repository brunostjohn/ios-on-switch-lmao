/* SPDX-License-Identifier: MIT */
/* A kernel channel a hang check finds stuck (TegraGM20BRecover.h tgm_hang_blame's TGM_HANG_KERNEL): which engines are
 * reset for it, and when a stall is a hardware failure. gpu_reload.c tgm_kernel_hang_locked takes the steps. */
#pragma once
#include <stdint.h>
#include "TegraGM20BHost.h"

/* Steps for one stall, each at a hang check that still finds the channel stuck with no live channel having completed
 * anything since the last step. A kernel channel can stop with no engine switching (T-v5a: PBDMA0 on the scrubber's
 * LAUNCH_DMA for seconds, GR idle on a client's context, no context-switch timeout), so no step waits for an engine to
 * look busy. */
enum tgm_kernel_hang {
	/* the engines of its runlist that hold its context or no live client's state (a copy engine's idle context is
	 * none), each under the hold that takes it off its PBDMA; nobody killed */
	TGM_KERNEL_HANG_OWN,
	/* every engine of its runlist, the live client channels whose contexts they hold or their PBDMA feeds killed first */
	TGM_KERNEL_HANG_ALL,
	/* an ALL reset with no client context resident did not free it: a hardware failure */
	TGM_KERNEL_HANG_FATAL,
};

/* What the channel's last step left (struct tgm_chan hang_step): none, OWN, ALL with client contexts resident, ALL with
 * none. */
enum {
	TGM_KERNEL_HANG_NONE,
	TGM_KERNEL_HANG_DID_OWN,
	TGM_KERNEL_HANG_DID_ALL,
	TGM_KERNEL_HANG_DID_ALL_CLEAN,
};

/* step_done: what the channel had completed at its last step; moved: a live channel completed work since that step.
 * Either ends that stall: the reset freed the GPU, and a stall now is a new one. */
static inline enum tgm_kernel_hang tgm_kernel_hang_step(int step, uint32_t step_done,
							uint32_t completed, int moved)
{
	if (step == TGM_KERNEL_HANG_NONE || completed != step_done || moved)
		return TGM_KERNEL_HANG_OWN;
	if (step == TGM_KERNEL_HANG_DID_ALL_CLEAN)
		return TGM_KERNEL_HANG_FATAL;
	return TGM_KERNEL_HANG_ALL;
}

/* Checks in a row a step waits for another channel's preempt to finish: the hold asks for none while one is in flight,
 * so a reset then would leave the stuck channel on its PBDMA. One still in flight after that is taken as never
 * finishing, and the step is taken anyway. */
#define TGM_KERNEL_HANG_WAITS 4

/* deferred: the checks in a row that put the step off so far. The count after this check; 0: take the step now. */
static inline int tgm_kernel_hang_defer(int deferred, int preempting)
{
	return preempting && deferred < TGM_KERNEL_HANG_WAITS ? deferred + 1 : 0;
}

static inline int tgm_kernel_hang_did(enum tgm_kernel_hang step, int clients)
{
	if (step == TGM_KERNEL_HANG_OWN)
		return TGM_KERNEL_HANG_DID_OWN;
	return clients ? TGM_KERNEL_HANG_DID_ALL : TGM_KERNEL_HANG_DID_ALL_CLEAN;
}

/* The contexts an engine's status names whatever it is doing, BUSY or not: an idle engine's VALID context is still in
 * it, and a reset loses it. VALID, SAVE and SWITCH name ID; LOAD and SWITCH name NEXT_ID. out: up to two. */
static inline int tgm_engine_contexts_held(uint32_t status, int out[2])
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);
	int n = 0;

	if ((ctx == PFIFO_ENG_CTX_VALID || ctx == PFIFO_ENG_CTX_SAVE ||
	     ctx == PFIFO_ENG_CTX_SWITCH) &&
	    !(status & PFIFO_ENG_ID_TSG))
		out[n++] = (int)PFIFO_ENG_ID(status);
	if ((ctx == PFIFO_ENG_CTX_LOAD || ctx == PFIFO_ENG_CTX_SWITCH) &&
	    !(status & PFIFO_ENG_NEXT_ID_TSG))
		out[n++] = (int)PFIFO_ENG_NEXT_ID(status);
	return n;
}

/* Whether the OWN step resets an engine of the stuck kernel channel's runlist. holds_kernel: a context it holds is that
 * channel's; live_client: one is a live client's; copy: a copy engine, whose context keeps no state once it is idle. */
static inline int tgm_kernel_hang_resets(uint32_t status, int holds_kernel, int live_client,
					 int copy)
{
	if (holds_kernel || !live_client)
		return 1;
	return copy && !(status & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW));
}
