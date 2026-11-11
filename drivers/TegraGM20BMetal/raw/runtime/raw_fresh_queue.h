// SPDX-License-Identifier: MIT
// The new textures whose first contents (raw_fresh.h) the raw channel has not written yet. A texture is added as it is made,
// before any command buffer can name it; the next raw submission takes every one and writes them ahead of its own commands, on
// its own channel, so every later command on that channel (that submission's included) finds them written. A submission of the
// other driver goes after a raw one that took them (RawMetalSubmit.mm's Order before m2v work), so its commands find them
// written too. A submission that fails gives them back for the next.
#pragma once
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace raw {

template <class Entry> class FreshQueue {
public:
    void add(Entry entry)
    {
        std::lock_guard<std::mutex> l(lock_);
        pending_.push_back(std::move(entry));
        any_.store(true, std::memory_order_release);
    }

    bool any() const
    {
        return any_.load(std::memory_order_acquire);
    }

    std::vector<Entry> take()
    {
        std::vector<Entry> taken;
        if (!any())
            return taken;
        std::lock_guard<std::mutex> l(lock_);
        taken.swap(pending_);
        any_.store(false, std::memory_order_release);
        return taken;
    }

    void settle(std::vector<Entry> &&taken, bool submitted)
    {
        if (submitted || taken.empty())
            return;
        std::lock_guard<std::mutex> l(lock_);
        taken.insert(taken.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
        pending_.swap(taken);
        any_.store(true, std::memory_order_release);
    }

private:
    std::mutex lock_;
    std::vector<Entry> pending_;
    std::atomic<bool> any_{false};
};

}  // namespace raw
