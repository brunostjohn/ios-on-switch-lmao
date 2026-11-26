// SwitchGpuSpareRecord.h: one boot's demand recorded, read back at the next boot, and the seed it gives.
#include <gtest/gtest.h>

#include <vector>

#include "drivers/TegraGM20B/SwitchGpuSpareRecord.h"

namespace
{
constexpr uint64_t kCap = SWITCH_GPU_SPARE_CAP_BYTES, kMiB = 1ULL << 20;

uint32_t slotOf(uint64_t bytes, switch_gpu_spare_kind kind)
{
	return switch_gpu_spare_slot(kind, switch_gpu_spare_class_up(bytes));
}

std::vector<uint8_t> encode(const switch_gpu_spare_books &b)
{
	std::vector<uint8_t> out(SWITCH_GPU_SPARE_RECORD_BYTES);
	out.resize(switch_gpu_spare_record_encode(b, out.data()));
	return out;
}

TEST(SpareRecord, ABootsAskedDepthsSeedTheNextBoot)
{
	switch_gpu_spare_books boot{};
	switch_gpu_spare_books_init(boot);
	const uint32_t wc = slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC);
	switch_gpu_spare_books_asked(boot, wc, SWITCH_GPU_SPARE_NONE);
	switch_gpu_spare_books_learn(boot, wc, SWITCH_GPU_SPARE_FIRST, 32 * kMiB, kCap);
	const std::vector<uint8_t> record = encode(boot);
	ASSERT_EQ(record.size(), SWITCH_GPU_SPARE_RECORD_HEADER + SWITCH_GPU_SPARE_RECORD_ENTRY);

	switch_gpu_spare_books next{};
	switch_gpu_spare_books_init(next);
	EXPECT_EQ(switch_gpu_spare_record_seed(next, record.data(), uint32_t(record.size()), kCap),
		  1u);
	EXPECT_EQ(next.depth[wc], 1);
	EXPECT_EQ(next.used[wc], SWITCH_GPU_SPARE_SEEDED);
	EXPECT_EQ(switch_gpu_spare_books_refill(next, kCap), wc)
		<< "a seeded class refills before the warm classes nobody has asked for";
}

// A seed this boot never asked for again is not recorded: depths do not grow from boot to boot on their own
TEST(SpareRecord, ASeedNoBootAsksForEndsAfterOneBoot)
{
	switch_gpu_spare_books b{};
	switch_gpu_spare_books_init(b);
	switch_gpu_spare_books_seed(b, slotOf(16 * kMiB, SWITCH_GPU_SPARE_WC), 1, kCap);
	EXPECT_EQ(encode(b).size(), SWITCH_GPU_SPARE_RECORD_HEADER);
	const uint32_t asked = slotOf(20 * kMiB, SWITCH_GPU_SPARE_WC);
	switch_gpu_spare_books_seed(b, asked, 1, kCap);
	switch_gpu_spare_books_asked(b, asked, asked);
	const std::vector<uint8_t> record = encode(b);
	ASSERT_EQ(record.size(), SWITCH_GPU_SPARE_RECORD_HEADER + SWITCH_GPU_SPARE_RECORD_ENTRY);
	EXPECT_EQ(record[1], 1);
	EXPECT_EQ(record[3], 1) << "the seeded depth, unchanged by a hit";
}

// A record of another version, length, kind or class size seeds nothing it cannot name
TEST(SpareRecord, ARecordOfAnotherBuildSeedsNothingWrong)
{
	switch_gpu_spare_books b{};
	switch_gpu_spare_books_init(b);
	const uint32_t wc = slotOf(24 * kMiB, SWITCH_GPU_SPARE_WC);
	switch_gpu_spare_books_asked(b, wc, SWITCH_GPU_SPARE_NONE);
	switch_gpu_spare_books_learn(b, wc, SWITCH_GPU_SPARE_FIRST, 32 * kMiB, kCap);
	std::vector<uint8_t> record = encode(b);
	const auto seeds = [](std::vector<uint8_t> r) {
		switch_gpu_spare_books n{};
		switch_gpu_spare_books_init(n);
		return switch_gpu_spare_record_seed(n, r.data(), uint32_t(r.size()), kCap);
	};
	EXPECT_EQ(seeds(record), 1u);
	std::vector<uint8_t> other = record;
	other[0] = SWITCH_GPU_SPARE_RECORD_VERSION + 1;
	EXPECT_EQ(seeds(other), 0u);
	other = record;
	other.pop_back();
	EXPECT_EQ(seeds(other), 0u);
	other = record;
	other[SWITCH_GPU_SPARE_RECORD_HEADER] = SWITCH_GPU_SPARE_KINDS;
	EXPECT_EQ(seeds(other), 0u);
	other = record;
	other[SWITCH_GPU_SPARE_RECORD_HEADER + 2] ^= 1;  // a class size this build does not have
	EXPECT_EQ(seeds(other), 0u);
	other = record;
	other[SWITCH_GPU_SPARE_RECORD_HEADER + 1] = SWITCH_GPU_SPARE_DEPTH_MAX + 1;
	EXPECT_EQ(seeds(other), 0u);
	EXPECT_EQ(seeds({}), 0u);
}

TEST(SpareRecord, EverySlotFitsTheRecordsFormat)
{
	static_assert(SWITCH_GPU_SPARE_KINDS <= 256 && SWITCH_GPU_SPARE_DEPTH_MAX <= 255);
	static_assert(SWITCH_GPU_SPARE_RECORD_ENTRIES <= 255);
	EXPECT_LE(switch_gpu_spare_class_bytes(SWITCH_GPU_SPARE_CLASSES - 1) >> 10, UINT32_MAX);
}
}  // namespace
