// SPDX-License-Identifier: MIT
// Metal's command predication (encodeStartIf:offset:comparison:referenceValue:, encodeStartElse, encodeEndIf on compute
// encoders; Metal's rules as m2v measured them on an M4: metal2vulkan-runtime/src/predication.rs) on the compute class's
// render enable (SwitchGM20BEmitPredicate.h), which only tests two 64-bit operands for equality. Each record takes a 64-byte
// slot of the command buffer's upload memory:
//   +0  A: the word, copied by the copy engine; the ordered compares fold the reference in with the copy's MIN/MAX release
//   +16 B: the reference, or the word again for the ordered compares
//   +32 F: low word 1 where the enclosing block runs, high word 1 where this block's test holds, each set by an inline
//          write that render enable gates
//   +48 K: 1 in both words
// A launch inside a block runs where F equals K. The enable is set around each launch and put back to always after it, so
// nothing else on the channel (3D work, raw's own inline writes, the next job) ever runs under it.
#pragma once
#include <cstdint>
#include <vector>
#include "raw_push.h"

namespace raw {

enum class PredicateCompare : uint8_t {
    Equal,
    NotEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
};

// Metal's `comparison:`: 0 to 5, any other value compares equal (m2v measured 6, 255, 2^32 + 2, 2^64 - 1)
PredicateCompare predicateCompare(uint64_t comparison);

// `word OP reference` (unsigned) as the operands and the equality the unit tests
struct PredicateTest {
    bool never = false;    // no word satisfies it: nothing is copied
    bool wordInB = false;  // B is the word too, else `reference`
    bool reduce = false;   // A = reduction(word, payload)
    uint32_t reduction = 0;
    uint32_t payload = 0;
    uint32_t reference = 0;
    bool equal = true;  // RENDER_IF_EQUAL, else RENDER_IF_NOT_EQUAL
};

PredicateTest predicateTest(PredicateCompare compare, uint32_t reference);

// What the unit decides for `word` under `test`: the copy, the reduction and the 64-bit compare
bool predicateRuns(const PredicateTest &test, uint32_t word);

struct PredicateSlot {
    uint8_t *cpu = nullptr;
    uint64_t va = 0;
};

class Predication {
public:
    static constexpr uint32_t kSlotBytes = 64, kSlotAlign = 16;
    static constexpr uint32_t kStartWords = 64, kGuardWords = 4, kReleaseWords = 4;

    enum class Answer : uint8_t {
        Done,
        Misused,  // an Else with no open If or after another Else, an End with no open block: Metal answers NO
    };

    Answer startIf(Push &p, PredicateSlot slot, uint64_t wordVa, const PredicateTest &test);
    Answer startElse(Push &p, PredicateSlot slot);
    Answer endIf();

    // Metal's unit ended (another encoder type or dispatch type, an event, the command buffer's end): a block still open
    // runs nothing more
    void endUnit()
    {
        levels_.clear();
    }

    bool active() const
    {
        return !levels_.empty();
    }

    size_t depth() const
    {
        return levels_.size();
    }

    // Around each launch inside a block
    void guard(Push &p) const;
    void release(Push &p) const;

private:
    struct Level {
        uint64_t ifVa;
        uint64_t liveVa;  // this branch's F/K pair
        PredicateTest test;
        bool inElse;
    };

    void initSlot(PredicateSlot slot, uint32_t live, uint32_t holds, uint32_t reference) const;
    void markLive(Push &p, PredicateSlot slot) const;

    std::vector<Level> levels_;
};

}  // namespace raw
