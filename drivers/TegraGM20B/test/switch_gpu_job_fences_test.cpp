// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../SwitchGpuJobFences.h"
#include "../TegraGM20BFences.h"

namespace {
std::vector<tgm_push> batches(std::initializer_list<uint32_t> sizes)
{
	std::vector<tgm_push> pushes;
	for (uint32_t size : sizes) {
		for (uint32_t i = 0; i < size; ++i)
			pushes.push_back({0x100000 + pushes.size() * 0x100, 16, 0, 0, 0, 0});
		pushes.back().fence = 1;
	}
	pushes.back().fence = 0;
	return pushes;
}

struct Stream {
	std::vector<std::string> items;

	static void entry(void *context, uint32_t push)
	{
		static_cast<Stream *>(context)->items.push_back("push " + std::to_string(push));
	}

	static void fence(void *context, uint32_t seqno)
	{
		static_cast<Stream *>(context)->items.push_back("fence " + std::to_string(seqno));
	}
};

uint32_t emit(const std::vector<tgm_push> &pushes, uint32_t submitted, Stream &stream)
{
	const switch_gpu_job_emitter emitter = {&stream, Stream::entry, Stream::fence};
	return switch_gpu_job_emit(pushes.data(), uint32_t(pushes.size()), submitted, &emitter);
}
}  // namespace

TEST(JobFences, OneJobCarriesEveryBatchWithAFenceAtEachBatchEnd)
{
	const auto pushes = batches({1, 2, 1});
	uint32_t fences = 0;
	ASSERT_EQ(switch_gpu_job_fences(pushes.data(), uint32_t(pushes.size()), &fences), 0);
	EXPECT_EQ(fences, 2u);
	Stream stream;
	EXPECT_EQ(emit(pushes, 40, stream), 43u);
	const std::vector<std::string> expected = {"push 0", "fence 41", "push 1", "push 2", "fence 42", "push 3"};
	EXPECT_EQ(stream.items, expected);
}

TEST(JobFences, AJobWithoutMarksIsTheSingleFenceSubmissionItWasBefore)
{
	const auto pushes = batches({3});
	uint32_t fences = 7;
	ASSERT_EQ(switch_gpu_job_fences(pushes.data(), uint32_t(pushes.size()), &fences), 0);
	EXPECT_EQ(fences, 0u);
	Stream stream;
	EXPECT_EQ(emit(pushes, 0xffffffffu, stream), 0u);
	EXPECT_EQ(stream.items.size(), 3u);
}

TEST(JobFences, SeqnosWrapWithTheChannelsFence)
{
	const auto pushes = batches({1, 1, 1});
	Stream stream;
	EXPECT_EQ(emit(pushes, 0xfffffffeu, stream), 1u);
	const std::vector<std::string> expected = {"push 0", "fence 4294967295", "push 1", "fence 0", "push 2"};
	EXPECT_EQ(stream.items, expected);
}

TEST(JobFences, AFenceNeverSplitsAMethodOrFollowsTheJobsLastPush)
{
	uint32_t fences = 0;
	auto last = batches({2});
	last.back().fence = 1;
	EXPECT_EQ(switch_gpu_job_fences(last.data(), uint32_t(last.size()), &fences), -22);

	auto incomplete = batches({2, 1});
	incomplete[1].incomplete = 1;
	EXPECT_EQ(switch_gpu_job_fences(incomplete.data(), uint32_t(incomplete.size()), &fences), -22);

	std::vector<uint32_t> sizes(TegraGM20BMaxJobFences + 2, 1);
	std::vector<tgm_push> many;
	for (size_t i = 0; i < sizes.size(); ++i)
		many.push_back({0x100000 + i * 0x100, 16, 0, 0, i + 1 < sizes.size(), 0});
	EXPECT_EQ(switch_gpu_job_fences(many.data(), uint32_t(many.size()), &fences), -7);
	many.pop_back();
	many.back().fence = 0;
	ASSERT_EQ(switch_gpu_job_fences(many.data(), uint32_t(many.size()), &fences), 0);
	EXPECT_EQ(fences, uint32_t(TegraGM20BMaxJobFences));
}

// gpu.c's kill (chan_killed) marks every seqno after the completed one up to the last submitted as failed: wherever in
// the job the fault lands, the seqno the job's reply named (its own fence's) fails, and the job before it does not.
TEST(JobFences, AFaultInAnyBatchFailsTheWholeJob)
{
	const auto pushes = batches({2, 1, 3, 1});
	Stream stream;
	const uint32_t submitted = 100, job = emit(pushes, submitted, stream);
	ASSERT_EQ(job, 104u);
	for (uint32_t completed = submitted; completed < job; ++completed) {
		TegraGM20BAbiFence fence = {completed, completed + 1, job, 2};
		EXPECT_EQ(tgm_fence_lost(&fence, job), 1) << completed;
		EXPECT_EQ(tgm_fence_lost(&fence, submitted), 0) << completed;
	}
}
