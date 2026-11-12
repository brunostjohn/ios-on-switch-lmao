// SPDX-License-Identifier: MIT
// A command buffer that counts as scheduled before its work reaches the kernel (held on a shared event,
// raw_scheduled_wait.h) promises the surfaces it writes first, and the promise ends once its writes reach the kernel or
// never will. No host model runs the facade, so these guard where RawMetal makes and ends it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

void expectBefore(const std::string &text, const std::string &first, const std::string &second)
{
    const size_t a = text.find(first), b = text.find(second);
    ASSERT_NE(a, std::string::npos) << first;
    ASSERT_NE(b, std::string::npos) << second;
    EXPECT_LT(a, b) << first << " before " << second;
}

TEST(RawWriterPromiseSource, AHeldCommandBufferPromisesItsWritesBeforeItsWaitReturns)
{
    const std::string schedule = SourceText("../raw/runtime/RawMetalSchedule.mm");
    const std::string commit = Between(schedule, "void commitBuffer(RMCommandBuffer *cb)", "\n}\n");
    expectBefore(commit, "r.waiting.commit(", "written = writtenSurfaces(cb);");
    expectBefore(commit, "written = writtenSurfaces(cb);", "RAW_DIAGNOSTIC(r.stats.deferred++);");
    expectBefore(commit, "promiseWrites(cb, written);", "cb->_held = true;");
    const std::string mark = Between(schedule, "static void markHeld(", "\n}\n");
    expectBefore(mark, "promiseWrites(cb, writtenSurfaces(cb));", "cb->_held = held;");
    const std::string promise = Between(schedule, "static void promiseWrites(", "\n}\n");
    EXPECT_THAT(promise, HasSubstr("gRM->raw->endPromise(cookie);"));  // unheld meanwhile: ended at once
    EXPECT_THAT(Between(schedule, "void endWriterPromise(", "\n}\n"), HasSubstr("exchange(kPromiseEnded"));

    const std::string events = SourceText("../raw/runtime/RawMetalEvents.mm");
    EXPECT_THAT(Between(events, "void unhold(RMCommandBuffer *cb)\n{", "\n}\n"), HasSubstr("endWriterPromise(cb);"));
    const std::string finish = SourceText("../raw/runtime/RawMetalFinish.mm");
    EXPECT_THAT(Between(finish, "void scheduled(RMCommandBuffer *cb)\n{", "\n}\n"), HasSubstr("unhold(cb);"));
    EXPECT_THAT(Between(finish, "void finish(RMCommandBuffer *cb, NSError *error)\n{", "\n}\n"),
                HasSubstr("unhold(cb);"));
}

TEST(RawWriterPromiseSource, TheKernelCallsAreMethod34)
{
    const std::string kernel = SourceText("../raw/runtime/raw_kernel.cpp");
    const std::string begin = Between(kernel, "uint64_t Kernel::promiseWrites(", "\n}\n");
    EXPECT_THAT(begin, HasSubstr("TegraGM20BPromiseBegin"));
    EXPECT_THAT(begin, HasSubstr("call(TegraGM20BMethodWriterPromise,"));
    EXPECT_THAT(begin, HasSubstr("uses[i] = {handles[i], TegraGM20BSurfaceWrite, 0};"));
    EXPECT_THAT(Between(kernel, "void Kernel::endPromise(", "\n}\n"), HasSubstr("TegraGM20BPromiseEnd"));
}

}  // namespace
