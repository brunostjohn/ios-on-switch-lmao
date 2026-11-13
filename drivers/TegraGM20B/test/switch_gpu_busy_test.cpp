// SPDX-License-Identifier: MIT
// SwitchGpuBusy.h between a submitter and the interrupt pass on two threads: a kick racing the pass's clear keeps its
// bit, so with the channel's newest seqno past its fence the bit is always found set once both are done.
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "drivers/TegraGM20B/SwitchGpuBusy.h"

TEST(GpuBusy, KicksRacingThePassNeverLoseTheirBit)
{
	constexpr uint32_t kChid = 77, kRounds = 100000;
	switch_gpu_busy busy = {};
	uint32_t submitted = 0;
	std::atomic<uint32_t> go{0}, settled{0};
	unsigned lost = 0;
	// Each round the pass settles the job before while the next one is kicked.
	std::thread pass([&] {
		for (uint32_t round = 1; round <= kRounds; ++round) {
			while (go.load(std::memory_order_acquire) != round)
				;
			switch_gpu_busy_settled(&busy, kChid, round - 1, &submitted);
			settled.store(round, std::memory_order_release);
		}
	});
	for (uint32_t round = 1; round <= kRounds; ++round) {
		go.store(round, std::memory_order_release);
		__atomic_store_n(&submitted, round, __ATOMIC_RELAXED);
		switch_gpu_busy_kicked(&busy, kChid);
		while (settled.load(std::memory_order_acquire) != round)
			;
		lost += !switch_gpu_busy_chan(&busy, kChid);
	}
	pass.join();
	EXPECT_EQ(lost, 0u) << "rounds that left the newest job in flight with its bit clear";
	switch_gpu_busy_settled(&busy, kChid, kRounds, &submitted);
	EXPECT_FALSE(switch_gpu_busy_chan(&busy, kChid));
}

TEST(GpuBusy, HighChannelsAreNoAppWork)
{
	switch_gpu_busy busy = {};
	switch_gpu_busy_level(&busy, 3, 1);
	switch_gpu_busy_kicked(&busy, 3);
	EXPECT_FALSE(switch_gpu_busy_low(&busy));
	uint64_t slow[SWITCH_GPU_BUSY_WORDS] = {};
	slow[200 / 64] = UINT64_C(1) << (200 % 64);
	EXPECT_FALSE(switch_gpu_busy_among(&busy, slow));
	EXPECT_EQ(switch_gpu_busy_kicked(&busy, 200), 1);
	EXPECT_EQ(switch_gpu_busy_kicked(&busy, 200), 0) << "already in flight";
	EXPECT_TRUE(switch_gpu_busy_low(&busy));
	EXPECT_TRUE(switch_gpu_busy_among(&busy, slow));
	switch_gpu_busy_gone(&busy, 200);
	EXPECT_FALSE(switch_gpu_busy_low(&busy));
	EXPECT_EQ(switch_gpu_busy_kicked(&busy, 512), 0) << "no such channel";
}
