// SPDX-License-Identifier: MIT
#pragma once
#include "switch_gpu_copy_stubs.h"
using task_t = void *;
using vm_offset_t = uintptr_t;
inline task_t kernel_task = nullptr;

inline task_t current_task()
{
	return nullptr;
}

class IOBufferMemoryDescriptor : public IOMemoryDescriptor
{
};

inline void IOLog(const char *, ...)
{
}
