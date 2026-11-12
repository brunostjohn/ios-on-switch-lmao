// SPDX-License-Identifier: MIT
// The compute encoder's predicated loops (raw_loop_copies.h).
#include "raw_compute.h"

namespace raw {

Predication::Answer ComputeEncoder::startWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference)
{
    const PredicateTest test = predicateTest(compare, reference);
    return startLoop(false, wordVa, &test);
}

Predication::Answer ComputeEncoder::startDoWhile()
{
    return startLoop(true, 0, nullptr);
}

Predication::Answer ComputeEncoder::endWhile()
{
    return endLoop(0, nullptr);
}

Predication::Answer ComputeEncoder::endDoWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference)
{
    const PredicateTest test = predicateTest(compare, reference);
    return endLoop(wordVa, &test);
}

Predication::Answer ComputeEncoder::startLoop(bool doWhile, uint64_t wordVa, const PredicateTest *test)
{
    room(LoopCopies::kStartRoom);
    uint64_t va;
    uint8_t *cpu = dev_.alloc(dev_.upload_, Predication::kSlotBytes, Predication::kSlotAlign, va);
    return loops_.start(stream(), cb_.predication_, {cpu, va}, doWhile, wordVa, test);
}

Predication::Answer ComputeEncoder::endLoop(uint64_t wordVa, const PredicateTest *test)
{
    room(LoopCopies::kEndRoom);
    const Predication::Answer answer = loops_.end(stream(), cb_.predication_, cb_.batchWork_, cb_.batchLimit(),
                                                  dev_.loopOverflowVa(cb_.stamp), cb_.stamp, wordVa, test);
    cb_.loopsLaid |= answer == Predication::Answer::Done;
    return answer;
}

}  // namespace raw
