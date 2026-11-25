// SwitchMotionFusion.h on the host: its float bit patterns against the host's floats, the attitude it seeds and
// integrates against closed-form rotations, its gyroscope bias estimate, and the device motion report's layout in both
// builds' lengths.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>

#include "drivers/SwitchMotion/SwitchMotionFusion.h"

namespace {
float FromBits(uint32_t bits)
{
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t Bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float PayloadFloat(const uint8_t *payload, size_t at)
{
    uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        bits |= static_cast<uint32_t>(payload[at + i]) << (8 * i);
    return FromBits(bits);
}

void Gravity(const SwitchMotionFusion &fusion, double out[3])
{
    int64_t g[3];
    fusion.gravity(g);
    for (unsigned i = 0; i < 3; ++i)
        out[i] = static_cast<double>(g[i]) / static_cast<double>(kSwitchOne);
}

// Feeds `seconds` of samples at 200 Hz: a fixed accelerometer and gyroscope reading.
void Feed(SwitchMotionFusion &fusion, const int32_t accel[3], const int32_t gyro[3], double seconds)
{
    const unsigned samples = static_cast<unsigned>(seconds * 200 + 0.5);
    for (unsigned i = 0; i < samples; ++i) {
        fusion.addAccel(accel);
        fusion.addGyro(gyro, 5000);
    }
}
}  // namespace

TEST(SwitchMotionFusion, FloatBitsMatchTheHostsRounding)
{
    EXPECT_EQ(switchFloatBits(0, 30), 0u);
    EXPECT_EQ(switchFloatBits(kSwitchOne, 30), Bits(1.0f));
    EXPECT_EQ(switchFloatBits(-kSwitchOne / 2, 30), Bits(-0.5f));
    EXPECT_EQ(switchMicroFloatBits(-1000000), Bits(-1.0f));
    std::mt19937_64 random(42);
    for (int i = 0; i < 100000; ++i) {
        const int64_t value = static_cast<int64_t>(random() >> 2) >> (random() % 60);
        const int64_t signedValue = (random() & 1) ? -value : value;
        EXPECT_EQ(switchFloatBits(signedValue, 24),
                  Bits(static_cast<float>(static_cast<double>(signedValue) / 16777216.0)))
            << signedValue;
    }
}

TEST(SwitchMotionFusion, SeedsTheTiltFromTheFirstAccelerometerSample)
{
    const int32_t cases[][3] = {
        {0, 0, -1000000}, {0, -1000000, 0},           {1000000, 0, 0},
        {0, 0, 1000000},  {300000, -400000, -866025}, {1, 0, 999999},
    };
    for (const auto &accel : cases) {
        SwitchMotionFusion fusion;
        fusion.addAccel(accel);
        ASSERT_TRUE(fusion.seeded);
        double g[3];
        Gravity(fusion, g);
        const double n = std::sqrt(static_cast<double>(accel[0]) * accel[0] + static_cast<double>(accel[1]) * accel[1] +
                                   static_cast<double>(accel[2]) * accel[2]);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(g[i], accel[i] / n, 2e-6) << accel[0] << " " << accel[1] << " " << accel[2];
    }
    SwitchMotionFusion faceUp;
    const int32_t down[3] = {0, 0, -1000000};
    faceUp.addAccel(down);
    EXPECT_EQ(faceUp.q[0], kSwitchOne) << "face up is the reference attitude";
}

TEST(SwitchMotionFusion, IntegratesARotationAboutTheVertical)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t spin[3] = {0, 0, 1000000};  // 1 rad/s about +Z
    Feed(fusion, down, spin, 1.0);
    const double half = 0.5;
    EXPECT_NEAR(static_cast<double>(fusion.q[0]) / kSwitchOne, std::cos(half), 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[3]) / kSwitchOne, std::sin(half), 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[1]) / kSwitchOne, 0, 1e-4);
    EXPECT_NEAR(static_cast<double>(fusion.q[2]) / kSwitchOne, 0, 1e-4);
}

TEST(SwitchMotionFusion, TiltingUpByAQuarterTurnAboutXIsUprightPortrait)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    fusion.addAccel(down);
    const int32_t shaken[3] = {0, 0, -2000000};  // 2 g: no correction, the gyroscope alone
    const int32_t tilt[3] = {1570796, 0, 0};     // pi/2 rad over one second
    Feed(fusion, shaken, tilt, 1.0);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[0], 0, 1e-3);
    EXPECT_NEAR(g[1], -1, 1e-3);
    EXPECT_NEAR(g[2], 0, 1e-3);
}

TEST(SwitchMotionFusion, GravityPullsADriftedAttitudeBack)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    fusion.addAccel(down);
    const int32_t shaken[3] = {0, 0, -2000000};
    const int32_t tilt[3] = {349066, 0, 0};  // 20 degrees of drift
    Feed(fusion, shaken, tilt, 1.0);
    const int32_t still[3] = {0, 0, 0};
    Feed(fusion, down, still, 5.0);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[2], -1, 1e-4);
    EXPECT_NEAR(g[1], 0, 1e-2);
}

TEST(SwitchMotionFusion, EstimatesAConstantGyroscopeBias)
{
    SwitchMotionFusion fusion;
    const int32_t down[3] = {0, 0, -1000000};
    const int32_t biased[3] = {10000, -5000, 0};  // 0.01 and -0.005 rad/s at rest
    Feed(fusion, down, biased, 240.0);            // the bias settles with a time constant of Kp / Ki = 50 s
    EXPECT_NEAR(fusion.biasNanoRad[0] / 1e9, 0.01, 0.001);
    EXPECT_NEAR(fusion.biasNanoRad[1] / 1e9, -0.005, 0.001);
    double g[3];
    Gravity(fusion, g);
    EXPECT_NEAR(g[2], -1, 1e-4) << "the bias no longer tilts the attitude";
    int64_t rate[3];
    fusion.rotationRate(biased, rate);
    EXPECT_LT(std::abs(rate[0]), 1000) << "rotation rate reported without the bias";
}

TEST(SwitchMotionFusion, ReportLayoutInBothBuildsLengths)
{
    SwitchMotionFusion fusion;
    const int32_t accel[3] = {0, -1000000, 0};
    fusion.addAccel(accel);
    const int32_t gyro[3] = {250000, 0, -125000};
    for (const size_t size : {kSwitchDeviceMotion21H461, kSwitchDeviceMotion24B119}) {
        uint8_t payload[kSwitchDeviceMotion24B119];
        std::memset(payload, 0xaa, sizeof(payload));
        switchDeviceMotionPayload(fusion, gyro, payload, size);
        EXPECT_EQ(payload[0], 3);
        EXPECT_EQ(payload[1] | payload[2] << 8, 0x100);
        for (unsigned i = 3; i < 12; ++i)
            EXPECT_EQ(payload[i], 0) << i;
        const double s = std::sqrt(0.5);
        EXPECT_NEAR(PayloadFloat(payload, 12), s, 1e-6);  // x
        EXPECT_NEAR(PayloadFloat(payload, 16), 0, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 20), 0, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 24), s, 1e-6);  // w
        EXPECT_NEAR(PayloadFloat(payload, 28), 0.25, 1e-6);
        EXPECT_NEAR(PayloadFloat(payload, 36), -0.125, 1e-6);
        for (unsigned i = 0; i < 3; ++i)
            EXPECT_NEAR(PayloadFloat(payload, 40 + 4 * i), 0, 1e-5);
        if (size == kSwitchDeviceMotion24B119) {
            EXPECT_EQ(PayloadFloat(payload, 52), -1.0f);
            EXPECT_EQ(PayloadFloat(payload, 56), 0.0f);
        } else {
            EXPECT_EQ(payload[52], 0xaa) << "nothing past 21H461's 52 bytes";
        }
    }
    SwitchMotionFusion unseeded;
    uint8_t payload[kSwitchDeviceMotion24B119];
    switchDeviceMotionPayload(unseeded, gyro, payload, sizeof(payload));
    EXPECT_EQ(payload[1] | payload[2] << 8, 0) << "not initialized before the first accelerometer sample";
}
