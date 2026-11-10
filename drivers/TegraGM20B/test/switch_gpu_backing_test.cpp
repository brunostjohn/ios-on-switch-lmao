// SPDX-License-Identifier: MIT
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
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
int switch_gpu_surface_lock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_lock_failed(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_will(void *, uint32_t, uint32_t);
void switch_gpu_surface_unlock_did(void *, uint32_t, uint32_t);
void switch_gpu_surface_sweep(void);
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
	void TearDown() override;
};
} // namespace

extern "C" void switch_gpu_backing_retire(void);
extern "C" int switch_gpu_backing_prepare_retirement(void) { return 0; }
extern "C" void switch_gpu_backing_schedule_retirement(void) { retirement_scheduled = true; }
extern "C" void switch_gpu_backing_cache(const uint64_t *, uint32_t, uint64_t offset, uint64_t bytes, int invalidate)
{
	cache.push_back({offset, bytes, invalidate});
}

void Backing::TearDown()
{
	switch_gpu_backing_retire();
	for (const auto &entry : timelines)
		EXPECT_EQ(entry.second.refs, 0);
}

TEST_F(Backing, TracksEveryWriterAndFailsClosedWhenDependencyArrayIsTooSmall)
{
	auto *allocation = backing();
	ASSERT_NE(allocation, nullptr);
	for (uint32_t i = 1; i <= 40; ++i)
		ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(i)), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(41);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	std::array<switch_gpu_backing_token, 40> dependencies;
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 16, &needed), -28);
	EXPECT_EQ(needed, 40u);
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 40, &needed), 0);
	EXPECT_EQ(needed, 40u);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
	EXPECT_TRUE(retirement_scheduled);
}

TEST_F(Backing, WritersWaitForReadersButReadersDoNotWaitForEachOther)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), 0);
	for (uint32_t access : {SWITCH_GPU_BACKING_READ, SWITCH_GPU_BACKING_WRITE}) {
		switch_gpu_backing_use use = {allocation, access, 0};
		switch_gpu_backing_transaction transaction = {};
		const auto completion = token(2);
		ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
		switch_gpu_backing_token dependency;
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
		EXPECT_EQ(needed, access == SWITCH_GPU_BACKING_WRITE ? 1u : 0u);
		switch_gpu_backing_abort(&transaction);
	}
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PartialCpuWriteInvalidatesBeforeCleanAndKeepsNeighboringGpuData)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	ASSERT_EQ(cache.size(), 1u);
	EXPECT_EQ(cache[0].invalidate, 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 7, 2, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache[1].invalidate, 1);
	EXPECT_EQ(cache[1].offset, 7u);
	EXPECT_EQ(cache[1].bytes, 2u);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 7, 2, SWITCH_GPU_BACKING_WRITE), 0);
	ASSERT_EQ(cache.size(), 3u);
	EXPECT_EQ(cache[2].invalidate, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	EXPECT_EQ(cache.size(), 3u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CpuAvoidSyncAndActiveCpuOwnershipRefuseConflictingEngines)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
		SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailureAndResetGenerationCannotBeReportedAsCompletion)
{
	auto *allocation = backing();
	const auto first = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, first), 0);
	timelines[key(&first)].error = -5;
	const auto reset = token(1, 99, SWITCH_GPU_ENGINE_GM20B, 2);
	timelines[key(&reset)].completed = 99;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), -5);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), -5);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, CompletionRecordsAndPinnedOwnerOutliveLastClientLease)
{
	const uint64_t page = next_page += 4096;
	int releases = 0;
	auto *allocation = switch_gpu_backing_register(&page, 1, 0, &releases,
		[](void *owner) { ++*static_cast<int *>(owner); });
	const auto completion = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, completion), 0);
	switch_gpu_backing_release(allocation);
	EXPECT_EQ(releases, 0);
	EXPECT_EQ(timelines[key(&completion)].refs, 1);
	switch_gpu_backing_retire();
	EXPECT_EQ(releases, 1);
	EXPECT_EQ(timelines[key(&completion)].refs, 0);
	EXPECT_EQ(switch_gpu_backing_find_pa(page), nullptr);
}

TEST_F(Backing, RegistrationRetainsIdentityAndExtendingPagesChangesGeneration)
{
	const uint64_t pages[] = {next_page += 4096, next_page += 4096};
	auto *first = switch_gpu_backing_register(pages, 1, 500, nullptr, nullptr);
	switch_gpu_backing_info before = {}, after = {};
	switch_gpu_backing_describe(first, &before);
	auto *second = switch_gpu_backing_register(pages, 2, 500, nullptr, nullptr);
	ASSERT_EQ(first, second);
	switch_gpu_backing_describe(second, &after);
	EXPECT_EQ(before.allocation_id, after.allocation_id);
	EXPECT_NE(before.allocation_generation, after.allocation_generation);
	EXPECT_EQ(before.pages[0], pages[0]);
	EXPECT_EQ(after.page_count, 2u);
	auto *by_surface = switch_gpu_backing_find_surface(500);
	EXPECT_EQ(by_surface, first);
	switch_gpu_backing_release(by_surface);
	switch_gpu_backing_release(first);
	switch_gpu_backing_release(second);
}

TEST_F(Backing, DuplicateUsesMergeAndAbortBalancesPreparedReferences)
{
	auto *one = backing(), *two = backing();
	switch_gpu_backing_use uses[] = {{two, SWITCH_GPU_BACKING_READ, 0}, {one, SWITCH_GPU_BACKING_READ, 0},
		{two, SWITCH_GPU_BACKING_WRITE, 0}};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	ASSERT_EQ(switch_gpu_backing_begin(uses, 3, &completion, &transaction), 0);
	EXPECT_EQ(transaction.count, 2u);
	EXPECT_EQ(timelines[key(&completion)].refs, 2);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(timelines[key(&completion)].refs, 0);
	switch_gpu_backing_release(one);
	switch_gpu_backing_release(two);
}

TEST_F(Backing, Host1xWrapAdvancesOnTheSameTimeline)
{
	auto *allocation = backing();
	const auto before_wrap = token(1, 0xfffffff0u, SWITCH_GPU_ENGINE_VIC);
	timelines[key(&before_wrap)].completed = 0xffffff00u;
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, before_wrap), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1, 5, SWITCH_GPU_ENGINE_VIC)), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1, 4, SWITCH_GPU_ENGINE_VIC)), -22);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, DiscardRequiresFullCacheLinesAndLayoutRequiresPaddedBlockExtent)
{
	auto *allocation = backing();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 1, 64,
		SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_DISCARD), -22);
	switch_gpu_backing_layout layout = {};
	layout.plane_count = 1;
	layout.planes[0] = {0, 4096, 64, 9, 256, 4, SWITCH_GPU_BACKING_BLOCK_LINEAR, 2};
	EXPECT_EQ(switch_gpu_backing_set_layout(allocation, &layout), -22);
	layout.planes[0].block_height_log2 = 1;
	EXPECT_EQ(switch_gpu_backing_set_layout(allocation, &layout), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, SurfaceUnlockCleansBeforeSeedAndKeepsOwnershipUntilAppleSucceeds)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 700, nullptr, nullptr);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 700, 0), 0);
	switch_gpu_surface_lock_did(&client, 700, 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 700, 1), 0);
	switch_gpu_surface_lock_did(&client, 700, 1);
	const size_t before = cache.size();
	switch_gpu_surface_unlock_will(&client, 700, 0);
	EXPECT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	switch_gpu_surface_unlock_did(&client, 700, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	switch_gpu_surface_unlock_will(&client, 700, 1);
	switch_gpu_surface_unlock_did(&client, 700, 1);
	EXPECT_EQ(cache.size(), before + 1);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedAppleLockUnwindsOnlyItsPendingReservation)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 701, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 701, 0), 0);
	switch_gpu_surface_lock_did(&client, 701, 0);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 701, 1), 0);
	switch_gpu_surface_lock_failed(&client, 701, 1);
	switch_gpu_surface_lock_failed(&client, 701, 1);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), -16);
	switch_gpu_surface_unlock_will(&client, 701, 0);
	switch_gpu_surface_unlock_did(&client, 701, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, DeadSurfaceClientReleasesCpuReservationsBeforeNextEngineSubmission)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 702, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 702, 0), 0);
	switch_gpu_surface_lock_did(&client, 702, 0);
	client.inactive = true;
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ExtendingGpuDirtyBackingCleansOnlyTheNewPages)
{
	const uint64_t pages[] = {next_page += 4096, next_page += 4096};
	auto *allocation = switch_gpu_backing_register(pages, 1, 0, nullptr, nullptr);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	auto *extended = switch_gpu_backing_register(pages, 2, 0, nullptr, nullptr);
	ASSERT_EQ(allocation, extended);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	ASSERT_EQ(cache.size(), 2u);
	EXPECT_EQ(cache[1].offset, 4096u);
	EXPECT_EQ(cache[1].bytes, 4096u);
	switch_gpu_backing_release(extended);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, RefusedTrapCannotCancelAnotherThreadsPendingLock)
{
	IOUserClient client;
	const uint64_t page = next_page += 4096;
	auto *allocation = switch_gpu_backing_register(&page, 1, 703, nullptr, nullptr);
	ASSERT_EQ(switch_gpu_surface_lock_will(&client, 703, 0), 0);
	std::thread refused([&] { switch_gpu_surface_lock_failed(&client, 703, 0); });
	refused.join();
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), -16);
	switch_gpu_surface_lock_did(&client, 703, 0);
	switch_gpu_surface_unlock_will(&client, 703, 0);
	switch_gpu_surface_unlock_did(&client, 703, 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(1)), 0);
	EXPECT_EQ(client.refs, 1u);
	switch_gpu_backing_release(allocation);
}
