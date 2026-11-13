// SPDX-License-Identifier: MIT
// A compositor's SubmitJob over N shared uses as the ownership layer runs it (begin, write ranges, dependencies, promise
// dependencies, commit), on the real backing registry and GM20B timeline engine among a device's worth of channels: the
// IOLocks it takes besides its uses' own locks, and its time, per job.
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <vector>
#include <IOKit/IOUserClient.h>
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"
#include "drivers/TegraGM20B/SwitchGpuTimeline.h"
#include "drivers/TegraGM20B/TegraGM20BABI.h"

struct switch_gpu_event_waits {
	int result;
};

extern "C" {
struct tgm_coh;

tgm_coh *tgm_coh_find_pa(uint64_t)
{
	return nullptr;
}

void tgm_views_sweep(tgm_coh *)
{
}

void tgm_views_deferred_lock(uint32_t, uint32_t)
{
}

void tgm_coherency_put(tgm_coh *)
{
}

int tegra_gm20b_surface_tiled(uint64_t)
{
	return 0;
}

int switch_gpu_surface_prepare(uint32_t, void *, uint32_t, switch_gpu_backing **backing,
			       switch_gpu_cpu_lease **owned, void **descriptor)
{
	*backing = nullptr;
	*owned = nullptr;
	*descriptor = nullptr;
	return -19;
}

void switch_gpu_surface_finish(void *)
{
}

uint32_t switch_gpu_surface_users(void *)
{
	return 0;
}

void switch_gpu_surface_descriptor_hold(void *, int)
{
}

int switch_gpu_backing_prepare_retirement(void)
{
	return 0;
}

void switch_gpu_backing_schedule_retirement(void)
{
}

uint64_t switch_gpu_backing_time_ns(void)
{
	return 100;
}

void switch_gpu_backing_arm_expiry(uint64_t)
{
}

void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t, uint64_t, int)
{
}

void switch_gpu_event_waits_retain(switch_gpu_event_waits *)
{
}

void switch_gpu_event_waits_release(switch_gpu_event_waits **batch)
{
	*batch = nullptr;
}

int switch_gpu_event_waits_cached_status(const switch_gpu_event_waits *batch)
{
	return batch->result;
}

int switch_gpu_event_waits_wait(switch_gpu_event_waits *batch, uint64_t)
{
	return batch->result;
}

int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *)
{
	return 1;
}

void switch_gpu_fence_hold(const volatile uint32_t *)
{
}

void switch_gpu_fence_put(const volatile uint32_t *)
{
}

void tgm_writer_wait(uint32_t, uint32_t)
{
}
}

namespace
{
// A device's channels: the compositor's, an app's for each of the textures' producers, and the rest of the boot's
// (keyboards, widgets, daemons), each a timeline in the engine's list.
constexpr uint32_t kApps = 12, kOtherChannels = 48;

struct Channel {
	volatile uint32_t fence = 0;
	switch_gpu_timeline *timeline = nullptr;

	switch_gpu_backing_token token(uint32_t value)
	{
		switch_gpu_backing_token made = {};
		switch_gpu_timeline_token(timeline, value, &made);
		return made;
	}
};

struct Cost {
	uint64_t global, own;
	uint32_t dependencies;
};

uint64_t next_page = 0x900000000ull;

switch_gpu_backing *surface(uint32_t pages)
{
	std::vector<uint64_t> list;
	for (uint32_t i = 0; i < pages; ++i)
		list.push_back(next_page += 4096);
	next_page += 1u << 20;
	return switch_gpu_backing_register(list.data(), pages, 0, nullptr, nullptr);
}

class SubmitCost : public testing::Test
{
protected:
	std::vector<Channel> channels = std::vector<Channel>(1 + kApps + kOtherChannels);
	Channel &compositor = channels[0];
	std::vector<switch_gpu_backing *> textures;
	switch_gpu_backing *target = nullptr;
	uint32_t frame = 0;

	void SetUp() override
	{
		ASSERT_EQ(switch_gpu_timeline_init(), 0);
		static uint64_t uid;
		for (auto &channel : channels)
			channel.timeline = switch_gpu_timeline_create(++uid, &channel.fence);
	}

	void TearDown() override
	{
		for (auto &channel : channels)
			switch_gpu_timeline_close(channel.timeline);
		for (auto *texture : textures)
			switch_gpu_backing_release(texture);
		switch_gpu_backing_release(target);
		for (int pass = 0; pass < 4; ++pass)
			switch_gpu_backing_retire();
	}

	Channel &app(uint32_t i)
	{
		return channels[1 + i % kApps];
	}

	// Each app draws its textures once, the GPU finishes, and the compositor composites a few frames behind the GPU.
	void scene(uint32_t uses)
	{
		target = surface(4);
		for (uint32_t i = 0; i < uses; ++i) {
			textures.push_back(surface(16));
			switch_gpu_backing_use use = {textures.back(), SWITCH_GPU_BACKING_WRITE, 0};
			const switch_gpu_backing_token drawn = app(i).token(1);
			switch_gpu_backing_transaction transaction = {};
			ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawn, &transaction), 0);
			switch_gpu_backing_commit(&transaction);
		}
		for (uint32_t i = 0; i < kApps; ++i)
			app(i).fence = 1;
		for (int warm = 0; warm < 4; ++warm)
			composite();
	}

	// One compositor job as SubmitJob runs it once its uses are resolved; the previous frame is still on the GPU.
	Cost composite()
	{
		std::vector<switch_gpu_backing_use> uses;
		for (auto *texture : textures)
			uses.push_back(
				{texture,
				 SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0});
		uses.push_back({target,
				SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0});
		switch_gpu_backing_write_range write = {target, 0, 0};
		std::vector<switch_gpu_backing_token> dependencies(TegraGM20BMaxJobDependencies);
		const switch_gpu_backing_token completion = compositor.token(++frame);
		const uint64_t own = own_takes(), all = ioLockTakes().load();
		switch_gpu_backing_transaction transaction = {};
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_begin_held(uses.data(), uint32_t(uses.size()), nullptr,
							0, nullptr, 0, &completion, &transaction),
			  0);
		EXPECT_EQ(switch_gpu_backing_write_ranges(&transaction, &write, 1), 0);
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(),
							  TegraGM20BMaxJobUses, &needed),
			  0);
		EXPECT_EQ(switch_gpu_backing_promise_dependencies(&transaction, dependencies.data(),
								  TegraGM20BMaxJobDependencies,
								  &needed, 0, 0),
			  0);
		switch_gpu_backing_commit(&transaction);
		const uint64_t taken = ioLockTakes().load() - all, ownTaken = own_takes() - own;
		compositor.fence = frame - 1;
		return {taken - ownTaken, ownTaken, needed};
	}

	uint64_t own_takes() const
	{
		uint64_t takes = target->lock->takes;
		for (const auto *texture : textures)
			takes += texture->lock->takes;
		return takes;
	}

	double mean_us(uint32_t jobs)
	{
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t i = 0; i < jobs; ++i)
			composite();
		const std::chrono::duration<double, std::micro> spent =
			std::chrono::steady_clock::now() - start;
		return spent.count() / jobs;
	}
};

TEST_F(SubmitCost, ScrollJobAtSixUses)
{
	scene(6);
	const Cost cost = composite();
	std::printf(
		"6 shared uses: %llu global IOLock takes, %llu of the uses' own, %.2f us a job\n",
		static_cast<unsigned long long>(cost.global),
		static_cast<unsigned long long>(cost.own), mean_us(200));
	RecordProperty("global_takes_6", std::to_string(cost.global));
	EXPECT_LE(cost.global, 2u);
}

// What resolves a scale animation's 55-61 shared uses: the same global locks as six, its uses' own locks once each.
TEST_F(SubmitCost, ScaleJobAtSixtyUsesTakesNoGlobalLockPerUse)
{
	scene(60);
	const Cost cost = composite();
	std::printf(
		"60 shared uses: %llu global IOLock takes, %llu of the uses' own, %.2f us a job\n",
		static_cast<unsigned long long>(cost.global),
		static_cast<unsigned long long>(cost.own), mean_us(200));
	RecordProperty("global_takes_60", std::to_string(cost.global));
	EXPECT_LE(cost.global, 2u);
	EXPECT_EQ(cost.own, 61u);
	EXPECT_EQ(cost.dependencies, 0u);
}

// An app still drawing one texture: the job waits for that app's channel, found under the texture's own lock.
TEST_F(SubmitCost, PendingProducerIsADependencyWithoutAGlobalLockPerUse)
{
	scene(60);
	switch_gpu_backing_use use = {textures[7], SWITCH_GPU_BACKING_WRITE, 0};
	const switch_gpu_backing_token drawing = app(7).token(2);
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawing, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	Cost cost = composite();
	EXPECT_EQ(cost.dependencies, 1u);
	EXPECT_LE(cost.global, 2u);
	app(7).fence = 2;
	cost = composite();
	EXPECT_EQ(cost.dependencies, 0u);
}
}  // namespace
