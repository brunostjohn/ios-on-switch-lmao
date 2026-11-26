// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware DMA re-pack behind NVJPG's syncpoint fence: VIC for '422f'/'444f' (SwitchVICDecodeOutput.h), and GM20B Copy
// Engine for pitch/block-linear layout mismatches (TegraGM20BCopy.h).
#pragma once
#include <stdint.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGRequest.h"
#include "../TegraVIC/TegraVICSurface.h"

struct switch_gpu_backing;

// Runs the decode from the scan at IOVA `bitstream` with VIC's or Copy Engine's re-pack queued behind its fence, and
// returns once the follow-up DMA has written the destination: 0, or the decode's or DMA's error (`why` set for a
// refusal before any engine work). Engine thread.
int tegra_nvjpg_repack(TegraNVJPGJob *engine, const TegraNVJPGPlan *plan, const TegraNVJPGDecodeRequest *request,
                       const TegraVICSurface *destination, switch_gpu_backing *backing, uint64_t bitstream,
                       nvjpg_dec_status *status, uint64_t *busyUs, const char **why);
