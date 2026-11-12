// SPDX-License-Identifier: MIT
// Writer promises (SwitchGpuWriterPromise.h) between a command buffer counted as scheduled before its work reached the
// kernel and a present of the surface it writes: the present's acquires are the pending promises, a job's fence releases
// the ones it keeps after its writes, and a cancel, a kill or a close release them from the CPU.
#include <gtest/gtest.h>
#include <utility>
#include <vector>
#include "../SwitchGpuWriterPromise.h"

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

namespace
{
volatile uint32_t pool[64];

volatile uint32_t *poolWord(uint32_t offset)
{
	return offset && offset / 4 < 64 ? &pool[offset / 4] : nullptr;
}

using Released = std::vector<std::pair<uint32_t, uint32_t>>;

void told(uint32_t offset, uint32_t value, void *arg)
{
	static_cast<Released *>(arg)->push_back({offset, value});
}
}  // namespace

// A reader stuck behind a promise whose command buffer never runs is let go by the hang rule, once; a promise released
// meanwhile is left alone, and so is one the maker's own release already passed.
TEST(WriterPromise, TheHangRuleReleasesOnlyThePromisesAStuckReaderStillWaitsFor)
{
	switch_gpu_promise_waits waits = {};
	Released released;
	pool[1] = 4;
	pool[2] = 7;
	pool[3] = 9;
	ASSERT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, 4, 5), 0);
	ASSERT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, 4, 5), 0);
	ASSERT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, 8, 7), 0);
	ASSERT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, 12, 10), 0);
	pool[3] = 11;
	EXPECT_EQ(switch_gpu_promise_waits_release(&waits, poolWord, told, &released), 1u);
	EXPECT_EQ(pool[1], 5u);
	EXPECT_EQ(pool[3], 11u);
	EXPECT_EQ(released, (Released{{4, 5}}));
	EXPECT_EQ(switch_gpu_promise_waits_release(&waits, poolWord, told, &released), 0u);
}

// Every frame of a compositor that reads the surface acquires the same promise: one record. A record of a released
// promise is reused; past SWITCH_GPU_PROMISE_WAITS pending ones the acquire is refused, never left out of the hang rule.
TEST(WriterPromise, AChannelRecordsEachPendingPromiseOnce)
{
	switch_gpu_promise_waits waits = {};
	for (uint32_t i = 1; i <= SWITCH_GPU_PROMISE_WAITS + 1; ++i)
		pool[i] = 0;
	for (uint32_t i = 1; i <= SWITCH_GPU_PROMISE_WAITS; ++i)
		ASSERT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, i * 4, 1), 0);
	EXPECT_EQ(switch_gpu_promise_waits_note(&waits, poolWord, 4, 1), 0);
	EXPECT_EQ(switch_gpu_promise_waits_note(&waits, poolWord,
						(SWITCH_GPU_PROMISE_WAITS + 1) * 4, 1),
		  -28);
	pool[1] = 1;
	EXPECT_EQ(switch_gpu_promise_waits_note(&waits, poolWord,
						(SWITCH_GPU_PROMISE_WAITS + 1) * 4, 1),
		  0);
}
