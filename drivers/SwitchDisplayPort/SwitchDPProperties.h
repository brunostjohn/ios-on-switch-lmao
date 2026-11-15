// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDPModes.h"

class OSDictionary;
class OSArray;

OSDictionary *switch_dp_timing_element(const struct switch_dp_mode *mode, uint32_t id, OSArray *colors);
OSDictionary *switch_dp_color_element();
