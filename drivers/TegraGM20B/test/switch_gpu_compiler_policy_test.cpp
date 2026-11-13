// SPDX-License-Identifier: MIT
// SwitchGpuShaderCompilerPolicy.h: when the shader compiler broker stops waiting for a compiler, so that no GPU client
// sleeps on a request no compiler will ever take: a decline stands, a compiler nothing restarts is a decline, and a
// restarted one is waited for until its third loss, registered or not.
#include "SwitchGpuShaderCompilerPolicy.h"

#include <gtest/gtest.h>

namespace
{

using switch_gpu_compiler::declinedAfterClose;
using switch_gpu_compiler::kLosses;

TEST(CompilerPolicy, ADeclineStandsWhenTheDeclinerCloses)
{
	uint32_t losses = 0;
	EXPECT_TRUE(declinedAfterClose(true, true, losses));
	EXPECT_EQ(losses, 0u);
}

TEST(CompilerPolicy, ACompilerNothingStartsAgainIsADecline)
{
	uint32_t losses = 0;
	EXPECT_TRUE(declinedAfterClose(false, false, losses));
}

TEST(CompilerPolicy, ARestartedCompilerIsWaitedForUntilItsThirdLoss)
{
	uint32_t losses = 0;
	for (uint32_t i = 1; i < kLosses; ++i)
		EXPECT_FALSE(declinedAfterClose(false, true, losses)) << i;
	EXPECT_TRUE(declinedAfterClose(false, true, losses));
	EXPECT_EQ(losses, kLosses);
}

}  // namespace
