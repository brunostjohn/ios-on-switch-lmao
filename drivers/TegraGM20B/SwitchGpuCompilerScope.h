// SPDX-License-Identifier: MIT
// The calling process's compile scope (TegraGM20BCompilerABI.h TegraGM20BAbiCompilerScope), read from its code signature as
// the kernel validated it; no digest is computed here.
#pragma once
#include "TegraGM20BCompilerABI.h"

void switch_gpu_compiler_scope(TegraGM20BAbiCompilerScope &out);
