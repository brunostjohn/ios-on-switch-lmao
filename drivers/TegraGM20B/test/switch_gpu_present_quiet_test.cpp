// SPDX-License-Identifier: MIT
// A watched present (SwitchGpuPresentWatch.h) over producers whose jobs may end quiet (switch_gpu_quiet_model.h), as
// gpu_present.c drives it: the offer arms the watch, asks each producer's channel for the interrupt at its seqno
// (tgm_chan_need_tokens, by switch_gpu_quiet_token_on), then looks at the producer once more. A producer's pass makes
// the watch due and the pass's end delivers it; the work item takes what the last look found. A present the watch does
// not take is the fence-only present job: its acquires wait for the producers on the GPU, and its fence, on a kernel
// channel, always interrupts.
#include <gtest/gtest.h>
#include <functional>
#include <utility>
#include <vector>
#include "../SwitchGpuPresentWatch.h"
#include "switch_gpu_quiet_model.h"

namespace
{
using quiet_model::Channel;

constexpr uint64_t kA = (uint64_t(2) << 32) | 17u;  // reset generation 3, uid 17
constexpr uint64_t kB = 18u;                        // reset generation 1, uid 18

struct Producer {
	uint64_t id;  // tgm_chan.id: (reset generation - 1) << 32 | uid
	Channel channel;

	switch_gpu_backing_token token(uint32_t seqno) const
	{
		return {seqno, (id >> 32) + 1, SWITCH_GPU_ENGINE_GM20B, uint32_t(id)};
	}
};

class Display
{
public:
	using Step = std::function<void()>;

	explicit Display(std::vector<Producer *> producers) : producers_(std::move(producers))
	{
		current_ = this;
		for (auto *producer : producers_)
			producer->channel.passed = [this, producer] { pass(*producer); };
	}

	// tgm_present_watch_dependencies; 1: the caller queues the present job
	int offer(const switch_gpu_backing_token *tokens, uint32_t count, const Step &afterArm = {},
		  const Step &afterAsk = {})
	{
		if (!switch_gpu_present_watchable(tokens, count)) {
			job.assign(tokens, tokens + count);
			return 1;
		}
		switch_gpu_present_watch_arm(&watch, &tokens[0]);
		if (afterArm)
			afterArm();
		if (asks)
			needTokens(tokens, count);
		if (afterAsk)
			afterAsk();
		if (physical(&watch.token))
			work();
		return 0;
	}

	// tgm_chan_need_tokens over the live channels
	void needTokens(const switch_gpu_backing_token *tokens, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i)
			for (auto *producer : producers_)
				if (switch_gpu_quiet_token_on(&tokens[i], producer->id))
					producer->channel.need(uint32_t(tokens[i].value));
	}

	// the owners' thread calls
	void ownerWakes()
	{
		for (auto *producer : producers_)
			producer->channel.ownerWake();
	}

	// The present job's acquires pass once every producer is there; then its own fence interrupts.
	bool runJob()
	{
		for (const auto &token : job)
			if (physical(&token) != 1)
				return false;
		job.clear();
		++jobInterrupts;
		++presented;
		return true;
	}

	switch_gpu_present_watch watch{};
	std::vector<switch_gpu_backing_token> job;
	uint32_t syncpoints[256] = {};
	bool asks = true;
	unsigned presented = 0, workItems = 0, jobInterrupts = 0;

private:
	static Display *current_;
	std::vector<Producer *> producers_;

	static int physical(const switch_gpu_backing_token *token)
	{
		if (token->engine != SWITCH_GPU_ENGINE_GM20B)
			return int32_t(current_->syncpoints[token->queue] - uint32_t(token->value)) >= 0;
		for (auto *producer : current_->producers_)
			if (switch_gpu_quiet_token_on(token, producer->id))
				return int32_t(producer->channel.fence - uint32_t(token->value)) >= 0;
		return -19;
	}

	// present_watch_fenced in the producer's pass, then tgm_present_pass at its end
	void pass(const Producer &producer)
	{
		switch_gpu_present_watch_fenced(&watch, producer.id, producer.channel.fence);
		if (watch.state != SWITCH_GPU_PRESENT_WATCH_DUE)
			return;
		if (switch_gpu_present_watch_take_visible(&watch, physical, physical))
			deliver();
		else
			work();
	}

	// present_watch_work
	void work()
	{
		++workItems;
		if (switch_gpu_present_watch_claim(&watch, physical, physical) !=
		    SWITCH_GPU_PRESENT_WATCH_PENDING)
			deliver();
	}

	void deliver()
	{
		switch_gpu_present_watch_reset(&watch);
		++presented;
	}
};

Display *Display::current_;
}  // namespace

TEST(QuietTokenOn, OnlyAGM20BSeqnoOfThatChannelAndResetGeneration)
{
	const switch_gpu_backing_token token = {40, 3, SWITCH_GPU_ENGINE_GM20B, 17};
	EXPECT_TRUE(switch_gpu_quiet_token_on(&token, kA));
	EXPECT_FALSE(switch_gpu_quiet_token_on(&token, kA + 1));                         // another uid
	EXPECT_FALSE(switch_gpu_quiet_token_on(&token, (uint64_t(3) << 32) | 17u));     // reset since
	switch_gpu_backing_token wide = token;
	wide.value = uint64_t(1) << 32;
	EXPECT_FALSE(switch_gpu_quiet_token_on(&wide, kA));
	for (const uint32_t engine : {SWITCH_GPU_ENGINE_CE, SWITCH_GPU_ENGINE_VIC, SWITCH_GPU_ENGINE_NVDEC,
				      SWITCH_GPU_ENGINE_DISPLAY, SWITCH_GPU_ENGINE_NVENC}) {
		switch_gpu_backing_token other = token;
		other.engine = engine;
		EXPECT_FALSE(switch_gpu_quiet_token_on(&other, kA)) << engine;
	}
}

// The compositor's frame ends quiet (the submit thread had more raw work behind it). The watch's ask gets a wake entry
// behind it, whose interrupt runs the pass that delivers the frame; unasked, nothing interrupts and the watch is left to
// the present deadline.
TEST(PresentQuiet, AQuietProducerIsPresentedByTheInterruptTheWatchAskedFor)
{
	for (const bool asks : {false, true}) {
		Producer a{kA, Channel(40)};
		Display d({&a});
		d.asks = asks;
		const auto token = a.token(a.channel.submit(3, true));
		ASSERT_EQ(d.offer(&token, 1), 0);
		EXPECT_EQ(a.channel.asked, asks);
		d.ownerWakes();
		a.channel.run();
		EXPECT_EQ(a.channel.fence, 43u);
		EXPECT_EQ(a.channel.wakeEntries, asks ? 1u : 0u);
		EXPECT_EQ(a.channel.interrupts, asks ? 1u : 0u);
		EXPECT_EQ(d.presented, asks ? 1u : 0u);
		EXPECT_EQ(d.workItems, 0u) << "the pass's end delivers it";
		EXPECT_EQ(switch_gpu_present_watch_armed(&d.watch) != 0, !asks);
	}
}

// A producer whose job interrupts anyway is not owed a wake entry.
TEST(PresentQuiet, AnInterruptingProducerGetsNoWakeEntry)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, false));
	ASSERT_EQ(d.offer(&token, 1), 0);
	EXPECT_FALSE(a.channel.asked);
	d.ownerWakes();
	a.channel.run();
	EXPECT_EQ(a.channel.wakeEntries, 0u);
	EXPECT_EQ(a.channel.interrupts, 1u);
	EXPECT_EQ(d.presented, 1u);
}

// The job ends quiet between the arm and the ask: its seqno has passed, so the ask changes nothing, and the offer's
// last look hands the watch to the work item.
TEST(PresentQuiet, AProducerThatEndsBeforeTheAskIsTakenByTheLastLook)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(1, true));
	const uint32_t wanted = a.channel.quiet.wanted;
	ASSERT_EQ(d.offer(&token, 1, [&] { a.channel.run(); }), 0);
	EXPECT_FALSE(a.channel.asked);
	EXPECT_EQ(a.channel.quiet.wanted, wanted);
	d.ownerWakes();
	EXPECT_EQ(a.channel.wakeEntries, 0u);
	EXPECT_EQ(a.channel.interrupts, 0u);
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.workItems, 1u);
}

// The job ends after the ask and before the last look: the look delivers it, and the wake entry's pass after that
// finds the watch taken.
TEST(PresentQuiet, AProducerThatEndsBetweenTheAskAndTheLookIsPresentedOnce)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const auto token = a.token(a.channel.submit(2, true));
	ASSERT_EQ(d.offer(&token, 1, {},
			  [&] {
				  d.ownerWakes();
				  a.channel.run(1);
			  }),
		  0);
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.workItems, 1u);
	a.channel.run();
	EXPECT_EQ(a.channel.interrupts, 1u);
	EXPECT_EQ(d.presented, 1u);
}

// Two producers on two channels, the second ending quiet: the watch takes one producer only, so the fence-only present
// job waits for both on the GPU, and its own fence interrupts. Neither producer is asked or interrupts for it.
TEST(PresentQuiet, TwoProducersOnTwoChannelsTheSecondQuietGoToThePresentJob)
{
	Producer a{kA, Channel(40)}, b{kB, Channel(7)};
	Display d({&a, &b});
	const switch_gpu_backing_token tokens[2] = {a.token(a.channel.submit(1, false)),
						    b.token(b.channel.submit(2, true))};
	EXPECT_EQ(d.offer(tokens, 2), 1);
	EXPECT_FALSE(switch_gpu_present_watch_armed(&d.watch));
	EXPECT_FALSE(a.channel.asked);
	EXPECT_FALSE(b.channel.asked);
	a.channel.run();
	EXPECT_FALSE(d.runJob()) << "its acquire on the second producer still waits";
	b.channel.run();
	EXPECT_EQ(b.channel.interrupts, 0u);
	EXPECT_TRUE(d.runJob());
	EXPECT_EQ(d.presented, 1u);
	EXPECT_EQ(d.jobInterrupts, 1u);
}

// Each token asked is asked of its own channel: two quiet producers each get the interrupt at their seqno.
TEST(PresentQuiet, EveryTokenIsAskedOfItsOwnChannel)
{
	Producer a{kA, Channel(40)}, b{kB, Channel(7)};
	Display d({&a, &b});
	const switch_gpu_backing_token tokens[2] = {a.token(a.channel.submit(1, true)),
						    b.token(b.channel.submit(2, true))};
	d.needTokens(tokens, 2);
	EXPECT_TRUE(a.channel.asked);
	EXPECT_TRUE(b.channel.asked);
	d.ownerWakes();
	a.channel.run();
	b.channel.run();
	EXPECT_EQ(a.channel.interrupts, 1u);
	EXPECT_EQ(b.channel.interrupts, 1u);
}

// A VIC job's token holds syncpoint 18 in its queue, the uid of channel B: it is never asked of B, the watch does not
// take it, and the present job waits for the syncpoint on the GPU.
TEST(PresentQuiet, AnExternalTokenIsNeverAskedOfAChannel)
{
	Producer b{kB, Channel(7)};
	Display d({&b});
	b.channel.submit(1, true);
	const switch_gpu_backing_token vic = {9, 1, SWITCH_GPU_ENGINE_VIC, 18};
	const uint32_t wanted = b.channel.quiet.wanted;
	d.needTokens(&vic, 1);
	EXPECT_FALSE(b.channel.asked);
	EXPECT_EQ(b.channel.quiet.wanted, wanted);
	EXPECT_EQ(d.offer(&vic, 1), 1);
	EXPECT_FALSE(b.channel.asked);
	EXPECT_FALSE(d.runJob());
	d.syncpoints[18] = 9;
	EXPECT_TRUE(d.runJob());
	EXPECT_EQ(d.presented, 1u);
	b.channel.run();
	EXPECT_EQ(b.channel.interrupts, 0u);
}

// A GM20B producer ending quiet with a VIC producer: the present job waits for both, and nothing asks the channel.
TEST(PresentQuiet, AGM20BAndAnExternalProducerGoToThePresentJob)
{
	Producer a{kA, Channel(40)};
	Display d({&a});
	const switch_gpu_backing_token tokens[2] = {a.token(a.channel.submit(1, true)),
						    {9, 1, SWITCH_GPU_ENGINE_NVDEC, 17}};
	EXPECT_EQ(d.offer(tokens, 2), 1);
	EXPECT_FALSE(a.channel.asked);
	d.syncpoints[17] = 9;
	EXPECT_FALSE(d.runJob());
	a.channel.run();
	EXPECT_EQ(a.channel.interrupts, 0u);
	EXPECT_TRUE(d.runJob());
	EXPECT_EQ(d.jobInterrupts, 1u);
}
