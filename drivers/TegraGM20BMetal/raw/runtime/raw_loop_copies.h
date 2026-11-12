// SPDX-License-Identifier: MIT
// A predicated loop's GP entries (raw_predication.h): the compute encoder lays the body down once, each of its segments
// beginning with a head word, and lists those segments once per copy. Without a backward jump the copies are a bound:
// a loop no block or loop holds that would run past it is laid down again after its job completed (the submission's
// continuation, one completion interrupt per laying); one inside another loop or a block fails the command buffer there
// (Predication::endLoop). Copies multiply with nesting and every copy is a GP entry per body segment, so a loop nest
// lists at most kEntryBudget entries (four jobs of TegraGM20BMaxJobPushes).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "raw_batch.h"
#include "raw_predication.h"

namespace raw {

// What a layout writes through: the encoder's push, the start of its open segment, the command buffer's GP entries, and
// where the push's words lie on the GPU
struct LoopStream {
    Push &p;
    uint32_t *&start;
    PushBatches &entries;
    const uint8_t *pushCpu;
    uint64_t pushVa;

    uint64_t va(const uint32_t *at) const
    {
        return pushVa + uint64_t(reinterpret_cast<const uint8_t *>(at) - pushCpu);
    }
};

class LoopCopies {
public:
    static constexpr uint32_t kOutermostCopies = 1024, kNestedCopies = 128;
    static constexpr size_t kMaxDepth = 2, kMaxSegments = 8, kEntryBudget = 4096;
    // Room a start or an end takes in the open push chunk, so its words stay in one segment
    static constexpr uint32_t kStartRoom = Predication::kArmWords(kMaxSegments) + Predication::kStartWords + 1;
    static constexpr uint32_t kEndRoom = Predication::kEndCopyWords(kMaxSegments) + Predication::kOverflowWords + 1;

    // The copies of a loop `depth` deep (1: no loop holds it) whose copy is `entries` GP entries; 0: none fit
    static uint32_t copies(size_t depth, size_t entries);

    bool open() const
    {
        return !frames_.empty();
    }

    // [start, p.at) into the GP entries: inside a loop, a body segment the fetcher reads for every copy
    void closeSegment(LoopStream s);
    // start at p.at; inside a loop, the segment's head first
    void openSegment(LoopStream s);
    // One copy's batch estimate grows by `work`
    void addWork(uint64_t work);

    // A loop's start: `slot` its slot, a While's test of the word at `wordVa`. Misused: deeper than kMaxDepth
    Predication::Answer start(LoopStream s, Predication &predication, PredicateSlot slot, bool doWhile, uint64_t wordVa,
                              const PredicateTest *test);
    // Its end: the copy's end (a DoWhile's test), the further copies, and `stamp` where the loop would run past them:
    // into `continuedVa` for a loop no block or loop holds, which the submission continues with another laying of the
    // loop after its job (RMSegment::loopTo), else into `failedVa`. Where no loop holds this one a batch ends after each
    // copy that fills one, so the kernel's stuck rule sees seqnos however many copies run. Misused: no loop open, a
    // body of more than kMaxSegments segments or with entries neither it nor a loop inside it laid, or no copies within
    // the budget
    Predication::Answer end(LoopStream s, Predication &predication, BatchWork &batches, uint64_t batchLimit,
                            uint64_t continuedVa, uint64_t failedVa, uint32_t stamp, uint64_t wordVa,
                            const PredicateTest *test);

private:
    struct Frame {
        size_t entriesAt = 0;         // the body's first segment in the GP entries
        std::vector<uint64_t> heads;  // each body segment's first word
        uint32_t *arm = nullptr;      // words before the first copy of a loop inside another, NOPs until armed
        uint64_t goingVa = 0;         // a DoWhile's
        uint64_t work = 0;            // one copy's batch estimate
        size_t nestedEntries = 0;     // the copies of the loops inside it
    };

    bool bodyIntact(const PushBatches &entries, const Frame &f) const;

    std::vector<Frame> frames_;
};

}  // namespace raw
