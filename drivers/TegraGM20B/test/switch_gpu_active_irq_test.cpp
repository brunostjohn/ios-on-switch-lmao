// SPDX-License-Identifier: MIT
#include "switch_gpu_active_irq_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuActiveIrq.h"
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class ActiveIrq : public testing::Test
{
protected:
	switch_irq_fixture *f = switch_irq_create();

	void TearDown() override
	{
		switch_irq_destroy(f);
	}
};

TEST_F(ActiveIrq, IdleChannelsDoNotEnterDispatcher)
{
	for (unsigned i = 0; i < 64; i++)
		switch_irq_add(f, i, 1);
	switch_irq_arm(f, 17, 4);
	switch_irq_arm(f, 29, 8);
	EXPECT_EQ(switch_irq_members(f), 2u);
	EXPECT_EQ(switch_irq_refcount(f), 2u);
	switch_irq_dispatch(f, 1);
	for (unsigned i = 0; i < 64; i++)
		EXPECT_EQ(switch_irq_calls(f, i), unsigned(i == 17 || i == 29));
	switch_irq_done(f, 17, 4);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_members(f), 1u);
	EXPECT_EQ(switch_irq_refcount(f), 1u);
	switch_irq_done(f, 29, 8);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_members(f), 0u);
	EXPECT_EQ(switch_irq_refcount(f), 0u);
}

TEST_F(ActiveIrq, LaterSubmissionKeepsMembershipUntilLatestFence)
{
	switch_irq_add(f, 0, 1);
	switch_irq_arm(f, 0, 1);
	switch_irq_arm(f, 0, 3);
	switch_irq_done(f, 0, 1);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_members(f), 1u);
	switch_irq_done(f, 0, 3);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_members(f), 0u);
	switch_irq_arm(f, 0, 4);
	EXPECT_EQ(switch_irq_members(f), 1u);
}

TEST_F(ActiveIrq, DispatchDropAndConcurrentRearmCannotLoseMembership)
{
	switch_irq_add(f, 0, 1);
	switch_irq_arm(f, 0, 1);
	switch_irq_done(f, 0, 1);

	struct Gate {
		std::mutex lock;
		std::condition_variable changed;
		bool inside = false, leave = false;
	} gate;

	switch_irq_hook_drop(
		f,
		[](void *p) {
			auto &g = *static_cast<Gate *>(p);
			std::unique_lock lock(g.lock);
			g.inside = true;
			g.changed.notify_all();
			g.changed.wait(lock, [&] {
				return g.leave;
			});
		},
		&gate);
	std::thread irq([&] {
		switch_irq_dispatch(f, 1);
	});
	{
		std::unique_lock lock(gate.lock);
		gate.changed.wait(lock, [&] {
			return gate.inside;
		});
	}
	std::atomic<bool> started = false, returned = false;
	std::thread submit([&] {
		started = true;
		switch_irq_arm(f, 0, 2);
		returned = true;
	});
	while (!started.load())
		std::this_thread::yield();
	EXPECT_FALSE(returned.load());
	{
		std::lock_guard lock(gate.lock);
		gate.leave = true;
		gate.changed.notify_all();
	}
	irq.join();
	submit.join();
	switch_irq_hook_drop(f, nullptr, nullptr);
	EXPECT_EQ(switch_irq_members(f), 1u);
	EXPECT_EQ(switch_irq_refcount(f), 1u);
	EXPECT_EQ(switch_irq_suspend(f, 0), 1);
	switch_irq_resume(f, 0);
	EXPECT_EQ(switch_irq_members(f), 1u);
	switch_irq_done(f, 0, 2);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_members(f), 0u);
}

TEST_F(ActiveIrq, SuspendRestoresOnlyActiveNotification)
{
	switch_irq_add(f, 0, 1);
	switch_irq_add(f, 1, 1);
	switch_irq_arm(f, 0, 7);
	EXPECT_EQ(switch_irq_suspend(f, 0), 1);
	EXPECT_EQ(switch_irq_suspend(f, 1), 0);
	EXPECT_EQ(switch_irq_members(f), 0u);
	switch_irq_resume(f, 0);
	switch_irq_resume(f, 1);
	EXPECT_EQ(switch_irq_members(f), 1u);
	switch_irq_done(f, 0, 7);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_suspend(f, 0), 0);
	switch_irq_resume(f, 0);
	EXPECT_EQ(switch_irq_members(f), 0u);
}

TEST_F(ActiveIrq, LegacyNotificationsKeepPermanentMembership)
{
	switch_irq_add(f, 0, 0);
	switch_irq_arm(f, 0, 1);
	switch_irq_done(f, 0, 1);
	switch_irq_dispatch(f, 0);
	switch_irq_dispatch(f, 0);
	EXPECT_EQ(switch_irq_calls(f, 0), 2u);
	EXPECT_EQ(switch_irq_members(f), 1u);
	switch_irq_remove(f, 0);
	EXPECT_EQ(switch_irq_members(f), 0u);
}

TEST_F(ActiveIrq, DeleteSerializesWithDispatchCallback)
{
	switch_irq_add(f, 0, 1);
	switch_irq_arm(f, 0, 1);
	switch_irq_done(f, 0, 1);
	switch_irq_remove(f, 0);
	switch_irq_dispatch(f, 1);
	EXPECT_EQ(switch_irq_calls(f, 0), 0u);
	EXPECT_EQ(switch_irq_refcount(f), 0u);
}

TEST(ActiveIrqState, WrappedSequencesKeepLatestTarget)
{
	uint64_t state = 0;
	EXPECT_EQ(switch_gpu_irq_arm(&state, UINT32_MAX), 1);
	EXPECT_EQ(switch_gpu_irq_arm(&state, 0), 0);
	EXPECT_EQ(switch_gpu_irq_complete(&state, UINT32_MAX), 0);
	EXPECT_EQ(switch_gpu_irq_complete(&state, 0), 1);
	EXPECT_EQ(switch_gpu_irq_arm(&state, 1), 1);
}
