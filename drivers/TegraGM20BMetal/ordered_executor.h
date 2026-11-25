// SPDX-License-Identifier: MIT
#pragma once
#include "follow.h"
#include "handoff_queue.h"
#include "lock.h"
#include <pthread.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace gm20b {

// Serial executor backed by one lazily started follower pthread with a known Mach thread port, so
// waitUntilScheduled can donate priority via OwnedWord across scheduled-handler execution with no
// ownerless gap and without spawning a priority-31 root-queue thread under CPU saturation.
class OrderedExecutor {
public:
    OrderedExecutor() = default;

    ~OrderedExecutor()
    {
        stop();
    }

    OrderedExecutor(const OrderedExecutor &) = delete;
    OrderedExecutor &operator=(const OrderedExecutor &) = delete;

    uint32_t ownerPort()
    {
        const uint32_t existing = owner_.load(std::memory_order_acquire);
        if (existing)
            return existing;
        std::lock_guard<gm20b::Mutex> l(lock_);
        return ensureStartedLocked();
    }

    void enqueue(std::function<void()> fn)
    {
        if (!fn)
            return;
        std::shared_ptr<State> state;
        {
            std::lock_guard<gm20b::Mutex> l(lock_);
            ensureStartedLocked();
            state = state_;
        }
        if (state)
            state->queue.push(std::move(fn));
    }

    void stop()
    {
        std::shared_ptr<State> state;
        pthread_t thread = nullptr;
        bool hasThread = false;
        {
            std::lock_guard<gm20b::Mutex> l(lock_);
            if (!started_)
                return;
            started_ = false;
            hasThread = true;
            thread = thread_;
            state = std::move(state_);
            owner_.store(0, std::memory_order_release);
        }
        if (state)
            state->queue.push(std::function<void()>{});
        if (hasThread) {
            if (pthread_equal(pthread_self(), thread))
                pthread_detach(thread);
            else
                pthread_join(thread, nullptr);
        }
    }

private:
    struct State {
        HandoffQueue<std::function<void()>> queue;
    };

    static void *run(void *arg)
    {
        std::unique_ptr<std::shared_ptr<State>> holder(static_cast<std::shared_ptr<State> *>(arg));
        std::shared_ptr<State> state = std::move(*holder);
        holder.reset();
        pthread_setname_np("com.brunostjohn.switch.gm20b.handlers");
        if (!followers().isFixed())
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        followers().join();
        for (;;) {
            std::function<void()> fn = std::move(state->queue.front());
            state->queue.pop();
            if (!fn)
                return nullptr;
            fn();
        }
    }

    uint32_t ensureStartedLocked()
    {
        if (started_)
            return owner_.load(std::memory_order_acquire);
        state_ = std::make_shared<State>();
        auto *arg = new std::shared_ptr<State>(state_);
        pthread_attr_t attr;
        followers().initAttr(&attr);
        const int err = pthread_create(&thread_, &attr, &OrderedExecutor::run, arg);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            delete arg;
            state_.reset();
            return 0;
        }
        started_ = true;
        const uint32_t port = uint32_t(pthread_mach_thread_np(thread_));
        owner_.store(port, std::memory_order_release);
        return port;
    }

    gm20b::Mutex lock_;
    std::shared_ptr<State> state_;
    pthread_t thread_ = nullptr;
    bool started_ = false;
    std::atomic<uint32_t> owner_{0};
};

}  // namespace gm20b
