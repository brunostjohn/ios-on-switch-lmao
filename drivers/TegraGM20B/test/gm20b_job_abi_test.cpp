// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstddef>
#include <string>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"
#include "drivers/TegraGM20B/TegraGM20BReadbackABI.h"
#include "drivers/TegraGM20B/TegraGM20BRegDumpABI.h"

TEST(JobAbi, MethodsFollowTheHashedHeadersOnes)
{
	const uint32_t hashed[] = {TegraGM20BMethodGetInfo, TegraGM20BMethodCpuComplete,
				   TegraGM20BMethodReadback, TegraGM20BMethodRegdump};
	const uint32_t job[] = {TegraGM20BMethodResourceSetCreate, TegraGM20BMethodResourceSetRelease,
				TegraGM20BMethodSubmitJob};
	for (uint32_t method : job) {
		EXPECT_GT(method, uint32_t(TegraGM20BMethodCpuComplete));
		for (uint32_t other : hashed)
			EXPECT_NE(method, other);
	}
}

TEST(JobAbi, SetGenerationsLieInTheFencePagesUnusedWords)
{
	constexpr uint32_t slots = 8 * 16, visibility = 128 + 8, bindEpoch = 256;
	EXPECT_GE(uint32_t(TegraGM20BSetGenerationWord), slots);
	EXPECT_GE(uint32_t(TegraGM20BSetGenerationWord), visibility);
	EXPECT_GT(uint32_t(TegraGM20BSetGenerationWord), bindEpoch);
	EXPECT_LE((TegraGM20BSetGenerationWord + TegraGM20BMaxResourceSets) * 4u, 4096u);
}

TEST(JobAbi, RecordsKeepTheirLayout)
{
	EXPECT_EQ(offsetof(TegraGM20BAbiJob, set), 8u);
	EXPECT_EQ(offsetof(TegraGM20BAbiJob, flags), 20u);
	EXPECT_EQ(sizeof(TegraGM20BAbiJob) % 8, 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiResourceSetCreateReply, generation_offset), 12u);
}

TEST(JobAbi, TheHashedAbiHeaderNeverIncludesIt)
{
	const std::string abi = switch_ios::test::SourceText("../TegraGM20BABI.h");
	ASSERT_FALSE(abi.empty());
	EXPECT_EQ(abi.find("TegraGM20BJobABI.h"), std::string::npos);
}
