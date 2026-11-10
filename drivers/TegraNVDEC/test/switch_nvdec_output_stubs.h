// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <cstdint>
using task_t = void *;
using IOReturn = int32_t;
using IOOptionBits = uint32_t;
constexpr IOOptionBits kIOMapAnywhere = 1, kIOMapDefaultCache = 2;
class OSObject {
public:
    uint32_t refs = 1;
    void retain() { ++refs; }
    void release() { --refs; }
};
class IOMemoryMap : public OSObject {};
class IOMemoryDescriptor : public OSObject {
public:
    IOMemoryMap *mapping = nullptr;
    task_t mappedTask = nullptr;
    IOOptionBits mappedOptions = 0;
    IOMemoryMap *createMappingInTask(task_t task, uint64_t, IOOptionBits options)
    {
        mappedTask = task;
        mappedOptions = options;
        return mapping;
    }
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};
struct switch_gpu_backing { uint32_t refs = 1; };
struct switch_gpu_cpu_lease { switch_gpu_backing *backing; };
