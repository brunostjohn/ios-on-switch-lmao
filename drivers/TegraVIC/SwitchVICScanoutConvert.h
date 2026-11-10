// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchNativeScanout.h"

// Source plane bases are offsets within the captured allocation. The caller keeps that snapshot through admission.
int switch_vic_scanout_convert(const SwitchNativeScanout *source, const TegraVICImage *image,
                               switch_gpu_backing *destination, uint32_t width, uint32_t height, uint32_t pitch,
                               const TegraVICOp *operation, SwitchNativeScanout *output);
