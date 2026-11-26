// SPDX-License-Identifier: MIT
// See SwitchGpuCompilerScope.h.
#include "SwitchGpuCompilerScope.h"
#include <string.h>

struct proc;
extern "C" {
struct proc *current_proc(void);
int proc_pid(struct proc *);
int proc_pidversion(struct proc *);
int csproc_get_platform_binary(struct proc *);
const char *cs_identity_get(struct proc *);
uint8_t *cs_get_cdhash(struct proc *);
}

void switch_gpu_compiler_scope(TegraGM20BAbiCompilerScope &out)
{
	constexpr uint32_t kCdhashBytes = 20;  // CS_CDHASH_LEN
	out = {};
	struct proc *p = current_proc();
	const char *identity = cs_identity_get(p);
	if (identity && csproc_get_platform_binary(p)) {
		out.kind = TegraGM20BCompilerScopePlatform;
		out.bytes = uint32_t(strnlen(identity, sizeof out.identity));
		memcpy(out.identity, identity, out.bytes);
	} else if (const uint8_t *cdhash = cs_get_cdhash(p)) {
		out.kind = TegraGM20BCompilerScopeCdhash;
		out.bytes = kCdhashBytes;
		memcpy(out.identity, cdhash, kCdhashBytes);
	} else {
		const int32_t which[2] = {proc_pid(p), proc_pidversion(p)};
		out.kind = TegraGM20BCompilerScopeProcess;
		out.bytes = sizeof which;
		memcpy(out.identity, which, sizeof which);
	}
}
