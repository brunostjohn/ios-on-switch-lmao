// SPDX-License-Identifier: MIT
/*
 * GM20B bring-up and the per-connection GPU model, on top of the vendored nouveau nvkm/nvif code.
 * Mirrors what nouveau's DRM driver does for the same jobs:
 *   bring-up      nouveau_platform.c (gm20b_platform_data) + nvkm_device_tegra_new() + nvkm_device_init(), the latter
 *                 as tgm_m2_device_init() (no fini pass, GPCPLL up before L2/FB/PMU, as nvgpu)
 *   address space nouveau_uvmm.c VM_INIT (RAW vmm, kernel range [1<<39, 1<<40)) and VM_BIND (raw get/map/unmap)
 *   memory        nvkm_mem_new_host() with a caller-owned DMA address array (nvif_mem_ram_v0.dma)
 *   channels      nouveau_chan.c nouveau_channel_ctor()/_init() + nvif/chan906f.c GPFIFO/pushbuf helpers
 *   submission    nouveau_exec.c (one GPFIFO entry per push) + nv84/gf100 fence semaphore release
 * Interrupts, no polled mode: from the end of M1 the GPU's stall/nonstall SPIs drive nvkm_intr from a work loop
 * (nvxnu_irq_start, shim/xnu_glue.cpp) and every wait sleeps in nvxnu_idle(); each job's fence also raises a nonstall
 * interrupt (NON_STALL_INTERRUPT after the semaphore release, as nouveau's nvc0_fence_emit32) that a per-channel
 * NON_STALL_INTR event turns into a wake (a job's internal fences raise none: SwitchGpuJobFences.h). A fence found
 * complete with no wake panics (nvxnu_wake_due).
 * GPU L2: user mappings are L2-cacheable instead of volatile, as nvgpu maps
 * the buffers its clients ask to be cacheable (gk20a/mm_gk20a.c __update_pte: VOL only for !cacheable); see l2_on().
 * GPU present: the display's frames are swizzled into its block-linear scanout by the copy
 * engine on a kernel channel that waits for the compositor's work on the GPU (see "GPU present" below).
 * GPU failures from the end of the self-test (M3) on: an error nvkm's interrupt handler reports (MMU fault, PBDMA, GR,
 * FECS, a context switch that times out, ...: nvxnu_error) on a client's channel, or work that stops completing with no
 * error at all (fence waits, sync_for_cpu, GPFIFO full, channel teardown, a late present), recovers the channel the GPU
 * is stuck on and kills only it (see "client fault isolation"); the compositor's channels included, as an app's. What
 * panics, with one line naming it, is a failure on the kernel's own channels and a GPU on which nothing progresses.
 */
// What the gpu_*.c units share: types, macros and the statics more than one unit uses. Private to them. What it
// declares is hidden, so the units reach each other's variables directly, not through the GOT.
#pragma once
#include <core/device.h>
#include <core/tegra.h>
#include <engine/device/priv.h> /* nvkm_device_fini: system sleep */
#include <engine/fifo.h>
#include <engine/fifo/runl.h> /* nvkm_runl_chan_get_chid, nvkm_runl_rc_cgrp: client fault isolation */
#include <engine/fifo/cgrp.h> /* nvkm_cgrp.rc: a channel nvkm itself put into recovery */
#include <engine/fifo/runq.h> /* nvkm_runq.id: the engine list's PBDMAs */
#include <core/gpuobj.h>      /* nvkm_wo32 on a channel's RAMFC: compositor timeslice */
#include <engine/gr.h>
#include <engine/gr/gf100.h> /* gf100_gr_zbc_color_get, _depth_get: tgm_zbc_add */
#include <subdev/acr.h>
#include <subdev/clk.h>
#include <subdev/clk/priv.h> /* nvkm_clk_func: tgm_clk_raise() runs calc/prog as gm20b_clk_init() does */
#include <subdev/clk/gk20a.h> /* gk20a_clk.parent_rate: clk_measure()'s reference */
#include <subdev/devinit.h>
#include <subdev/fb.h>
#include <subdev/ltc.h>
#include <subdev/mmu.h>
#include <subdev/timer.h>
#include <subdev/timer/priv.h> /* nvkm_timer_func.time: GPU time reseeded, never behind what it handed out */
#include <subdev/top.h>
#include <nvif/class.h>
#include <nvif/cl0080.h>
#include <nvif/cl9097.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/driver.h>
#include <nvif/event.h>
#include <nvif/fifo.h>
#include <nvif/if000a.h>
#include <nvif/if0020.h>
#include <nvif/if900d.h>
#include <nvif/ifb00d.h>
#include <nvif/mem.h>
#include <nvif/mmu.h>
#include <nvif/parent.h>
#include <nvif/vmm.h>
#include <nvif/chan.h>
#include <nvif/push906f.h>
#include "TegraGM20BClkgate.h"
#include "TegraGM20BComp.h"
#include "TegraGM20BCopyEmit.h"
#include "TegraGM20BGpu.h"
#include "SwitchGpuJobFences.h"
#include "SwitchGpuJobTiming.h"
#include "SwitchGpuProfileSequence.h"
#include "SwitchGpuTimeline.h"
#include "SwitchGpuChannelPolicy.h"
#include "SwitchGpuPresentTokens.h"
#include "SwitchGpuMapping.h"
#include "SwitchGpuPathStats.h"
#include "SwitchGpuJobStats.h"
#include "TegraGM20BSyncpt.h"
#include "TegraGM20BFences.h"
#include "TegraGM20BHangDecode.h"
#include "TegraGM20BRail.h"
#include "TegraGM20BRecover.h"
#include "TegraGM20BSched.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BSleepGate.h"
#include "TegraGM20BTlb.h"
#include "../SwitchSuspend/SwitchSleepSteps.h"
#include "../TegraDisplayFlip/SwitchFrameTimeline.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#pragma GCC visibility push(hidden)
extern struct platform_device tgm_gpu_pdev;

struct gpu_state {
	int stage;
	struct nvkm_device *device;
	u32 boot0;
	struct nvif_client client;
	struct nvif_device dev;
	struct nvif_mmu mmu;
	int mem_type;
	struct mutex lock; /* serialises every nvif call (nouveau holds cli->mutex likewise) */
	int railed; /* rail-gated (TegraGM20BRail.h): powered down, under tgm_gpu.lock; the gate shut */
};
extern struct gpu_state tgm_gpu;

struct rail_ups_state {
	u32 display, demand;
	const void *first_site;
	u64 cpu_zeroed;
};
extern struct rail_ups_state tgm_rail_ups;
extern u64 tgm_stamp_last;

void tgm_rail_up_locked(const void *site);
noinline void tgm_gpu_lock(void);

/* Fence waits by channel (nouveau_fence_chan.waiting): a fence's nonstall interrupt wakes the waiters of the channels
 * whose fence moved, not every thread in a GPU wait. nvkm runs every channel's nonstall handler on each nonstall
 * interrupt, so one global wake per handler woke each waiter once per live channel per fence. Channels share the words
 * by id; a waiter of another channel on the same word wakes, finds its fence unchanged and sleeps again. Static: a
 * waiter may still sleep on its channel's word while the channel is destroyed. */
#define TGM_WAITERS 128
extern volatile u32 tgm_waiter_words[];
extern u64 tgm_waiter_woken_ns[];

struct wstat_state {
	u32 wakes, slept, woken, other, deadline;
	u32 jobs; /* kicked (tgm_chan_fence_job), every channel */
	u64 lat_sum, lat_max;
	u64 stall, nonstall; /* the interrupt passes at the window's start */
};
extern struct wstat_state tgm_wstat;
extern struct tgm_gate tgm_sc7_gate;
extern struct mutex tgm_gate_lock;
void tgm_gate_enter(void);
int tgm_clk_raise(char *line, unsigned int len);
int tgm_m2_device_init(struct nvkm_device *device, const char **at);
extern u64 tgm_cpu_zeroes, tgm_cpu_zero_bytes;

static void scrub_kick(void);

struct tgm_vm {
	struct nvif_vmm vmm;
	struct mutex map_lock;
	struct list_head kmaps;
	u64 fences_va; /* every connection's fence page, read-only (tgm_chan_order's acquires), 0: not mapped */
	struct list_head
		imports; /* struct tgm_import: first pages of the host imports made in it (tgm_live_lock) */
	int imports_lost;         /* one could not be recorded: taken as importing every page */
	struct tgm_tlb_space tlb; /* GPU-ordered maps (TegraGM20BTlb.h) */
	const void *owner;        /* tgm_vm_claim: the connection whose channels alone address it */
	const void *connection; /* tgm_vm_claim_connection: the client whose pushes a hang reads */
	/* tgm_vm_bind_wait: the connection's bind epoch word (GPU VA, kernel address) and the epoch its next submissions
	 * wait for; bind_cpu 0: never set */
	u64 bind_va;
	const volatile u32 *bind_cpu;
	u32 bind_target;
};

struct tgm_import {
	struct list_head head;
	u64 pa;
	u32 refs;
};

struct switch_gpu_memory_view {
	struct switch_gpu_memory_view *next;
	struct nvif_mem mem;
	u64 offset, bytes;
};

struct tgm_mem {
	struct nvif_mem mem;
	u64 bytes;
	u8 page; /* the largest GMMU page it may be mapped with */
	const u64 *pages;
	u32 npages;
	struct switch_gpu_memory_view *big_views;
	bool big_views_ready;
};

int tgm_vm_kmap_raw(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		    uint64_t *va);
int tgm_fences_map(struct tgm_vm *vm);

struct pfence_state {
	void *va;
	u64 pa;
	struct tgm_mem *mem;
};
extern struct pfence_state tgm_pfence;
int tgm_pfence_get(struct tgm_vm *vm);

static int stats_on(void);
int tgm_vm_map_raw(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		   uint64_t offset, uint8_t kind, int read_only, int vol, int gpu_ordered,
		   uint8_t *page_shift);

#define CHAN_PUSH_BYTES 0x10000 /* nouveau_channel_ctor: plength */
#define CHAN_GPFIFO_BYTES \
	0x8000 /* 4096 entries, including a full 1024-entry job and its kernel packets */
#define CHAN_STAMPS (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + 0x800)
#define CHAN_PROF                              \
	(CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + \
	 0x1000) /* one 16-byte stamp per push, TGM_PROF_MAX */
#define CHAN_NSTAMPS \
	64 /* by seqno; one record = two 16-byte semaphore releases (payload, 0, PTIMER ns) */

/* What a 16-byte semaphore release writes (NV906F SEMAPHORED RELEASE_SIZE_16BYTE) */
struct tgm_stamp {
	u32 payload, pad;
	u64 ns;
};

struct tgm_stamps {
	struct tgm_stamp start,
		end; /* the GPU starts the submission (WFI: the one before finished) / finished it */
};

/* An acquire tgm_chan_order queued for a channel's next submission: another channel's fence word reaching seqno. */
struct tgm_order {
	u64 va;                  /* the source's fence word in the waiter's address space */
	const volatile u32 *cpu; /* and in the kernel's */
	u64 id;                  /* the source channel */
	u32 seqno;
	u32 chid; /* the source's hardware channel */
};

struct tgm_chan {
	struct switch_gpu_timeline *timeline;
	struct tgm_vm *vm;
	struct tgm_tlb_chan tlb; /* the space's GPU-ordered maps this channel has invalidated for */
	struct nvif_object user;
	struct nvif_object obj[5];
	struct nvif_event kill;
	struct nvif_event nonstall;
	struct nvif_chan chan;
	void *ring; /* push | gpfifo | sema, pool memory */
	u64 ring_pa, ring_va;
	u64 *ring_pas;
	struct tgm_mem *ring_mem;
	struct nvif_vma ring_vma;
	volatile u32 *fence_cpu;
	volatile u32 *visible_cpu;
	u64 visible_gpu;
	u64 fence_gpu;
	u32 submitted;
    u64 irq_state;
	u32 error;
	u32 chid;
	int nobj;
	int kernel; /* a kernel channel, no client waits on it and the present does not wait for it: CHAN_PRESENT, CHAN_SCRUB */
	int critical; /* the compositor's or the kernel's: the high timeslice */
	int high; /* the compositor's or the present's: high on the runlist (TegraGM20BSched.h) */
	int isolating; /* client fault isolation: its error or hang was taken as its own, the kill is under way */
	u64 isolated_ns;
	struct list_head live; /* tgm_live_chans, for tegra_gm20b_sync_for_cpu */
	u64 id;
	char owner[17];  /* process that created it */
	char fault[200]; /* the nvkm report it was isolated for */
	u64 fault_va;    /* error 1: the VA its MMU fault report names */
	u64 submit_ns;   /* when the last fence was kicked */
	u64 busy_ns;     /* tgm_hang_recover: when it last went from idle to work pending */
	u64 progress_ns; /* and when its fence was last seen moving: by the interrupt pass (to progress_done) or a check */
	/* the hold's time on it (tgm_hold_held_ns) at the later of those two */
	u64 progress_held;
	u32 progress_done;
	u32 seen_done; /* a hang check's last look at the fence word (chan_progress_ns; tgm_live_lock) */
	u32 stall_done; /* tgm_chan_check_progress: completed seqno first seen stalled, and when */
	u64 stall_ns, stall_held;
	int app;     /* a user channel: the hold may take it from its first submission */
	int tracked; /* the hold may take it (tgm_hold_track) */
	u32 timeslice_due; /* RAMFC timeslice for a level change, written once the host holds the channel no more */
	/* gpu_acquire.c: its RAMFC ACQUIRE has the timeout off since an ACQUIRE interrupt (written again once the host
	 * holds the channel no more); that interrupt's time; the last check of its acquire while it stays parked (0: none)
	 * and the seqno completed then; a parked acquire that never interrupted was logged (tgm_live_lock) */
	int acquire_due;
	u64 acquire_fired_ns;
	u64 acquire_check_ns;
	u32 acquire_done;
	int acquire_unfired;
	u32 acquire_unread; /* checks since its interrupt that found RAMFC naming nothing yet */

	/* its last jobs; high channels': each job's kick, the app channel it was queued behind and when that one was asked to
	 * leave the GPU (PTIMER), whether an app held the GPU at its kick, whether acquires came before its start stamp */
	struct switch_gpu_jobs jobs;

	u32 gov_done;   /* idle clock: last seqno counted retired (idle_retire) */
	u32 woken_done; /* the completed seqno chan_wake_waiters last looked at */
	u64 wanted;     /* the lowest seqno a fence waiter sleeps for (switch_gpu_waiter_want) */
	void (*fenced)(void *arg); /* tgm_chan_on_fence */
	void *fenced_arg;
	u32 stat_done; /* stats: last seqno whose latency was counted, submit times by seqno */
	u64 stat_sub_ns[CHAN_NSTAMPS];
	u64 stat_sub_ptimer[CHAN_NSTAMPS];
	u8 stat_kick[CHAN_NSTAMPS]; /* stats: what each high kick found (tgm_sched_kick) */
	u64 stat_exec,
		stat_exec_max; /* stats: this window's stamped exec time (PTIMER units), for its owner */
	u32 stat_exec_n;
	/* stats: each job's seqnos by its first, the job being timed, and this window's jobs (SwitchGpuJobTiming.h) */
	struct {
		u32 first, last;
	} stat_job[CHAN_NSTAMPS];
	struct switch_gpu_job_run stat_run;
	struct switch_gpu_job_window stat_jobs;
	/* push profile: the profiled submission in flight (0: none), its pushes' classes, and this window's GPU time by
	 * class (PTIMER units) and the pushes counted, for its owner */
	u32 prof_seqno, prof_first, prof_count, prof_subs; /* prof_seqno: the job's last, prof_first its first */
	u8 prof_cls[TGM_PROF_MAX], prof_ilk[TGM_PROF_MAX];
	u16 prof_draws[TGM_PROF_MAX], prof_sdraws[TGM_PROF_MAX], prof_kpx[TGM_PROF_MAX];
	struct switch_gpu_prof_seq prof_run, prof_slow; /* the profile being read, the window's slowest */
	u64 prof_ns[TGM_PC_COUNT];
	u32 prof_n[TGM_PC_COUNT];
	u64 prof_dn[TGM_PC_COUNT],
		prof_sdn[TGM_PC_COUNT]; /* draws, and draws shaded per sample, by class */
	/* tgm_chan_order: its connection's task (tgm_chan_set_group), the acquires queued for its next submission
	 * (tgm_live_lock), and the last one kicked (a hang's line names it while it has not passed) */
	const void *group;
	u32 norder;
	struct tgm_order order[TGM_ORDER_MAX];
	struct tgm_order waited;
	struct tgm_hang_log hang;
	/* tgm_hang_recover: its GPFIFO and pushbuffer GET as last read, and when they last moved */
	u64 get_seen, get_ns;
	u32 hang_strikes, hang_seen; /* tgm_hang_confirm's state between hang checks */
	u32 fault_seqno;             /* faulted: the seqno a GR error stopped in (gpu_fault.c) */
	int faulted;
#if TGM_STATS
	u64 killed_ns; /* tgm_chan_killed's time, for a fault's line (TegraGM20BUserClient.cpp) */
	/* this window's SubmitAddress calls on it, by phase (tgm_stats_submit), for its owner */
	u64 stat_submit_ns[TGM_SUBMIT_PHASES], stat_submit_total, stat_submit_max;
	u32 stat_submits;
	unsigned int gr_slot; /* gpu_stats_ctx.c's slot for its GR context, plus 1; 0: none */
#endif
};

#define CHAN_SCRUB 2 /* the pool scrubber's */
extern struct list_head tgm_live_chans;
extern struct mutex tgm_live_lock;
extern u64 tgm_live_ids;
void tgm_chan_describe(struct tgm_chan *chan, char *buf, unsigned int len);
/* GR's, FECS's and PBDMA0's state for a context switch that timed out (TegraGM20BHangDecode.h tgm_engine_format) */
void tgm_engine_state(char *buf, unsigned int len);

#define TGM_HANG_NS (2 * NSEC_PER_SEC)

/* gpu_acquire.c: a semaphore or syncpoint acquire still waiting this long after it began interrupts (RAMFC ACQUIRE).
 * Above any wait a working GPU makes (TGM_STUCK_NS), at the present's own limit (TGM_PRESENT_NS). */
#define TGM_ACQUIRE_NS (2 * TGM_STUCK_NS)
void tgm_ramfc_write(u32 chid, u32 offset, u32 value);
void tgm_acquire_arm(u32 chid);
enum tgm_wait_source {
	TGM_WAITS_NONE,
	TGM_WAITS_CHANNEL, /* a live channel's fence */
	TGM_WAITS_VICTIM,  /* a word the kernel releases (a bind epoch) or a host1x syncpoint */
	TGM_WAITS_UNKNOWN, /* an acquire on an address only its own pushes name */
	TGM_WAITS_UNREAD,  /* parked on an acquire RAMFC does not name yet (the PBDMA still holds it): a victim */
};
enum tgm_wait_source tgm_chan_wait_source(struct tgm_chan *chan, struct tgm_chan **source, char *what,
					  unsigned int len);
void tgm_acquire_unfired(struct tgm_chan *chan, u64 now);

/* client fault isolation:
 * With every process on the GPU, one app's bad shader or stale address must not take the system down. nvgpu's policy is
 * taken (gk20a_fifo_recover, gk20a_gr_isr: the faulting channel gets an error and only it is torn down): after an
 * interrupt pass in which nvkm reported errors (xnu_glue.cpp gpuInterrupt), tgm_errors_contained() takes the channels the
 * reports name ("on channel N [" for MMU faults, "ch N" for PBDMA errors and GR exceptions) and those nvkm put into
 * recovery in that pass without naming them. A context-switch timeout (a shader that never ends, a switch that never
 * drains) is a tick recover.c waits out until the engine's channel is stuck (tgm_ctxsw_decide), then recovers as nvkm does
 * (gf100_fifo_intr_ctxsw_timeout: the engine faulted off it); an engine stuck on a killed channel's context is reset, and
 * every other scheduler error is recover.c's as well. If each is a client channel
 * (the compositor's too: Core Animation makes its resources again after an error, as on iOS), the errors are that
 * client's: nvkm's recovery (nvkm_runl_rc_cgrp: runlist blocked and preempted, the channel disabled and removed, an
 * engine still on it reset; nvkm starts it itself for MMU faults, tgm_errors_contained() for the rest, which nvkm only
 * reports) kills each one, tgm_chan_killed() marks it dead and releases its fence word so no acquire or CPU wait stays on
 * it, and the GPU goes on. A hang with no error report (a wait that ran out) is recovered the same way by
 * tgm_hang_recover(), from what the GPU itself is stuck on. Everything else panics: an error on a kernel channel, a report
 * naming no channel or an unknown one (one naming none is taken as fallout while a recovery is under way), a kill not
 * done TGM_ISOLATE_NS after its report, and a hang on a kernel channel or with nothing progressing (tgm_chan_hung). */
#define TGM_ISOLATE_NS (500 * NSEC_PER_MSEC)

struct iso_state {
	void *deadline; /* tgm_iso_deadline */
	u64 until;      /* end of the recovery window, 0: none (tgm_live_lock) */
};
extern struct iso_state tgm_iso;

/* CONTRACT.md error codes: 1 MMU fault, 2 GR exception, 3 host error, 4 timeout (a hang, tgm_hang_recover) */
#define HANG_REPORT "hang: "
int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc);
void tgm_chan_isolate(struct tgm_chan *chan, const char *report, u64 now);
int tgm_errors_contained(const char *errors);
void tgm_iso_deadline(void *arg);
enum tgm_hang tgm_hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns);

struct stats_state {
	u64 since;      /* start of the current window, 0 before the first submission */
	u64 cal_ptimer; /* PTIMER at `since`: its rate over the window */
	u32 submits, fences, imports, timed;
	u32 idle;            /* presents that found no user work pending */
	u32 writer;          /* presents that waited for their surface's importers only */
	u32 present_flushes; /* L2_FLUSH_DIRTYs the presents pushed */
	u32 allocs;          /* BoAllocs */
	u64 alloc_bytes, alloc_ns;
	u64 lat_sum, lat_max;                         /* user submissions only */
	u64 queue_sum, queue_max, exec_sum, exec_max; /* GPU-side, from the stamps (PTIMER units) */
	u32 copies,
		gaps; /* presents stamped: the copy's own GPU time; the GPU's gap from the end of the last user
				   * work the copy waited for to the copy's start (scheduling and semaphore acquire latency) */
	u64 copy_sum, copy_max, gap_sum, gap_max; /* PTIMER units */
	/* the compositor's submissions: the GPU queue (kick to start stamp) of all of them, and of those whose kick found an
	 * app on the GPU (tgm_sched_kicked: preempted, mid-switch or deferred), PTIMER units */
	u32 comp, behind;
	u64 comp_sum, comp_max, behind_sum, behind_max;
	/* tgm_gpu.lock held by a channel's setup (chan_new) and teardown (chan_free): every VmBind, BoAlloc, BoImport and BoFree
	 * of every process, the compositor's too, waits for it; nanoseconds, the sum counts them */
	u64 lock_new_sum, lock_new_max, lock_free_sum, lock_free_max;
	u32 lock_new, lock_free;
	s64 live_bytes; /* host imports */
	s32 live;
};
extern struct stats_state tgm_stats;
extern u64 tgm_compositor_pool;

struct frame_gpu_state {
	u64 first, last, exec_ns;
	u32 submits, others;
	u32 imports, binds, allocs, frees;
	u64 import_ns, bind_ns, alloc_ns, clean_ns, clean_bytes;
};
extern struct frame_gpu_state tgm_frame_gpu;
void tgm_stats_waits(u64 ms);
void tgm_stats_mem(u64 ms);

/* Submit-to-fence latency (CPU clock), seen from the fence's nonstall interrupt (every channel's handler runs on each
 * one), and from the GPU's stamps (PTIMER): queue = the GPU started the submission - the kernel kicked it, exec = the
 * GPU finished it (L2 flush included) - started it. The present channel's stamps time its copy instead
 * (present_gap: and the gap before it). */
static u64 present_gap(u32 seqno, u64 start);
const char *tgm_prof_name(unsigned int c, char *buf, unsigned int len);

void tgm_present_stats(u32 *presents, u64 *sum, u64 *max);
void tgm_scrub_stats(char *buf, unsigned int len, u64 window_ms);

/* The stats line's GPU clock, every window: the rate the GPCPLL is programmed for (nvkm_clk_read: its M/N/PL registers),
 * whether the GPU runs from it (SEL_VCO bit 0, GPC2CLK_OUT's bypass divider 0 = 1:1), and the rate GPC 0's clock
 * counter measured (nvgpu gm20b_clk_get_gpcclk_clock_counter: NCGPCCLK cycles counted over 800 cycles of the PLL's
 * 38.4 MHz reference, hw_trim_gm20b.h trim_gpc_clk_cntr_ncgpcclk_cfg/cnt). A count takes 21 us; each window reads the
 * count the previous window started and starts the next, so nothing waits for it. A measured rate below the programmed
 * one would show a slowdown or gating the PLL's registers do not: with it the line prints THERM's idle slowdown factor
 * (CLK_SLOWDOWN_0 bits 21:16, 0 = off; gm20b_clk_init clears it) and GR's engine clock gating mode (THERM_GATE_CTRL(0)
 * bits 1:0: 0 run, 1 auto, 2 stop). Called by one submission at a time (the window's winner, tgm_stats_tick). */
#define TRIM_GPC0_NCGPCCLK_CFG 0x134124
#define TRIM_GPC0_NCGPCCLK_CNT 0x134128
#define NCGPCCLK_CFG_RESET 0x01000000u
#define NCGPCCLK_CFG_ENABLE 0x00100000u
#define NCGPCCLK_CFG_WRITE_EN 0x00010000u
#define NCGPCCLK_CYCLES 800u
void tgm_clk_measure(char *buf, unsigned int len);
void tgm_stats_gr_bind(struct tgm_chan *chan);
void tgm_stats_gr_unbind(struct tgm_chan *chan);
void tgm_stats_gr_context(void);
void tgm_stats_tick(void);
#define IDLE_LOG_NS (10ULL * 1000000000ULL)
#define GPCPLL_COEFF_R 0x137004 /* M 7:0, N 15:8, PL 21:16 */

struct idle_state {
	int on;       /* the idle clock is in use (atomic) */
	int low;      /* the GPCPLL runs the idle divider (atomic; changed under clk) */
	int dropping; /* a drop is queued or running (atomic) */
	void *clk;    /* mutex: every divider change */
	void *drop_w, *raise_w;
	u32 run_pl, idle_pl, run_khz, idle_khz;
	u32 vco_coeff, vco_khz, maximum_khz, requested_khz;
	int clock_available;
	u64 last_busy; /* atomic: the last kick or hint (ns) */
    u64 service_until;
    u64 kicked, retired; /* atomic: seqnos kicked / retired since boot (SwitchGpuIdleDeadline.h) */
    /* the periodic line (under clk) */
    u64 log_start, low_since, low_ns, raise_sum, raise_max;
    u32 drops, raises, hinted;
};
extern struct idle_state tgm_idle;
int tgm_clock_ceiling_restore(void);
u64 tgm_idle_raise(void);
void tgm_idle_start(void);
extern u64 tgm_high_end_ns, tgm_high_end_pt;

/* nouveau_fence_wait_uevent_handler(): a fence's NON_STALL_INTERRUPT arrived; wake the fence waiters. */
static void present_fenced(struct tgm_chan *chan);
static void scrub_fenced(struct tgm_chan *chan);
void tgm_chan_ramfc_due(struct tgm_chan *chan);

static int chan_push_wait(struct tgm_chan *chan, u32 push_nr);

static void order_forget(struct tgm_chan *chan);

/* fence pages:
 * Every connection's fence page (its channels' seqno words) is one page of a single pool range, so the GPU present's
 * address space maps all of them once and waits on any user channel with a semaphore acquire. Pages are handed out
 * round-robin and zeroed, so a page freed with an acquire still pending on it is not reused at once. */
#define TGM_FENCE_USER_PAGES \
	64 /* one per connection (TegraGM20BUserClient.cpp kMaxConnectionsAll) */
/* and one after them for the kernel's own client channels (tgm_fence_kernel), so every address space's fence view holds
 * their fences too */
#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)
/* A fence page is a whole VM page at most 16 KiB (XNU's PAGE_MAX_SIZE): its client maps it read-only, and a mapping is
 * whole VM pages, so a smaller slot would show the client its neighbours'. The GPU maps the range in 4 KiB pages. */
#define TGM_FENCE_SLOT ((size_t)16 * 1024)
#define TGM_FENCE_BYTES (TGM_FENCE_PAGES * TGM_FENCE_SLOT)
#define TGM_FENCE_GPU_PAGES (TGM_FENCE_BYTES / PAGE_SIZE)

struct fences_state {
	void *va;
	u64 pa;
	u64 used; /* bitmap of pages in use */
	u32 refs[TGM_FENCE_USER_PAGES];
	u32 next;
};
extern struct fences_state tgm_fence_pages;
extern u64 tgm_fences_pas[];
extern struct tgm_mem *tgm_fences_mem;
int tgm_fences_map(struct tgm_vm *vm);
int tgm_order_waits(struct tgm_chan *chan);

/* CPU readers of GPU-written memory:
 * The GPU is not IO-coherent; its sysmem PTEs are volatile (no GPU L2 lines), so a submission's writes are in DRAM once
 * its fence has passed, but the CPU may still hold stale lines of those pages (e.g. from copying the same IOSurface
 * three frames ago). For a reader outside the GPU client, NativeLegacyDisplay's copy of a swapped IOSurface: wait until
 * every submission made so far on every live channel has completed (or its channel died), then clean+invalidate
 * [va, va + bytes) to the point of coherency. With no user channel it returns at once and touches no cache, so the CPU
 * renderer's path pays nothing. Returns 0, or -ETIMEDOUT after timeout_ns (the range is invalidated either way). */
#define TGM_SYNC_MAX 512 /* 64 connections x 8 channels */

/* Host methods of MAXWELL_CHANNEL_GPFIFO_A not in the trimmed cl906f.h (open-gpu-doc classes/host/clb06f.h) */
#define NVB06F_FB_FLUSH 0x0024
#define NVB06F_MEM_OP_C 0x0030
#define NVB06F_MEM_OP_D 0x0034
#define NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY (0x10u << 27)
#define NVB06F_WFI 0x0078

/* Push dwords chan_fence() may write, at most */
#define CHAN_FENCE_DWORDS 48
int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr);
/* external: TGM_FENCE_VOLATILE_WRITES when every write the fence covers went around the L2: the engines idle and their
 * writes committed, no L2 flush */
#define TGM_FENCE_VOLATILE_WRITES 2
/* gpu_fault.c; TGM_KILL_GR_ERROR is the kill record's error for a GR error (gpu_hang.c fault_code) */
#define TGM_KILL_GR_ERROR 2
void tgm_fault_publish(struct tgm_chan *chan);
void tgm_fault_unpublish(struct tgm_chan *chan);
void tgm_fence_kill_record(volatile u32 *fence_cpu, u32 first, u32 last, u32 error);
u32 tgm_chan_fence_job(struct tgm_chan *chan, int external, u32 first, u32 next);
int tgm_backing_dependencies(struct tgm_chan *chan, const struct switch_gpu_backing_token *tokens,
			     u32 count, int emit);

/* GPU present (TegraDisplayFlip/TegraScanoutCopy.h):
 * The display's swap path hands a frame (an IOSurface, pitch-linear BGRA) to the GPU instead
 * of waiting for the GPU and swizzling it with the CPU (TegraGM20BUserClient.cpp tegra_gm20b_scanout_copy). One kernel
 * channel in its own address space, which maps the scanout surfaces, the swapped surfaces and every connection's fence
 * page (tgm_fence_page_get), does per frame:
 *  - a semaphore ACQUIRE_GEQ (acquire switch on: the channel yields while it waits) on the last submitted seqno of every
 *    user channel whose address space imports the surface (its first page, tgm_vm_import_note; every user channel if
 *    none does), so the copy runs after the compositor's rendering in GPU order, no
 *    CPU waits for it, and an app's GPU work does not hold up the frame;
 *  - one MAXWELL_DMA_COPY_A LAUNCH_DMA, pitch source (the surface's row bytes) to a BLOCKLINEAR destination: GOBs of
 *    64 bytes x 8 lines (GOB_HEIGHT_FERMI_8), blocks one GOB wide and 16 GOBs high, width 5120 bytes: the layout window A
 *    column-scans (NativeScanout.h native_legacy_block_copy);
 *  - the kernel fence (chan_fence: WFI, the L2 flush, semaphore release, NON_STALL_INTERRUPT).
 * The fence's nonstall interrupt completes it: the caller's callback (native_legacy_gpu_complete, which any context may
 * call) runs from the interrupt pass, with TEGRA_SCANOUT_SOURCE_LOST when a writer it waited for was killed before that
 * submission ended (the acquire passed on the kill's release, over a partial frame). A present that has not completed 1 s
 * after it was queued panics, naming what it waits for. Every mapping is volatile: the source was written by work whose
 * fence flushed the L2 (or bypassed it), the destination is read by the display controller only. The copy is
 * kits/SwitchGM20BEmit's (TegraGM20BCopyEmit.c).
 * No user submission waits for a present: the compositor never writes a surface a copy still reads. Core Animation
 * recycles a framebuffer only when a later swap completes, and the display offers the next swap only once the copy
 * before it completed (NativeLegacyDisplay offerGpuCopy). */
#define TGM_PRESENT_NS \
	(1000 *        \
	 NSEC_PER_MSEC) /* as the display's own limit: compositor jobs up to ~90 ms were seen */

struct present_state {
	struct mutex lock;
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *fence_mem, *fences_mem;
	void *fence; /* the channel's own fence page (pool) */
	u64 fence_pa, fence_gpu, fences_gpu;
	void *deadline_work, *completion_work;
	struct switch_gpu_present_tokens tokens;
	u32 typed_pending;
	int completion_status;
	u32 pending; /* seqno of the present in flight, 0: none (atomic: the interrupt pass completes it) */
	u64 queued;
	void (*done)(void *, int);
	void *arg;
	u32 completing; /* completions claimed whose callback has not returned yet */
	u32 waits;      /* the acquires of the present in flight, for the deadline's line */
	int writers;    /* they are the surface's writers', not every user channel's */

	struct {
		u64 id;
		u32 seqno;
		const volatile struct TegraGM20BAbiFence *fence;
	} wait[TGM_SYNC_MAX];
	volatile struct tgm_stamp
		*wait_end[TGM_SYNC_MAX]; /* stats: their end stamps (pool memory, kept mapped) */
	u32 stamped;  /* stats: seqno of the present in flight, whose copy is stamped */
	u32 presents; /* stats */
	u64 lat_sum, lat_max;
	u64 recovered; /* when the present in flight last found a recovery under way, or work still progressing */
};
extern struct present_state tgm_present;
void tgm_present_stats(u32 *presents, u64 *sum, u64 *max);

/* pool scrub: the copy engine zeroes pool pages:
 * The CPU would spend 0.3 s of every boot zeroing the pool at M1 (256 MiB through the write-combined aperture), and a
 * thread call's memset 4.5 ms per 16 MiB of freed pool BO. So its pages start dirty (shim nvxnu_pool_init) and the
 * GPU zeroes them: a kernel channel (CHAN_SCRUB) in an address space of its own, which maps
 * the whole pool once (volatile, at tgm_scrub.pool_va), runs MAXWELL_DMA_COPY_A fills of remapped constant 0 (as Mesa's
 * nvk_cmd_fill_memory_ce and M3), each range followed by the kernel fence (chan_fence: WFI, FB flush, L2 flush of
 * dirty lines, then the semaphore release and the nonstall interrupt), so the zeros are in DRAM before the range goes
 * back clean (nvxnu_pool_free_zeroed) from that interrupt's pass. Nothing waits for it on the CPU: until then a range is
 * the scrubber's (allocated), and an allocation takes other pages, zeroing on the CPU only those a free left dirty.
 *  - Boot: once M3 passed, the scrubber's work item claims the free dirty pages (shim nvxnu_pool_claim_dirty) a run of at
 *    most SCRUB_CHUNK at a time, with at most SCRUB_INFLIGHT bytes queued, and claims more as ranges complete, so it
 *    never holds more than that of the pool from an allocation.
 *  - A freed pool BO (TegraGM20BUserClient.cpp poolZeroLater): tgm_pool_scrub queues its range.
 * The CPU has no cacheable view of the pool, so the zeros need no cache maintenance. A range not back SCRUB_NS after it
 * was queued panics with one line, as a present does: its nonstall interrupt never came, or the copy never ran. */
#define SCRUB_JOBS 64

struct scrub_state {
	struct mutex lock; /* submissions: the ring's head, the channel's push buffer */
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *pool_mem, *fence_mem;
	u64 *pool_pas;
	u64 pool_va, fence_gpu, fence_pa;
	volatile u32 *fence;
	void *work, *deadline;

	struct {
		u64 pa, bytes, queued;
		u32 seqno;
	} job[SCRUB_JOBS];

	u32 head,
		tail; /* job[tail % SCRUB_JOBS .. head) queued; head moves under lock, tail in scrub_fenced only */
	u64 inflight;  /* bytes queued (atomic) */
	int boot_done; /* the boot scrub found nothing left to claim */
	u64 boot_start, boot_bytes;
	/* stats: this window's ranges zeroed, their bytes and queue-to-done time (ns) */
	u64 n, bytes, ns, max_ns;
};
extern struct scrub_state tgm_scrub;
void tgm_scrub_open(void);
void tgm_scrub_stats(char *buf, unsigned int len, u64 window_ms);
void tgm_rail_up_locked(const void *site);
/* gpu_zcull.c: zcull's layout read from GR's registers, once GR is up (m2) */
void tgm_zcull_init(struct nvkm_device *device);
#pragma GCC visibility pop

#include "SwitchGpuActiveIrq.h"
#include "SwitchGpuIdleCount.h"
#include "gpu_inline.h"
#include "gpu_fenced.h"
