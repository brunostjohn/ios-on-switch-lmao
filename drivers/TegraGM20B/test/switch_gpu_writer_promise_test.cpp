// SPDX-License-Identifier: MIT
// Writer promises (SwitchGpuWriterPromise.h) between a command buffer counted as scheduled before its work reached the
// kernel and a present of the surface it writes: the present's acquires are the pending promises, a job's fence releases
// the ones it keeps after its writes, and a cancel, a kill or a close release them from the CPU.
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "../SwitchGpuWriterPromise.h"
#include "switch_gpu_interrupts_off_stubs.h"

namespace
{
constexpr uint32_t kSlots = 32;

struct Connection {
	// `base`: its words' fence-pool offset
	explicit Connection(uint32_t base = 0) : base(base)
	{
		for (uint32_t i = 0; i < kSlots; ++i)
			words[i] = 100 + i;
	}

	// method 34 Make for the surface
	int make(struct Surface &surface);

	uint32_t pending(const struct Surface &surface) const;

	uint32_t base;
	switch_gpu_promise_slot slots[kSlots] = {};
	volatile uint32_t words[kSlots];
};

struct Surface {
	switch_gpu_promise_ref refs[SWITCH_GPU_BACKING_PROMISES] = {};
	uint32_t count = 0;
	uint32_t content = 0;  // what the GPU last finished writing
};

int Connection::make(Surface &surface)
{
	const int slot = switch_gpu_promise_make(slots, words, kSlots);
	if (slot < 0)
		return slot;
	const switch_gpu_promise_ref ref = {&words[slot], base + uint32_t(slot) * 4,
					    slots[slot].value};
	return switch_gpu_promise_ref_add(surface.refs, &surface.count, &ref) ? -28 : slot;
}

// What a job of this connection that uses the surface waits for (SubmitJob's own range)
uint32_t Connection::pending(const Surface &surface) const
{
	uint32_t offsets[SWITCH_GPU_BACKING_PROMISES], values[SWITCH_GPU_BACKING_PROMISES];
	return uint32_t(switch_gpu_promise_ref_pending(surface.refs, surface.count, base,
						       base + kSlots * 4, offsets, values,
						       SWITCH_GPU_BACKING_PROMISES));
}

// A present of the surface: the acquires it takes, and the frame it shows once they pass
struct Present {
	explicit Present(const Surface &surface)
	{
		uint32_t offsets[SWITCH_GPU_BACKING_PROMISES];
		values.resize(SWITCH_GPU_BACKING_PROMISES);
		const int n =
			switch_gpu_promise_ref_pending(surface.refs, surface.count, 0, 0, offsets,
						       values.data(), SWITCH_GPU_BACKING_PROMISES);
		values.resize(n > 0 ? uint32_t(n) : 0);
		for (int i = 0; i < n; ++i)
			slots.push_back(offsets[i] / 4);
	}

	bool passes(const Connection &c) const
	{
		for (size_t i = 0; i < slots.size(); ++i)
			if (int32_t(c.words[slots[i]] - values[i]) < 0)
				return false;
		return true;
	}

	std::vector<uint32_t> slots, values;
};

// The kept job runs: its writes, then its fence's releases of the promises it keeps
void runJob(Connection &c, Surface &surface, uint32_t frame, const std::vector<int> &kept)
{
	surface.content = frame;
	for (int slot : kept)
		c.words[slot] = c.slots[slot].value;
}
}  // namespace

// The swap before the job reached the kernel: the present waits for the promise and shows the frame the job finished.
TEST(WriterPromise, ASwapBeforeTheJobShowsTheFinishedFrame)
{
	Connection c;
	Surface drawable;
	const int promise = c.make(drawable);
	ASSERT_GE(promise, 0);
	const Present present(drawable);
	ASSERT_EQ(present.slots.size(), 1u);
	EXPECT_FALSE(present.passes(c));
	switch_gpu_promise_kept(&c.slots[promise], 0);
	switch_gpu_promise_ref_drop(drawable.refs, &drawable.count, uint32_t(promise) * 4);
	EXPECT_FALSE(present.passes(c)) << "kept: released only at its job's end";
	runJob(c, drawable, 7, {promise});
	ASSERT_TRUE(present.passes(c));
	EXPECT_EQ(drawable.content, 7u);
}

// Its channel killed before the job ran: the kernel releases it, so the present goes on.
TEST(WriterPromise, AKillReleasesAKeptPromise)
{
	Connection c;
	Surface drawable;
	const int promise = c.make(drawable);
	const Present present(drawable);
	switch_gpu_promise_kept(&c.slots[promise], 3);
	switch_gpu_promise_release(&c.slots[promise], &c.words[promise]);
	EXPECT_TRUE(present.passes(c));
	EXPECT_EQ(c.slots[promise].state, SWITCH_GPU_PROMISE_FREE);
}

// A command buffer that fails before its job: the cancel releases its promise and the surface keeps no ref to it.
TEST(WriterPromise, ACancelReleasesItAndDropsItsRef)
{
	Connection c;
	Surface drawable;
	const int promise = c.make(drawable);
	const Present early(drawable);
	switch_gpu_promise_ref_drop(drawable.refs, &drawable.count, uint32_t(promise) * 4);
	switch_gpu_promise_release(&c.slots[promise], &c.words[promise]);
	EXPECT_TRUE(early.passes(c));
	EXPECT_TRUE(Present(drawable).slots.empty());
}

// Two command buffers writing one drawable: the present waits for both, whichever job ends first.
TEST(WriterPromise, TwoCommandBuffersWritingOneDrawableAreBothWaitedFor)
{
	Connection c;
	Surface drawable;
	const int first = c.make(drawable), second = c.make(drawable);
	ASSERT_NE(first, second);
	const Present present(drawable);
	ASSERT_EQ(present.slots.size(), 2u);
	runJob(c, drawable, 2, {second});
	EXPECT_FALSE(present.passes(c));
	runJob(c, drawable, 1, {first});
	EXPECT_TRUE(present.passes(c));
}

// A connection's jobs never wait for its own promises, whichever command buffer made them: a held command buffer's
// render range before its signal (that range keeps nothing) and two held command buffers writing one drawable would each
// wait on the same channel for a job behind them. Another connection's job waits for both.
TEST(WriterPromise, AConnectionsJobsNeverWaitForItsOwnPromises)
{
	Connection app(4096), compositor(8192);
	Surface drawable;
	const int first = app.make(drawable), second = app.make(drawable);
	EXPECT_EQ(app.pending(drawable), 0u);
	EXPECT_EQ(compositor.pending(drawable), 2u);
	app.words[first] = app.slots[first].value;
	EXPECT_EQ(compositor.pending(drawable), 1u);
	app.words[second] = app.slots[second].value;
	EXPECT_EQ(compositor.pending(drawable), 0u);
}

// A kept promise's slot is taken again only once its job released it; a surface holds a bounded number pending.
TEST(WriterPromise, SlotsAndRefsAreBounded)
{
	Connection c;
	Surface drawable;
	for (int i = 0; i < SWITCH_GPU_BACKING_PROMISES; ++i)
		ASSERT_GE(c.make(drawable), 0);
	EXPECT_EQ(c.make(drawable), -28) << "the kernel gives that slot back (method 34's failure)";
	for (uint32_t i = SWITCH_GPU_BACKING_PROMISES + 1; i < kSlots; ++i) {
		Surface other;
		ASSERT_GE(c.make(other), 0) << i;
	}
	EXPECT_EQ(switch_gpu_promise_make(c.slots, c.words, kSlots), -28);
	switch_gpu_promise_kept(&c.slots[0], 0);
	EXPECT_EQ(switch_gpu_promise_make(c.slots, c.words, kSlots), -28);
	c.words[0] = c.slots[0].value;
	EXPECT_EQ(switch_gpu_promise_make(c.slots, c.words, kSlots), 0);
	EXPECT_EQ(c.slots[0].value, c.words[0] + 1);
}

typedef int32_t s32;
// gpu_promise_giveup.c's own promise_raise, cut out by :promise_raise_source
#include "drivers/TegraGM20B/test/promise_raise_source.h"

namespace
{
volatile uint32_t pool[64];

volatile uint32_t *poolWord(uint32_t offset)
{
	return offset && offset / 4 < 64 ? &pool[offset / 4] : nullptr;
}

uint32_t poolOwner(uint32_t)
{
	return 1;
}

const switch_gpu_promise_fence poolFence = {poolWord, promise_raise, poolOwner};

// The fence pool as T210 has it: mapped write-combined, where only a plain store completes. The model's page is
// read-only, so any other write (a compare-and-swap, any atomic read-modify-write) faults; the raise's own store is
// the one write it lets through.
struct FenceModel {
	volatile uint32_t *page = nullptr;
	size_t bytes = size_t(getpagesize());
	uint32_t owner = 1;   // tgm_fence_owner
	bool reused = false;  // handed to a connection that made no promise on it yet

	FenceModel()
	{
		void *made =
			mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		page = static_cast<volatile uint32_t *>(made);
		mprotect(made, bytes, PROT_READ);
	}

	~FenceModel()
	{
		munmap(const_cast<uint32_t *>(page), bytes);
	}

	void set(uint32_t offset, uint32_t value)
	{
		store(&page[offset / 4], value);
	}

	// its connection closed, the page goes to another, cleared (tgm_fence_page_get)
	void reuse()
	{
		for (size_t i = 0; i < bytes / 4; ++i)
			store(&page[i], 0);
		++owner;
		reused = true;
	}

	void store(volatile uint32_t *word, uint32_t value)
	{
		mprotect(const_cast<uint32_t *>(page), bytes, PROT_READ | PROT_WRITE);
		__atomic_store_n(word, value, __ATOMIC_RELEASE);
		mprotect(const_cast<uint32_t *>(page), bytes, PROT_READ);
	}
};

FenceModel *model;

volatile uint32_t *modelWord(uint32_t offset)
{
	return offset && offset / 4 < model->bytes / 4 ? &model->page[offset / 4] : nullptr;
}

int modelRaise(volatile uint32_t *word, uint32_t value)
{
	if (model->reused) {
		std::fputs("raise into a fence page another connection owns now\n", stderr);
		std::abort();
	}
	if (int32_t(__atomic_load_n(word, __ATOMIC_ACQUIRE) - value) >= 0)
		return 0;
	model->store(word, value);
	return 1;
}

uint32_t modelOwner(uint32_t)
{
	return model->owner;
}

const switch_gpu_promise_fence modelFence = {modelWord, modelRaise, modelOwner};

using Released = std::vector<std::pair<uint32_t, uint32_t>>;

void told(uint32_t offset, uint32_t value, int step, void *arg)
{
	if (step == SWITCH_GPU_PROMISE_RAISED)
		static_cast<Released *>(arg)->push_back({offset, value});
}

struct Steps {
	std::vector<int> steps;
	std::vector<uint32_t> words;        // the word as each step found it
	uint32_t makerAt = 0, makerTo = 0;  // the maker's GPU release, landing at RAISING when set
};

void stepped(uint32_t offset, uint32_t, int step, void *arg)
{
	auto *s = static_cast<Steps *>(arg);
	s->steps.push_back(step);
	s->words.push_back(uint32_t(pool[offset / 4]));
	if (step == SWITCH_GPU_PROMISE_RAISING && s->makerAt == offset)
		pool[offset / 4] = s->makerTo;
}
}  // namespace

// The promise is marked given up on (RAISING) before the word moves, so a reader the raise lets go already finds it; and
// a maker's GPU release that lands first after all (KEPT_FIRST) takes the mark back, its writes being there.
TEST(WriterPromise, TheGiveUpMarksBeforeTheWordMovesAndTellsWhoWon)
{
	Steps raised, kept;
	pool[5] = 1;
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 20, 2, 1, stepped, &raised), 1);
	EXPECT_EQ(raised.steps,
		  (std::vector<int>{SWITCH_GPU_PROMISE_RAISING, SWITCH_GPU_PROMISE_RAISED}));
	EXPECT_EQ(raised.words, (std::vector<uint32_t>{1, 2}));
	pool[5] = 1;
	kept.makerAt = 20;
	kept.makerTo = 2;
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 20, 2, 1, stepped, &kept), 0);
	EXPECT_EQ(kept.steps,
		  (std::vector<int>{SWITCH_GPU_PROMISE_RAISING, SWITCH_GPU_PROMISE_KEPT_FIRST}));
	EXPECT_EQ(pool[5], 2u);
}

// The give-up's raise of a pending word is its one write to the fence pool, a plain store: on the model's read-only
// page any compare-and-swap faults, as T210's bus answers one on the write-combined pool with an SError.
TEST(WriterPromise, TheRaiseIsAPlainStoreAndTheFencePoolTakesNoOtherWrite)
{
	FenceModel fence;
	model = &fence;
	fence.set(20, 1);
	fence.set(24, 7);
	Released released;
	EXPECT_EQ(switch_gpu_promise_give_up(&modelFence, 20, 2, fence.owner, told, &released), 1);
	EXPECT_EQ(switch_gpu_promise_give_up(&modelFence, 24, 7, fence.owner, told, &released), 0);
	EXPECT_EQ(released, (Released{{20, 2}}));
	EXPECT_EQ(fence.page[5], 2u);
	EXPECT_EQ(fence.page[6], 7u);
	EXPECT_DEATH(
		{
			uint32_t expected = 2;
			__atomic_compare_exchange_n(&fence.page[5], &expected, 3u, false,
						    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
		},
		"");
	model = nullptr;
}

// The raise looks and stores with interrupts off, so nothing between them may lock, log or allocate.
TEST(WriterPromise, TheKernelRaiseStoresWithInterruptsOffAndNothingThatBlocks)
{
	volatile uint32_t word = 5;
	const unsigned saves = interrupts_off::saves;
	interrupts_off::watched = &word;
	EXPECT_EQ(promise_raise(&word, 7), 1);
	EXPECT_EQ(interrupts_off::atSave, 5u);
	EXPECT_EQ(interrupts_off::atRestore, 7u);
	EXPECT_EQ(promise_raise(&word, 6), 0);
	EXPECT_EQ(promise_raise(&word, 7), 0);
	EXPECT_EQ(word, 7u);
	word = 0xfffffffeu;
	EXPECT_EQ(promise_raise(&word, 1), 1);
	EXPECT_EQ(word, 1u);
	EXPECT_EQ(interrupts_off::saves - saves, 4u);
	EXPECT_EQ(interrupts_off::depth, 0);
	interrupts_off::watched = nullptr;
}

TEST(WriterPromise, WhatBlocksThrowsOnlyWithInterruptsOff)
{
	const std::vector<std::function<void()>> blocking = {
		[] {
			IOLock lock;
			IOLockLock(&lock);
		},
		[] {
			IORecursiveLock lock;
			IORecursiveLockLock(&lock);
		},
		[] {
			std::free(IOMalloc(16));
		},
		[] {
			IOLog("%d\n", 1);
		},
		[] {
			IOCommandGate().runAction();
		},
		[] {
			struct mutex lock = {};
			mutex_lock(&lock);
		},
		[] {
			std::free(kmalloc(16, 0));
		},
		[] {
			std::free(kzalloc(16, 0));
		},
		[] {
			nvxnu_log("%d\n", 1);
		},
	};
	unsigned long flags = 0;
	for (const auto &call : blocking)
		EXPECT_NO_THROW(call());
	local_irq_save(flags);
	for (const auto &call : blocking)
		EXPECT_THROW(call(), std::runtime_error);
	local_irq_restore(flags);
	EXPECT_EQ(interrupts_off::depth, 0);
}

// The maker closed and its fence page went to another connection, cleared: the word is not the promise's any more. Its
// give-up is dropped, never raised (a raise would write the new owner's word), and nothing is told.
TEST(WriterPromise, AGiveUpOnAPageAnotherConnectionOwnsNowIsDroppedNeverRaised)
{
	FenceModel fence;
	model = &fence;
	fence.set(20, 1);
	const uint32_t owner = fence.owner;
	fence.reuse();
	EXPECT_EXIT(
		{
			Released released;
			const int raised = switch_gpu_promise_give_up(&modelFence, 20, 2, owner,
								      told, &released);
			std::exit(raised == 0 && released.empty() && fence.page[5] == 0 ? 0 : 1);
		},
		testing::ExitedWithCode(0), "");
	model = nullptr;
}

// An abandoned promise is found until the maker's own release takes it back; the set keeps the latest ones.
TEST(WriterPromise, TheAbandonedSetNamesEachPromiseByWordAndValue)
{
	switch_gpu_promise_abandoned set = {};
	switch_gpu_promise_abandoned_add(&set, 20, 2);
	EXPECT_TRUE(switch_gpu_promise_abandoned_has(&set, 20, 2));
	EXPECT_FALSE(switch_gpu_promise_abandoned_has(&set, 20, 3));
	EXPECT_FALSE(switch_gpu_promise_abandoned_has(&set, 24, 2));
	switch_gpu_promise_abandoned_drop(&set, 20, 2);
	EXPECT_FALSE(switch_gpu_promise_abandoned_has(&set, 20, 2));
	for (uint32_t i = 0; i <= SWITCH_GPU_PROMISE_ABANDONED; ++i)
		switch_gpu_promise_abandoned_add(&set, 4, i + 1);
	EXPECT_FALSE(switch_gpu_promise_abandoned_has(&set, 4, 1));
	EXPECT_TRUE(switch_gpu_promise_abandoned_has(&set, 4, SWITCH_GPU_PROMISE_ABANDONED + 1));
}

// A give-up raises a word still short of its value, once; a word the maker's own release already passed is left alone.
TEST(WriterPromise, AGiveUpRaisesOnlyAWordStillShortOfItsValue)
{
	Released released;
	pool[1] = 4;
	pool[2] = 7;
	pool[3] = 11;
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 4, 5, 1, told, &released), 1);
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 8, 7, 1, told, &released), 0);
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 12, 10, 1, told, &released), 0);
	EXPECT_EQ(switch_gpu_promise_give_up(&poolFence, 4, 5, 1, told, &released), 0);
	EXPECT_EQ(pool[1], 5u);
	EXPECT_EQ(pool[3], 11u);
	EXPECT_EQ(released, (Released{{4, 5}}));
}
