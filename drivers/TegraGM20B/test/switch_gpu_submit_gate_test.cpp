// SPDX-License-Identifier: MIT
#include <atomic>
#include "bazel/testing/require.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuSubmitGate.h"
using namespace std::chrono_literals;

TEST(SubmitGate, MutationDrainsExistingSubmissionsBeforeNewEntries)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.enter(&lock));
	ASSERT_TRUE(gate.enter(&lock));
	std::promise<void> asleep;
	bool notified = false;
	lock.sleeping = [&] {
		if (!notified) {
			notified = true;
			asleep.set_value();
		}
	};
	IOLockUnlock(&lock);
	std::atomic<int> order{0};
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		ASSERT_TRUE(gate.mutate(&lock));
		ASSERT_TRUE(order.fetch_add(1) == 0);
		IOLockUnlock(&lock);
	});
	ASSERT_TRUE(asleep.get_future().wait_for(2s) == std::future_status::ready);
	auto next = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		ASSERT_TRUE(gate.enter(&lock));
		ASSERT_TRUE(order.fetch_add(1) == 1);
		gate.leave(&lock);
		IOLockUnlock(&lock);
	});
	IOLockLock(&lock);
	gate.leave(&lock);
	ASSERT_TRUE(order.load() == 0);
	gate.leave(&lock);
	IOLockUnlock(&lock);
	mutation.get();
	next.get();
}

TEST(SubmitGate, CloseDrainsAndRefusesLaterWork)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.enter(&lock));
	IOLockUnlock(&lock);
	std::promise<void> asleep;
	bool notified = false;
	lock.sleeping = [&] {
		if (!notified) {
			notified = true;
			asleep.set_value();
		}
	};
	auto closing = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		gate.close(&lock);
		IOLockUnlock(&lock);
	});
	ASSERT_TRUE(asleep.get_future().wait_for(2s) == std::future_status::ready);
	IOLockLock(&lock);
	ASSERT_TRUE(gate.closed());
	ASSERT_TRUE(!gate.enter(&lock));
	ASSERT_TRUE(!gate.mutate(&lock));
	gate.leave(&lock);
	IOLockUnlock(&lock);
	closing.get();
}

TEST(SubmitGate, ConcurrentChannelsCannotOverlapMutations)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	std::atomic<int> inside{0}, violations{0};
	std::vector<std::thread> threads;
	for (int j = 0; j < 4; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 1000; i++) {
				IOLockLock(&lock);
				ASSERT_TRUE(gate.enter(&lock));
				IOLockUnlock(&lock);
				inside++;
				std::this_thread::yield();
				inside--;
				IOLockLock(&lock);
				gate.leave(&lock);
				IOLockUnlock(&lock);
			}
		});
	for (int j = 0; j < 2; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 200; i++) {
				IOLockLock(&lock);
				ASSERT_TRUE(gate.mutate(&lock));
				if (inside)
					violations++;
				std::this_thread::yield();
				if (inside)
					violations++;
				IOLockUnlock(&lock);
			}
		});
	for (auto &thread : threads)
		thread.join();
	ASSERT_TRUE(violations == 0);
}

TEST(SubmitGate, MutationReservationSurvivesDroppingConnectionLock)
{
    IOLock lock;
    SwitchGpuSubmitGate gate;
    IOLockLock(&lock);
    ASSERT_TRUE(gate.beginMutation(&lock));
    IOLockUnlock(&lock);
    std::atomic<unsigned> admitted{0};
    auto submit = std::async(std::launch::async, [&] {
        IOLockLock(&lock);
        REQUIRE(gate.enter(&lock));
        ++admitted;
        gate.leave(&lock);
        IOLockUnlock(&lock);
    });
    auto mutation = std::async(std::launch::async, [&] {
        IOLockLock(&lock);
        REQUIRE(gate.beginMutation(&lock));
        ++admitted;
        gate.endMutation(&lock);
        IOLockUnlock(&lock);
    });
    EXPECT_EQ(submit.wait_for(10ms), std::future_status::timeout);
    EXPECT_EQ(mutation.wait_for(10ms), std::future_status::timeout);
    EXPECT_EQ(admitted.load(), 0u);
    IOLockLock(&lock);
    gate.endMutation(&lock);
    IOLockUnlock(&lock);
    submit.get(); mutation.get();
    EXPECT_EQ(admitted.load(), 2u);
}

TEST(SubmitGate, CloseWaitsForAnUnlockedMutationToFinish)
{
    IOLock lock;
    SwitchGpuSubmitGate gate;
    IOLockLock(&lock);
    ASSERT_TRUE(gate.beginMutation(&lock));
    IOLockUnlock(&lock);
    auto closing = std::async(std::launch::async, [&] {
        IOLockLock(&lock);
        gate.close(&lock);
        IOLockUnlock(&lock);
    });
    EXPECT_EQ(closing.wait_for(10ms), std::future_status::timeout);
    IOLockLock(&lock);
    gate.endMutation(&lock);
    IOLockUnlock(&lock);
    EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
    closing.get();
}
