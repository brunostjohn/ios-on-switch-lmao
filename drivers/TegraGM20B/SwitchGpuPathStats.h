// SPDX-License-Identifier: MIT
// The paths behind the capability bits GM20B clients take (debug kernels): counted per process for the HW Test's
// gpu.path.* cases (kern.switch_gpu_paths: the flags GetInfo advertises and the reader's own counts) and per stats window,
// every process together, for the periodic "paths" line. A release kernel's are empty.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_gpu_path {
	SWITCH_GPU_PATH_INTERNAL_JOB,   /* JobVisibility: a job that ended without its L2 flush */
	SWITCH_GPU_PATH_EXTERNAL_JOB,   /* JobVisibility: a job the runtime or the kernel made External */
	SWITCH_GPU_PATH_UNLOCKED_WRITE, /* External because it wrote an IOSurface over its creator's memory */
	SWITCH_GPU_PATH_VOLATILE_BIND,  /* JobVisibility: a control BO bound GPU-volatile */
	SWITCH_GPU_PATH_LEAN_COPY_JOB,  /* LeanCopyChannel: a job on a copy-only channel */
	SWITCH_GPU_PATH_CONTROL_BO,     /* ControlReserve: a control BO from the reserve */
	SWITCH_GPU_PATH_CPU_COMPLETE,   /* CpuComplete: a call */
	SWITCH_GPU_PATH_RANGED_USE,     /* UseRanges: a use with a byte range */
	SWITCH_GPU_PATH_VISIBLE_WAIT,   /* a CPU relayout's wait for the surface's GPU writes to reach DRAM */
	SWITCH_GPU_PATH_COUNT
};

void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count);
void switch_gpu_path_job(uint32_t visibility, int unlocked_writes, int lean_copy, uint32_t ranged_uses);
/* The window's counts as "name=N ...", zeroed for the next window */
void switch_gpu_path_window(char *line, size_t size);
/* kern.switch_gpu_paths: "flags=0x... pid=N name=N ...", the reading process's counts */
void switch_gpu_path_register(void);

/* TegraGM20BUserClient.cpp: what GetInfo advertises */
uint32_t switch_gpu_info_flags(void);

#ifdef __cplusplus
}
#endif
