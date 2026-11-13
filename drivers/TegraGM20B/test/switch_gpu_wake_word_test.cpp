// SPDX-License-Identifier: MIT
// SwitchGpuWakeWord.h: an interrupt pass with nobody asleep on the global word wakes no thread, and a sleeper racing a
// pass never misses its wake (a lost one would hang the waiter here).
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "drivers/TegraGM20B/SwitchGpuWakeWord.h"

namespace
{
// xnu_glue.cpp's nvxnu_wake and nvxnu_idle over a mutex and condition variable standing in for assert_wait and
// thread_wakeup on the word's address
struct Word {
	switch_gpu_wake_word word = {};
	std::mutex lock;
	std::condition_variable asleep;
	unsigned wakeups = 0;

	void wake()
	{
		if (switch_gpu_wake_bump(&word)) {
			std::lock_guard<std::mutex> held(lock);
			++wakeups;
			asleep.notify_all();
		}
	}

	void idle(uint32_t seq)
	{
		switch_gpu_wake_sleep(&word, 1);
		{
			std::unique_lock<std::mutex> held(lock);
			asleep.wait(held, [&] {
				return __atomic_load_n(&word.seq, __ATOMIC_ACQUIRE) != seq;
			});
		}
		switch_gpu_wake_sleep(&word, 0);
	}
};
}  // namespace

// A compositor frame's passes (two jobs, each its own interrupt) with no kernel thread asleep on the word.
TEST(WakeWord, PassesWithNobodyAsleepWakeNoThread)
{
	Word w;
	for (int pass = 0; pass < 2; ++pass)
		w.wake();
	EXPECT_EQ(w.wakeups, 0u);
	EXPECT_EQ(w.word.seq, 2u);
	switch_gpu_wake_sleep(&w.word, 1);
	w.wake();
	EXPECT_EQ(w.wakeups, 1u) << "a sleeper is woken";
	switch_gpu_wake_sleep(&w.word, 0);
}

TEST(WakeWord, ASleeperRacingThePassIsWoken)
{
	constexpr unsigned kRounds = 20000;
	Word w;
	std::atomic<unsigned> ready{0}, done{0};
	std::thread waiter([&] {
		for (unsigned round = 1; round <= kRounds; ++round) {
			for (;;) {
				const uint32_t seq = __atomic_load_n(&w.word.seq, __ATOMIC_ACQUIRE);
				if (ready.load(std::memory_order_acquire) >= round)
					break;
				w.idle(seq);
			}
			done.store(round, std::memory_order_release);
		}
	});
	for (unsigned round = 1; round <= kRounds; ++round) {
		ready.store(round, std::memory_order_release);
		w.wake();
		while (done.load(std::memory_order_acquire) != round)
			;
	}
	waiter.join();
	EXPECT_EQ(done.load(), kRounds);
	EXPECT_LE(w.wakeups, kRounds);
}
