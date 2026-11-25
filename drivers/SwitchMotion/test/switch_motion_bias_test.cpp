// SwitchMotionFusion.h's gyroscope bias learning on the host: offsets at rest are learned, and slow steady turns and
// tilts, which pass any stillness test a 6-axis part can make, are not.
#include <gtest/gtest.h>

#include <cmath>

#include "drivers/SwitchMotion/test/fusion_sim.h"

namespace {
double Bias(const SwitchMotionFusion &fusion, unsigned axis)
{
    return static_cast<double>(fusion.biasNanoRad[axis]) / 1e9;
}

constexpr double kSt = 12 * M_PI / 180;  // LSM6DS3H: 10 dps typical, plus drift
}  // namespace

TEST(SwitchMotionBias, ASlowSteadyTurnAboutTheVerticalIsNotLearned)
{
    for (const double seconds : {2.0, 10.0}) {
        SwitchMotionFusion fusion;
        double down[3] = {0, 0, -1};
        const double turn[3] = {0, 0, 0.05};
        const double none[3] = {};
        SimTurn(fusion, down, turn, none, seconds);
        EXPECT_LT(std::fabs(Bias(fusion, 2)), 0.002) << seconds << " s at 0.05 rad/s";
    }
}

TEST(SwitchMotionBias, ASlowTiltIsNotLearned)
{
    SwitchMotionFusion fusion;
    fusion.zeroRateNanoRad = static_cast<int64_t>(kSt * 1e9);
    double down[3] = {0, 0, -1};
    const double tilt[3] = {0.05, 0, 0};
    const double none[3] = {};
    SimTurn(fusion, down, tilt, none, 2.0);
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_LT(std::fabs(Bias(fusion, i)), 0.002) << i;
}

// Once the console has rested, a steady turn is told apart from the offset even on a part whose offset bound is far
// above the turn's rate.
TEST(SwitchMotionBias, ASlowTurnAfterRestingLeavesTheLearnedOffset)
{
    for (const unsigned axis : {0u, 2u}) {
        SwitchMotionFusion fusion;
        fusion.zeroRateNanoRad = static_cast<int64_t>(kSt * 1e9);
        double down[3] = {0, 0, -1};
        const double offset[3] = {0.002, -0.001, 0.003};
        SimRest(fusion, down, offset, 3.0);
        ASSERT_TRUE(fusion.biasKnown);
        double turn[3] = {};
        turn[axis] = 0.05;
        SimTurn(fusion, down, turn, offset, 10.0);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(Bias(fusion, i), offset[i], 0.0005) << "turn about " << axis << ", axis " << i;
    }
}

TEST(SwitchMotionBias, AnOffsetAtRestIsLearnedWithinTheFirstSecond)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0.01, -0.005, 0.02};
    SimRest(fusion, down, offset, 1.1);
    ASSERT_TRUE(fusion.biasKnown);
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_NEAR(Bias(fusion, i), offset[i], 0.0002) << i;
}

TEST(SwitchMotionBias, AnOffsetAsLargeAsThePartsBoundIsLearned)
{
    SwitchMotionFusion fusion;
    fusion.zeroRateNanoRad = static_cast<int64_t>(kSt * 1e9);
    double down[3] = {0, 0, -1};
    const double offset[3] = {0, 0, 0.15};  // 8.6 dps
    SimRest(fusion, down, offset, 3.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.15, 0.0002);
}

TEST(SwitchMotionBias, AnOffsetPastThePartsBoundIsLeftToTheGravityCorrection)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double offset[3] = {0, 0, 0.05};
    SimRest(fusion, down, offset, 5.0);
    EXPECT_FALSE(fusion.biasKnown);
    EXPECT_EQ(fusion.biasNanoRad[2], 0);
}

TEST(SwitchMotionBias, ADriftingOffsetIsFollowed)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double cold[3] = {0, 0, 0.003};
    SimRest(fusion, down, cold, 2.0);
    const double warm[3] = {0, 0, 0.007};
    SimRest(fusion, down, warm, 6.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.007, 0.0002);
}

TEST(SwitchMotionBias, AChangedOffsetReplacesTheLearnedOneOnlyAfterLongStillness)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double before[3] = {0, 0, 0.003};
    SimRest(fusion, down, before, 2.0);
    const double after[3] = {0, 0, 0.02};
    SimRest(fusion, down, after, 10.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.003, 0.0002) << "10 s that disagree: kept";
    SimRest(fusion, down, after, 15.0);
    EXPECT_NEAR(Bias(fusion, 2), 0.02, 0.0002) << "past 20 s in a row: replaced";
}

TEST(SwitchMotionBias, MovementBetweenRestsDoesNotAddUpToAReplacement)
{
    SwitchMotionFusion fusion;
    double down[3] = {0, 0, -1};
    const double before[3] = {0, 0, 0.003};
    SimRest(fusion, down, before, 2.0);
    const double after[3] = {0, 0, 0.02};
    const double shake[3] = {0.5, 0, 0};
    const double back[3] = {-0.5, 0, 0};
    for (int i = 0; i < 6; ++i) {
        SimRest(fusion, down, after, 5.0);
        SimTurn(fusion, down, shake, after, 0.1);
        SimTurn(fusion, down, back, after, 0.1);  // face up again: the gravity correction still cannot see Z
    }
    EXPECT_NEAR(Bias(fusion, 2), 0.003, 0.0002);
}

TEST(SwitchMotionBias, GapsBetweenGyroscopeSamples)
{
    EXPECT_EQ(switchGyroGapMicros(0, 5000), 5000u) << "the first sample of a session";
    EXPECT_EQ(switchGyroGapMicros(5200, 5000), 5200u);
    EXPECT_EQ(switchGyroGapMicros(15000, 5000), 15000u) << "two samples the part overwrote";
    EXPECT_EQ(switchGyroGapMicros(100, 5000), 2500u);
    EXPECT_EQ(switchGyroGapMicros(1000000, 5000), 20000u);
}

TEST(SwitchMotionBias, ARotationIsIntegratedOverTheRealGaps)
{
    SwitchMotionFusion fusion;
    const int32_t shaken[3] = {0, 0, -2000000};  // 2 g: no gravity correction
    const int32_t faceUp[3] = {0, 0, -1000000};
    fusion.addAccel(faceUp);
    fusion.addAccel(shaken);
    const int32_t spin[3] = {0, 0, 1000000};
    for (int i = 0; i < 100; ++i)
        fusion.addGyro(spin, i % 2 ? 2500 : 7500);  // the same second at uneven gaps
    const double yaw = 2 * std::atan2(static_cast<double>(fusion.q[3]), static_cast<double>(fusion.q[0]));
    EXPECT_NEAR(yaw, 0.5, 1e-4);
}
