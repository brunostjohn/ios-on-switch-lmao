// SPDX-License-Identifier: MIT
// The pool scrubber: freed pool pages zeroed by the copy engine.
#include "gpu_internal.h"

#define SCRUB_CHUNK (16UL << 20)
#define SCRUB_INFLIGHT (48UL << 20)
#define SCRUB_NS (1000 * NSEC_PER_MSEC)
#define SCRUB_LINE \
	(128U << 10) /* bytes per line of a fill: 32768 four-byte elements, Mesa's max_dim */
struct scrub_state scrub = {.lock = {NULL}};

/* One range: copy-engine fills, then the fence. Under scrub.lock, with a free job slot. 0 or a push error. */
static int scrub_submit(u64 pa, u64 bytes)
{
	struct tgm_chan *chan = scrub.chan;
	struct nvif_push *push = &chan->chan.push;
	u64 va = scrub.pool_va + (pa - nvxnu_pool_base_pa());
	u64 lines = bytes / SCRUB_LINE;
	u64 rest = bytes % SCRUB_LINE;
	u32 slot = 0;
	u32 seqno = 0;
	int ret = chan_wait_room(chan, 4, 32 + CHAN_FENCE_DWORDS);

	if (ret)
		return ret;
	push->cur += tgm_fill_setup(push->cur, 0);
	push->cur += tgm_fill_pitch(push->cur, SCRUB_LINE);
	if (lines) /* whole 128 KiB lines, one multi-line fill */
		push->cur += tgm_fill_lines(push->cur, va, SCRUB_LINE, (u32)lines, 1);
	if (rest) /* the last pages, one line (a multiple of 4 KiB) */
		push->cur += tgm_fill_lines(push->cur, va + lines * SCRUB_LINE, (u32)rest, 1, 0);
	slot = scrub.head % SCRUB_JOBS;
	scrub.job[slot].pa = pa;
	scrub.job[slot].bytes = bytes;
	scrub.job[slot].queued = nvxnu_nanotime();
	scrub.job[slot].seqno = seqno = chan->submitted + 1;
	__atomic_fetch_add(&scrub.inflight, bytes, __ATOMIC_RELAXED);
	/* the job is visible to scrub_fenced before its fence can complete */
	__atomic_store_n(&scrub.head, scrub.head + 1, __ATOMIC_RELEASE);
	if (chan_fence(chan) != seqno) {
		nvxnu_panic("tegra-gm20b: pool scrub fence %u is not the job's %u", chan->submitted,
			    seqno);
	}
	nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS);
	return 0;
}

/* Room for one more range: a free job slot (the boot's claims also keep to SCRUB_INFLIGHT). Under scrub.lock. */
static int scrub_room(void)
{
	return scrub.head - __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE) < SCRUB_JOBS;
}

/* The work item: claim and queue the free pages not known to be zero, as long as there is room (see above). */
static void scrub_work(void *arg)
{
	u64 pa = 0;
	unsigned long bytes = 0;
	(void)arg;
	if (!gate_try())
		return; /* the free dirty pages stay as they are: the wake kicks this again */
	mutex_lock(&scrub.lock);
	while (scrub.chan && !READ_ONCE(scrub.chan->error) && scrub_room() &&
	       __atomic_load_n(&scrub.inflight, __ATOMIC_RELAXED) + SCRUB_CHUNK <= SCRUB_INFLIGHT) {
		bytes = nvxnu_pool_claim_dirty(SCRUB_CHUNK, (unsigned long long *)&pa);
		if (!bytes) {
			if (!scrub.boot_done) {
				scrub.boot_done = 1;
				nvxnu_log("tegra-gm20b: pool scrub: %llu MiB queued to the copy "
					  "engine %llu ms after M3 (the CPU cleared it at M1 "
					  "before)\n",
					  scrub.boot_bytes >> 20,
					  (nvxnu_nanotime() - scrub.boot_start) / NSEC_PER_MSEC);
			}
			break;
		}
		if (scrub_submit(pa, bytes)) {
			/* back as it was, dirty (the CPU zeroes it if taken), without kicking this work again */
			dma_free_attrs(&gpu_pdev.dev, bytes, NULL, pa, 0);
			break;
		}
		if (!scrub.boot_done)
			scrub.boot_bytes += bytes;
	}
	mutex_unlock(&scrub.lock);
	gate_leave();
}

/* From the scrub channel's nonstall interrupt: every range whose fence passed goes back clean; then more claims. */
void scrub_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 tail = scrub.tail;
	u32 head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE);
	u64 now = stats_on() ? nvxnu_nanotime() : 0;
	u64 took = 0;
	int freed = 0;

	while (tail != head && (s32)(done - scrub.job[tail % SCRUB_JOBS].seqno) >= 0) {
		u32 slot = tail % SCRUB_JOBS;
		nvxnu_pool_free_zeroed(scrub.job[slot].pa, scrub.job[slot].bytes);
		__atomic_fetch_sub(&scrub.inflight, scrub.job[slot].bytes, __ATOMIC_RELAXED);
        if (stats_on()) {
            took = now - scrub.job[slot].queued;
            __atomic_fetch_add(&scrub.n, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&scrub.bytes, scrub.job[slot].bytes, __ATOMIC_RELAXED);
            stats_add(&scrub.ns, &scrub.max_ns, took);
        }
		__atomic_store_n(&scrub.tail, ++tail, __ATOMIC_RELEASE);
		freed = 1;
	}
	if (tail == head)
		nvxnu_work_cancel(scrub.deadline);
	if (freed && nvxnu_pool_dirty_free_pages())
		nvxnu_work_schedule(scrub.work);
}

/* A range not back SCRUB_NS after it was queued: the client channel the GPU is stuck on is killed and the range gets
 * SCRUB_NS more; with none, one line and a panic. */
static void scrub_deadline(void *arg)
{
	/* the last time a recovery or progress restarted the wait (one work item: no race) */
	static u64 recovered;
	u32 tail = __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE);
	u32 head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE);
	u32 slot = tail % SCRUB_JOBS;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u64 age = 0;
	char state[TGM_HANG_LINE];

	(void)arg;
	if (tail == head)
		return;
	age = nvxnu_nanotime() - max(scrub.job[slot].queued, recovered);
	if (age < SCRUB_NS) {
		nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS - age);
		return;
	}
	age = nvxnu_nanotime() - scrub.job[slot].queued;
	if ((s32)(tgm_chan_completed(scrub.chan) - scrub.job[slot].seqno) >= 0) {
		nvxnu_panic(
			"tegra-gm20b: pool scrub fence %u complete after %llu ms but its nonstall "
			"interrupt never came",
			scrub.job[slot].seqno, age / 1000000);
	}
	mutex_lock(&live_lock);
	verdict = hang_recover(scrub.chan, "pool scrub", scrub.job[slot].seqno, age);
	mutex_unlock(&live_lock);
	if (verdict != TGM_HANG_STUCK) {
		recovered = nvxnu_nanotime();
		nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS);
		return;
	}
	chan_describe(scrub.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: pool scrub of %llu KiB at %#llx not done %llu ms after it was "
		    "queued: scrub %s",
		    scrub.job[slot].bytes >> 10, scrub.job[slot].pa, age / 1000000, state);
}

/* After M3: the scrubber's address space (the whole pool mapped once), fence page and channel; then the boot scrub. One
 * line and no scrubber if any of it fails: pool pages are then zeroed by the CPU as they are taken. */
void scrub_open(void)
{
	u64 npages = nvxnu_pool_size() >> PAGE_SHIFT;
	u64 i = 0;
	struct tgm_chan *chan = NULL;
	void *fence_va = NULL;
	int ret = 0;

	mutex_init(&scrub.lock);
	scrub.boot_start = nvxnu_nanotime();
	scrub.work = nvxnu_work_alloc(scrub_work, NULL);
	scrub.deadline = nvxnu_work_alloc(scrub_deadline, NULL);
	scrub.pool_pas =
		kcalloc(npages, sizeof(u64), GFP_KERNEL); /* nvkm keeps the array (mem.c) */
	fence_va = tgm_pool_alloc(PAGE_SIZE, 0, &scrub.fence_pa);
	ret = scrub.work && scrub.deadline && scrub.pool_pas && fence_va ? tgm_vm_new(&scrub.vm)
									 : -ENOMEM;
	for (i = 0; !ret && i < npages; i++)
		scrub.pool_pas[i] = nvxnu_pool_base_pa() + (i << PAGE_SHIFT);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, scrub.pool_pas, (u32)npages, &scrub.pool_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.pool_mem, npages << PAGE_SHIFT, &scrub.pool_va);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, &scrub.fence_pa, 1, &scrub.fence_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.fence_mem, PAGE_SIZE, &scrub.fence_gpu);
	scrub.fence = fence_va;
	if (!ret)
		ret = tgm_chan_new_copy(scrub.vm, NULL, scrub.fence, scrub.fence_gpu, "scrub", 1, &chan);
	if (ret) {
		/* no teardown: what was made stays unused (a few pages); allocations zero on the CPU */
		nvxnu_log("tegra-gm20b: pool scrub unavailable (%d): pool pages are zeroed by the "
			  "CPU "
			  "as they are taken\n",
			  ret);
		return;
	}
	chan->kernel =
		CHAN_SCRUB; /* before anyone can submit on it: its nonstall goes to scrub_fenced */
	__atomic_store_n(&scrub.chan, chan, __ATOMIC_RELEASE);
	nvxnu_work_schedule(scrub.work);
}

int tgm_pool_scrub(uint64_t pa, uint64_t bytes)
{
	int ret = 0;
	if (!__atomic_load_n(&scrub.chan, __ATOMIC_ACQUIRE))
		return -ENODEV;
	if ((pa | bytes) & (PAGE_SIZE - 1) || !bytes || pa < nvxnu_pool_base_pa() ||
	    pa + bytes > nvxnu_pool_base_pa() + nvxnu_pool_size())
		return -EINVAL;
	if (!gate_try()) {
		if (TGM_STATS && READ_ONCE(gpu.railed))
			__atomic_fetch_add(&rail_ups.cpu_zeroed, bytes, __ATOMIC_RELAXED);
		return -EBUSY; /* the caller zeroes it on the CPU */
	}
	mutex_lock(&scrub.lock);
	if (!READ_ONCE(scrub.chan->error) && scrub_room()) {
		ret = scrub_submit(pa, bytes);
	} else {
		ret = -EBUSY;
	}
	mutex_unlock(&scrub.lock);
	gate_leave();
	return ret;
}

#if TGM_STATS
/* stats: this window's scrubbed ranges for the stats2 line */
void scrub_stats(char *buf, unsigned int len, u64 window_ms)
{
	u64 n = __atomic_exchange_n(&scrub.n, 0, __ATOMIC_RELAXED);
	u64 bytes = __atomic_exchange_n(&scrub.bytes, 0, __ATOMIC_RELAXED);
	u64 ns = __atomic_exchange_n(&scrub.ns, 0, __ATOMIC_RELAXED);
	u64 max = __atomic_exchange_n(&scrub.max_ns, 0, __ATOMIC_RELAXED);
	u64 cpu_n = __atomic_exchange_n(&cpu_zeroes, 0, __ATOMIC_RELAXED);
	u64 cpu_bytes = __atomic_exchange_n(&cpu_zero_bytes, 0, __ATOMIC_RELAXED);
	u64 moves = 0, move_bytes = 0;
	(void)window_ms;
	tgm_relayout_cpu_stats(&moves, &move_bytes);
	scnprintf(buf, len,
		  "scrub %llu (%llu MiB, queued to done mean %llu us max %llu us), %lu KiB dirty, "
		  "CPU fallbacks: zero %llu (%llu KiB), relayout %llu (%llu KiB)",
		  n, bytes >> 20, n ? ns / n / 1000 : 0, max / 1000,
		  nvxnu_pool_dirty_free_pages() << (PAGE_SHIFT - 10u), cpu_n, cpu_bytes >> 10,
		  moves, move_bytes >> 10);
}
#endif
