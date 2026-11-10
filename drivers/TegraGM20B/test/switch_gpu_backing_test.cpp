// SPDX-License-Identifier: MIT
#include <array>
#include <thread>
#include "switch_gpu_backing_fixture.h"

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

TEST_F(Backing, CpuReadLeaseHandsOwnershipToThePublishedGpuReader)
{
	auto *allocation = backing();
	const uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	const uint32_t write = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, read), 0);
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, reader), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, write), -16);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, read), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto writer = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &writer, &transaction), 0);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 1u);
	EXPECT_EQ(dependency.queue, reader.queue);
	EXPECT_EQ(dependency.value, reader.value);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedAvoidSyncReadLeavesNoCpuReservation)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
		SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC), -16);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PreserveDirtyOnlyCleansAfterTheNonblockingProducerCheck)
{
	auto *allocation = backing();
	const auto producer = token(1);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, producer), 0);
	const uint32_t access = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC |
		SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY;
	const size_t before = cache.size();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, access), -16);
	EXPECT_EQ(cache.size(), before);
	timelines[key(&producer)].completed = producer.value;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, access), 0);
	ASSERT_EQ(cache.size(), before + 1);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_CLEAN_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, access), 0);
	const auto next = token(2);
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, next), 0);
	timelines[key(&next)].completed = next.value;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(cache.back().invalidate, SWITCH_GPU_CACHE_INVALIDATE);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, PreserveDirtyRejectsOtherOwnershipModes)
{
	auto *allocation = backing();
	for (uint32_t access : {uint32_t(SWITCH_GPU_BACKING_READ), uint32_t(SWITCH_GPU_BACKING_WRITE),
		uint32_t(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC | SWITCH_GPU_BACKING_CPU_NO_CACHE),
		uint32_t(SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_AVOID_SYNC | SWITCH_GPU_BACKING_CPU_CONCURRENT)})
		EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096,
			access | SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY), -22);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ConversionUsesOnlyItsOwnedNativeAndPitchLeases)
{
	auto *native = backing();
	auto *pitch = backing();
	const auto producer = token(1), initialized = token(2);
	ASSERT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, producer), 0);
	ASSERT_EQ(publish(pitch, SWITCH_GPU_BACKING_READ, initialized), 0);
	timelines[key(&producer)].completed = producer.value;
	timelines[key(&initialized)].completed = initialized.value;
	const size_t cacheBefore = cache.size();
	switch_gpu_cpu_lease *nativeLease = nullptr, *pitchLease = nullptr;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(native, 0, 4096,
		SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE, &nativeLease), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(pitch, 0, 4096,
		SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE, &pitchLease), 0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(native, &info);
	EXPECT_NE(info.dirty & SWITCH_GPU_BACKING_GPU_DIRTY, 0u);
	EXPECT_EQ(info.dirty & SWITCH_GPU_BACKING_CPU_DIRTY, 0u);
	switch_gpu_backing_use uses[] = {{native, SWITCH_GPU_BACKING_READ, 0}, {pitch, SWITCH_GPU_BACKING_WRITE, 0}};
	switch_gpu_cpu_lease *leases[] = {nativeLease, pitchLease};
	switch_gpu_backing_transaction transaction = {};
	const auto conversion = token(3, 1, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(switch_gpu_backing_begin_conversion(uses, 2, leases, 2, &conversion, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(cache.size(), cacheBefore);
	EXPECT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, token(4)), -16);
	EXPECT_EQ(publish(pitch, SWITCH_GPU_BACKING_WRITE, token(4)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&nativeLease, 0), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&pitchLease, 0), 0);
	EXPECT_EQ(nativeLease, nullptr);
	EXPECT_EQ(pitchLease, nullptr);
	for (auto *allocation : {native, pitch}) {
		switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
		const auto consumer = token(4);
		ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &consumer, &transaction), 0);
		switch_gpu_backing_token dependency = {};
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
		EXPECT_EQ(needed, 1u);
		EXPECT_EQ(dependency.engine, conversion.engine);
		EXPECT_EQ(dependency.queue, conversion.queue);
		switch_gpu_backing_abort(&transaction);
	}
	switch_gpu_backing_release(native);
	switch_gpu_backing_release(pitch);
}

TEST_F(Backing, ConversionDoesNotIgnoreAnotherCpuReader)
{
	auto *allocation = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	const uint32_t read = SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, read, &lease), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, read), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	EXPECT_EQ(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, read), 0);
	ASSERT_EQ(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction), 0);
	switch_gpu_backing_abort(&transaction);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, ConversionRejectsForeignDuplicateAndPartialLeases)
{
	auto *allocation = backing();
	auto *other = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	const uint32_t access = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, access, &lease), 0);
	switch_gpu_backing_use use = {other, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	EXPECT_EQ(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction), -22);
	use.backing = allocation;
	switch_gpu_cpu_lease *duplicates[] = {lease, lease};
	EXPECT_EQ(switch_gpu_backing_begin_conversion(&use, 1, duplicates, 2, &completion, &transaction), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 64, 64, access, &lease), 0);
	EXPECT_EQ(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction), -22);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096, SWITCH_GPU_BACKING_READ, &lease), -22);
	EXPECT_EQ(lease, nullptr);
	switch_gpu_backing_release(allocation);
	switch_gpu_backing_release(other);
}

TEST_F(Backing, AbortedConversionKeepsItsCpuExclusion)
{
	auto *allocation = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096,
		SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE, &lease), 0);
	switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_READ, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto completion = token(1);
	ASSERT_EQ(switch_gpu_backing_begin_conversion(&use, 1, &lease, 1, &completion, &transaction), 0);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedOwnedAvoidSyncAcquisitionReturnsNoLease)
{
	auto *allocation = backing();
	ASSERT_EQ(publish(allocation, SWITCH_GPU_BACKING_WRITE, token(1)), 0);
	switch_gpu_cpu_lease *lease = nullptr;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire_owned(allocation, 0, 4096,
		SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_CPU_NO_CACHE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC, &lease), -16);
	EXPECT_EQ(lease, nullptr);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
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
	const int logged = ioLogLines();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(ioLogLines(), logged + 1);
	EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, SWITCH_GPU_BACKING_READ), 0);
	EXPECT_EQ(publish(allocation, SWITCH_GPU_BACKING_READ, token(2)), 0);
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
