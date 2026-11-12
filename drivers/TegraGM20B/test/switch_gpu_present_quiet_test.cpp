// SPDX-License-Identifier: MIT
// A watched present over producers whose jobs may end quiet (switch_gpu_present_model.h): which interrupt completes it.
#include <gtest/gtest.h>
#include "switch_gpu_present_model.h"

using present_model::Channel;
using present_model::Display;
using present_model::kA;
using present_model::kB;
using present_model::Producer;

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
