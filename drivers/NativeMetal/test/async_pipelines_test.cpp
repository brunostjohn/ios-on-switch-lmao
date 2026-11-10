// SPDX-License-Identifier: MIT
#include "../AsyncPipelines.h"
#include <gtest/gtest.h>

TEST(AsyncPipelines, LazyObservationStartsWorkAndPublishesBeforeCallbacks)
{
    auto state = std::make_shared<nbasync::Making>();
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false, release = false, notified = false;
    std::atomic<unsigned> makes{0};
    state->work = [&](NSString **) -> id {
        makes++;
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        return [NSObject new];
    };
    nbasync::startLazy(state);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   EXPECT_EQ(nbasync::observe(state), 1);  // callback must run outside the state lock
                                   std::lock_guard<std::mutex> guard(lock);
                                   notified = true;
                                   changed.notify_all();
                               }),
              0);
    EXPECT_EQ(nbasync::observe(state), 0);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered;
        }));
        release = true;
        changed.notify_all();
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return notified;
        }));
    }
    EXPECT_EQ(makes.load(), 1u);
    EXPECT_EQ(nbasync::observe(state), 1);
    EXPECT_TRUE(nbasync::ready(state));
}

TEST(AsyncPipelines, FailedCreationIsTerminalAndNotifiesEveryObserver)
{
    auto state = std::make_shared<nbasync::Making>();
    state->work = [](NSString **failure) -> id {
        *failure = @"test refusal";
        return nil;
    };
    unsigned callbacks = 0;
    // Non-lazy queued state: observation is separate from its executor, so register both before running it.
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              0);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              0);
    EXPECT_TRUE(nbasync::run(state));
    EXPECT_FALSE(nbasync::run(state));
    EXPECT_EQ(callbacks, 2u);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              -1);
    EXPECT_EQ(callbacks, 2u);
    EXPECT_FALSE(nbasync::ready(state));
}

TEST(AsyncPipelines, ObservingCompletionCannotLoseTheNotification)
{
    for (unsigned i = 0; i < 100; ++i) {
        auto state = std::make_shared<nbasync::Making>();
        state->work = [](NSString **) -> id {
            return [NSObject new];
        };
        std::atomic<unsigned> callbacks{0};
        std::thread worker([state] {
            @autoreleasepool {
                nbasync::run(state);
            }
        });
        const int observed = nbasync::observe(state, [&] {
            callbacks++;
        });
        worker.join();
        EXPECT_TRUE(observed == 0 || observed == 1);
        EXPECT_EQ(callbacks.load(), observed == 0 ? 1u : 0u);
        EXPECT_EQ(nbasync::observe(state), 1);
    }
}
