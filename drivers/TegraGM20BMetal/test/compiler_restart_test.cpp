// SPDX-License-Identifier: MIT
// again or the kernel stopped waiting; a transient one exits 1 for its launcher. A compile context that did not open fails
// that compile alone (Failed), and the next compile opens afresh.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../compiler/compiler_restart.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// A context that did not open fails its compile alone: no count, no latch, the next compile opens afresh
TEST(CompilerRestart, EachCompileOpensAContextThatDidNotOpenAfresh)
{
    const std::string engine = SourceText("../compiler/compiler_engine.mm");
    const std::string open = Body(engine, "bool Engine::openCompiler(std::string &why)");
    EXPECT_THAT(open, HasSubstr("if (s.direct)\n        return true;"));
    EXPECT_THAT(open, HasSubstr("native_bridge_metal_compile_open(s.inputs.nvk.c_str())"));
    EXPECT_THAT(open, Not(HasSubstr("opens")));
    EXPECT_THAT(open, Not(HasSubstr("openRefusal")));
    const std::string compile = Body(engine, "int32_t Engine::compile(const Prepared &p,");
    EXPECT_THAT(compile, HasSubstr("if (!openCompiler(why))\n        return TegraGM20BCompileFailed;"));
}

// An exit for a fresh compiler would register again, zero the kernel's losses and could loop for the boot
TEST(CompilerRestart, ACompilerWhoseOpensAreSpentKeepsServingItsStore)
{
    const std::string run = Body(SourceText("../compiler/compiler_service.mm"), "int32_t Service::run(Work &work)");
    EXPECT_THAT(run, HasSubstr("engine_.compile(p, entry, why)"));
    EXPECT_THAT(run, Not(HasSubstr("exit(")));
}

}  // namespace
