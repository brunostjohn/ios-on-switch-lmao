// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

inline bool switch_vic_surface_extent_valid(uint64_t memoryBytes, uint64_t baseOffset, uint64_t allocationBytes)
{
    return allocationBytes && !(baseOffset & 4095) && baseOffset <= memoryBytes &&
           allocationBytes <= memoryBytes - baseOffset && allocationBytes <= UINT64_MAX - baseOffset - 4095;
}

inline bool switch_vic_surface_subrange(uint64_t allocationBytes, uint64_t offset, uint64_t bytes)
{
    return bytes && offset <= allocationBytes && bytes <= allocationBytes - offset &&
           offset <= UINT64_MAX - 4095 && bytes <= UINT64_MAX - 4095 - offset;
}
