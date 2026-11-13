// SwitchGpuSpares.h: why a cached BoAlloc missed its zeroed spare, and how deep its class is kept warm afterwards.
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/SwitchGpuSpares.h"

namespace
{

TEST(Spares, EachMissHasItsReason)
{
	EXPECT_EQ(switch_gpu_spare_miss_of(0, 0, false), SWITCH_GPU_SPARE_FIRST);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 1, false), SWITCH_GPU_SPARE_ZEROING);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 0, false), SWITCH_GPU_SPARE_DRAINED);
	EXPECT_EQ(switch_gpu_spare_miss_of(2, 1, true), SWITCH_GPU_SPARE_PRESSURE);
}

TEST(Spares, AWarmClassDoublesItsDepthAtEachMissUpToTheMaximum)
{
	uint16_t depth = 0;
	const uint16_t expected[] = {1, 2, 4, 8, 8};
	for (const uint16_t want : expected) {
		depth = switch_gpu_spare_depth_after_miss(
			depth, true, switch_gpu_spare_miss_of(depth, 0, false));
		EXPECT_EQ(depth, want);
	}
}

TEST(Spares, PressureIneligibleAndUnlearnableClassesKeepTheirDepth)
{
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(2, true, SWITCH_GPU_SPARE_PRESSURE), 2);
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(0, true, SWITCH_GPU_SPARE_INELIGIBLE), 0);
	EXPECT_EQ(switch_gpu_spare_depth_after_miss(0, false, SWITCH_GPU_SPARE_FIRST), 0);
}

// A burst of eight BoAllocs of one class, its spares refilled to depth between bursts: the second burst is served whole.
// At the old fixed depth of 1 it zeroed seven of them on the allocating thread again.
TEST(Spares, ARepeatedBurstIsServedFromSpares)
{
	uint16_t depth = 0, ready = 0;
	unsigned missed[2] = {};
	for (unsigned burst = 0; burst < 2; ++burst) {
		for (int i = 0; i < 8; ++i) {
			if (ready) {
				--ready;
				continue;
			}
			++missed[burst];
			depth = switch_gpu_spare_depth_after_miss(
				depth, true, switch_gpu_spare_miss_of(depth, 0, false));
		}
		ready = depth;
	}
	EXPECT_EQ(missed[0], 8u);
	EXPECT_EQ(missed[1], 0u);
}

// Boot 21 14:21:09: "System will begin reaping long-idle processes. memorystatus_available_pages: 48999", with the
// pressure level normal and 95888 KiB of zeroed spares kept wired; the reaper then killed about 200 idle processes
TEST(Spares, GoBackWithinACapOfTheReapersThreshold)
{
	constexpr uint64_t kReaper = 49000, kCap = (96ULL << 20) / 16384;
	EXPECT_TRUE(switch_gpu_spares_give_back(0, 48999, kReaper, kCap));
	EXPECT_TRUE(switch_gpu_spares_give_back(0, kReaper + kCap - 1, kReaper, kCap));
	EXPECT_FALSE(switch_gpu_spares_give_back(0, kReaper + kCap, kReaper, kCap));
}

TEST(Spares, GoBackAtAnyRaisedPressureLevel)
{
	EXPECT_TRUE(switch_gpu_spares_give_back(1, 1u << 30, 0, 0));
	EXPECT_TRUE(switch_gpu_spares_give_back(4, 1u << 30, 49000, 6144));
}

TEST(Spares, WithoutAReaperOnlyThePressureLevelCounts)
{
	EXPECT_FALSE(switch_gpu_spares_give_back(0, 0, 0, 6144));
}

}  // namespace
