// SPDX-License-Identifier: MIT
// A command buffer's predication tape submitted as the facade submits it (raw_loop_ranges.h): split after each block
// that holds a loop, each range laid down as the compute encoder lays it (loop_program.h) and run on the predication
// model, and where a loop's record says it ran past its copies, its continuation laid down next, until nothing is
// left. Every shape's launches and words must be Metal's, as a CPU walk of the same tape runs them.
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/test/loop_program.h"

namespace {

using namespace loop_program;
using Records = raw::PredicateRecords;

constexpr uint32_t kWordCount = 8;

// A tape entry: a predication record, or a dispatch whose program adds `delta` to word `w` and sets word `set` to
// `value` (`set` kWordCount: none)
struct Op {
    enum Kind : uint8_t { Dispatch, If, Else, EndIf, While, EndWhile, DoWhile, EndDoWhile } kind;
    uint32_t w = 0;
    PredicateCompare compare = PredicateCompare::NotEqual;
    uint32_t reference = 0;
    int32_t delta = 0;
    uint32_t set = kWordCount, value = 0;
};

Op dispatch(uint32_t w, int32_t delta, uint32_t set = kWordCount, uint32_t value = 0)
{
    return {Op::Dispatch, w, {}, 0, delta, set, value};
}

Op test(Op::Kind k, uint32_t w, PredicateCompare c = PredicateCompare::NotEqual, uint32_t reference = 0)
{
    return {k, w, c, reference};
}

using Words = std::array<uint32_t, kWordCount>;

void effect(const Op &op, Words &words)
{
    words[op.w] += uint32_t(op.delta);
    if (op.set < kWordCount)
        words[op.set] = op.value;
}

bool holds(const Op &op, const Words &words)
{
    return raw::predicateRuns(raw::predicateTest(op.compare, op.reference), words[op.w]);
}

// Metal's answer: the tape walked on the CPU, the indices of the dispatches it runs in order
std::vector<uint32_t> metal(const std::vector<Op> &tape, Words &words)
{
    std::vector<uint32_t> ran;
    std::map<uint32_t, uint32_t> match;
    std::vector<uint32_t> open;
    for (uint32_t i = 0; i < tape.size(); ++i) {
        const Op::Kind k = tape[i].kind;
        if (k == Op::If || k == Op::While || k == Op::DoWhile) {
            open.push_back(i);
        } else if (k == Op::Else) {
            match[open.back()] = i;
            match[i] = open.back();
        } else if (k == Op::EndIf || k == Op::EndWhile || k == Op::EndDoWhile) {
            const uint32_t s = open.back();
            open.pop_back();
            if (!match.count(s))
                match[s] = i;
            else
                match[match[s]] = i;
            match[i] = s;
        }
    }
    uint32_t pc = 0;
    uint64_t steps = 0;
    while (pc < tape.size() && ++steps < 10'000'000) {
        const Op &op = tape[pc];
        switch (op.kind) {
        case Op::Dispatch:
            ran.push_back(pc);
            effect(op, words);
            ++pc;
            break;
        case Op::If:
            pc = holds(op, words) ? pc + 1 : match[pc] + 1;
            break;
        case Op::Else:
            pc = match[pc] + 1;
            break;
        case Op::While:
            pc = holds(op, words) ? pc + 1 : match[pc] + 1;
            break;
        case Op::EndWhile:
            pc = match[pc];
            break;
        case Op::DoWhile:
        case Op::EndIf:
            ++pc;
            break;
        case Op::EndDoWhile:
            pc = holds(op, words) ? match[pc] + 1 : pc + 1;
            break;
        }
    }
    return ran;
}

raw::LoopTapeEntry entry(const Op &op)
{
    static const Records::Kind kinds[] = {Records::StartIf,      Records::StartIf,    Records::StartElse,
                                          Records::EndIf,        Records::StartWhile, Records::EndWhile,
                                          Records::StartDoWhile, Records::EndDoWhile};
    if (op.kind == Op::Dispatch)
        return {};
    return {raw::LoopTapeEntry::Predicate, uint32_t(kinds[op.kind])};
}

struct Submitted {
    std::vector<uint32_t> ran;
    uint32_t layings = 0, continuations = 0;
};

// The submission: each range laid down on a fresh command buffer over the same words, its record read once it ran
Submitted submit(const std::vector<Op> &tape, Words &words, bool sees)
{
    const auto tapeAt = [&](uint32_t at) { return entry(tape[at]); };
    Submitted out;
    std::deque<raw::TapeRange> queue;
    const auto enqueue = [&](raw::TapeRange r, std::deque<raw::TapeRange>::iterator at) {
        std::vector<raw::TapeRange> parts;
        uint32_t from = r.from;
        for (const uint32_t split : raw::loopSplits(tapeAt, r.from, r.to)) {
            parts.push_back({from, split});
            from = split;
        }
        parts.push_back({from, r.to});
        return queue.insert(at, parts.begin(), parts.end()) + ptrdiff_t(parts.size());
    };
    enqueue({0, uint32_t(tape.size())}, queue.end());
    while (!queue.empty() && out.layings < 10000) {
        const raw::TapeRange range = queue.front();
        queue.pop_front();
        if (range.from == range.to)
            continue;
        LoopProgram prog;
        for (uint32_t w = 0; w < kWordCount; ++w)
            prog.gpu.put(word(w), words[w]);
        std::map<uint32_t, uint32_t> launchOf;
        bool laid = true;
        for (uint32_t at = range.from; at < range.to; ++at) {
            const Op &op = tape[at];
            switch (op.kind) {
            case Op::Dispatch:
                launchOf[prog.dispatch()] = at;
                break;
            case Op::If:
                prog.startIf(op.w, op.compare, op.reference);
                break;
            case Op::Else:
                prog.startElse();
                break;
            case Op::EndIf:
                prog.endIf();
                break;
            case Op::While:
                laid &= prog.startWhile(op.w, op.compare, op.reference) == Answer::Done;
                break;
            case Op::EndWhile:
                laid &= prog.endWhile(at) == Answer::Done;
                break;
            case Op::DoWhile:
                laid &= prog.startDoWhile() == Answer::Done;
                break;
            case Op::EndDoWhile:
                laid &= prog.endDoWhile(op.w, op.compare, op.reference, at) == Answer::Done;
                break;
            }
        }
        if (!laid) {
            ADD_FAILURE() << "a loop of [" << range.from << ", " << range.to << ") was not laid down";
            return out;
        }
        prog.run(sees, [&](uint32_t id, Gpu &gpu) {
            const Op &op = tape[launchOf[id]];
            out.ran.push_back(launchOf[id]);
            Words now;
            for (uint32_t w = 0; w < kWordCount; ++w)
                now[w] = gpu.word(word(w));
            effect(op, now);
            for (uint32_t w = 0; w < kWordCount; ++w)
                gpu.put(word(w), now[w]);
        });
        EXPECT_TRUE(prog.qmdsIntact);
        for (uint32_t w = 0; w < kWordCount; ++w)
            words[w] = prog.gpu.word(word(w));
        ++out.layings;
        if (const raw::LoopContinue loop = prog.record(); loop.continues) {
            ++out.continuations;
            const auto pieces = raw::continuation(tapeAt, range.from, range.to, loop.tag);
            EXPECT_FALSE(pieces.empty()) << "tag " << loop.tag;
            auto at = queue.begin();
            for (const raw::TapeRange &piece : pieces)
                at = enqueue(piece, at);
        }
    }
    return out;
}

void expectMetal(const std::vector<Op> &tape, Words start, uint32_t continuations, const char *shape)
{
    Words want = start;
    const std::vector<uint32_t> metalRan = metal(tape, want);
    for (bool sees : {true, false}) {
        Words got = start;
        const Submitted s = submit(tape, got, sees);
        EXPECT_EQ(s.ran, metalRan) << shape << " sees " << sees;
        EXPECT_EQ(got, want) << shape;
        EXPECT_GE(s.continuations, continuations) << shape;
    }
}

TEST(RawLoopContinuation, AWhileNoBlockHoldsRunsPastItsCopies)
{
    expectMetal({dispatch(1, 1), test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0), dispatch(1, 1)},
                {2500}, 2, "while");
}

TEST(RawLoopContinuation, AWhileInsideAnIfRunsPastItsCopiesAndTheElseStaysSkipped)
{
    for (uint32_t taken : {0u, 1u})
        expectMetal({test(Op::If, 2, PredicateCompare::Equal, 1), test(Op::While, 0), dispatch(0, -1),
                     test(Op::EndWhile, 0), dispatch(3, 1), test(Op::Else, 0), dispatch(4, 1), test(Op::EndIf, 0),
                     dispatch(5, 1)},
                    {1500, 0, taken}, taken, "while inside if");
}

TEST(RawLoopContinuation, AWhileInsideAnElseRunsPastItsCopies)
{
    expectMetal({test(Op::If, 2, PredicateCompare::Equal, 1), dispatch(4, 1), test(Op::Else, 0), test(Op::While, 0),
                 dispatch(0, -1), test(Op::EndWhile, 0), dispatch(3, 1), test(Op::EndIf, 0)},
                {1100, 0, 0}, 1, "while inside else");
}

// for (outer) { inner = 300; while (inner) inner--; after++ }: the inner loop runs past its 128 copies each time
TEST(RawLoopContinuation, AnInnerLoopPastItsCopiesFinishesAndItsOuterLoopGoesOn)
{
    expectMetal({test(Op::While, 0), dispatch(0, -1, 1, 300), test(Op::While, 1), dispatch(1, -1),
                 test(Op::EndWhile, 1), dispatch(2, 1), test(Op::EndWhile, 0), dispatch(3, 1)},
                {3}, 3, "while inside while");
}

TEST(RawLoopContinuation, ADoWhileInsideAWhileInsideAnIfRunsPastItsCopies)
{
    expectMetal({test(Op::If, 5, PredicateCompare::Equal, 0), test(Op::While, 0), dispatch(0, -1, 1, 200),
                 test(Op::DoWhile, 0), dispatch(1, -1), test(Op::EndDoWhile, 1), dispatch(2, 1),
                 test(Op::EndWhile, 0), test(Op::Else, 0), dispatch(4, 1), test(Op::EndIf, 0)},
                {2}, 2, "do-while inside while inside if");
}

// Four loops deep, each with fewer copies of its own: the innermost (two copies) continues every other iteration
TEST(RawLoopContinuation, LoopsFourDeepRunAsMetalRunsThem)
{
    std::vector<Op> tape;
    for (uint32_t d = 0; d < 4; ++d) {
        tape.push_back(test(Op::While, d));
        tape.push_back(dispatch(d, -1, d + 1, 3));
    }
    tape.push_back(dispatch(5, 1));
    for (uint32_t d = 4; d-- > 0;)
        tape.push_back(test(Op::EndWhile, d));
    expectMetal(tape, {3, 0, 0, 0, 0}, 1, "four deep");
}

TEST(RawLoopContinuation, NothingContinuesWithinItsCopies)
{
    for (uint32_t n : {0u, 1u, 100u}) {
        Words got{n};
        const Submitted s = submit({test(Op::While, 0), dispatch(0, -1), test(Op::EndWhile, 0)}, got, true);
        EXPECT_EQ(s.continuations, 0u);
        EXPECT_EQ(s.ran.size(), n);
    }
}

}  // namespace
