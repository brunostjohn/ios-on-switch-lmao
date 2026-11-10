// SPDX-License-Identifier: MIT
// The ownership layer on the host: engines whose timelines a test completes or fails by hand, and the kernel calls
// SwitchGpuBacking.cpp and SwitchGpuSurfaceAccess.cpp make. Included once per test binary.
#pragma once
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>
#include <IOKit/IOUserClient.h>
#include "drivers/TegraGM20B/SwitchGpuBacking.h"

extern "C" {
struct tgm_coh;
tgm_coh *tgm_coh_find_pa(uint64_t) { return nullptr; }
void tgm_views_sweep(tgm_coh *) {}
void tgm_coherency_put(tgm_coh *) {}
int tegra_gm20b_surface_tiled(uint64_t) { return 0; }
int switch_gpu_surface_backing(uint32_t id, void *, switch_gpu_backing **backing)
{
	*backing = switch_gpu_backing_find_surface(id);
	return 0;
}
int switch_gpu_surface_lock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_failed(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_sweep(void);
void switch_gpu_backing_retire(void);
}

namespace {
using Key = std::tuple<uint32_t, uint32_t, uint64_t>;
struct Timeline { uint64_t completed = 0; int error = 0, refs = 0; };
std::map<Key, Timeline> timelines;
std::mutex timeline_lock;
struct Cache { uint64_t offset, bytes; int invalidate; };
std::vector<Cache> cache;
uint64_t next_page = 0x80000000;
bool retirement_scheduled;

Key key(const switch_gpu_backing_token *token)
{
	return {token->engine, token->queue, token->reset_generation};
}

int status(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	const auto &timeline = timelines[key(token)];
	const bool reached = token->engine == SWITCH_GPU_ENGINE_VIC ?
		int32_t(uint32_t(timeline.completed - token->value)) >= 0 : timeline.completed >= token->value;
	return reached ? 1 : timeline.error;
}

int wait(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	auto &timeline = timelines[key(token)];
	if (timeline.error)
		return timeline.error;
	timeline.completed = token->value;
	return 1;
}

int retain(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	++timelines[key(token)].refs;
	return 0;
}

void release(const switch_gpu_backing_token *token)
{
	std::lock_guard<std::mutex> held(timeline_lock);
	EXPECT_GT(timelines[key(token)].refs, 0);
	--timelines[key(token)].refs;
}

switch_gpu_backing_token token(uint32_t queue, uint64_t value = 1, uint32_t engine = SWITCH_GPU_ENGINE_GM20B,
		uint64_t generation = 1)
{
	return {value, generation, engine, queue};
}

// The channel of `of` killed: the work it had not finished reports failure.
[[maybe_unused]] void kill_channel(const switch_gpu_backing_token &of)
{
	timelines[key(&of)].error = -5;
}

[[maybe_unused]] void complete(const switch_gpu_backing_token &of)
{
	timelines[key(&of)].completed = of.value;
}

[[maybe_unused]] int refs(const switch_gpu_backing_token &of)
{
	return timelines[key(&of)].refs;
}

switch_gpu_backing *backing(uint32_t count = 1)
{
	std::vector<uint64_t> pages;
	for (uint32_t i = 0; i < count; ++i)
		pages.push_back(next_page += 4096);
	return switch_gpu_backing_register(pages.data(), count, 0, nullptr, nullptr);
}

int publish(switch_gpu_backing *allocation, uint32_t access, switch_gpu_backing_token completion)
{
	switch_gpu_backing_use use = {allocation, access, 0};
	switch_gpu_backing_transaction transaction = {};
	const int result = switch_gpu_backing_begin(&use, 1, &completion, &transaction);
	if (!result)
		switch_gpu_backing_commit(&transaction);
	return result;
}

class Backing : public testing::Test {
protected:
	void SetUp() override
	{
		static bool registered;
		if (!registered) {
			const switch_gpu_backing_engine gm = {status, wait, retain, release, 64};
			const switch_gpu_backing_engine vic = {status, wait, retain, release, 32};
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_GM20B, &gm), 0);
			ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_VIC, &vic), 0);
			registered = true;
		}
		timelines.clear();
		cache.clear();
		retirement_scheduled = false;
	}
	void TearDown() override
	{
		switch_gpu_backing_retire();
		for (const auto &entry : timelines)
			EXPECT_EQ(entry.second.refs, 0);
	}
};
} // namespace

extern "C" int switch_gpu_backing_prepare_retirement(void) { return 0; }
extern "C" void switch_gpu_backing_schedule_retirement(void) { retirement_scheduled = true; }
extern "C" void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t offset, uint64_t bytes, int invalidate)
{
	cache.push_back({offset, bytes, invalidate});
}
