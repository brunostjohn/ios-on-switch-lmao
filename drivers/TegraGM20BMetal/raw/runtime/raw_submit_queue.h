// SPDX-License-Identifier: MIT
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <vector>

namespace raw {

template <typename Key, typename Value> class SubmitQueue {
    struct Entry {
        Value value;
        bool committed = false;
        bool held = false;
    };

    struct Queue {
        Key key;
        std::deque<Entry> entries;
        bool held = false;
    };

    std::list<Queue> queues_;
    size_t size_ = 0;
    size_t heldQueues_ = 0;

public:
    void reserve(Key key, Value value)
    {
        Queue *unused = nullptr;
        for (Queue &q : queues_) {
            if (q.key == key) {
                q.entries.push_back({value});
                ++size_;
                return;
            }
            if (q.entries.empty())
                unused = &q;
        }
        if (unused) {
            heldQueues_ -= unused->held;
            unused->key = key;
            unused->held = false;
            unused->entries.push_back({value});
        } else {
            queues_.push_back({key, {{value}}});
        }
        ++size_;
    }

    bool commit(Key key, Value value)
    {
        for (Queue &q : queues_)
            if (q.key == key)
                for (Entry &e : q.entries)
                    if (e.value == value) {
                        e.committed = true;
                        return true;
                    }
        return false;
    }

    template <typename Ready> Value ready(Ready eligible) const
    {
        for (const Queue &q : queues_) {
            if (q.entries.empty())
                continue;
            const Entry &e = q.entries.front();
            if (e.committed && eligible(e.value))
                return e.value;
        }
        return {};
    }

    void heads(std::vector<Value> &out) const
    {
        out.clear();
        for (const Queue &q : queues_)
            if (!q.entries.empty() && q.entries.front().committed)
                out.push_back(q.entries.front().value);
    }

    bool retire(Key key, Value value)
    {
        for (auto at = queues_.begin(); at != queues_.end(); ++at) {
            if (at->key != key)
                continue;
            if (at->entries.empty() || at->entries.front().value != value)
                return false;
            at->entries.pop_front();
            --size_;
            queues_.splice(queues_.end(), queues_, at);
            return true;
        }
        return false;
    }

    // The queue's head waits for what Metal's GPU, not its scheduler, waits for (raw_scheduled_wait.h holds): it and every
    // job committed behind it count as scheduled. `held`: those not held before.
    void hold(Key key, std::vector<Value> &held)
    {
        for (Queue &q : queues_)
            if (q.key == key) {
                heldQueues_ += !q.held;
                q.held = true;
                for (Entry &e : q.entries)
                    if (e.committed && !e.held) {
                        e.held = true;
                        held.push_back(e.value);
                    }
                return;
            }
    }

    // The queue's head goes on: `released`, the jobs its hold held
    void release(Key key, std::vector<Value> &released)
    {
        if (!heldQueues_)
            return;
        for (Queue &q : queues_)
            if (q.key == key) {
                if (!q.held)
                    return;
                --heldQueues_;
                q.held = false;
                for (Entry &e : q.entries)
                    if (e.held) {
                        e.held = false;
                        released.push_back(e.value);
                    }
                return;
            }
    }

    // A partially submitted job retains its queue reservation while other queues get a scheduling opportunity.
    void yield(Key key)
    {
        for (auto at = queues_.begin(); at != queues_.end(); ++at)
            if (at->key == key) {
                queues_.splice(queues_.end(), queues_, at);
                return;
            }
    }

    size_t size() const
    {
        return size_;
    }

    bool empty() const
    {
        return size_ == 0;
    }
};

enum class EventReadiness {
    ready,
    blocked,
    expired
};

class EventDeadline {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    EventDeadline(Time start, uint32_t timeoutMs)
        : until_(start + (timeoutMs ? std::chrono::milliseconds(timeoutMs) : std::chrono::seconds(10))),
          timed_(timeoutMs != 0)
    {}

    EventReadiness state(bool covered, Time now) const
    {
        if (covered || (timed_ && now >= until_))
            return EventReadiness::ready;
        return now < until_ ? EventReadiness::blocked : EventReadiness::expired;
    }

    Time until() const
    {
        return until_;
    }

private:
    Time until_;
    bool timed_;
};

}  // namespace raw
