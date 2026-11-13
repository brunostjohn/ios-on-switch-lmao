// SPDX-License-Identifier: MIT
// When the shader compiler broker stops waiting for a compiler (SwitchGpuShaderCompiler.cpp): the rules, without IOKit, so
// a host test holds them.
#pragma once
#include <stdint.h>

namespace switch_gpu_compiler
{
// Compilers lost this boot before the kernel stops waiting for the next
constexpr uint32_t kLosses = 3;

// After the compiler's connection closed, registered or only announced: whether no compiler comes this boot. A decline
// stands until a compiler registers; one that nothing starts again is a decline; one its launcher starts again is
// waited for until the third loss, as many as the launchers start (the BlueTool stand-in's kCompilerDeaths).
inline bool declinedAfterClose(bool declined, bool restarts, uint32_t &losses)
{
	if (declined || !restarts)
		return true;
	return ++losses >= kLosses;
}

}  // namespace switch_gpu_compiler
