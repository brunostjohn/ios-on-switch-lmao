// SPDX-License-Identifier: MIT
// A killed channel's records against later users of the same allocation (SwitchGpuOwnership.cpp): a failed read changes
// nothing, a failed write fails the first reader of what it wrote and nobody after, and no record outlives its use.
#include <array>
#include "switch_gpu_backing_fixture.h"

namespace {
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;

int cpu_read(switch_gpu_backing *allocation)
{
	const int result = switch_gpu_backing_cpu_acquire(allocation, 0, 4096, kRead);
	if (!result)
		EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, 0, 4096, kRead), 0);
	return result;
}

int cpu_write(switch_gpu_backing *allocation, uint64_t offset, uint64_t bytes, uint32_t flags = 0)
{
	const int result = switch_gpu_backing_cpu_acquire(allocation, offset, bytes, kWrite | flags);
	if (!result)
		EXPECT_EQ(switch_gpu_backing_cpu_release(allocation, offset, bytes, kWrite | flags), 0);
	return result;
}
} // namespace

TEST_F(Backing, FailedReaderLetsTheNextWriterAndReaderRun)
{
	auto *allocation = backing();
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, kRead, reader), 0);
	kill_channel(reader);
	EXPECT_EQ(publish(allocation, kWrite, token(2)), 0);
	EXPECT_EQ(refs(reader), 0);
	EXPECT_EQ(publish(allocation, kRead, token(3)), 0);
	EXPECT_EQ(cpu_read(allocation), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedWriterFailsOnlyItsFirstReader)
{
	auto *allocation = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(publish(allocation, kRead, token(2)), -5);
	EXPECT_EQ(refs(writer), 0);
	EXPECT_EQ(publish(allocation, kRead, token(3)), 0);
	EXPECT_EQ(publish(allocation, kRead | kWrite, token(4)), 0);
	EXPECT_EQ(cpu_read(allocation), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, FailedWriterFailsTheFirstCpuReaderOnce)
{
	auto *allocation = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(cpu_read(allocation), -5);
	EXPECT_EQ(cpu_read(allocation), 0);
	EXPECT_EQ(publish(allocation, kRead, token(2)), 0);
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocation, &info);
	EXPECT_EQ(info.cpu_locks, 0u);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, WritersOfFailedContentsNeverFailAndAPartialWriteLeavesThemUndefined)
{
	auto *gpu = backing(), *cpu = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(gpu, kWrite, writer), 0);
	ASSERT_EQ(publish(cpu, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(publish(gpu, kWrite, token(2)), 0);
	EXPECT_EQ(cpu_write(cpu, 0, 64, SWITCH_GPU_BACKING_CPU_DISCARD), 0);
	EXPECT_EQ(cpu_write(cpu, 64, 64), 0);
	EXPECT_EQ(publish(gpu, kRead, token(3)), -5);
	EXPECT_EQ(publish(cpu, kRead, token(3)), -5);
	EXPECT_EQ(publish(gpu, kRead, token(4)), 0);
	EXPECT_EQ(publish(cpu, kRead, token(4)), 0);
	switch_gpu_backing_release(gpu);
	switch_gpu_backing_release(cpu);
}

TEST_F(Backing, CompleteOverwriteStartsCleanContents)
{
	auto *allocation = backing(2);
	const auto writer = token(1);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	kill_channel(writer);
	EXPECT_EQ(cpu_write(allocation, 0, 8192, SWITCH_GPU_BACKING_CPU_DISCARD), 0);
	EXPECT_EQ(publish(allocation, kRead, token(2)), 0);
	EXPECT_EQ(cpu_read(allocation), 0);
	const auto next = token(3);
	ASSERT_EQ(publish(allocation, kWrite, next), 0);
	kill_channel(next);
	EXPECT_EQ(cpu_read(allocation), -5);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, OneSubmissionAbsorbsEveryFailedInputItReads)
{
	std::array<switch_gpu_backing *, 3> inputs = {backing(), backing(), backing()};
	const auto writer = token(1);
	for (auto *input : inputs)
		ASSERT_EQ(publish(input, kWrite, writer), 0);
	kill_channel(writer);
	std::array<switch_gpu_backing_use, 3> uses = {};
	for (size_t i = 0; i < uses.size(); ++i)
		uses[i] = {inputs[i], kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto first = token(2), second = token(2, 2);
	EXPECT_EQ(switch_gpu_backing_begin(uses.data(), uint32_t(uses.size()), &first, &transaction), -5);
	for (size_t i = 0; i < uses.size(); ++i)
		uses[i] = {inputs[i], kRead, 0};
	ASSERT_EQ(switch_gpu_backing_begin(uses.data(), uint32_t(uses.size()), &second, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	EXPECT_EQ(refs(writer), 0);
	for (auto *input : inputs)
		switch_gpu_backing_release(input);
}

TEST_F(Backing, ProducerKilledAfterBeginFailsItsReaderInDependencies)
{
	auto *allocation = backing();
	const auto writer = token(1);
	ASSERT_EQ(publish(allocation, kWrite, writer), 0);
	switch_gpu_backing_use use = {allocation, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto reader = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &reader, &transaction), 0);
	kill_channel(writer);
	std::array<switch_gpu_backing_token, 4> dependencies = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies.data(), 4, &needed), -5);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(publish(allocation, kRead, token(2, 2)), 0);
	switch_gpu_backing_release(allocation);
}

TEST_F(Backing, WriterDoesNotWaitForOrFailOnAKilledReader)
{
	auto *allocation = backing();
	const auto reader = token(1);
	ASSERT_EQ(publish(allocation, kRead, reader), 0);
	switch_gpu_backing_use use = {allocation, kWrite, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto writer = token(2);
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &writer, &transaction), 0);
	kill_channel(reader);
	switch_gpu_backing_token dependency = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed), 0);
	EXPECT_EQ(needed, 0u);
	switch_gpu_backing_commit(&transaction);
	switch_gpu_backing_release(allocation);
}

// Process A writes `drawn` and samples `shared`; process B samples `shared` too. A's channel dies with both pending.
TEST_F(Backing, KillInOneProcessFailsOnlyTheOtherProcessesReadsOfWhatItWrote)
{
	auto *drawn = backing(), *shared = backing();
	const auto a = token(1), b = token(2);
	ASSERT_EQ(publish(drawn, kWrite, a), 0);
	ASSERT_EQ(publish(shared, kRead, a), 0);
	ASSERT_EQ(publish(shared, kRead, b), 0);
	kill_channel(a);
	EXPECT_EQ(publish(shared, kRead, token(2, 2)), 0);
	EXPECT_EQ(refs(a), 2);
	EXPECT_EQ(publish(drawn, kRead, token(2, 3)), -5);
	EXPECT_EQ(publish(drawn, kRead, token(2, 4)), 0);
	complete(token(2, 4));
	EXPECT_EQ(publish(shared, kWrite, token(2, 5)), 0);
	EXPECT_EQ(publish(drawn, kWrite, token(2, 5)), 0);
	EXPECT_EQ(refs(a), 0);
	switch_gpu_backing_release(drawn);
	switch_gpu_backing_release(shared);
}
