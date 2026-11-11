// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeSurface.h"

struct SwitchNativeScanout {
    SwitchNativeSurfaceView view = {};
    switch_gpu_cpu_lease *lease = nullptr;
    switch_gpu_backing_info allocation = {};
    uint64_t pitchOffset = 0;
};

// turns: a picture TegraVIC recorded turned (SwitchVICTurn.h) too, for window A's column scan; view.turn says which.
int switch_native_scanout_acquire(OSObject *surface, SwitchNativeScanout *scanout, bool turns = false);
int switch_native_scanout_pitch(OSObject *surface, SwitchNativeScanout *scanout);
int switch_native_scanout_pitch_wait(OSObject *surface, SwitchNativeScanout *scanout);
void switch_native_scanout_release(SwitchNativeScanout *scanout);
