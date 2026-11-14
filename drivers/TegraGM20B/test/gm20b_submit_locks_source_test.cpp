// SPDX-License-Identifier: MIT
// SubmitJob's global locks that switch_gpu_submit_cost_test cannot run: its exec's producer channels resolved under one
// tgm_live_lock a pass, every status read before it, and the job's ownership transaction taking no reference of its
// own on its uses (the connection's BOs hold them inside the submit gate).
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
std::string unit(const char *name)
{
	return SourceText(std::string("../") + name);
}
}  // namespace

TEST(GM20BSubmitLocksSource, ProducerChannelsResolveUnderOneLiveLockAPass)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText all = Block(exec, "int tgm_backing_dependencies(");
	EXPECT_EQ(all.Count("mutex_lock(&tgm_live_lock)"), 1u);
	EXPECT_TRUE(all.Before("switch_gpu_backing_token_status(&tokens[i])",
			       "mutex_lock(&tgm_live_lock)"));
	EXPECT_TRUE(all.Before("mutex_lock(&tgm_live_lock)",
			       "backing_dependency(chan, &tokens[i], emit)"));
	const CodeText one = Block(exec, "static int backing_dependency(");
	EXPECT_TRUE(one.Has("live_by_uid(token->queue)"));
	EXPECT_FALSE(one.Has("mutex_lock("));
	EXPECT_FALSE(one.Has("switch_gpu_backing_token_status("));
}

TEST(GM20BSubmitLocksSource, SubmitJobTakesNoReferencePerUse)
{
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Has("switch_gpu_backing_begin_held("));
	EXPECT_FALSE(job.Has("switch_gpu_backing_begin_checked("));
	EXPECT_FALSE(job.Has("switch_gpu_backing_begin_conversion_checked("));
}

// A job's records go up at exec's point of no return: past the last step that can still fail and roll its push back,
// before the dependencies' acquires (tgm_live_lock), the GPFIFO writes and the doorbell, so none of those runs under its
// backings' locks. The emitting pass skips a producer gone since the checking pass, and both submission paths hand exec
// their transaction instead of committing it after the kick (switch_gpu_kick_race_test runs what that buys).
TEST(GM20BSubmitLocksSource, AJobsRecordsGoUpBeforeItsKick)
{
	const std::string exec = unit("gpu_exec.c");
	const CodeText body =
		Block(exec, "static inline __attribute__((always_inline)) int chan_exec_body(");
	EXPECT_EQ(body.Count("chan->chan.gpfifo.cur = ring_cur;"), 1u);
	EXPECT_TRUE(body.AfterLast("chan->chan.gpfifo.cur = ring_cur;")
			    .Has("switch_gpu_backing_commit(owned);"));
	const CodeText published = body.AfterLast("switch_gpu_backing_commit(owned);");
	EXPECT_TRUE(published.StartsWith(
		"tgm_backing_dependencies(chan, dependencies, dependency_count, 1);"));
	EXPECT_TRUE(published.Has("PUSH_KICK(&chan->chan.push);"));
	EXPECT_FALSE(published.Has("return ret;"));
	EXPECT_FALSE(published.Has("return -"));
	EXPECT_TRUE(
		Block(exec, "static int backing_dependency(").Has("return emit ? 0 : -ENODEV;"));
	const CodeText job = Block(unit("TegraGM20BUserClientJobSubmit.cpp"),
				   "IOReturn AGXDeviceUserClient::runJobUses(");
	EXPECT_TRUE(job.Has("jobVisibility, releases, keptCount, &transaction);"));
	EXPECT_FALSE(job.Has("switch_gpu_backing_commit("));
	const CodeText legacy = Block(unit("TegraGM20BUserClientSubmit.cpp"),
				      "IOReturn AGXDeviceUserClient::exec(");
	EXPECT_TRUE(legacy.Has("jobVisibility, nullptr, 0, &transaction);"));
	EXPECT_FALSE(legacy.Has("switch_gpu_backing_commit("));
}
