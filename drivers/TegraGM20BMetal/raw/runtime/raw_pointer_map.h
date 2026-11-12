// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace raw {

// A map keyed by object pointers whose clear keeps its storage: a command buffer's recording names the objects of the
// last one again, and inserts them with no allocation once the table has grown to a frame's.
template <typename V> class PointerMap {
public:
    V *find(const void *key)
    {
        if (slots_.empty())
            return nullptr;
        Slot &slot = slots_[at(key)];
        return slot.stamp == stamp_ ? &slot.value : nullptr;
    }

    const V *find(const void *key) const
    {
        return const_cast<PointerMap *>(this)->find(key);
    }

    // The value at `key`, `value` put there where it had none (true then)
    std::pair<V *, bool> insert(const void *key, const V &value)
    {
        if ((size_ + 1) * 2 > slots_.size())
            grow();
        Slot &slot = slots_[at(key)];
        if (slot.stamp == stamp_)
            return {&slot.value, false};
        slot = {key, stamp_, value};
        ++size_;
        return {&slot.value, true};
    }

    void clear()
    {
        size_ = 0;
        if (++stamp_)
            return;
        for (Slot &slot : slots_)
            slot.stamp = 0;
        stamp_ = 1;
    }

    size_t size() const
    {
        return size_;
    }

    size_t storageBytes() const
    {
        return slots_.capacity() * sizeof(Slot);
    }

    template <typename F> void forEach(F visit) const
    {
        for (const Slot &slot : slots_)
            if (slot.stamp == stamp_)
                visit(slot.key, slot.value);
    }

private:
    struct Slot {
        const void *key = nullptr;
        uint32_t stamp = 0;
        V value{};
    };

    // The slot of `key`, or the empty one its probe reached
    size_t at(const void *key) const
    {
        const size_t mask = slots_.size() - 1;
        size_t i = size_t((uint64_t(reinterpret_cast<uintptr_t>(key)) >> 4) * 0x9e3779b97f4a7c15ull) & mask;
        while (slots_[i].stamp == stamp_ && slots_[i].key != key)
            i = (i + 1) & mask;
        return i;
    }

    void grow()
    {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 64 : old.size() * 2, Slot{});
        const uint32_t live = stamp_;
        stamp_ = 1;
        for (Slot &slot : old)
            if (slot.stamp == live)
                slots_[at(slot.key)] = {slot.key, stamp_, slot.value};
    }

    std::vector<Slot> slots_;
    uint32_t stamp_ = 1;
    size_t size_ = 0;
};

}  // namespace raw
