/* SPDX-License-Identifier: MIT */
/* The calling process's GPU path counts, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on a
 * TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled) answering kern.switch_gpu_paths' line
 * (SwitchGpuPathStats.h), which an app's sandbox may deny it as a sysctl. Kept out of TegraGM20BABI.h, which NVK, the GPU
 * seed and the raw stack key build from: the method number is far from theirs. */
#ifndef TEGRA_GM20B_PATHS_ABI_H
#define TEGRA_GM20B_PATHS_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodPaths = 0x5050, /* 'PP' */
};

struct TegraGM20BAbiPaths {
	uint32_t op; /* 0 */
	uint32_t reserved;
};

/* "flags=0x... pid=N name=N ...", NUL-terminated */
struct TegraGM20BAbiPathsReply {
	char line[512];
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiPaths) == 8, "TegraGM20BAbiPaths layout");
static_assert(sizeof(TegraGM20BAbiPathsReply) == 512, "TegraGM20BAbiPathsReply layout");
#endif
#endif
