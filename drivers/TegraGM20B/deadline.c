// SPDX-License-Identifier: MIT
/*
 * The compositor's deadline (TegraGM20BSched.h). The display sends Core Animation a vsync only while CA has vsync on (it
 * turns it on when it schedules an update, off after an idle countdown: QuartzCore IOMFBServer::set_vsync_enabled and
 * vsync_callback), and CA starts a frame at it for the vblank after the one it came with; the display passes each one on
 * here with its real vblank (tegra_gm20b_compositor_vsync). 24B119's CA never turns it on (IOMFBServer's use-VBL bit is
 * cleared at construction and nothing sets it; its frame clock is a CADisplayTimer), so there the display passes each
 * frame end on instead. A window then holds each app channel slow enough to be worth it (tgm_hold_worth) from its own
 * preemption latency before the compositor's latest start (that vblank less the compositor's GPU time per frame) until the
 * frame's present completes (hold.c); with no such channel no window opens. Nor does one while none of them has work in
 * flight (tgm_busy): the vblank it would aim for is kept, and the first such channel's kick before it opens the window
 * and takes the holds due by then before its doorbell, as an open window's hold would have found it. The GPU time per
 * frame is learned from the high channels' completions (gpu_fenced.h
 * high_fenced), summed per present. QuartzCore's own hint (tegra_gm20b_display_deadline) comes once a second
 * (hint_agx_timing::next_agx_hint), too seldom to start a window from. Every decision is made at a vsync, a kick, a
 * fence interrupt or a host-time callout; nothing waits or polls.
 */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BCostHist.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

static struct {
	spinlock_t lock;
	struct nvkm_device *device;
	void *hold_work, *check_work, *expire_work;
	u64 interval;    /* the frame interval */
	u64 next_target; /* the vblank the frame CA started at the last vsync aims for */
	u64 vsync_last, vsync_since; /* the last vsync to CA, and the start of its unbroken run */
	u64 expected;                /* the compositor's GPU time per frame */
	/* the high kicks' own, plain stores and no lock (a kick's time is a hint a racing kick may set a few us back): the
	 * compositor's last kick, the last kick that found an app on the GPU, the vblank the frame's first kick aimed for */
	u64 kick_last, kick_app_last, frame_target;
	/* the interrupt pass's own: the compositor's GPU time since the present that moved frame_epoch to pass_epoch, and
	 * whether an app biased it; the PTIMER anchor and rate, anchored again when resume moves recal */
	u64 pass_ns, cal_ns, cal_pt, pt_rate;
	u32 pass_epoch, recal_seen;
	int pass_biased;
	/* a present's (dl.lock): when the last one completed, the frame epoch it closed, and whether a work item completed it
	 * (a kill's, a late look's): a pass may still add that frame's last job to the next frame's time */
	u64 presented_at;
	u32 frame_epoch, recal;
	int tail;

	/* the window: the vblank it aims for (0: none), when it opened (the compositor kicked in it when kick_last is
	 * later); the vblank of one no app's work in flight opened (0: none), which an app's first kick opens
	 * (tgm_deadline_app_kicked) */
	u64 target, opened, expire_at, dormant, dormant_since;
	u32 gen, expire_gen;
	int suspended, check_chid;
	u32 frames, missed_app, missed_clear, windows, empty, expired;
} dl;

#if TGM_STATS
/* The stats window's learned frames: `dl.expected` is a peak tracker (tgm_expect), so the line sets them beside it. */
static struct {
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum, max;
} frame_cost;

static void frame_cost_add(u64 ns)
{
	frame_cost.hist[tgm_cost_bucket(ns)]++;
	frame_cost.n++;
	frame_cost.sum += ns;
	if (ns > frame_cost.max)
		frame_cost.max = ns;
}
#endif

static void hold_work(void *arg);
static void check_work(void *arg);
static void expire_work(void *arg);

void tgm_deadline_init(struct nvkm_device *device)
{
	if (dl.device)
		return;
	spin_lock_init(&dl.lock);
	dl.hold_work = nvxnu_work_alloc(hold_work, NULL);
	dl.check_work = nvxnu_work_alloc(check_work, NULL);
	dl.expire_work = nvxnu_work_alloc(expire_work, NULL);
	if (!dl.hold_work || !dl.check_work || !dl.expire_work)
		nvxnu_panic("tegra-gm20b: deadline: no thread calls");
	WRITE_ONCE(dl.device, device);
}

static int drawing(u64 now)
{
	return tgm_compositor_drawing(now, dl.interval, dl.vsync_last, dl.vsync_since,
				      READ_ONCE(dl.kick_last));
}

static void after(void *work, u64 at, u64 now)
{
	nvxnu_work_schedule_after(work, at > now ? at - now : 0);
}

/* dl.lock held: a window for the frame aiming at `target`; the holds come from hold_work, now and at each one's time */
static void window_open(u64 target, u64 now)
{
	WRITE_ONCE(dl.dormant, 0);
	dl.target = target;
	dl.opened = now;
	dl.expire_at = target;
	dl.expire_gen = ++dl.gen;
	if (TGM_STATS)
		dl.windows++;
}

/* dl.lock held */
static void window_close(u64 now)
{
	tgm_hold_release(dl.device, now);
	dl.target = 0;
}

/* dl.lock held, no window open: the window for the frame aiming at `target` opens now if a slow app channel has work in
 * flight; with only slow ones that have none it waits for the first one's kick (dormant). Returns the window's vblank. */
static u64 window_due(u64 target, u64 now)
{
	WRITE_ONCE(dl.dormant, 0);
	if (dl.suspended || !dl.interval || dl.interval > TGM_INTERVAL_MAX_NS || !drawing(now) ||
	    !tgm_hold_wanted(0))
		return 0;
	if (!tgm_hold_wanted(1)) {
		/* kept, then looked at again: a kick between the two either sees it or is seen */
		dl.dormant_since = now;
		__atomic_store_n(&dl.dormant, target, __ATOMIC_RELEASE);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!tgm_hold_wanted(1))
			return 0;
	}
	window_open(target, now);
	return target;
}

void tegra_gm20b_compositor_vsync(uint64_t stamp, uint32_t lead_us, uint64_t interval)
{
	struct nvkm_device *device = READ_ONCE(dl.device);
	unsigned long flags = 0;
	u64 now = nvxnu_nanotime();
	u64 step = 0;
	u64 vblank = 0;
	u64 target = 0;

	if (!device)
		return;
	step = nvxnu_abstime_to_ns(interval);
	vblank = nvxnu_abstime_to_ns(stamp) + lead_us * 1000ULL;
	spin_lock_irqsave(&dl.lock, flags);
	if (!dl.vsync_last || now - dl.vsync_last > step * 3 / 2)
		dl.vsync_since = now;
	dl.vsync_last = now;
	dl.interval = step;
	dl.next_target = vblank + step;
	if (!dl.target)
		target = window_due(dl.next_target, now);
	spin_unlock_irqrestore(&dl.lock, flags);
	if (target) {
		nvxnu_work_schedule(dl.hold_work);
		after(dl.expire_work, target, now);
	}
}

/* dl.lock held: a window is open and the GPU may be touched (system sleep cancels and waits for the callouts after it
 * sets suspended, and they look before they read a register) */
static int window_live(u64 now)
{
	return dl.target && !dl.suspended && drawing(now);
}

static void hold_work(void *arg)
{
	u64 now = nvxnu_nanotime();
	u64 next = 0;
	int resident = 0;
	int preempt = -1;
	int live = 0;
	struct tgm_window w;
	unsigned long flags = 0;

	(void)arg;
	spin_lock_irqsave(&dl.lock, flags);
	live = window_live(now);
	spin_unlock_irqrestore(&dl.lock, flags);
	if (!live)
		return;
	resident = tgm_sched_resident(dl.device);
	spin_lock_irqsave(&dl.lock, flags);
	if (window_live(now)) {
		w.now = now;
		w.vblank = dl.target;
		w.interval = dl.interval;
		w.expected = dl.expected;
		next = tgm_hold_due(dl.device, &w, resident, &preempt);
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (preempt >= 0 && tgm_hold_preempt(dl.device, preempt)) {
		WRITE_ONCE(dl.check_chid, preempt);
		nvxnu_work_schedule_after(dl.check_work, tgm_hold_latency(preempt) / 2);
	}
	if (next)
		after(dl.hold_work, next, now);
}

static void check_work(void *arg)
{
	(void)arg;
	if (!READ_ONCE(dl.suspended))
		tgm_hold_check(dl.device, READ_ONCE(dl.check_chid));
}

/* At the window's vblank: a window the compositor never kicked in had no frame (CA drew nothing at that vsync) and ends;
 * one with a frame still in flight gets one more interval, then ends. */
static void expire_work(void *arg)
{
	u64 now = nvxnu_nanotime();
	unsigned long flags = 0;
	int again = 0;

	(void)arg;
	spin_lock_irqsave(&dl.lock, flags);
	if (dl.target && dl.expire_gen == dl.gen) {
		const int kicked = READ_ONCE(dl.kick_last) >= dl.opened;

		if (kicked && dl.expire_at == dl.target) {
			dl.expire_at = dl.target + dl.interval;
			again = 1;
		} else {
			if (TGM_STATS && kicked) {
				dl.expired++;
			} else if (TGM_STATS) {
				dl.empty++;
			}
			window_close(now);
		}
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (again)
		after(dl.expire_work, dl.expire_at, now);
}

void tgm_deadline_kicked(int compositor, int app)
{
	const u64 now = nvxnu_nanotime();

	if (!READ_ONCE(dl.device))
		return;
	if (compositor) {
		if (READ_ONCE(dl.kick_last) <= READ_ONCE(dl.presented_at))
			WRITE_ONCE(dl.frame_target, READ_ONCE(dl.next_target));
		WRITE_ONCE(dl.kick_last, now);
	}
	if (app)
		WRITE_ONCE(dl.kick_app_last, now);
}

/* The kicking thread, before its doorbell. No slow channel had work at the frame end, so none is on the GPU now and
 * the kicking one is not yet: the resident look reads nothing. */
int tgm_deadline_app_kicked(void)
{
	const u64 dormant = __atomic_load_n(&dl.dormant, __ATOMIC_ACQUIRE);
	u64 now = 0;
	u64 target = 0;
	u64 next = 0;
	int preempt = -1;
	struct tgm_window w;
	unsigned long flags = 0;

	if (!dormant)
		return 0;
	now = nvxnu_nanotime();
	if (dormant <= now || !tgm_hold_wanted(1))
		return 0;
	spin_lock_irqsave(&dl.lock, flags);
	if (!dl.target && dl.dormant > now && !dl.suspended && drawing(now)) {
		/* the window the frame end kept, from then: a compositor kick or present since counts */
		window_open(dl.dormant, dl.dormant_since);
		target = dl.target;
		w.now = now;
		w.vblank = dl.target;
		w.interval = dl.interval;
		w.expected = dl.expected;
		next = tgm_hold_due(dl.device, &w, TGM_VICTIM_NONE, &preempt);
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (!target)
		return 0;
	if (next)
		after(dl.hold_work, next, now);
	after(dl.expire_work, target, now);
	return 1;
}

/* Interrupt pass. The anchor moves on every 2^40 ns (18 minutes), keeping the rate, so the span never overflows. */
void tgm_deadline_clock(uint64_t ns, uint64_t pt)
{
	const u32 recal = READ_ONCE(dl.recal);
	u64 rate = 0;

	if (recal != dl.recal_seen || !dl.cal_ns || pt < dl.cal_pt ||
	    ns - dl.cal_ns >= 1ULL << 40) {
		dl.recal_seen = recal;
		dl.cal_ns = ns;
		dl.cal_pt = pt;
		return;
	}
	rate = tgm_pt_rate(ns - dl.cal_ns, pt - dl.cal_pt);
	if (rate)
		WRITE_ONCE(dl.pt_rate, rate);
}

uint64_t tgm_deadline_pt_ns(uint64_t pt_span)
{
	return tgm_pt_ns(pt_span, READ_ONCE(dl.pt_rate));
}

void tgm_deadline_progress(uint64_t ns, int biased)
{
	const u32 epoch = READ_ONCE(dl.frame_epoch);

	if (dl.pass_epoch != epoch) {
		WRITE_ONCE(dl.pass_ns, 0);
		WRITE_ONCE(dl.pass_biased, 0);
		WRITE_ONCE(dl.pass_epoch, epoch);
	}
	WRITE_ONCE(dl.pass_ns, dl.pass_ns + ns);
	if (biased)
		WRITE_ONCE(dl.pass_biased, 1);
}

/* The present completed: the frame's GPU time is learned (unless an app held the GPU at one of its kicks), and the window
 * whose frame this is ends (a present queued before it opened is an earlier frame's). If CA has started the next frame
 * since, its window opens now. */
void tgm_deadline_presented(uint64_t queued, int pass)
{
	u64 now = nvxnu_nanotime();
	u64 target = 0;
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	/* a compositor kick since the last present: a frame; the pass's time is this frame's while its epoch is ours, whole
	 * only at the pass's end, and only its own after a present the pass completed */
	const int tail = dl.tail;

	dl.tail = !pass;
	if (READ_ONCE(dl.kick_last) > dl.presented_at) {
		const u64 frame_target = READ_ONCE(dl.frame_target);
		const u64 frame_ns = READ_ONCE(dl.pass_ns);
		const int ours = READ_ONCE(dl.pass_epoch) == dl.frame_epoch;

		if (TGM_STATS) {
			dl.frames++;
			if (frame_target && now > frame_target &&
			    READ_ONCE(dl.kick_app_last) > dl.presented_at) {
				dl.missed_app++;
			} else if (frame_target && now > frame_target) {
				dl.missed_clear++;
			}
		}
		if (pass && !tail && ours && !READ_ONCE(dl.pass_biased) && frame_ns) {
			dl.expected = tgm_expect(dl.expected, frame_ns);
#if TGM_STATS
			frame_cost_add(frame_ns);
#endif
		}
	}
	WRITE_ONCE(dl.frame_epoch, dl.frame_epoch + 1);
	WRITE_ONCE(dl.presented_at, now);
	tgm_hold_unneed(queued);
	if (dl.target && queued >= dl.opened) {
		u64 closed = dl.target;

		window_close(now);
		if (dl.next_target > closed && dl.next_target > now)
			target = window_due(dl.next_target, now);
	} else if (!dl.target && dl.dormant && queued >= dl.dormant_since) {
		u64 closed = dl.dormant;

		WRITE_ONCE(dl.dormant, 0);
		if (dl.next_target > closed && dl.next_target > now)
			target = window_due(dl.next_target, now);
	}
	spin_unlock_irqrestore(&dl.lock, flags);
	if (target) {
		nvxnu_work_schedule(dl.hold_work);
		after(dl.expire_work, target, now);
	}
}

void tgm_deadline_suspend(void)
{
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	dl.suspended = 1;
	WRITE_ONCE(dl.dormant, 0);
	if (dl.target)
		window_close(nvxnu_nanotime());
	spin_unlock_irqrestore(&dl.lock, flags);
	nvxnu_work_cancel_wait(dl.hold_work);
	nvxnu_work_cancel_wait(dl.check_work);
	nvxnu_work_cancel_wait(dl.expire_work);
}

void tgm_deadline_resume(void)
{
	unsigned long flags = 0;

	if (!READ_ONCE(dl.device))
		return;
	spin_lock_irqsave(&dl.lock, flags);
	dl.suspended = 0;
	WRITE_ONCE(dl.recal, dl.recal + 1);
	spin_unlock_irqrestore(&dl.lock, flags);
}

void tgm_deadline_stats(char *buf, unsigned int len)
{
	u32 frames = 0;
	u32 missed_app = 0;
	u32 missed_clear = 0;
	u32 windows = 0;
	u32 empty = 0;
	u32 expired = 0;
	unsigned long flags = 0;
	u64 expected = 0;
	u64 learned = 0;
	u64 mean = 0;
	u64 p95 = 0;
	u64 max = 0;

	if (!READ_ONCE(dl.device)) {
		scnprintf(buf, len, "deadline off");
		return;
	}
	spin_lock_irqsave(&dl.lock, flags);
	frames = dl.frames;
	missed_app = dl.missed_app;
	missed_clear = dl.missed_clear;
	windows = dl.windows;
	empty = dl.empty;
	expired = dl.expired;
	expected = dl.expected;
	dl.frames = dl.missed_app = dl.missed_clear = dl.windows = dl.empty = dl.expired = 0;
#if TGM_STATS
	learned = frame_cost.n;
	mean = learned ? frame_cost.sum / learned / 1000 : 0;
	p95 = tgm_cost_percentile(frame_cost.hist, learned, 950);
	max = frame_cost.max / 1000;
	memset(&frame_cost, 0, sizeof(frame_cost));
#endif
	spin_unlock_irqrestore(&dl.lock, flags);
	scnprintf(buf, len,
		  "%u compositor frames, %u missed their vblank with an app on the GPU at a "
		  "compositor kick, %u without; compositor GPU time a frame: mean %llu us, "
		  "p95 %llu us, max %llu us over %llu learned frames, peak tracker %llu us; "
		  "%u windows (%u with no frame, %u expired)",
		  frames, missed_app, missed_clear, mean, p95, max, learned, expected / 1000,
		  windows, empty, expired);
}
