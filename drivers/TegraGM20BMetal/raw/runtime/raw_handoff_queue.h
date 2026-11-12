// SPDX-License-Identifier: MIT
#pragma once
#include <dispatch/dispatch.h>
#include <atomic>
#include <cstddef>
#include <utility>

namespace raw {

// Items one thread hands to another, taken in the order given, with no lock on either side. An item moves into a node of
// the queue's own free list (a node is allocated only while the list grows to the most ever in flight), the node goes
// onto a stack the taker exchanges whole and appends to its own list. The wake counts: a hand-off that wakes signals
// once after its push, as does wake(), and the taker sleeps only after a look found its condition false, so no wake is
// slept through; a wake whose change the taker saw with an earlier one costs it one more look.
template <typename T> class HandoffQueue {
    struct Node {
        T item{};
        Node *next = nullptr;
    };

public:
    HandoffQueue() : wake_(dispatch_semaphore_create(0))
    {}

    ~HandoffQueue()
    {
        for (Node *list : {head_, given_.load(), free_.load()})
            while (list) {
                Node *next = list->next;
                delete list;
                list = next;
            }
#if !__has_feature(objc_arc)
        dispatch_release(wake_);
#endif
    }

    HandoffQueue(const HandoffQueue &) = delete;
    HandoffQueue &operator=(const HandoffQueue &) = delete;

    // The giving thread's. An item handed over with wake false leaves a sleeping taker asleep.
    void push(T &&item, bool wake = true)
    {
        Node *node = free_.load(std::memory_order_acquire);
        while (node && !free_.compare_exchange_weak(node, node->next, std::memory_order_acquire))
            ;
        if (!node)
            node = new Node;
        node->item = std::move(item);
        count_.fetch_add(1, std::memory_order_relaxed);
        node->next = given_.load(std::memory_order_relaxed);
        while (!given_.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed))
            ;
        if (wake)
            dispatch_semaphore_signal(wake_);
    }

    // Any thread's, after it changed what the taker's condition reads
    void wake()
    {
        dispatch_semaphore_signal(wake_);
    }

    // The taking thread's: every item given so far appended to its own list, until `ready(*this)` holds of them
    template <typename Ready> void waitFor(Ready &&ready)
    {
        take();
        while (!ready(static_cast<const HandoffQueue &>(*this))) {
            dispatch_semaphore_wait(wake_, DISPATCH_TIME_FOREVER);
            take();
        }
    }

    // The taking thread's: the oldest item not popped, waiting for one where there is none
    T &front()
    {
        waitFor([](const HandoffQueue &q) {
            return q.head_ != nullptr;
        });
        return head_->item;
    }

    // The taking thread's, once it is done with front(): the item counts as given no more
    void pop()
    {
        Node *node = head_;
        head_ = node->next;
        if (!head_)
            tail_ = nullptr;
        node->item = T();
        node->next = free_.load(std::memory_order_relaxed);
        while (!free_.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed))
            ;
        count_.fetch_sub(1, std::memory_order_release);
    }

    // Items given and not popped, as any thread sees it
    size_t size() const
    {
        return count_.load(std::memory_order_acquire);
    }

    // The taking thread's own list (the items the last take found), oldest first
    class Iterator {
    public:
        explicit Iterator(const Node *node) : node_(node)
        {}

        const T &operator*() const
        {
            return node_->item;
        }

        Iterator &operator++()
        {
            node_ = node_->next;
            return *this;
        }

        bool operator!=(const Iterator &other) const
        {
            return node_ != other.node_;
        }

    private:
        const Node *node_;
    };

    Iterator begin() const
    {
        return Iterator(head_);
    }

    Iterator end() const
    {
        return Iterator(nullptr);
    }

    // The taking thread's: its list's item `at` places from the oldest (at < the list's length)
    const T &at(size_t at) const
    {
        const Node *node = head_;
        while (at--)
            node = node->next;
        return node->item;
    }

private:
    void take()
    {
        Node *given = given_.exchange(nullptr, std::memory_order_acquire);
        if (!given)
            return;
        Node *first = nullptr, *last = given;
        while (given) {
            Node *next = given->next;
            given->next = first;
            first = given;
            given = next;
        }
        if (tail_)
            tail_->next = first;
        else
            head_ = first;
        tail_ = last;
    }

    std::atomic<Node *> given_{nullptr}, free_{nullptr};
    std::atomic<size_t> count_{0};
    Node *head_ = nullptr, *tail_ = nullptr;  // the taker's own list, oldest first
    dispatch_semaphore_t wake_;
};

}  // namespace raw
