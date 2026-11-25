// SPDX-License-Identifier: BSD-2-Clause-Patent
// The gyroscope offset SwitchMotionFusion subtracts, learned while the console rests (SwitchMotionFusion.h).
//
// A 6-axis part cannot tell a turn about the vertical from an offset along gravity. A console on a turntable, a swivel
// chair or a stand being panned reads like a console at rest whose gyroscope is off by the turn's rate, at any rate and
// for any length of time. It has no vibration, and its centripetal acceleration (0.13 mg at 0.05 rad/s half a metre from
// the axis) is under the parts' 0.24 mg LSB. So a still window teaches only the offset perpendicular to its gravity: the
// gyroscope's mean less the tilt the accelerometer saw over the window. The component along gravity is learned at
// another attitude, where gravity lies elsewhere in the device frame. A console that only ever rests one way up keeps
// that one component unlearned, and yaw drifts at its offset while it lies that way.
//
// A still window is half a second in which every gyroscope sample stays within 0.01 rad/s of the window's mean (about
// ten times the parts' noise) and every accelerometer axis within 10 mg of the window's first sample, at 1 g within
// 0.05 g. The tilt is taken off only when the accelerometer's change between the window's halves stands out of the
// change's own noise (its square past nine times its variance): at rest the tilt estimate is the accelerometer's noise,
// several times the gyroscope mean's. The first offset is the mean of two such windows in a row that agree within
// 5 mrad/s. The component along the first rest's gravity is the mean of four more at an attitude 30 degrees or more
// away, whose solve doubles the noise at 30 degrees. After that each window moves the offset a sixteenth of the way (a
// quarter while that component is missing), at most 1.25 mrad/s per window, so a smoothly changing linear acceleration
// (which the tilt estimate takes for a tilt) moves it little. A window that turns about the vertical faster than any
// offset the part can have is a turn, and is skipped: its centripetal acceleration tilts the measured gravity and leaks
// the turn into the perpendicular part.
#pragma once
#include <stdint.h>
#include <string.h>
#include "SwitchMotionMath.h"

struct SwitchGyroBias {
    static constexpr int64_t kStillMicroRad = 10000;
    static constexpr int32_t kStillMicroG = 10000;
    static constexpr int64_t kStillNearOneG = 50000;
    static constexpr uint32_t kWindowMicros = 500000;
    static constexpr int64_t kAgreeNanoRad = 5000000;
    static constexpr int64_t kStepNanoRad = 1250000;
    static constexpr int64_t kSecondAttitude = kSwitchOne / 4;  // sin^2(30 degrees)
    static constexpr unsigned kFirstWindows = 2;
    static constexpr unsigned kSecondWindows = 4;

    enum class Learned : uint8_t {
        Nothing,
        AllButGravity,  // all but the component along `unlearned`, the first rest's gravity
        All,
    };

    Learned learned = Learned::Nothing;
    int64_t unlearned[3] = {};  // Q30 unit vector

    void restart()
    {
        window = {};
        agreeing = 0;
    }

    // One gyroscope sample (micro-rad/s) standing for `dtMicros`, with the latest accelerometer sample (micro-g) and its
    // magnitude. `bias` (nano-rad/s) moves, each axis within +-`limit`.
    void add(const int32_t gyro[3], uint32_t dtMicros, const int32_t accel[3], int64_t accelNorm, int64_t bias[3],
             int64_t limit)
    {
        if (window.samples && (!steady(accel, accelNorm) || !nearMean(gyro)))
            restart();
        if (!window.samples) {
            memcpy(window.firstAccel, accel, sizeof(window.firstAccel));
            if (!steady(accel, accelNorm))
                return;
        }
        window.micros += dtMicros;
        const unsigned half = window.micros <= kWindowMicros / 2 ? 0 : 1;
        for (unsigned i = 0; i < 3; ++i) {
            window.gyroSum[i] += gyro[i];
            window.accelSum[half][i] += accel[i];
            window.accelSquares[half][i] += int64_t{accel[i]} * accel[i];
        }
        window.timeSum[half] += window.micros;
        ++window.count[half];
        ++window.samples;
        if (window.micros < kWindowMicros)
            return;
        const Window done = window;
        window = {};
        if (done.count[0] < 2 || done.count[1] < 2) {
            agreeing = 0;
            return;
        }
        int64_t rate[3];
        int64_t down[3];
        measure(done, rate, down);
        stillWindow(rate, down, bias, limit);
    }

private:
    struct Window {
        int64_t gyroSum[3];
        int64_t accelSum[2][3];  // each half of the window
        int64_t accelSquares[2][3];
        int64_t timeSum[2];  // micros into the window
        uint32_t count[2];
        int32_t firstAccel[3];
        uint32_t samples, micros;
    };

    Window window = {};
    unsigned agreeing = 0;      // still windows in a row whose proposed offsets agree
    int64_t candidate[3] = {};  // the first of them, nano-rad/s
    int64_t total[3] = {};      // their sum

    static int64_t dot(const int64_t a[3], const int64_t b[3])
    {
        return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) >> kSwitchQ;
    }

    static bool within(const int64_t a[3], const int64_t b[3], int64_t bound)
    {
        for (unsigned i = 0; i < 3; ++i) {
            if (a[i] - b[i] > bound || b[i] - a[i] > bound)
                return false;
        }
        return true;
    }

    static int64_t clamp(int64_t value, int64_t bound)
    {
        if (value > bound)
            return bound;
        return value < -bound ? -bound : value;
    }

    bool steady(const int32_t accel[3], int64_t norm) const
    {
        if (norm < 1000000 - kStillNearOneG || norm > 1000000 + kStillNearOneG)
            return false;
        for (unsigned i = 0; i < 3; ++i) {
            const int32_t moved = accel[i] - window.firstAccel[i];
            if (moved > kStillMicroG || moved < -kStillMicroG)
                return false;
        }
        return true;
    }

    bool nearMean(const int32_t gyro[3]) const
    {
        for (unsigned i = 0; i < 3; ++i) {
            const int64_t mean = window.gyroSum[i] / window.samples;
            if (gyro[i] - mean > kStillMicroRad || mean - gyro[i] > kStillMicroRad)
                return false;
        }
        return true;
    }

    // Whether the change between the halves' accelerometer means (micro-g) is a tilt rather than noise: its square past
    // nine times its variance, which the halves' own spread gives.
    static bool tilted(const Window &w, const int64_t change[3])
    {
        const int64_t n0 = w.count[0], n1 = w.count[1];
        int64_t variance = 0;
        int64_t moved = 0;
        for (unsigned i = 0; i < 3; ++i) {
            int64_t spread = 0;
            for (unsigned h = 0; h < 2; ++h) {
                const int64_t squares = w.accelSquares[h][i] - w.accelSum[h][i] * w.accelSum[h][i] / w.count[h];
                spread += squares > 0 ? squares : 0;
            }
            variance += spread / (n0 + n1 - 2);
            moved += change[i] * change[i];
        }
        return moved > 9 * (variance * (n0 + n1) / (n0 * n1));
    }

    // The window's gyroscope mean less the tilt its accelerometer saw (micro-rad/s), and its gravity direction (Q30). The
    // measured down direction turns as dg/dt = -w x g, so the rotation perpendicular to it is (dg/dt) x g, here from the
    // mean of each half of the window.
    static void measure(const Window &w, int64_t rate[3], int64_t down[3])
    {
        int64_t mean[3];
        int64_t change[3];
        for (unsigned i = 0; i < 3; ++i) {
            mean[i] = (w.accelSum[0][i] + w.accelSum[1][i]) / w.samples;
            change[i] = w.accelSum[1][i] / w.count[1] - w.accelSum[0][i] / w.count[0];
        }
        const bool tiltSeen = tilted(w, change);
        const int64_t norm = static_cast<int64_t>(
            switchIsqrt(static_cast<uint64_t>(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2])));
        int64_t turned[3];
        for (unsigned i = 0; i < 3; ++i) {
            down[i] = mean[i] * kSwitchOne / norm;
            turned[i] = change[i] * kSwitchOne / norm;
        }
        const int64_t micros = w.timeSum[1] / w.count[1] - w.timeSum[0] / w.count[0];
        const int64_t tilt[3] = {
            (turned[1] * down[2] - turned[2] * down[1]) >> kSwitchQ,
            (turned[2] * down[0] - turned[0] * down[2]) >> kSwitchQ,
            (turned[0] * down[1] - turned[1] * down[0]) >> kSwitchQ,
        };
        for (unsigned i = 0; i < 3; ++i)
            rate[i] = w.gyroSum[i] / w.samples - (tiltSeen ? (tilt[i] * 1000000 / micros) * 1000000 >> kSwitchQ : 0);
    }

    void stillWindow(const int64_t rate[3], const int64_t down[3], int64_t bias[3], int64_t limit)
    {
        const int64_t along = dot(rate, down);
        if (along > limit / 1000 || along < -limit / 1000) {
            agreeing = 0;
            return;
        }
        int64_t miss[3];
        for (unsigned i = 0; i < 3; ++i)
            miss[i] = rate[i] - bias[i] / 1000;
        const int64_t missAlong = dot(miss, down);
        int64_t across[3];  // micro-rad/s
        for (unsigned i = 0; i < 3; ++i)
            across[i] = miss[i] - ((missAlong * down[i]) >> kSwitchQ);
        int64_t proposed[3];
        if (learned == Learned::Nothing) {
            for (unsigned i = 0; i < 3; ++i)
                proposed[i] = bias[i] + across[i] * 1000;
            acquire(proposed, down, bias, limit);
            return;
        }
        const int64_t cosine = dot(unlearned, down);
        const int64_t sine2 = kSwitchOne - ((cosine * cosine) >> kSwitchQ);
        if (learned == Learned::AllButGravity && sine2 >= kSecondAttitude) {
            // Only `unlearned`'s component is missing: the window's miss, perpendicular to its gravity, is that component
            // times the part of `unlearned` the window sees.
            const int64_t component = dot(across, unlearned) * kSwitchOne / sine2;
            for (unsigned i = 0; i < 3; ++i)
                proposed[i] = bias[i] + ((component * unlearned[i]) >> kSwitchQ) * 1000;
            acquire(proposed, down, bias, limit);
            return;
        }
        agreeing = 0;
        const int64_t gain = learned == Learned::All ? 16 : 4;
        for (unsigned i = 0; i < 3; ++i)
            bias[i] = clamp(bias[i] + clamp(across[i] * 1000 / gain, kStepNanoRad), limit);
    }

    void acquire(const int64_t proposed[3], const int64_t down[3], int64_t bias[3], int64_t limit)
    {
        if (!agreeing || !within(proposed, candidate, kAgreeNanoRad)) {
            agreeing = 1;
            memcpy(candidate, proposed, sizeof(candidate));
            memcpy(total, proposed, sizeof(total));
            return;
        }
        ++agreeing;
        for (unsigned i = 0; i < 3; ++i)
            total[i] += proposed[i];
        if (agreeing < (learned == Learned::Nothing ? kFirstWindows : kSecondWindows))
            return;
        for (unsigned i = 0; i < 3; ++i)
            bias[i] = clamp(total[i] / agreeing, limit);
        agreeing = 0;
        if (learned == Learned::Nothing) {
            learned = Learned::AllButGravity;
            memcpy(unlearned, down, sizeof(unlearned));
        } else {
            learned = Learned::All;
        }
    }
};
