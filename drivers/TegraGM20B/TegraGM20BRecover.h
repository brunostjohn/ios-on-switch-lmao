/* SPDX-License-Identifier: MIT */
/* What the GPU is stuck on (recover.c), for gpu_hang.c's client fault isolation. */
#pragma once
#include <stdint.h>
#include "TegraGM20BAcquire.h"
#include "TegraGM20BHost.h"

struct nvkm_device;
struct nvkm_engine;
struct nvkm_engn;
struct nvkm_fifo;
struct nvkm_fifo_func;
struct nvkm_runl;
struct nvkm_runq;

/* Once, after the FIFO exists (M2): recover.c takes PFIFO's scheduler errors and PBDMA ENG_RESET from nvkm's interrupt
 * handlers, and a context-switch timeout recovers the context only once its channel is stuck. */
void tgm_recover_init(struct nvkm_device *device);
/* PFIFO's engine context-switch timeout, after every FIFO init (bring-up and each wake): each period an engine's switch
 * stays pending raises SCHED_ERROR CTXSW_TIMEOUT. */
void tgm_recover_arm(struct nvkm_device *device);
/* Power-down, once the GPU's interrupts are off: no tick's work runs until the next one. */
void tgm_recover_suspend(void);

/* What an engine's switch that timed out (or that a scheduler error found pending) on hardware channel chid gets. */
enum tgm_ctxsw {
	TGM_CTXSW_WAIT, /* a live channel's work still drains: the next tick looks again */
	TGM_CTXSW_KILL, /* a live client's channel is stuck: it is killed, and nvkm's recovery faults the engine off it */
	TGM_CTXSW_RESET, /* the context is a killed channel's or no channel's: nvkm finds no channel group left to recover
			  * (nvkm_runl_rc_engn), so recover.c resets the engine */
	TGM_CTXSW_REDO, /* a kernel channel is stuck: the engine is reset and the work it was on redone, nothing killed */
	TGM_CTXSW_RELOAD, /* the engine is stuck loading a live channel's context: reset under it, nothing killed */
	TGM_CTXSW_FATAL, /* a kernel channel is stuck again before any work submitted after its last reset completed */
};

/* known: one of gpu_chan.c's channels; dead: killed already; stuck: tgm_stuck; reset_pending: a kernel channel whose
 * engine was reset for it has completed nothing submitted after that reset (tgm_ctxsw_redo_pending). */
static inline enum tgm_ctxsw tgm_ctxsw_verdict(int known, int dead, int kernel, int stuck,
					       int reset_pending)
{
	if (!known || dead)
		return TGM_CTXSW_RESET;
	if (!stuck)
		return TGM_CTXSW_WAIT;
	if (!kernel)
		return TGM_CTXSW_KILL;
	return reset_pending ? TGM_CTXSW_FATAL : TGM_CTXSW_REDO;
}

/* A kernel channel's engine was reset after it had submitted reset_after: until a later seqno completes, the redone work
 * has not landed. */
static inline int tgm_ctxsw_redo_pending(int reset, uint32_t reset_after, uint32_t completed)
{
	return reset && (int32_t)(completed - reset_after) <= 0;
}

/* gpu_hang.c, from recover.c's work (thread context): tgm_ctxsw_verdict for hardware channel chid, switched on the engine
 * named `engine`, GR (gr) or another: stuck when nothing completed for tgm_stuck_window(gr), or when it has nothing
 * pending at all (tgm_switch_unowned). A stuck client channel is being killed on return. For TGM_CTXSW_REDO the kernel
 * channel's work is marked to be redone (gpu_present.c, gpu_scrub.c) before the caller resets the engine; a second
 * timeout before that work lands panics. */
enum tgm_ctxsw tgm_ctxsw_decide(int chid, const char *engine, int gr);

#if TGM_STATS
/* gpu_copy_reset.c's HW Test hook, thread context: what ctxsw_work does for TGM_CTXSW_REDO, on the copy engine if it is
 * busy with hardware channel chid's context; mark runs first (tgm_ctxsw_redo_mark). *status: the engine's
 * PFIFO_ENG_STATUS. 1 if it was reset. */
int tgm_recover_copy_reset(int chid, void (*mark)(void), uint32_t *status);
#endif

/* For an engine stuck loading a channel's context (tgm_engine_load_stuck): known and dead as tgm_ctxsw_verdict takes
 * them; reloaded: an engine was reset under this channel's load before and the channel has completed nothing since. The
 * channel being loaded has run nothing on the engine since the load began, so a reset under it loses none of its work. */
static inline enum tgm_ctxsw tgm_load_verdict(int known, int dead, int reloaded)
{
	if (!known || dead)
		return TGM_CTXSW_RESET;
	return reloaded ? TGM_CTXSW_FATAL : TGM_CTXSW_RELOAD;
}

static inline int tgm_load_reloaded(int reset, uint32_t completed_at_reset, uint32_t completed)
{
	return reset && completed == completed_at_reset;
}

/* gpu_reload.c, from recover.c's work: tgm_load_verdict for hardware channel chid, loaded on the engine named `engine`.
 * TGM_CTXSW_RELOAD marks the channel before the caller resets the engine; a second stuck load panics. */
enum tgm_ctxsw tgm_ctxsw_load_decide(int chid, const char *engine);

/* NV_PFIFO_INTR_SCHED_ERROR_CODE as NVIDIA's open gv100 dev_fifo.ref names it. gm20b's nvgpu header names only
 * CTXSW_TIMEOUT, 0x0a there as well, so the rest of Maxwell's encoding is taken to be the same. */
#define TGM_SCHED_CTXSW_TIMEOUT 0x0au

static inline const char *tgm_sched_error_name(uint32_t code)
{
	switch (code) {
	case 0x00:
		return "NO_ERROR";
	case 0x05:
		return "ENGINE_RESET";
	case 0x06:
		return "RL_ACK_TIMEOUT";
	case 0x07:
		return "RL_ACK_EXTRA";
	case 0x08:
		return "RL_RDAT_TIMEOUT";
	case 0x09:
		return "RL_RDAT_EXTRA";
	case TGM_SCHED_CTXSW_TIMEOUT:
		return "CTXSW_TIMEOUT";
	case 0x0c:
		return "RL_REQ_TIMEOUT";
	case 0x0d:
		return "NEW_RUNLIST";
	case 0x0e:
		return "CONFIG_WHILE_BUSY";
	case 0x20:
		return "BAD_TSG";
	default:
		return "unknown";
	}
}

/* NV_PFIFO_INTR_BIND_ERROR_CODE as nouveau's gk104_fifo_intr_bind_reason names it */
static inline const char *tgm_bind_error_name(uint32_t code)
{
	switch (code) {
	case 0x01:
		return "BIND_NOT_UNBOUND";
	case 0x02:
		return "SNOOP_WITHOUT_BAR1";
	case 0x03:
		return "UNBIND_WHILE_RUNNING";
	case 0x05:
		return "INVALID_RUNLIST";
	case 0x06:
		return "INVALID_CTX_TGT";
	case 0x0b:
		return "UNBIND_WHILE_PARKED";
	default:
		return "unknown";
	}
}

/* 1 if a busy or switching engine (its PFIFO_ENG_STATUS) holds hardware channel chid's context: resident, saving it out
 * (ID), or loading it (NEXT_ID); CTX_STATUS 1 valid, 5 load, 6 save, 7 switch (nvgpu hw_fifo_gm20b.h,
 * gk20a_fifo_engines_on_id). An engine waiting on a context load is not BUSY but has CTXSW set: T-v2's GR sat 3 s in
 * 0x0014a014, loading the context of a channel a recovery had just killed. */
static inline int tgm_engine_holds(uint32_t status, int chid)
{
	uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);
	int id = (ctx == 1 || ctx == 6 || ctx == 7) && !(status & PFIFO_ENG_ID_TSG) &&
		 (int)PFIFO_ENG_ID(status) == chid;
	int next = (ctx == 5 || ctx == 7) && !(status & PFIFO_ENG_NEXT_ID_TSG) &&
		   (int)PFIFO_ENG_NEXT_ID(status) == chid;

	return (status & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW)) && (id || next);
}

/* The channel an engine's context status names: VALID and SAVE are on ID, LOAD on NEXT_ID, and a SWITCH on NEXT_ID once
 * FECS has begun the load (fecs_loading, nvkm's chsw_load) and on ID before, BUSY or not. nouveau's gk104_engn_status
 * names ID, the context switched away from, for a LOAD with BUSY clear. -1: none. */
static inline int tgm_engine_chid(uint32_t status, int fecs_loading, int *tsg)
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);
	const int next = ctx == PFIFO_ENG_CTX_LOAD || (ctx == PFIFO_ENG_CTX_SWITCH && fecs_loading);

	if (ctx != PFIFO_ENG_CTX_VALID && ctx != PFIFO_ENG_CTX_LOAD && ctx != PFIFO_ENG_CTX_SAVE &&
	    ctx != PFIFO_ENG_CTX_SWITCH)
		return -1;
	*tsg = next ? (status & PFIFO_ENG_NEXT_ID_TSG) != 0 : (status & PFIFO_ENG_ID_TSG) != 0;
	return (int)(next ? PFIFO_ENG_NEXT_ID(status) : PFIFO_ENG_ID(status));
}

/* A context switch pending on an engine that is not BUSY, its status LOAD: a context load that does not finish.
 * nouveau's gk104_engn_chsw counts a switch only while BUSY, so its context-switch timeout ticks get no decision there. */
static inline int tgm_engine_load_stuck(uint32_t status)
{
	return (status & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW)) == PFIFO_ENG_CHSW &&
	       PFIFO_ENG_CTX_STATUS(status) == PFIFO_ENG_CTX_LOAD;
}

/* The channel a BUSY engine is switching away from: a SAVE, or a SWITCH before FECS begins the load; -1: none. T-v3g's
 * 0x802ac02a is GR saving channel 42. */
static inline int tgm_engine_saving(uint32_t status, int fecs_loading)
{
	const uint32_t ctx = PFIFO_ENG_CTX_STATUS(status);

	if (!(status & PFIFO_ENG_BUSY) || !(status & PFIFO_ENG_CHSW) || (status & PFIFO_ENG_ID_TSG))
		return -1;
	if (ctx != PFIFO_ENG_CTX_SAVE && (ctx != PFIFO_ENG_CTX_SWITCH || fecs_loading))
		return -1;
	return (int)PFIFO_ENG_ID(status);
}

/* An engine whose context-switch timeout tick gets a decision: a switch pending while BUSY (nvkm's), or a stuck load */
static inline int tgm_engine_switch_pending(uint32_t status)
{
	return ((status & PFIFO_ENG_BUSY) && (status & PFIFO_ENG_CHSW)) ||
	       tgm_engine_load_stuck(status);
}

/* PBDMA_INTR_0 ENG_RESET: an engine of the PBDMA's runlist was reset while the PBDMA was on a channel, which stalls
 * there until the bit is cleared. Cleared before the engine is back, the PBDMA sends that channel's method into an engine
 * still in reset. So every engine reset holds the bit (tgm_recover_reset_hold): masked from before the reset, the
 * PBDMA's live channel preempted off it to be loaded again from its saved state, acknowledged once the engine is back,
 * a killed channel's method replaced with a NOP as nvkm's PBDMA handler replaces a faulting one. nvkm's handler would
 * report the bit as the channel's error. */
#define TGM_PBDMA_ENG_RESET 0x01000000u

/* The channel a PBDMA is on at a reset's hold (PBDMA_STATUS: one is loaded), by whether nvkm has it alive */
enum tgm_reset_pbdma {
	TGM_RESET_PBDMA_IDLE,
	TGM_RESET_PBDMA_LIVE, /* preempted off the PBDMA before the reset */
	TGM_RESET_PBDMA_DEAD, /* its method replaced with a NOP at the release */
};

static inline enum tgm_reset_pbdma tgm_reset_pbdma(uint32_t pbdma_status, int alive)
{
	if (!(pbdma_status & PBDMA_BUSY))
		return TGM_RESET_PBDMA_IDLE;
	return alive ? TGM_RESET_PBDMA_LIVE : TGM_RESET_PBDMA_DEAD;
}

/* Engine resets nest (recover.c's reset of GR goes through GR's own reset): the outermost hold masks ENG_RESET and the
 * outermost release acknowledges and unmasks it. *holds is device->intr.lock's, so the interrupt pass never sees the bit
 * while a reset holds it. */
static inline int tgm_reset_hold_enter(uint32_t *holds)
{
	return (*holds)++ == 0;
}

static inline int tgm_reset_hold_leave(uint32_t *holds)
{
	return *holds && --*holds == 0;
}

/* recover_engine.c. Around every engine reset, from before it starts until the engine is back (clkgate.c's GR reset,
 * tgm_engine_reset_locked); runl->mutex held. */
void tgm_recover_reset_hold(struct nvkm_engine *engine);
void tgm_recover_reset_release(struct nvkm_engine *engine);
/* recover.c's PBDMA handler: an ENG_RESET no hold masked, from a reset nvkm made without one */
void tgm_eng_reset_intr(struct nvkm_device *device, struct nvkm_runq *runq);
/* As nvkm_runl_rc resets an engine: runlist blocked, the engine reset under a hold, exception state cleared, runlist let
 * go. runl->mutex held, and the engine found holding hardware channel id under it. */
void tgm_engine_reset_locked(struct nvkm_runl *runl, struct nvkm_engn *engn, int id,
			     const char *why);
/* Debug kernels: the engines reset since the last call */
uint32_t tgm_engine_resets_take(void);

/* The PBDMA_INTR_0 bits nvkm's handler still gets. ACQUIRE is gpu_acquire.c's: nvkm's would kill the waiting channel and
 * write a NOP over the acquire it waits on. */
static inline uint32_t tgm_pbdma_for_nvkm(uint32_t intr0)
{
	return intr0 & ~(TGM_PBDMA_ENG_RESET | TGM_PBDMA_INTR_ACQUIRE);
}

/* gpu_acquire.c: the PBDMA's acquire timeout (its interrupt from recover.c's handler, its work items from
 * tgm_recover_init, cancelled with tgm_recover_suspend). */
void tgm_acquire_intr(struct nvkm_device *device, int runq);
void tgm_acquire_init(void);
void tgm_acquire_suspend(void);

/* Fault every busy engine off hardware channel chid (as a context-switch timeout's recovery does), so a recovery of a
 * channel an engine hangs on does not wait for that engine to drain. */
void tgm_recover_fault(struct nvkm_device *device, int chid);
/* Resets every engine still holding dead hardware channel chid (tgm_engine_holds); how many. The PBDMA that fed it gets
 * ENG_RESET and drops the dead channel's method (recover.c runq_intr). */
int tgm_recover_residue(struct nvkm_device *device, int chid);
/* The name of an engine stuck loading hardware channel chid's context (tgm_engine_load_stuck), else NULL */
const char *tgm_recover_loading(struct nvkm_device *device, int chid);
/* Resets every engine stuck loading chid's context; how many */
int tgm_recover_reload(struct nvkm_device *device, int chid);
/* runl->mutex held, in nvkm_runl_rc before it waits for a posted preempt: an engine of runl stuck saving a context with
 * nothing to drain (a killed channel's, or a client's with nothing pending) holds that preempt up, and is reset under
 * the hold, nothing killed, so the wait ends at once. */
void tgm_recover_preempt_unblock(struct nvkm_runl *runl);
/* Whether a BUSY engine is switching away from hardware channel chid's context (tgm_engine_saving) */
int tgm_recover_saving(struct nvkm_device *device, int chid);
/* Resets every engine that is busy or switching, under the hold; how many */
int tgm_recover_engines(struct nvkm_device *device);
/* nvkm's FIFO functions as a writable copy put in place of its const table, once: recover.c and sched.c replace entries
 * of it, and vendored nouveau stays as it is. */
struct nvkm_fifo_func *tgm_fifo_func(struct nvkm_fifo *fifo);
/* Debug kernels: this window's context-switch timeout ticks waited out and recovered. */
void tgm_recover_stats(char *buf, unsigned int len);
/* The channels the GPU is on, most likely culprit first: each busy engine's (mid-switch, the context it saves or loads,
 * as nvkm's own recovery picks it), then each busy PBDMA's that is not parked on a semaphore acquire
 * (tgm_pbdma_acquiring). Returns how many, at most max. */
int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max);

/* A PBDMA on a host semaphore acquire that has not passed (NV906F SEMAPHORED with an ACQUIRE, ACQ_GEQ or ACQ_AND
 * operation): its channel waits for another one's release, and the PBDMA switches between such channels
 * (ACQUIRE_SWITCH), so the one a check finds it on is only a waiter. A kernel channel waiting so (the present's acquire
 * of the compositor's fence) would otherwise be taken for a stuck kernel channel, which is fatal. */
static inline int tgm_pbdma_acquiring(uint32_t method0, uint32_t data0)
{
	const uint32_t operation = data0 & 0xf;

	return PBDMA_METHOD_ADDR(method0) == 0x1c &&
	       (operation == 0x1 || operation == 0x4 || operation == 0x8);
}

/* A channel with work pending that has completed nothing for this long is stuck (one rule for the context-switch timeout
 * and every late wait). Above any finite drain seen (backboardd's two 144 ms m2v passes a frame), below TGM_HANG_NS and the
 * present's 1 s, so the channels a late wait finds stuck at its deadline were stuck for most of it. */
#define TGM_STUCK_NS (500ull * 1000 * 1000)
/* A present's first look when its job parks on an unkept writer promise: stuck since its submission, which comes just
 * after the look is armed, so a few ms past TGM_STUCK_NS. Its second look, TGM_STUCK_NS later, confirms the strike that
 * gives the promise up at about TGM_PRESENT_NS (1 s), when a watched present gives its promise up. */
#define TGM_PROMISE_FIRST_LOOK_NS (TGM_STUCK_NS + 10ull * 1000 * 1000)

/* The next hang check of a wait (tgm_chan_check_progress). One whose channel acquires an unkept writer promise is
 * looked at as a present's is (TGM_PROMISE_FIRST_LOOK_NS, then TGM_STUCK_NS after its first strike): its promise is
 * given up at about 1 s. Any other wait keeps its period, hang_ns. Keyed on what the wait is on, never on how long it
 * has waited. */
static inline uint64_t tgm_hang_check_interval(int waits_promise, uint32_t strikes,
					       uint64_t hang_ns)
{
	if (!waits_promise)
		return hang_ns;
	return strikes ? TGM_STUCK_NS : TGM_PROMISE_FIRST_LOOK_NS;
}

static inline int tgm_stuck(int dead, uint32_t completed, uint32_t submitted, uint64_t now,
			    uint64_t progress_ns)
{
	return !dead && (int32_t)(completed - submitted) < 0 && now - progress_ns >= TGM_STUCK_NS;
}

/* Only GR runs work that is long and finite (single GR jobs up to 428 ms were measured, T-v3e's boot stats): a copy
 * engine's work that completes nothing for this long while its switch is pending is stuck, so the context-switch
 * timeout's first tick (100 ms) decides it. */
#define TGM_STUCK_COPY_NS (90ull * 1000 * 1000)

static inline uint64_t tgm_stuck_window(int gr)
{
	return gr ? TGM_STUCK_NS : TGM_STUCK_COPY_NS;
}

static inline int tgm_stuck_for(int dead, uint32_t completed, uint32_t submitted, uint64_t now,
				uint64_t progress_ns, uint64_t window)
{
	return !dead && (int32_t)(completed - submitted) < 0 && now - progress_ns >= window;
}

/* An engine whose switch away from a live channel is pending at a context-switch timeout tick, the channel having
 * nothing pending: no work it owns can drain, so it is stuck at that tick. The fence counts only what the channel
 * submitted: a job can complete and still leave the engine unable to save its context (T-v3g's 2D solid fill with a
 * float colour format into an 8-bit target: fence 3/3, then GR stuck saving channel 42). */
static inline int tgm_switch_unowned(int dead, uint32_t completed, uint32_t submitted)
{
	return !dead && completed == submitted;
}

/* A job's internal fences move its channel's fence word with no interrupt, so a check reads the word itself: a value
 * neither the interrupt pass (irq_done) nor an earlier check (*seen) accounted for is progress at the check's time. */
static inline int tgm_progress_seen(uint32_t completed, uint32_t irq_done, uint32_t *seen)
{
	if (completed == irq_done || completed == *seen)
		return 0;
	*seen = completed;
	return 1;
}

/* The first seqno a kill fails: the one a GR error stopped in, taken at the error (gpu_fault.c tgm_gr_fault) because
 * GR can finish that job and release its fence before the kill comes; else the first not complete. */
static inline uint32_t tgm_kill_first(int faulted, uint32_t fault_seqno, uint32_t completed)
{
	return faulted ? fault_seqno : completed + 1;
}

/* A hang check that finds nothing to recover (TGM_HANG_STUCK) resets every engine that is busy or switching, kills no
 * one and waits on (reset: done before for the late channel, completed_at_reset: what it had completed then). Only the
 * same stall again with nothing completed since is fatal: a reset that did not free the GPU. */
static inline int tgm_hang_last_resort(int reset, uint32_t completed_at_reset, uint32_t completed)
{
	return !reset || completed != completed_at_reset;
}

/* tgm_iso_deadline for a channel being killed past TGM_ISOLATE_NS: nvkm's recovery of its group still under way
 * (marked) is waited for up to nvkm's own bounded waits (its runlist update and preempt waits, 2 s each); a recovery
 * never scheduled is asked for once more; past that the kill is lost. */
enum tgm_iso {
	TGM_ISO_WAIT,
	TGM_ISO_RETRY,
	TGM_ISO_LOST
};

#define TGM_ISOLATE_NVKM_NS (4500ull * 1000 * 1000)

static inline enum tgm_iso tgm_iso_verdict(int marked, uint64_t since_ns, int retried)
{
	if (marked && since_ns < TGM_ISOLATE_NVKM_NS)
		return TGM_ISO_WAIT;
	if (!marked && !retried)
		return TGM_ISO_RETRY;
	return TGM_ISO_LOST;
}

/* What a wait that ran out with no GPU error reported found (gpu_hang.c tgm_hang_recover). */
enum tgm_hang {
	TGM_HANG_STUCK, /* nothing to recover: the GPU is stuck on a kernel channel, or nothing progresses: panic */
	TGM_HANG_RECOVERING, /* a client's channel the GPU is stuck on is being killed (or the late one died already) */
	TGM_HANG_PROGRESSING, /* no channel is stuck: the late work is slow, not hung */
	TGM_HANG_RESIDUE, /* an engine or PBDMA is still on a dead channel (*blame): reset it, kill no one */
	TGM_HANG_RELOAD /* an engine is stuck loading a live channel's context (*blame): reset under it, kill no one */
};

struct tgm_hang_candidate {
	uint8_t live; /* a channel of the kernel's list */
	uint8_t stuck; /* work pending and nothing completed for the stuck time (TGM_STUCK_NS, gpu_hang.c) */
	uint8_t kernel;   /* the present's or the scrubber's */
	uint8_t resident; /* an engine or PBDMA is on it (tgm_recover_suspects) */
	uint8_t moving; /* its fence, or its GPFIFO and pushbuffer GET, moved within the stuck time */
	uint8_t waits; /* 1 + the candidate its pending acquire waits on; 0: none */
	uint8_t victim; /* its acquire waits on what the kernel or a host1x engine releases, never on its own work */
	uint8_t dead; /* not a live channel, or one killed already: an engine or PBDMA left on it is a recovery's residue */
	uint8_t loading; /* an engine is stuck loading its context (tgm_engine_load_stuck): its work has not begun there */
	uint8_t saving; /* an engine is BUSY switching away from its context (tgm_engine_saving): the context-switch timeout
			  * names it, and no channel waiting behind that engine is to blame meanwhile */
};

/* A long single job and a hung one look alike for the whole check: the PBDMA's GET stops once the engine's method FIFO is
 * full, USERD's GET is a written-back copy, and GR's status and FECS's current context show the engine busy on the
 * channel either way. So a channel stuck and not moving at a hang check is blamed only when the check before it saw the
 * same with its fence where it is now: two consecutive checks, at the checks' own cadence. *strikes and *seen carry the
 * channel's state between checks. */
static inline int tgm_hang_confirm(int stuck_still, uint32_t completed, uint32_t *strikes,
				   uint32_t *seen)
{
	if (!stuck_still) {
		*strikes = 0;
		return 0;
	}
	if (*strikes && *seen == completed)
		return 1;
	*strikes = 1;
	*seen = completed;
	return 0;
}

/* Candidates in order: tgm_recover_suspects' channels, the channels the late one waits on through acquires, the late
 * one (last). An engine or PBDMA on a dead channel comes first: what it holds up is no channel's fault. So does an
 * engine stuck loading a live channel's context, with the PBDMA waiting behind it on that channel: neither is that
 * channel's work. Then a channel an engine is on that is stuck and does not move is to blame (*blame, its index;
 * else -1): a client's is killed, a kernel channel's is fatal. While an engine is on a channel that moves the GPU is
 * busy, not hung, and nothing is killed, whatever waits behind it. With no engine on any of them, the blame follows the
 * late one's acquires to the channel at the end of that chain, and only a stuck one that does not move is killed; a
 * chain through a source that moves is slow work, and a chain that ends on a victim kills nothing and is never fatal. A
 * dead late channel is recovered already, and other late kernel work with nothing progressing anywhere is fatal. */
static inline enum tgm_hang tgm_hang_blame(const struct tgm_hang_candidate *c, int n, int late_dead,
					   int late_kernel, int any_progress, int *blame)
{
	int i = 0, at = n - 1;

	*blame = -1;
	for (i = 0; i < n; i++) {
		if (c[i].resident && c[i].dead) {
			*blame = i;
			return TGM_HANG_RESIDUE;
		}
	}
	for (i = 0; i < n; i++) {
		if (c[i].live && c[i].resident && c[i].loading) {
			*blame = i;
			return TGM_HANG_RELOAD;
		}
	}
	for (i = 0; i < n; i++) {
		if (!c[i].live || !c[i].resident || !c[i].saving)
			continue;
		if (!c[i].stuck)
			return TGM_HANG_PROGRESSING;
		if (c[i].kernel)
			return TGM_HANG_STUCK;
		*blame = i;
		return TGM_HANG_RECOVERING;
	}
	for (i = 0; i < n; i++) {
		if (!c[i].live || !c[i].resident || !c[i].stuck || c[i].moving)
			continue;
		if (c[i].kernel)
			return TGM_HANG_STUCK;
		*blame = i;
		return TGM_HANG_RECOVERING;
	}
	for (i = 0; i < n; i++)
		if (c[i].live && c[i].resident && c[i].moving)
			return TGM_HANG_PROGRESSING;
	for (i = 0; n && i < n && c[at].waits && c[at].waits <= n; i++) {
		at = c[at].waits - 1;
		if (!c[at].live || c[at].moving)
			return late_dead ? TGM_HANG_RECOVERING : TGM_HANG_PROGRESSING;
	}
	if (n && c[at].live && c[at].stuck && !c[at].moving && !c[at].victim) {
		if (c[at].kernel)
			return TGM_HANG_STUCK;
		*blame = at;
		return TGM_HANG_RECOVERING;
	}
	if (late_dead)
		return TGM_HANG_RECOVERING;
	if (late_kernel && !any_progress && !(n && c[at].victim))
		return TGM_HANG_STUCK;
	return TGM_HANG_PROGRESSING;
}

/* The faulting VA of nvkm's MMU fault report ("fault %02x [%s] at %016llx engine ... on channel %d [...]",
 * gf100_fifo_mmu_fault_recover) among a pass's reports (" | " apart): the one naming hardware channel chid, else one naming
 * no channel nvkm knew (-1). 0: neither. */
static inline uint64_t tgm_fault_va(const char *reports, int chid)
{
	uint64_t unnamed = 0;
	const char *p = reports;

	for (;;) {
		const char *end = __builtin_strstr(p, " | ");
		const char *fault = __builtin_strstr(p, "fault ");
		const char *at = __builtin_strstr(p, "] at ");
		const char *on = __builtin_strstr(p, "on channel ");
		uint64_t va = 0;
		int chan = 0;

		if (fault && at && on && fault < at && at < on && (!end || on < end)) {
			for (at += 5; (*at >= '0' && *at <= '9') || (*at >= 'a' && *at <= 'f');
			     at++)
				va = va << 4 | (uint64_t)(*at <= '9' ? *at - '0' : *at - 'a' + 10);
			on += 11;
			if (*on == '-') {
				unnamed = unnamed ? unnamed : va;
			} else {
				for (; *on >= '0' && *on <= '9'; on++)
					chan = chan * 10 + (*on - '0');
				if (chan == chid)
					return va;
			}
		}
		if (!end)
			return unnamed;
		p = end + 3;
	}
}
