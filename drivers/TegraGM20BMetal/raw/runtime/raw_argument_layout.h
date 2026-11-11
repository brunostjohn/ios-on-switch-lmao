// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// Where MTLArgumentEncoder writes each argument of a layout made from argument descriptors (newArgumentEncoderWithArguments:):
// metal2vulkan's bind/argument_buffer.rs (ArgumentBufferLayout::from_descriptors, place, type_size_align, metal_align),
// whose rules are Metal's answers on an M3 Max and an M4. The descriptors lay out as a C struct in the order given: every
// object is 8 bytes; a constant takes its MSL size and alignment, a vector of 3 the room of 4 and a matrix its columns'
// vectors; a run of constants is a block whose first may raise its alignment, stepped as (o + a - 1) & ~(a - 1) even where
// a is no power of two. The layout's alignment is the largest type alignment, its length the end rounded up to it. An index
// two descriptors claim is the later one's.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

enum class ArgumentKind : uint8_t {
    Buffer,
    Texture,
    Sampler,
    Constant,
    IndirectCommandBuffer,
    ComputePipeline,
    RenderPipeline,
    VisibleFunctionTable,
    IntersectionFunctionTable,
    AccelerationStructure,
    DepthStencilState,
};

constexpr uint64_t kArgumentHandleBytes = 8;

// One element of an argument's type: a handle, or a constant of `columns` vectors of `rows` scalars of `scalar` bytes
struct ArgumentType {
    ArgumentKind kind = ArgumentKind::Buffer;
    uint8_t scalar = 0, columns = 1, rows = 1;

    uint64_t alignment() const
    {
        return kind == ArgumentKind::Constant ? uint64_t(scalar) * (rows == 3 ? 4 : rows) : kArgumentHandleBytes;
    }

    uint64_t size() const
    {
        return kind == ArgumentKind::Constant ? alignment() * columns : kArgumentHandleBytes;
    }
};

struct ArgumentDescription {
    ArgumentType type;
    uint32_t index = 0, arrayLength = 0, constantBlockAlignment = 0;
};

struct ArgumentMember {
    uint32_t id = 0, count = 1;
    uint64_t offset = 0, size = 0;
    ArgumentKind kind = ArgumentKind::Buffer;
};

struct ArgumentPlace {
    ArgumentKind kind = ArgumentKind::Buffer;
    uint64_t offset = 0, size = 0;
};

enum class ArgumentLayoutError : uint8_t {
    None,
    NoArguments,
    ConstantBlockAlignment,
    TooLarge
};

inline bool metalAlign(uint64_t offset, uint64_t a, uint64_t &out)
{
    a = std::max<uint64_t>(a, 1);
    if (offset > UINT64_MAX - (a - 1))
        return false;
    out = (offset + a - 1) & ~(a - 1);
    return true;
}

struct ArgumentLayout {
    uint64_t encodedLength = 0, alignment = 1;
    std::vector<ArgumentMember> members;

    // `position`: the description a refusal names
    static ArgumentLayoutError fromDescriptions(const std::vector<ArgumentDescription> &descriptions,
                                                ArgumentLayout &out, size_t &position)
    {
        out = {};
        if (descriptions.empty())
            return ArgumentLayoutError::NoArguments;
        uint64_t cursor = 0;
        bool inBlock = false;
        for (position = 0; position < descriptions.size(); position++) {
            const ArgumentDescription &d = descriptions[position];
            const uint64_t natural = d.type.alignment(), size = d.type.size();
            uint64_t align = natural;
            if (d.type.kind == ArgumentKind::Constant) {
                if (d.constantBlockAlignment) {
                    if (inBlock)
                        return ArgumentLayoutError::ConstantBlockAlignment;
                    align = std::max<uint64_t>(align, d.constantBlockAlignment);
                }
                inBlock = true;
            } else {
                inBlock = false;
            }
            const uint32_t count = std::max<uint32_t>(d.arrayLength, 1);
            uint64_t offset;
            if (!metalAlign(cursor, align, offset) || size > (UINT64_MAX - offset) / count)
                return ArgumentLayoutError::TooLarge;
            cursor = offset + size * count;
            out.alignment = std::max(out.alignment, natural);
            out.members.push_back({d.index, count, offset, size, d.type.kind});
        }
        return metalAlign(cursor, out.alignment, out.encodedLength) ? ArgumentLayoutError::None
                                                                    : ArgumentLayoutError::TooLarge;
    }

    // Where argument `index` lands: in the last member whose indices hold it
    bool place(uint32_t index, ArgumentPlace &out) const
    {
        for (auto m = members.rbegin(); m != members.rend(); ++m) {
            if (index < m->id || index - m->id >= m->count)
                continue;
            out = {m->kind, m->offset + m->size * (index - m->id), m->size};
            return true;
        }
        return false;
    }
};

}  // namespace raw
