/* SPDX-License-Identifier: MIT */
/* What a legacy submission (Exec, or Submit with ExecSurfaces semantics) tells the ownership layer about each BO of its
 * connection. Only a listed import is a claim: the job's list says it reads or writes it, and a listed job touches no
 * import it did not list (TegraGM20BABI.h method 17). No CPU owner refuses a claim: the legacy ABI has no such
 * refusal, NVK takes one as fatal, and Busy would make it abort or spin. Everything else the job may touch is UNLISTED:
 * CPU access still orders after the job. An import is listable once it has a coherency entry, as in ExecSurfaces'
 * index. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"
#include "TegraGM20BABI.h"

static inline uint32_t switch_gpu_legacy_access(bool listable, bool read_only, bool listed, uint32_t listed_access)
{
	const uint32_t touch = read_only ? SWITCH_GPU_BACKING_READ : SWITCH_GPU_BACKING_WRITE;
	if (!listable || !listed)
		return touch | SWITCH_GPU_BACKING_UNLISTED;
	uint32_t access = 0;
	if (listed_access & TegraGM20BSurfaceRead)
		access |= SWITCH_GPU_BACKING_READ;
	if (listed_access & TegraGM20BSurfaceWrite)
		access |= touch;
	return access ? access | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION : 0;
}

/* bos: the connection's BO list (next, backing, coh, readOnly, handle); listed_access(handle): the TegraGM20BSurface*
 * access the job's list gives an import. Returns the uses written to out, at most one per BO. */
template <class Bo, class ListedAccess>
uint32_t switch_gpu_legacy_uses(const Bo *bos, bool listed, ListedAccess listed_access, switch_gpu_backing_use *out)
{
	uint32_t count = 0;
	for (const Bo *bo = bos; bo; bo = bo->next) {
		const bool listable = bo->coh != nullptr;
		const uint32_t access = switch_gpu_legacy_access(listable, bo->readOnly, listed,
			listable && listed ? listed_access(bo->handle) : 0u);
		if (access)
			out[count++] = {bo->backing, access, 0};
	}
	// A connection lists its BOs newest first, and begin insertion-sorts uses by allocation id: oldest first keeps that
	// sort linear in the BO count instead of quadratic.
	for (uint32_t i = 0; i < count / 2; ++i) {
		const switch_gpu_backing_use swap = out[i];
		out[i] = out[count - 1 - i];
		out[count - 1 - i] = swap;
	}
	return count;
}
