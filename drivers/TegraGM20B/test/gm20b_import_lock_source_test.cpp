// SPDX-License-Identifier: MIT
// BoImport wires the task's pages (and takes their zero-fill faults), walks them, makes nvkm's memory, registers and
// pins the surface and cleans it before the connection lock and the submit gate; only the table insert runs under them,
// so a SubmitJob of the connection never parks in enterSlow behind an import's wiring. No host model runs the user
// client's methods, so this guards where each step is.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;

TEST(GM20BImportLockSource, TheLockedPartOfAnImportOnlyInsertsIt)
{
	const std::string run = Body(SourceText("../TegraGM20BUserClientDispatch.cpp"),
				     "IOReturn AGXDeviceUserClient::run(");
	const std::string bo = SourceText("../TegraGM20BUserClientBo.cpp");
	const std::string importCase = Between(run.substr(run.find("const bool mutation")),
					       "case TegraGM20BMethodBoImport:", "break;");
	const std::string locked = Body(
		bo, "IOReturn AGXDeviceUserClient::" + Between(importCase, "ret = ", "(") + "(");
	ASSERT_FALSE(locked.empty());
	size_t steps = 0;
	for (const char *step :
	     {"->prepare()", "collectPages(", "tgm_mem_new_waited(", "tgm_cpu_clean_pages(",
	      "tgm_coherency_add(", "switch_gpu_pin_memory("})
		steps += Count(locked, step);
	// base 6: every one of them under lock_ with the submit gate closed
	EXPECT_EQ(steps, 0u);
}

TEST(GM20BImportLockSource, AnImportIsPreparedBeforeTheConnectionLockAndReleasedAfterIt)
{
	const std::string run = Body(SourceText("../TegraGM20BUserClientDispatch.cpp"),
				     "IOReturn AGXDeviceUserClient::run(");
	const size_t prepare = run.find("boImportPrepare("),
		     locked = run.rfind("IOLockLock(lock_);",
					run.find("submitGate_.beginMutation(lock_)"));
	ASSERT_NE(prepare, std::string::npos);
	EXPECT_LT(prepare, locked);
	EXPECT_GT(run.rfind("releaseBo(imported);"), run.rfind("IOLockUnlock(lock_);"));
}

// A cached BoAlloc's backing (a ready spare, or one made with its zero-fill and clean) is taken before the connection
// lock; under it the BO only takes what was made.
TEST(GM20BImportLockSource, ACachedBoAllocMakesItsBackingBeforeTheConnectionLock)
{
	const std::string run = Body(SourceText("../TegraGM20BUserClientDispatch.cpp"),
				     "IOReturn AGXDeviceUserClient::run(");
	const size_t made = run.find("spareTakeOrMake("),
		     locked = run.rfind("IOLockLock(lock_);",
					run.find("submitGate_.beginMutation(lock_)"));
	// base: no backing is made before lock_; the spare or the new buffer comes inside boAlloc, under it
	ASSERT_NE(made, std::string::npos);
	EXPECT_LT(made, locked);
	EXPECT_GT(run.rfind("spareReturn(backing);"), run.rfind("IOLockUnlock(lock_);"));
	const std::string alloc = Body(SourceText("../TegraGM20BUserClientBo.cpp"),
				       "IOReturn AGXDeviceUserClient::boAlloc(");
	EXPECT_NE(alloc.find("spare = taken ? taken : spareTake(in->bytes);"), std::string::npos);
}
