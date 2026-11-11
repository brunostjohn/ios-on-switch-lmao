// SPDX-License-Identifier: MIT
// Channel progress, hang detection, client fault isolation and recovery.
#include "gpu_internal.h"

uint32_t tgm_chan_submitted(struct tgm_chan *chan)
{
	return chan->submitted;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

uint32_t tgm_chan_error(struct tgm_chan *chan)
{
	return chan->error;
}

/* The target is released after the caller's snapshot of each channel's submitted seqno and acquired before
 * chan_exec moves its channel's: a submission that waits for this epoch is never in that snapshot. */
void tgm_vm_bind_wait(struct tgm_vm *vm, uint64_t va, const volatile uint32_t *cpu, uint32_t target)
{
	WRITE_ONCE(vm->bind_va, va);
	__atomic_store_n(&vm->bind_target, target, __ATOMIC_RELEASE);
	__atomic_store_n(&vm->bind_cpu, cpu, __ATOMIC_RELEASE);
}

uint32_t tgm_chan_settled(struct tgm_chan *chan, int *failed)
{
	const u32 done = __atomic_load_n(chan->fence_cpu, __ATOMIC_ACQUIRE);

	*failed = READ_ONCE(chan->error) != 0;
	return done;
}

uint64_t tgm_chan_fault_va(struct tgm_chan *chan)
{
	return READ_ONCE(chan->fault_va);
}
#if TGM_STATS
uint64_t tgm_chan_killed_ns(struct tgm_chan *chan)
{
	return READ_ONCE(chan->killed_ns);
}

#endif
uint32_t tgm_chan_waiter(struct tgm_chan *chan)
{
	return (uint32_t)(chan->id % TGM_WAITERS);
}

/* One line of what the GPU is doing about a channel: owner, hardware channel id, error, completed/submitted fence and
 * how long ago the last one was kicked; GPFIFO GET/PUT as the host sees them (USERD 0x88/0x8c) and the PUT the kernel
 * wrote; PGRAPH_STATUS (0x400700, bit 0 busy), PFIFO engine 0 (GR) status (0x2640); PBDMA 0: status (0x3080, bits 13-15
 * busy, gk104_runq_idle), the channel it serves (0x40120) and the method/data it is on (0x400c0/0x400c4: subchannel
 * 18:16, method 13:2, as gf100_runq_intr decodes them; a NV906F semaphore method there means a semaphore acquire that is
 * not satisfied); PMC_INTR_0; and the memory controller's last refused access (MC_INTSTATUS, MC_ERR_STATUS, MC_ERR_ADR). */
void chan_describe(struct tgm_chan *chan, char *buf, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	volatile u32 *userd = chan->user.map.ptr;
	const u32 eng = nvkm_rd32(device, 0x002640);
	u32 mc_int = 0;
	u32 mc_err = 0;
	u32 mc_adr = 0;
	char engine[48];
	char note[112];

	tgm_hang_engine(eng, engine, sizeof(engine));
	tgm_hang_note(&chan->hang, tgm_chan_completed(chan) + 1, chan->submitted,
		      chan->chan.gpfifo.map.ptr, chan->chan.gpfifo.max, chan->ring_va,
		      CHAN_PUSH_BYTES, chan->vm->connection, note, sizeof(note));
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	scnprintf(
		buf, len,
		"%s ch %u err %u fence %u/%u kicked %llu ms ago%s, gp %u/%u (put %u), PGRAPH %08x "
		"ENG0 %08x (%s), "
		"PBDMA0 %08x ch %u mthd %08x data %08x, PMC %08x, MC %08x err %08x at %08x",
		chan->owner[0] ? chan->owner : "?", chan->chid, chan->error,
		tgm_chan_completed(chan), chan->submitted,
		chan->submit_ns ? (nvxnu_nanotime() - chan->submit_ns) / 1000000 : 0ULL, note,
		userd ? userd[0x88 / 4] : 0, userd ? userd[0x8c / 4] : 0, chan->chan.gpfifo.cur,
		nvkm_rd32(device, 0x400700), eng, engine, nvkm_rd32(device, 0x003080),
		nvkm_rd32(device, 0x040120) & 0xfff, nvkm_rd32(device, 0x0400c0),
		nvkm_rd32(device, 0x0400c4), nvkm_rd32(device, 0x000100), mc_int, mc_err, mc_adr);
}

void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	char state[TGM_HANG_LINE];
	char order[64] = "";
	const struct tgm_order *w = &chan->waited;
	chan_describe(chan, state, sizeof(state));
	/* still waiting for another channel of its task (tgm_chan_order) */
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0) {
		scnprintf(order, sizeof(order), ", order wait on chan %llu fence %u (at %u)", w->id,
			  w->seqno, READ_ONCE(*w->cpu));
	}
	nvxnu_panic("tegra-gm20b: %s: fence %u still pending after %llu ms and no GPU error "
		    "reported: %s%s",
		    who, seqno, waited_ns / 1000000, state, order);
}

/* The time the hold kept the channel off the GPU is not the channel's: it never counts toward a stall. */
uint64_t tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno)
{
	u32 done = tgm_chan_completed(chan);
	u64 now = nvxnu_nanotime();
	u64 since = READ_ONCE(chan->stall_ns);
	u64 held = tgm_hold_held_ns(chan->chid, now);
	u64 stalled = 0;

	if ((s32)(done - chan->submitted) >= 0) {
		WRITE_ONCE(chan->stall_ns, 0);
		return 0;
	}
	if (!since || READ_ONCE(chan->stall_done) != done) {
		WRITE_ONCE(chan->stall_done, done);
		WRITE_ONCE(chan->stall_held, held);
		WRITE_ONCE(chan->stall_ns, now);
		return TGM_HANG_NS;
	}
	stalled = tgm_unheld_ns(now, since, held, READ_ONCE(chan->stall_held));
	if (stalled < TGM_HANG_NS)
		return TGM_HANG_NS - stalled;
	tgm_chan_stalled(chan, who, seqno, stalled);
	WRITE_ONCE(chan->stall_held, held);
	WRITE_ONCE(chan->stall_ns, now);
	return TGM_HANG_NS;
}

int tgm_all_clients(void)
{
	return 1; /* every process that asks gets a device */
}
struct iso_state iso;

static u32 fault_code(const char *report)
{
	if (!strncmp(report, HANG_REPORT, strlen(HANG_REPORT)) || strstr(report, "CTXSW_TIMEOUT"))
		return 4;
	if (strstr(report, "fault "))
		return 1;
	if (strstr(report, "TRAP") || strstr(report, "ILLEGAL_") || strstr(report, "DATA_ERROR"))
		return 2;
	return 3;
}

static void fence_kill_record(volatile u32 *fence_cpu, u32 submitted, u32 error)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->killed_first, READ_ONCE(fence->completed) + 1);
	WRITE_ONCE(fence->killed_last, submitted);
	__asm__ volatile("dsb sy" ::: "memory");
	WRITE_ONCE(fence->error, error);
	__asm__ volatile("dsb sy" ::: "memory");
}

void tgm_fence_kill_clear(volatile uint32_t *fence_cpu)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->error, 0);
	WRITE_ONCE(fence->killed_first, 0);
	WRITE_ONCE(fence->killed_last, 0);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* nvkm killed the channel (nvkm_chan_error, from its recovery work). Before M3, M3 reads chan->error. From M3 on: an
 * isolated client channel is dead (see above); any other panics, with the report that started it if the interrupt pass
 * did not panic first. */
int chan_killed(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, kill);
	const char *report = chan->fault[0] ? chan->fault : nvxnu_gpu_errors();
	char state[TGM_HANG_LINE];

	(void)repv;
	(void)repc;
	if (gpu.stage >= 3 && (chan->kernel || !tgm_all_clients())) {
		chan_describe(chan, state, sizeof(state));
		nvxnu_panic("tegra-gm20b: channel killed by nvkm recovery after \"%s\": %s",
			    nvxnu_gpu_errors(), state);
	}
	if (gpu.stage < 3) {
		if (!chan->error)
			chan->error = 3;
		return NVIF_EVENT_DROP;
	}
	/* ERRORED is delivered after runlist recovery quiesces the channel. Publish
	 * its failure before advancing abandoned fences or releasing memory leases. */
#if TGM_STATS
	WRITE_ONCE(chan->killed_ns, nvxnu_nanotime());
#endif
	if (fault_code(report) == 1)
		WRITE_ONCE(chan->fault_va, tgm_fault_va(report, (int)chan->chid));
	WRITE_ONCE(chan->error, fault_code(report));
	switch_gpu_timeline_failed(chan->timeline, READ_ONCE(*chan->fence_cpu));
	fence_kill_record(chan->fence_cpu, chan->submitted, chan->error);
	__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_log("tegra-gm20b: GPU error in %s (channel %u): channel killed, GPU running: %s\n",
		  chan->owner, chan->chid, report);
	nvxnu_wake();
	chan_wake_all_waiters(chan);
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return NVIF_EVENT_DROP;
}

/* The next channel id a report names after p ("channel N [" or "ch N"), -2 when there is none; -1 is nvkm's "unknown". */
static const char *next_chid(const char *p, int *chid)
{
	static const char *const tags[] = {"channel ", "ch "};
	const char *best = NULL;
	const char *at = NULL;
	unsigned int i = 0;
	unsigned int len = 0;

	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		for (at = p; (at = strstr(at, tags[i])); at++) {
			if (at == p || at[-1] == ' ' || at[-1] == '|')
				break;
		}
		if (at && (!best || at < best)) {
			best = at;
			len = strlen(tags[i]);
		}
	}
	if (!best)
		return NULL;
	best += len;
	*chid = -2;
	if (*best == '-' || (*best >= '0' && *best <= '9'))
		*chid = (int)simple_strtol(best, (char **)&best, 10);
	return best;
}

/* 1 if nvkm marked the channel's group for recovery (nvkm_runl_rc_cgrp). */
static int chan_marked(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;
	int marked = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (!nchan)
			continue;
		marked = atomic_read(&nchan->cgrp->rc) != NVKM_CGRP_RC_NONE;
		nvkm_chan_put(&nchan, flags);
		break;
	}
	return marked;
}

static int fifo_error(const char *report, const char *end)
{
	const char *sched = strstr(report, "SCHED_ERROR");
	const char *bind = strstr(report, "BIND_ERROR");

	return (sched && (!end || sched < end)) || (bind && (!end || bind < end));
}

/* 1 if every report of the pass is a scheduler or bind error: recover.c takes those from nvkm's handler and decides on
 * them (tgm_ctxsw_decide), so one here came between its handler and nvkm's. */
static int only_fifo_errors(const char *errors)
{
	const char *p = errors;
	const char *bar = NULL;

	for (;;) {
		bar = strstr(p, " | ");
		if (!fifo_error(p, bar))
			return 0;
		if (!bar)
			return 1;
		p = bar + 3;
	}
}

static void chan_recover(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_runl_rc_cgrp(nchan->cgrp); /* a no-op if nvkm already started it */
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

/* live_lock held */
void chan_isolate(struct tgm_chan *chan, const char *report, u64 now)
{
	if (chan->isolating || chan->error)
		return;
	chan->isolating = 1;
	chan->isolated_ns = now;
	tgm_hold_drop(chan->chid);
	strscpy(chan->fault, report, sizeof(chan->fault));
	chan_recover(chan->chid);
	iso.until = now + TGM_ISOLATE_NS;
	nvxnu_work_schedule_after(iso.deadline, TGM_ISOLATE_NS);
}

/* From the GPU interrupt work loop after a pass with error reports (nvxnu_errors_fatal's filter): 1 if they stay with
 * client channels, whose kills are then under way; 0 to panic. */
int errors_contained(const char *errors)
{
	struct tgm_chan *named[8];
	struct tgm_chan *chan = NULL;
	struct tgm_chan *found = NULL;
	const char *p = errors;
	unsigned int n = 0;
	unsigned int i = 0;
	int contained = 1;
	int chid = 0;
	u64 now = nvxnu_nanotime();

	mutex_lock(&live_lock);
	while (contained) {
		p = next_chid(p, &chid);
		if (!p)
			break;
		if (chid == -2)
			continue; /* "ch" with no number: not an attribution */
		found = NULL;
		list_for_each_entry(chan, &live_chans, live)
		{
			if ((int)chan->chid == chid) {
				found = chan;
				break;
			}
		}
		if (!found || found->kernel)
			contained = 0;
		for (i = 0; contained && i < n && named[i] != found; i++)
			;
		if (contained && i == n && n < ARRAY_SIZE(named))
			named[n++] = found;
	}
	/* channels nvkm put into recovery itself, not yet dead or isolated */
	list_for_each_entry(chan, &live_chans, live)
	{
		if (!contained || chan->error || chan->isolating || !chan_marked(chan->chid))
			continue;
		for (i = 0; i < n && named[i] != chan; i++)
			;
		if (chan->kernel) {
			contained = 0;
		} else if (i == n && n < ARRAY_SIZE(named)) {
			named[n++] = chan;
		}
	}
	if (!n && (!iso.until || now >= iso.until) && !only_fifo_errors(errors))
		contained = 0;
	for (i = 0; contained && i < n; i++)
		chan_isolate(named[i], errors, now);
	mutex_unlock(&live_lock);
	return contained;
}

void iso_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	char state[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();

	(void)arg;
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->isolating && !READ_ONCE(chan->error) &&
		    now - chan->isolated_ns >= TGM_ISOLATE_NS) {
			chan_describe(chan, state, sizeof(state));
			nvxnu_panic("tegra-gm20b: GPU error in %s not recovered %llu ms after it "
				    "(\"%s\"): %s",
				    chan->owner,
				    (unsigned long long)(TGM_ISOLATE_NS / NSEC_PER_MSEC),
				    chan->fault, state);
		}
	}
	if (iso.until && now >= iso.until)
		iso.until = 0;
	mutex_unlock(&live_lock);
}

/* live_lock held. Internal fences move the fence word without an interrupt: a move the interrupt pass has not seen
 * counts as progress now, when a check first sees it. */
static u64 chan_progress_ns(struct tgm_chan *chan, u64 now)
{
	const u32 done = tgm_chan_completed(chan);

	if (tgm_progress_seen(done, READ_ONCE(chan->progress_done), &chan->seen_done)) {
		WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, now));
		WRITE_ONCE(chan->progress_ns, now);
	}
	return max(READ_ONCE(chan->progress_ns), READ_ONCE(chan->busy_ns));
}

/* The hold's time since the channel's last progress is left out, as tgm_chan_check_progress leaves it out. */
static int chan_stuck(struct tgm_chan *chan, u64 now)
{
	u64 since = chan_progress_ns(chan, now);
	u64 unheld = tgm_unheld_ns(now, since, tgm_hold_held_ns(chan->chid, now),
				   READ_ONCE(chan->progress_held));

	return tgm_stuck(READ_ONCE(chan->error) != 0, tgm_chan_completed(chan),
			 READ_ONCE(chan->submitted), since + unheld, since);
}

/* Whether the channel's fence moved within the stuck time, or its GET did since hang_recover last read it (a long job
 * whose methods the engine still takes moves GET, not the fence). USERD's GP_GET and GET are what the host wrote back. */
static int chan_moving(struct tgm_chan *chan, u64 now)
{
	volatile u32 *userd = chan->user.map.ptr;
	const u64 get =
		userd ? (u64)userd[0x88 / 4] << 40 ^ (u64)userd[0x44 / 4] << 32 ^ userd[0x40 / 4]
		      : 0;

	if (get != chan->get_seen) {
		chan->get_seen = get;
		chan->get_ns = now;
	}
	return now - max(chan_progress_ns(chan, now), chan->get_ns) < TGM_STUCK_NS;
}

/* A wait on `late` (the present's or the scrubber's channel when its own work is late) ran out with no GPU error
 * reported. The channel to blame (TegraGM20BRecover.h tgm_hang_blame) is the first stuck one of what the engines and
 * PBDMAs are on (a context switch that does not finish blames the context nvkm's own recovery would), the channel of its
 * task `late` still waits on (tgm_chan_order), and `late`. A client's is killed as a faulting client's is, and the waits
 * it held up go on. live_lock held. */
#if TGM_STATS
/* live_lock held: the blamed channel's own order wait and the late one's progress, as a line of its
 * own (TegraGM20BHangDecode.h tgm_hang_detail) */
static void hang_detail(struct tgm_chan *blame, struct tgm_chan *late, u64 now)
{
	const struct tgm_order *w = &blame->waited;
	struct tgm_hang_detail d = {0};
	struct tgm_chan *source = NULL;
	const volatile u32 *bind = NULL;
	char line[TGM_HANG_LINE];

	d.chid = blame->chid;
	d.stalled_ms = (now - chan_progress_ns(blame, now)) / 1000000;
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0) {
		d.waits = 1;
		d.wait_chid = w->chid;
		d.wait_seqno = w->seqno;
		d.wait_at = READ_ONCE(*w->cpu);
		source = live_by(w->id, 0);
	}
	if (source) {
		d.source_live = 1;
		d.source_done = tgm_chan_completed(source);
		d.source_submitted = READ_ONCE(source->submitted);
		d.source_ms = (now - chan_progress_ns(source, now)) / 1000000;
	}
	d.late_chid = late->chid;
	d.late_done = tgm_chan_completed(late);
	d.late_submitted = READ_ONCE(late->submitted);
	d.late_ms = (now - chan_progress_ns(late, now)) / 1000000;
	tgm_hang_detail_format(line, sizeof(line), &d);
	bind = blame->vm ? __atomic_load_n(&blame->vm->bind_cpu, __ATOMIC_ACQUIRE) : NULL;
	nvxnu_log("tegra-gm20b: GPU hang detail: %s; bind epoch %u of %u\n", line,
		  bind ? READ_ONCE(*bind) : 0, bind ? READ_ONCE(blame->vm->bind_target) : 0);
}

#endif
enum tgm_hang hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns)
{
	struct tgm_hang_candidate c[8] = {};
	struct tgm_chan *chans[8] = {};
	struct tgm_chan *chan = NULL;
	struct tgm_chan *blame = NULL;
	char report[sizeof(late->fault)];
	char state[TGM_HANG_LINE];
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u64 now = nvxnu_nanotime();
	int suspects[4];
	int resident = 0;
	int n = 0;
	int i = 0;
	int j = 0;
	int b = 0;
	int progress = 0;

	if (gpu.stage < 3 || !iso.deadline)
		return TGM_HANG_STUCK;
	resident = tgm_recover_suspects(gpu.device, suspects, ARRAY_SIZE(suspects));
	for (n = 0; n < resident; n++)
		chans[n] = live_by(0, suspects[n]);
	/* the late one's order acquires, source by source, up to two hops, then the late one itself last */
	for (chan = late, i = 0; chan && i < 2; i++) {
		struct tgm_chan *source = NULL;

		if (!chan->waited.cpu ||
		    (s32)(READ_ONCE(*chan->waited.cpu) - chan->waited.seqno) >= 0)
			break;
		source = live_by(chan->waited.id, 0);
		if (!source)
			break;
		chans[n++] = source;
		chan = source;
	}
	chans[n] = late;
	for (i = 0; i <= n; i++) {
		c[i].live = chans[i] != NULL;
		c[i].kernel = chans[i] && chans[i]->kernel;
		c[i].resident = i < resident;
		c[i].moving = chans[i] && chan_moving(chans[i], now);
		for (j = 0; j < i && chans[j] != chans[i]; j++)
			;
		/* once per channel and check: a channel among both suspects and sources is one observation */
		if (j < i)
			c[i].stuck = c[j].stuck;
		else
			c[i].stuck =
				chans[i] &&
				tgm_hang_confirm(chan_stuck(chans[i], now) && !c[i].moving,
						 tgm_chan_completed(chans[i]),
						 &chans[i]->hang_strikes, &chans[i]->hang_seen);
	}
	for (i = 0; i <= n; i++) {
		const struct tgm_order *w = chans[i] ? &chans[i]->waited : NULL;

		if (!w || !w->cpu || (s32)(READ_ONCE(*w->cpu) - w->seqno) >= 0)
			continue;
		for (j = 0; j <= n; j++)
			if (j != i && chans[j] && chans[j]->id == w->id)
				c[i].waits = (u8)(j + 1);
	}
	list_for_each_entry(chan, &live_chans, live)
	{
		if (now - chan_progress_ns(chan, now) < TGM_STUCK_NS)
			progress = 1;
	}
	verdict = tgm_hang_blame(c, n + 1, READ_ONCE(late->error) != 0, late->kernel, progress, &b);
	if (b < 0 || chans[b]->isolating)
		return verdict;
	blame = chans[b];
	chan_describe(blame, state, sizeof(state));
	scnprintf(report, sizeof(report), HANG_REPORT "%s waited %llu ms for %s fence %u; %s", who,
		  waited_ns / 1000000, late->owner, seqno, state);
	nvxnu_log(
		"tegra-gm20b: GPU hang, recovering %s (channel %u): %s waited %llu ms for %s fence "
		"%u; %s\n",
		blame->owner, blame->chid, who, waited_ns / 1000000, late->owner, seqno, state);
#if TGM_STATS
	hang_detail(blame, late, now);
#endif
	chan_isolate(blame, report, now);
	tgm_recover_fault(gpu.device, blame->chid);
	return verdict;
}

enum tgm_ctxsw tgm_ctxsw_decide(int chid)
{
	struct tgm_chan *chan = NULL;
	char report[sizeof(chan->fault)];
	char state[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();
	enum tgm_ctxsw verdict = TGM_CTXSW_RESET;

	mutex_lock(&live_lock);
	chan = live_by(0, chid);
	if (chan) {
		verdict = tgm_ctxsw_verdict(1, READ_ONCE(chan->error) != 0, chan->kernel,
					    chan_stuck(chan, now));
		if (verdict == TGM_CTXSW_FATAL) {
			chan_describe(chan, state, sizeof(state));
			nvxnu_panic("tegra-gm20b: context switch timed out on %s's channel: %s",
				    chan->owner, state);
		}
		if (verdict == TGM_CTXSW_KILL && !chan->isolating) {
			chan_describe(chan, state, sizeof(state));
			scnprintf(report, sizeof(report), "CTXSW_TIMEOUT: %s", state);
			nvxnu_log("tegra-gm20b: context switch timed out, recovering %s (channel "
				  "%u): %s\n",
				  chan->owner, chan->chid, state);
			chan_isolate(chan, report, now);
		}
	}
	mutex_unlock(&live_lock);
	return verdict;
}

void tgm_chan_stalled(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	enum tgm_hang verdict = TGM_HANG_STUCK;

	mutex_lock(&live_lock);
	verdict = hang_recover(chan, who, seqno, waited_ns);
	mutex_unlock(&live_lock);
	if (verdict == TGM_HANG_STUCK)
		tgm_chan_hung(chan, who, seqno, waited_ns);
}
