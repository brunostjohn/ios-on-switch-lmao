// SPDX-License-Identifier: BSD-2-Clause-Patent
// When the rail Joy-Cons are a game controller in use (SwitchJoyConRail.cpp; the host test
// test/switch_joycon_use_test.cpp drives this header).
//
// iPadOS treats a connected gamepad as an input the user navigates with. UIKitCore 24B119 sets up a focus system in
// every scene of an app linked on or after iOS 26 (_UISupportsGameControllers) whenever an IOHIDDevice with usage pair
// Generic Desktop/Game Pad or Joystick exists (_UIFocusGameControllerObserver: an IOHIDManager on those pairs, active
// while it holds a device), and its programmatic focus updates then run every second. A Bluetooth controller is such a
// device only from the press that connects it until GameController disconnects it after 900 s without input
// (GCGamepadHIDServicePlugin defaultIdleTimeoutDuration; -[_GCNintendoJoyConDevice(Idle) requestIdleDisconnect:]
// asks bluetoothd, a no-op for a Joy-Con on the rail). The rail pair keeps that lifecycle: its gamepad devices exist
// from a gamepad press or stick push on either Joy-Con until both have been idle that long. Home and Capture are the
// iPad's Home button and screenshot key and never count as gamepad use.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "SwitchJoyConForward.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_JOYCON_USE_IDLE_MS 900000u
#define SWITCH_JOYCON_USE_TRAVEL  512u  // of 4096 from where the stick rested: a push, not drift

// Standard input bytes 3..5: right buttons (Y X B A SR SL R ZR), shared (- + RS LS Home Capture, grip 0x80), left
// buttons (down up right left SR SL L ZL). SL and SR face the rail.
#define SWITCH_JOYCON_USE_RIGHT  0xcfu
#define SWITCH_JOYCON_USE_SHARED 0x0fu
#define SWITCH_JOYCON_USE_LEFT   0xcfu

struct switch_joycon_use {
    bool rested;
    uint8_t rest[6];  // both sticks' bytes (report bytes 6..11) as they first read
};

static inline void switch_joycon_use_reset(struct switch_joycon_use *u)
{
    u->rested = false;
}

// True: this report (0x21, 0x30 or 0x31) is the user playing.
static inline bool switch_joycon_use_report(struct switch_joycon_use *u, const uint8_t *report, size_t length)
{
    if (length < 12 || (report[0] != 0x21 && report[0] != 0x30 && report[0] != 0x31))
        return false;
    if ((report[3] & SWITCH_JOYCON_USE_RIGHT) || (report[4] & SWITCH_JOYCON_USE_SHARED) ||
        (report[5] & SWITCH_JOYCON_USE_LEFT))
        return true;
    const uint8_t *sticks = report + 6;
    if (!u->rested) {
        for (unsigned i = 0; i < 6; ++i)
            u->rest[i] = sticks[i];
        u->rested = true;
        return false;
    }
    for (unsigned stick = 0; stick < 2; ++stick) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            const unsigned a = switch_joycon_axis(u->rest + 3u * stick, axis);
            const unsigned b = switch_joycon_axis(sticks + 3u * stick, axis);
            if ((a > b ? a - b : b - a) > SWITCH_JOYCON_USE_TRAVEL)
                return true;
        }
    }
    return false;
}

static inline bool switch_joycon_use_idle(uint64_t last_use_ms, uint64_t now_ms)
{
    return now_ms - last_use_ms >= SWITCH_JOYCON_USE_IDLE_MS;
}

#ifdef __cplusplus
}
#endif
