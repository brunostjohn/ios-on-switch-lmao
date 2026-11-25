// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device motion fused in SwitchMotion, as a real iPad's motion coprocessor delivers it: attitude, user acceleration and
// bias-corrected rotation rate in one report per sample (SwitchMotion.cpp file comment; SwitchHID.h has the report).
//
// Kexts build with -mgeneral-regs-only, so everything here is integer: the attitude is a Q30 quaternion and the
// report's floats are written as IEEE-754 bit patterns. Inputs are SwitchMotion's device-frame samples: acceleration as
// the gravity vector in micro-g (lying face up reads z = -1 g), angular velocity in micro-rad/s.
//
// The quaternion rotates device-frame vectors into CoreMotion's reference frame (Z up), so the gravity it implies in the
// device frame is -(2(xz - wy), 2(yz + wx), w^2 - x^2 - y^2 + z^2): identity lying face up, (0, -1, 0) held upright in
// portrait. The filter is Mahony's: the measured gravity direction pulls the attitude through a proportional gain and a
// gyroscope bias integral, only while the acceleration is close to 1 g. Gravity says nothing about rotation about the
// vertical, so the bias is also learned on all three axes whenever the console is still (the gyroscope steady near
// its running mean, the acceleration steady at 1 g), and it outlives a session: restart() resets only the attitude.
// The first accelerometer sample of a session seeds the tilt (yaw 0), as CoreMotion's xArbitraryZVertical frame
// allows.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr int kSwitchQ = 30;
constexpr int64_t kSwitchOne = int64_t{1} << kSwitchQ;

inline uint64_t switchIsqrt(uint64_t n)
{
    uint64_t root = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

// value / 2^fraction as an IEEE-754 single, rounded to nearest (ties to even).
inline uint32_t switchFloatBits(int64_t value, int fraction)
{
    if (value == 0)
        return 0;
    const uint32_t sign = value < 0 ? 0x80000000u : 0;
    uint64_t magnitude = value < 0 ? uint64_t{0} - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    int top = 63;
    while (!(magnitude >> top))
        --top;
    int exponent = top - fraction;
    uint64_t mantissa = 0;
    if (top > 23) {
        const int shift = top - 23;
        mantissa = magnitude >> shift;
        const uint64_t rest = magnitude & ((uint64_t{1} << shift) - 1);
        const uint64_t half = uint64_t{1} << (shift - 1);
        if (rest > half || (rest == half && (mantissa & 1)))
            ++mantissa;
        if (mantissa >> 24) {
            mantissa >>= 1;
            ++exponent;
        }
    } else {
        mantissa = magnitude << (23 - top);
    }
    return sign | static_cast<uint32_t>(exponent + 127) << 23 | (static_cast<uint32_t>(mantissa) & 0x7fffffu);
}

// A micro-unit (micro-g, micro-rad/s) as a float of the unit.
inline uint32_t switchMicroFloatBits(int64_t micro)
{
    return switchFloatBits(micro * (int64_t{1} << 24) / 1000000, 24);
}

struct SwitchMotionFusion {
    // Proportional gain 1 rad/s per unit of direction error, bias integral 0.02 /s^2: a tilt error settles in about a
    // second and a gyroscope bias in tens of seconds at rest.
    static constexpr int64_t kKpMicroRad = 1000000;
    static constexpr int64_t kKiMilli = 20;
    static constexpr int64_t kMaxBiasNanoRad = 100000000;  // 0.1 rad/s: far beyond the IMU's datasheet offsets
    static constexpr int64_t kNearOneG = 150000;           // correct only within 0.15 g of 1 g
    static constexpr int64_t kStillNanoRad = 10000000;     // still: every axis within 0.01 rad/s of its running mean,
    static constexpr int64_t kStillMicroG = 50000;         // the acceleration within 0.05 g of 1 g,
    static constexpr unsigned kStillSamples = 100;         // for this many gyroscope samples in a row

    int64_t q[4] = {kSwitchOne, 0, 0, 0};  // w, x, y, z
    int64_t biasNanoRad[3] = {};
    int64_t meanNanoRad[3] = {};  // the raw gyroscope's running mean (1/32 a sample)
    unsigned stillSamples = 0;
    int32_t accel[3] = {};  // the latest accelerometer sample, micro-g
    bool haveAccel = false;
    bool seeded = false;

    // A new session: the attitude from the next accelerometer sample; the learned bias stays.
    void restart()
    {
        q[0] = kSwitchOne;
        q[1] = q[2] = q[3] = 0;
        haveAccel = seeded = false;
        stillSamples = 0;
    }

    // The gravity the attitude implies, device frame, Q30.
    void gravity(int64_t out[3]) const
    {
        const int64_t w = q[0];
        const int64_t x = q[1];
        const int64_t y = q[2];
        const int64_t z = q[3];
        out[0] = -2 * ((x * z - w * y) >> kSwitchQ);
        out[1] = -2 * ((y * z + w * x) >> kSwitchQ);
        out[2] = -((w * w - x * x - y * y + z * z) >> kSwitchQ);
    }

    void addAccel(const int32_t sample[3])
    {
        memcpy(accel, sample, sizeof(accel));
        haveAccel = true;
        if (!seeded)
            seed();
    }

    // One gyroscope sample over `dtMicros` since the last.
    void addGyro(const int32_t sample[3], uint32_t dtMicros)
    {
        learnWhileStill(sample);
        if (!seeded)
            return;
        int64_t omega[3] = {sample[0], sample[1], sample[2]};
        int64_t down[3];
        if (unitAccel(down)) {
            int64_t estimate[3];
            gravity(estimate);
            const int64_t error[3] = {
                (down[1] * estimate[2] - down[2] * estimate[1]) >> kSwitchQ,
                (down[2] * estimate[0] - down[0] * estimate[2]) >> kSwitchQ,
                (down[0] * estimate[1] - down[1] * estimate[0]) >> kSwitchQ,
            };
            for (unsigned i = 0; i < 3; ++i) {
                // Mahony adds the error's integral to the rate; the bias is that integral's negative.
                int64_t bias = biasNanoRad[i] - ((kKiMilli * error[i] * dtMicros) >> kSwitchQ);
                if (bias > kMaxBiasNanoRad)
                    bias = kMaxBiasNanoRad;
                if (bias < -kMaxBiasNanoRad)
                    bias = -kMaxBiasNanoRad;
                biasNanoRad[i] = bias;
                omega[i] += (kKpMicroRad * error[i]) >> kSwitchQ;
            }
        }
        int64_t half[3];
        for (unsigned i = 0; i < 3; ++i) {
            omega[i] -= biasNanoRad[i] / 1000;
            // Half the rotation angle, Q30: omega (urad/s) * dt (us) / 2 is in 1e-12 rad.
            half[i] = (omega[i] * static_cast<int64_t>(dtMicros) / 2000) * kSwitchOne / 1000000000;
        }
        rotate(half);
    }

    // The latest sample's rotation rate without the estimated bias, micro-rad/s.
    void rotationRate(const int32_t gyro[3], int64_t out[3]) const
    {
        for (unsigned i = 0; i < 3; ++i)
            out[i] = gyro[i] - biasNanoRad[i] / 1000;
    }

    // The acceleration that is not gravity, micro-g (CMDeviceMotion: total = gravity + user acceleration).
    void userAcceleration(int64_t out[3]) const
    {
        int64_t g[3];
        gravity(g);
        for (unsigned i = 0; i < 3; ++i)
            out[i] = accel[i] - g[i] * 1000000 / kSwitchOne;
    }

private:
    void learnWhileStill(const int32_t sample[3])
    {
        bool still = haveAccel && accelWithin(kStillMicroG);
        for (unsigned i = 0; i < 3; ++i) {
            const int64_t nano = int64_t{sample[i]} * 1000;
            meanNanoRad[i] += (nano - meanNanoRad[i]) / 32;
            const int64_t spread = nano - meanNanoRad[i];
            still = still && spread < kStillNanoRad && spread > -kStillNanoRad && meanNanoRad[i] < kMaxBiasNanoRad &&
                    meanNanoRad[i] > -kMaxBiasNanoRad;
        }
        stillSamples = still ? stillSamples + 1 : 0;
        if (stillSamples < kStillSamples)
            return;
        for (unsigned i = 0; i < 3; ++i)
            biasNanoRad[i] += (meanNanoRad[i] - biasNanoRad[i]) / 64;
    }

    bool accelWithin(int64_t microG) const
    {
        const int64_t n2 = int64_t{accel[0]} * accel[0] + int64_t{accel[1]} * accel[1] + int64_t{accel[2]} * accel[2];
        const int64_t norm = static_cast<int64_t>(switchIsqrt(static_cast<uint64_t>(n2)));
        return norm > 1000000 - microG && norm < 1000000 + microG;
    }

    // The measured gravity direction, Q30, when the acceleration is close enough to 1 g to be gravity alone.
    bool unitAccel(int64_t out[3]) const
    {
        if (!haveAccel)
            return false;
        const int64_t n2 = int64_t{accel[0]} * accel[0] + int64_t{accel[1]} * accel[1] + int64_t{accel[2]} * accel[2];
        const int64_t norm = static_cast<int64_t>(switchIsqrt(static_cast<uint64_t>(n2)));
        if (norm < 1000000 - kNearOneG || norm > 1000000 + kNearOneG)
            return false;
        for (unsigned i = 0; i < 3; ++i)
            out[i] = int64_t{accel[i]} * kSwitchOne / norm;
        return true;
    }

    // The shortest rotation taking the measured up direction (-accel) to the reference Z.
    void seed()
    {
        const int64_t n2 = int64_t{accel[0]} * accel[0] + int64_t{accel[1]} * accel[1] + int64_t{accel[2]} * accel[2];
        const int64_t norm = static_cast<int64_t>(switchIsqrt(static_cast<uint64_t>(n2)));
        if (norm < 1000000 / 2)
            return;
        const int64_t up[3] = {-accel[0] * kSwitchOne / norm, -accel[1] * kSwitchOne / norm,
                               -accel[2] * kSwitchOne / norm};
        int64_t raw[4] = {kSwitchOne + up[2], up[1], -up[0], 0};
        if (raw[0] < kSwitchOne / 1000000) {
            raw[0] = 0;
            raw[1] = kSwitchOne;
            raw[2] = 0;
        }
        const uint64_t length2 = static_cast<uint64_t>((raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]) >> 4);
        const int64_t length = static_cast<int64_t>(switchIsqrt(length2)) << 2;
        for (unsigned i = 0; i < 4; ++i)
            q[i] = raw[i] * kSwitchOne / length;
        seeded = true;
    }

    // q = q * (cos|h|, sin|h| h/|h|), third order in |h|, then back to unit length.
    void rotate(const int64_t half[3])
    {
        const int64_t h2 = (half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) >> kSwitchQ;
        const int64_t c = kSwitchOne - h2 / 2;
        const int64_t s = kSwitchOne - h2 / 6;
        const int64_t r[4] = {c, (half[0] * s) >> kSwitchQ, (half[1] * s) >> kSwitchQ, (half[2] * s) >> kSwitchQ};
        const int64_t w = q[0];
        const int64_t x = q[1];
        const int64_t y = q[2];
        const int64_t z = q[3];
        int64_t next[4] = {
            (w * r[0] - x * r[1] - y * r[2] - z * r[3]) >> kSwitchQ,
            (w * r[1] + x * r[0] + y * r[3] - z * r[2]) >> kSwitchQ,
            (w * r[2] - x * r[3] + y * r[0] + z * r[1]) >> kSwitchQ,
            (w * r[3] + x * r[2] - y * r[1] + z * r[0]) >> kSwitchQ,
        };
        const int64_t n2 = (next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]) >> kSwitchQ;
        const int64_t k = (3 * kSwitchOne - n2) / 2;  // one Newton step of 1/sqrt(n2): n2 stays within 1e-6 of 1
        for (unsigned i = 0; i < 4; ++i)
            q[i] = (next[i] * k) >> kSwitchQ;
    }
};

// CoreMotion's 6-axis device motion report (CLSensorFusionServiceSPU::populateDeviceMotion, type 3), little-endian:
// type, status (bit 8: initialized; CoreMotion drops samples without it), client ID (logged only), quaternion x, y, z,
// w, rotation rate (rad/s), user acceleration (g). 21H461 takes exactly these 52 bytes; 24B119 takes 60, the last two
// floats ones it defaults to -1 and 0 itself.
constexpr size_t kSwitchDeviceMotion21H461 = 52;
constexpr size_t kSwitchDeviceMotion24B119 = 60;
constexpr uint8_t kSwitchDeviceMotionSixAxis = 3;
constexpr uint16_t kSwitchDeviceMotionInitialized = 0x100;

inline void switchDeviceMotionPayload(const SwitchMotionFusion &fusion, const int32_t gyro[3], uint8_t *out,
                                      size_t size)
{
    memset(out, 0, size);
    const auto put = [out](size_t at, uint32_t bits) {
        for (unsigned i = 0; i < 4; ++i)
            out[at + i] = static_cast<uint8_t>(bits >> (8 * i));
    };
    out[0] = kSwitchDeviceMotionSixAxis;
    const uint16_t status = fusion.seeded ? kSwitchDeviceMotionInitialized : 0;
    out[1] = static_cast<uint8_t>(status);
    out[2] = static_cast<uint8_t>(status >> 8);
    put(12, switchFloatBits(fusion.q[1], kSwitchQ));
    put(16, switchFloatBits(fusion.q[2], kSwitchQ));
    put(20, switchFloatBits(fusion.q[3], kSwitchQ));
    put(24, switchFloatBits(fusion.q[0], kSwitchQ));
    int64_t rate[3];
    int64_t user[3];
    fusion.rotationRate(gyro, rate);
    fusion.userAcceleration(user);
    for (unsigned i = 0; i < 3; ++i) {
        put(28 + 4 * i, switchMicroFloatBits(rate[i]));
        put(40 + 4 * i, switchMicroFloatBits(user[i]));
    }
    if (size >= kSwitchDeviceMotion24B119)
        put(52, 0xbf800000u);
}
