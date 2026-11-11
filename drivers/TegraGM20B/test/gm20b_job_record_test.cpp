// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuJobRecord.h"

namespace
{
struct Record {
	std::vector<uint64_t> words;
	size_t bytes = 0;

	Record(TegraGM20BAbiJob head, uint32_t waits = 0, uint32_t signals = 0)
	{
		bytes = sizeof(head) + head.push_count * sizeof(TegraGM20BAbiPush) +
			head.shared_count * sizeof(TegraGM20BAbiSurfaceUse);
		if (head.flags & TegraGM20BJobHasEvents)
			bytes += sizeof(TegraGM20BAbiSubmitEvents) + (waits + signals) * sizeof(TegraGM20BAbiEventUse);
		words.assign((bytes + 7) / 8, 0);
		memcpy(words.data(), &head, sizeof(head));
		if (head.flags & TegraGM20BJobHasEvents) {
			auto *base = reinterpret_cast<uint8_t *>(words.data());
			auto *events = reinterpret_cast<TegraGM20BAbiSubmitEvents *>(
				base + bytes - sizeof(TegraGM20BAbiSubmitEvents) -
				(waits + signals) * sizeof(TegraGM20BAbiEventUse));
			*events = {waits, signals, 0};
			auto *uses = reinterpret_cast<TegraGM20BAbiEventUse *>(events + 1);
			for (uint32_t i = 0; i < waits + signals; ++i)
				uses[i] = {i + 1, 1};
		}
	}

	SwitchGpuJobRecord parse(size_t size = 0)
	{
		return SwitchGpuJobRecord::parse(words.data(), size ? size : bytes);
	}
};
}  // namespace

TEST(JobRecord, PartsFollowTheHeader)
{
	Record record({1, 2, 3, 7, 4, TegraGM20BJobExternal | TegraGM20BJobHasEvents}, 1, 2);
	const auto parsed = record.parse();
	ASSERT_TRUE(parsed.valid());
	EXPECT_EQ(reinterpret_cast<uint8_t *>(parsed.pushes) - reinterpret_cast<uint8_t *>(parsed.job), 24);
	EXPECT_EQ(reinterpret_cast<const uint8_t *>(parsed.uses) - reinterpret_cast<uint8_t *>(parsed.job), 24 + 32);
	ASSERT_NE(parsed.events, nullptr);
	EXPECT_EQ(parsed.events->signal_count, 2u);
	EXPECT_EQ(Record({1, 0, 0, 0, 0, 0}).parse().events, nullptr);
}

TEST(JobRecord, MalformedRecordsAreRefused)
{
	EXPECT_FALSE(Record({0, 0, 0, 0, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 0, 0, 0, 1u << 7}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 1, 0, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, 0, 3, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, 0, TegraGM20BMaxResourceSets + 1, 1, 0, 0}).parse().valid());
	EXPECT_FALSE(Record({1, TegraGM20BMaxJobPushes + 1, 0, 0, 0, 0}).parse().valid());
	Record ok({1, 1, 0, 0, 1, 0});
	EXPECT_TRUE(ok.parse().valid());
	EXPECT_FALSE(ok.parse(ok.bytes - 8).valid());
	EXPECT_FALSE(Record({1, 0, 0, 0, 0, TegraGM20BJobHasEvents}, TegraGM20BMaxJobEvents, 1).parse().valid());
	Record events({1, 0, 0, 0, 0, TegraGM20BJobHasEvents}, 1, 0);
	EXPECT_TRUE(events.parse().valid());
	EXPECT_FALSE(events.parse(events.bytes - sizeof(TegraGM20BAbiEventUse)).valid());
	EXPECT_LE(SwitchGpuJobRecord::kMaxBytes, size_t(1) << 16);
}
