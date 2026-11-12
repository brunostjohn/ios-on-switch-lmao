// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuAdmission.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t avoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;

TEST_F(Backing, AdmissionReadKeepsExistingCpuReadersAndBlocksNewWriter)
{
	auto *b = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kRead, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kWrite | avoid), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	admission.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kWrite | avoid), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionWriterBlocksCpuButLeavesGpuDependencyChainingAvailable)
{
	auto *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	EXPECT_EQ(publish(b, kWrite, token(1)), 0);
	EXPECT_EQ(publish(b, kRead, token(2)), 0);
	admission.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionFailureDoesNotReserveEarlierResources)
{
	auto *a = backing(), *b = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use uses[]{{a, kWrite, 0}, {b, kWrite, 0}};
	EXPECT_EQ(admission.begin(uses, 2, nullptr, 0, nullptr, 0), -16);
	EXPECT_FALSE(admission.active());
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(a, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(a, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionTransfersPreparedConversionWithoutAnOwnershipGap)
{
	auto *b = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(b, kWrite, true, &lease), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, &lease, 1), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_conversion_ready(b, kRead), 0);
	EXPECT_EQ(switch_gpu_backing_admission_ready(b, kWrite), 1);
	EXPECT_EQ(switch_gpu_backing_conversion_reserve(b, kRead, true, &lease), -16);
	EXPECT_EQ(publish(b, kWrite, token(1)), 0);
	admission.end();
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionAliasUnionAndContinuationCoverageAreExact)
{
	auto *a = backing(), *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use uses[]{{a, kRead, 0}, {a, kWrite, 0}};
	ASSERT_EQ(admission.begin(uses, 2, nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.covers(uses, 2));
	switch_gpu_backing_use other{b, kRead, 0};
	EXPECT_FALSE(admission.covers(&other, 1));
	admission.end();
	ASSERT_EQ(admission.begin(uses, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_FALSE(admission.covers(uses + 1, 1));
	admission.end();
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionRetainsIndependentQueueCountsUntilBothEnd)
{
	auto *b = backing();
	SwitchGpuAdmission first, second;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(first.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	ASSERT_EQ(second.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	first.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	second.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionStaleGuardRefusesBeforeReservationAndFailedContentDoesNot)
{
	auto *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	alignas(8) volatile uint64_t generation = 3;
	switch_gpu_backing_guard guard{b, &generation, 2};
	EXPECT_EQ(admission.begin(&use, 1, &guard, 1, nullptr, 0), SWITCH_GPU_BACKING_STALE);
	EXPECT_FALSE(admission.active());
	ASSERT_EQ(publish(b, kWrite, token(1)), 0);
	const auto failed = token(1);
	timelines[key(&failed)].error = -5;
	EXPECT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.active());
	admission.end();
	switch_gpu_backing_release(b);
}
}

TEST_F(Backing, AdmissionOrdersLargeAliasedUseSetAndRejectsGrowthWhileHeld)
{
	std::vector<switch_gpu_backing *> allocations;
	for (unsigned i = 0; i < 256; ++i)
		allocations.push_back(backing());
	std::vector<switch_gpu_backing_use> uses;
	for (unsigned i = 0; i < 1024; ++i)
		uses.push_back({allocations[(1023 - i) % 256], i & 1 ? kRead : kWrite, 0});
	SwitchGpuAdmission admission;
	ASSERT_EQ(admission.begin(uses.data(), uint32_t(uses.size()), nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.covers(uses.data(), uint32_t(uses.size())));
	std::array<uint64_t, 2> pages;
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocations.front(), &info);
	pages[0] = info.pages[0];
	pages[1] = pages[0] + 4096;
	EXPECT_EQ(switch_gpu_backing_register(pages.data(), 2, 0, nullptr, nullptr), nullptr);
	admission.end();
	for (auto *allocation : allocations)
		switch_gpu_backing_release(allocation);
}

// A command buffer's writes promised before its job exists: the display's non-waiting snapshot and a CPU lock that avoids
// a sync see a writer, nothing the display or the CPU holds already refuses it, and its end lets both in again.
TEST_F(Backing, AWriterPromiseHoldsSnapshotsAndCpuLocksOffAndNoOwnerRefusesIt)
{
	auto *b = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &lease), 0);  // the display's slot lease
	SwitchGpuAdmission promise;
	const switch_gpu_backing_use uses[]{{b, kWrite, 0}, {b, kWrite, 0}};
	ASSERT_EQ(promise.promise(uses, 2), 0);
	EXPECT_TRUE(promise.active());
	switch_gpu_cpu_lease *second = nullptr;
	EXPECT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &second), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kWrite | avoid), -16);
	// the promised job itself is never held off (a GM20B user submission runs beside CPU owners)
	EXPECT_EQ(publish(b, kWrite | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, token(1)), 0);
	promise.end();
	EXPECT_FALSE(promise.active());
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &second), 0);
	EXPECT_EQ(producer.value, token(1).value);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&second, 0), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AWriterPromiseNamesWritesOnly)
{
	auto *b = backing();
	SwitchGpuAdmission promise;
	const switch_gpu_backing_use read{b, kRead, 0};
	EXPECT_EQ(promise.promise(&read, 1), -22);
	EXPECT_EQ(promise.promise(nullptr, 0), -22);
	EXPECT_FALSE(promise.active());
	const switch_gpu_backing_use write{b, kWrite | kRead, 0};
	ASSERT_EQ(promise.promise(&write, 1), 0);
	EXPECT_EQ(promise.promise(&write, 1), -22);  // one at a time
	promise.end();
	switch_gpu_backing_release(b);
}

// The promised job's own submission may reserve a conversion of, lay out and grow the very backings it promised: none
// of those reads the promise, so none waits for the job that ends it.
TEST_F(Backing, AWriterPromiseNeverHoldsOffItsOwnJobsConversionLayoutOrGrowth)
{
	auto *b = backing();
	SwitchGpuAdmission promise;
	const switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(promise.promise(&use, 1), 0);
	EXPECT_EQ(switch_gpu_backing_conversion_ready(b, kWrite), 1);
	switch_gpu_cpu_lease *conversion = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(b, kWrite, false, &conversion), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&conversion, 0), 0);
	switch_gpu_backing_layout layout = {};
	layout.plane_count = 1;
	layout.planes[0] = {0, 4096, 64, 9, 256, 4, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
	EXPECT_EQ(switch_gpu_backing_set_layout(b, &layout), 0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(b, &info);
	const uint64_t pages[2] = {info.pages[0], info.pages[0] + 4096};
	auto *grown = switch_gpu_backing_register(pages, 2, 0, nullptr, nullptr);
	EXPECT_EQ(grown, b);
	if (grown)
		switch_gpu_backing_release(grown);
	promise.end();
	switch_gpu_backing_release(b);
}

// A snapshot that may wait sleeps through the promise and wakes at its end, naming the promised job.
TEST_F(Backing, AWaitingSnapshotWakesWhenThePromiseEnds)
{
	auto *b = backing();
	SwitchGpuAdmission promise;
	const switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(promise.promise(&use, 1), 0);
	std::atomic<int> result{1};
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	std::thread display([&] {
		result = switch_gpu_backing_snapshot_read_wait(b, &producer, &lease);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(result.load(), 1);  // still asleep
	EXPECT_EQ(publish(b, kWrite | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, token(3, 7)), 0);
	promise.end();
	display.join();
	EXPECT_EQ(result.load(), 0);
	EXPECT_EQ(producer.value, 7u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(b);
}
