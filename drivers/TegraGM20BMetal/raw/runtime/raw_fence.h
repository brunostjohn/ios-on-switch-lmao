// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BABI.h"
#include <atomic>
#include <cstdint>

namespace raw {

enum class SemanticFenceStatus : uint8_t { Clear, Pending, Failed };

struct SemanticFenceRead {
    SemanticFenceStatus status = SemanticFenceStatus::Clear;
    uint32_t first = 0;
};

template<class Read> SemanticFenceRead readSemanticFence(uint32_t seqno, Read read)
{
    const uint32_t before = read(0);
    if (before & 1)
        return {SemanticFenceStatus::Pending, 0};
    const uint32_t pending = read(1), failed = read(2), flags = read(3);
    // Pair the kernel's odd/fields/even publication without spinning on its writer.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (read(0) != before || flags & ~(TegraGM20BFenceSemanticPending | TegraGM20BFenceSemanticFailed))
        return {SemanticFenceStatus::Pending, 0};
    if ((flags & TegraGM20BFenceSemanticFailed) && int32_t(seqno - failed) >= 0)
        return {SemanticFenceStatus::Failed, failed};
    if ((flags & TegraGM20BFenceSemanticPending) && int32_t(seqno - pending) >= 0)
        return {SemanticFenceStatus::Pending, pending};
    return {};
}

inline SemanticFenceRead readSemanticFence(const volatile TegraGM20BAbiFenceSemantic *record, uint32_t seqno)
{
    return readSemanticFence(seqno, [record](unsigned field) {
        switch (field) {
        case 0: return __atomic_load_n(&record->epoch, __ATOMIC_ACQUIRE);
        case 1: return __atomic_load_n(&record->pending_first, __ATOMIC_RELAXED);
        case 2: return __atomic_load_n(&record->failed_first, __ATOMIC_RELAXED);
        default: return __atomic_load_n(&record->flags, __ATOMIC_RELAXED);
        }
    });
}

} // namespace raw
