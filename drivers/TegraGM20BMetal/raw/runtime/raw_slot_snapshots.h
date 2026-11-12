// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace raw {

// The captured values of a table of N slots, as StateSnapshots keeps them, each held as its chunks of K slots in an
// append-only arena: a capture copies only the chunks with a slot edited since the capture before, and names the earlier
// capture's chunks for the rest. A draw's setBytes edit a few slots of a 31-slot table: a capture copies one chunk.
template <typename Slot, size_t N, size_t K = 4> class SlotSnapshots {
public:
    using Table = std::array<Slot, N>;
    using Chunk = std::array<Slot, K>;
    static constexpr size_t kChunks = (N + K - 1) / K;
    using Header = std::array<uint32_t, kChunks>;

    class View {
    public:
        View(const Chunk *chunks, const Header &header) : chunks_(chunks), header_(&header)
        {}

        const Slot &operator[](size_t slot) const
        {
            return chunks_[(*header_)[slot / K]][slot % K];
        }

        static constexpr size_t size()
        {
            return N;
        }

        class Iterator {
        public:
            Iterator(const View &view, size_t slot) : view_(&view), slot_(slot)
            {}

            const Slot &operator*() const
            {
                return (*view_)[slot_];
            }

            Iterator &operator++()
            {
                ++slot_;
                return *this;
            }

            bool operator!=(const Iterator &other) const
            {
                return slot_ != other.slot_;
            }

        private:
            const View *view_;
            size_t slot_;
        };

        Iterator begin() const
        {
            return Iterator(*this, 0);
        }

        Iterator end() const
        {
            return Iterator(*this, N);
        }

    private:
        const Chunk *chunks_;
        const Header *header_;
    };

    const Table &current() const
    {
        return current_;
    }

    View operator[](uint32_t index) const
    {
        return View(chunks_.data(), headers_[index]);
    }

    uint64_t generation() const
    {
        return generation_;
    }

    size_t size() const
    {
        return headers_.size();
    }

    size_t storageBytes() const
    {
        return headers_.capacity() * sizeof(Header) + chunks_.capacity() * sizeof(Chunk) +
               changed_.capacity() * sizeof(uint32_t);
    }

    template <typename Edit> bool update(Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        pending_ = edited_ = ~0u;
        return true;
    }

    // An edit of slot `slot` alone
    template <typename Edit> bool update(uint32_t slot, Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        const uint32_t bit = slot < 32 ? 1u << slot : ~0u;
        pending_ |= bit;
        edited_ |= bit;
        return true;
    }

    uint32_t capture()
    {
        if (captured_ == generation_)
            return index_;
        Header header;
        const bool first = headers_.empty();
        for (size_t c = 0; c < kChunks; ++c) {
            if (!first && !(pending_ & chunkSlots(c))) {
                header[c] = headers_.back()[c];
                continue;
            }
            header[c] = static_cast<uint32_t>(chunks_.size());
            Chunk &chunk = chunks_.emplace_back();
            for (size_t k = 0; k < K; ++k)
                chunk[k] = c * K + k < N ? current_[c * K + k] : Slot{};
        }
        index_ = static_cast<uint32_t>(headers_.size());
        headers_.push_back(header);
        changed_.push_back(pending_);
        pending_ = 0;
        captured_ = generation_;
        return index_;
    }

    // The slots in which value `index` may differ from value `from`: exactly the edited ones where `index` was captured
    // next after `from`, every slot otherwise
    uint32_t changedFrom(uint32_t from, uint32_t index) const
    {
        return index == from + 1 ? changed_[index] : ~0u;
    }

    // Only the slots edited since the last reset differ from the empty table
    void resetCurrent()
    {
        current_ = {};
        ++generation_;
        pending_ |= edited_;
        edited_ = 0;
    }

    void clear()
    {
        headers_.clear();
        chunks_.clear();
        changed_.clear();
        pending_ = ~0u;
        edited_ = 0;
        current_ = {};
        generation_ = 0;
        captured_ = std::numeric_limits<uint64_t>::max();
        index_ = std::numeric_limits<uint32_t>::max();
    }

private:
    static constexpr uint32_t chunkSlots(size_t chunk)
    {
        return chunk * K >= 32 ? ~0u : uint32_t(((uint64_t(1) << K) - 1) << (chunk * K));
    }

    Table current_{};
    uint64_t generation_ = 0;
    uint64_t captured_ = std::numeric_limits<uint64_t>::max();
    uint32_t index_ = std::numeric_limits<uint32_t>::max();
    std::vector<Header> headers_;
    std::vector<Chunk> chunks_;
    std::vector<uint32_t> changed_;
    uint32_t pending_ = ~0u;
    uint32_t edited_ = 0;
};

}  // namespace raw
