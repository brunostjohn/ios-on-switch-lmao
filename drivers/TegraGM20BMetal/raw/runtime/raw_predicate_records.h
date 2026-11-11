// SPDX-License-Identifier: MIT
// Metal's answers to the predication records as a command buffer takes them (the encode comes later). A unit is a run of
// compute encoders of one dispatch type; any other encoder, an event or the command buffer's end closes it, and with it
// every block still open (m2v's measurements, metal2vulkan-runtime/src/predication.rs).
#pragma once
#include <cstdint>
#include <vector>

namespace raw {

class PredicateRecords {
public:
    enum Kind : uint8_t {
        StartIf = 1,
        StartElse,
        EndIf,
        StartWhile,
        EndWhile,
        StartDoWhile,
        EndDoWhile,
    };

    void computeEncoder(uint32_t dispatchType)
    {
        if (!unit_ || type_ != dispatchType)
            open_.clear();
        unit_ = true;
        type_ = dispatchType;
    }

    void otherWork()
    {
        unit_ = false;
        open_.clear();
    }

    // Metal's answer for an End (YES where it closes a block of its kind)
    bool record(Kind k)
    {
        recorded_ = true;
        switch (k) {
        case StartIf:
            open_.push_back(Block::If);
            return true;
        case StartElse:
            if (open_.empty() || open_.back() != Block::If)
                return misuse();
            open_.back() = Block::Else;
            return true;
        case EndIf:
            if (open_.empty() || open_.back() == Block::Loop)
                return misuse();
            open_.pop_back();
            return true;
        case StartWhile:
        case StartDoWhile:
            loops_ = true;
            open_.push_back(Block::Loop);
            return true;
        case EndWhile:
        case EndDoWhile:
            loops_ = true;
            if (open_.empty() || open_.back() != Block::Loop)
                return misuse();
            open_.pop_back();
            return true;
        }
        return misuse();
    }

    bool recorded() const
    {
        return recorded_;
    }

    // A record with no block of its kind open: Metal kills or drops work by rules raw does not model
    bool misused() const
    {
        return misused_;
    }

    // Loops run as GPU-tested copies of their body, which the raw path does not lay down
    bool loops() const
    {
        return loops_;
    }

    size_t depth() const
    {
        return open_.size();
    }

private:
    enum class Block : uint8_t {
        If,
        Else,
        Loop,
    };

    bool misuse()
    {
        misused_ = true;
        return false;
    }

    std::vector<Block> open_;
    uint32_t type_ = 0;
    bool unit_ = false, recorded_ = false, misused_ = false, loops_ = false;
};

}  // namespace raw
