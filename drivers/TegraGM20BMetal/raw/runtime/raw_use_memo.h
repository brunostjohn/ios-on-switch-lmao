// SPDX-License-Identifier: MIT
// The buffers and textures a command buffer named to its job already (raw::CommandBuffer::use): every draw's binding
// table names them again, and a second name at the same address with the same access changes nothing in the job's
// list. A read and a write are apart: a texture's read names the view readTexture gives, its write the texture.
// A slot another name took makes the next of the first a full one again, never a missed one.
#pragma once
#include <array>
#include <cstdint>

namespace raw {

class UseMemo {
public:
    void clear()
    {
        if (++epoch_ == 0) {
            entries_ = {};
            epoch_ = 1;
        }
    }

    // True: `object` at `va` was named with `write` since clear(); else it is noted as named now
    bool named(const void *object, uint64_t va, bool write)
    {
        const uintptr_t key = uintptr_t(object) ^ uintptr_t(write) << 3;
        Entry &e = entries_[(key >> 3 ^ key >> 9) & (kEntries - 1)];
        if (e.epoch == epoch_ && e.object == object && e.va == va && e.write == write)
            return true;
        e = {object, va, epoch_, write};
        return false;
    }

private:
    static constexpr size_t kEntries = 64;

    struct Entry {
        const void *object = nullptr;
        uint64_t va = 0;
        uint32_t epoch = 0;
        bool write = false;
    };

    std::array<Entry, kEntries> entries_{};
    uint32_t epoch_ = 1;
};

}  // namespace raw
