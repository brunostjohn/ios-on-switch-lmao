// SPDX-License-Identifier: MIT
// Where the submission splits a native range for a predicated loop's continuation (RMSegment::loopTo): after each loop
// no block holds, with blocks opened and closed as the records' units keep them (raw_predicate_records.h). The encoder
// marks the same loops' overflow as continued (raw_loop_copies.cpp LoopCopies::end): its predication's units close
// where the records' do, so the two agree on which loops are outermost.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "raw_predicate_records.h"

namespace raw {

class LoopRanges {
public:
    void computeEncoder(uint32_t dispatchType)
    {
        blocks_.computeEncoder(dispatchType);
    }

    void otherWork()
    {
        blocks_.otherWork();
    }

    // The record at `at`: true where it ends a loop no block holds, which began at loopFrom()
    bool record(PredicateRecords::Kind kind, uint32_t at)
    {
        using Records = PredicateRecords;
        if ((kind == Records::StartWhile || kind == Records::StartDoWhile) && !blocks_.depth())
            loopFrom_ = at;
        const bool outermost = (kind == Records::EndWhile || kind == Records::EndDoWhile) && blocks_.depth() == 1;
        return blocks_.record(kind) && outermost;
    }

    uint32_t loopFrom() const
    {
        return loopFrom_;
    }

private:
    PredicateRecords blocks_;
    uint32_t loopFrom_ = 0;
};

// What a retired range that laid loops left in its stamp's two loop words (raw_stamps.h loopOverflowOffset): its loop no
// block holds would run past its copies (Continue: the submission lays it again), or one inside another or a block
// would (Fail: the command buffer fails)
enum class LoopOutcome {
    Done,
    Continue,
    Fail
};

inline LoopOutcome loopOutcome(uint32_t stamp, uint32_t continuedWord, uint32_t failedWord)
{
    if (!stamp)
        return LoopOutcome::Done;
    if (failedWord == stamp)
        return LoopOutcome::Fail;
    return continuedWord == stamp ? LoopOutcome::Continue : LoopOutcome::Done;
}

// The range at `next` follows a loop (loopTo): where that loop's last laying retired as Continue, the loop is laid
// again ahead of it, and the range waits for that laying in turn
template <class Segment> bool layLoopAgain(std::vector<Segment> &segs, size_t next, bool continues)
{
    if (!continues || !segs[next].loopTo)
        return false;
    Segment loop{};
    loop.from = segs[next].loopFrom;
    loop.to = segs[next].loopTo;
    loop.raw = true;
    segs.insert(segs.begin() + std::ptrdiff_t(next), loop);
    return true;
}

}  // namespace raw
