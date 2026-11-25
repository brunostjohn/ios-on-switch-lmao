// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "drivers/TegraVIC/SwitchVICRequests.h"

namespace {
struct Conn {
    uint32_t waiting = 0;
};

struct Request {
    Request *next = nullptr;
    Conn *conn = nullptr;
    bool eventReady = false, listed = false;
    int eventStatus = 0;
    const void *touched[2] = {};
    bool reserving = false;
};

using Requests = SwitchVICRequests<Request>;

std::vector<Request> make(Conn *conn, uint32_t count, bool ready)
{
    std::vector<Request> requests(count);
    for (auto &request : requests) {
        request.conn = conn;
        request.eventReady = ready;
    }
    return requests;
}
}  // namespace

TEST(SwitchVICRequests, ClientWaitingOnEventsThatNeverFireStallsOnlyItself)
{
    Requests queue;
    Conn stuck, other;
    auto waiting = make(&stuck, Requests::kWaitingPerConnection + 1, false);
    for (uint32_t i = 0; i < Requests::kWaitingPerConnection; ++i) {
        ASSERT_TRUE(queue.admits(waiting[i]));
        queue.push(&waiting[i]);
    }
    EXPECT_FALSE(queue.admits(waiting.back()));
    auto runnable = make(&other, Requests::kRunnable, true);
    for (auto &request : runnable) {
        ASSERT_TRUE(queue.admits(request));
        queue.push(&request);
    }
    for (auto &request : runnable)
        EXPECT_EQ(queue.pop(), &request);
    EXPECT_EQ(queue.pop(), nullptr);
    auto otherWaiting = make(&other, 1, false);
    EXPECT_TRUE(queue.admits(otherWaiting[0]));
}

TEST(SwitchVICRequests, FiredEventsMoveARequestToTheRunnableBound)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, 2, false);
    queue.push(&requests[0]);
    queue.push(&requests[1]);
    EXPECT_EQ(conn.waiting, 2u);
    queue.ready(&requests[1], 0);
    EXPECT_EQ(conn.waiting, 1u);
    EXPECT_EQ(queue.pop(), &requests[1]);
    EXPECT_EQ(queue.pop(), nullptr);
    queue.ready(&requests[0], -5);
    EXPECT_EQ(conn.waiting, 0u);
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_EQ(requests[0].eventStatus, -5);
    EXPECT_FALSE(requests[0].listed);
}

TEST(SwitchVICRequests, EventsFiringBeforeTheRequestIsListedOnlyMarkIt)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, 1, false);
    queue.ready(&requests[0], 0);
    EXPECT_EQ(conn.waiting, 0u);
    queue.push(&requests[0]);
    EXPECT_EQ(conn.waiting, 0u);
    EXPECT_EQ(queue.pop(), &requests[0]);
}

TEST(SwitchVICRequests, CloseFailsEveryQueuedRequestOfItsConnectionOnly)
{
    Requests queue;
    Conn closing, other;
    auto mine = make(&closing, 3, false);
    mine[2].eventReady = true;
    auto theirs = make(&other, 1, false);
    for (auto &request : mine)
        queue.push(&request);
    queue.push(&theirs[0]);
    queue.cancel(&closing, -125);
    EXPECT_EQ(closing.waiting, 0u);
    EXPECT_EQ(other.waiting, 1u);
    for (auto &request : mine) {
        EXPECT_EQ(queue.pop(), &request);
        EXPECT_EQ(request.eventStatus, -125);
    }
    EXPECT_EQ(queue.pop(), nullptr);
    EXPECT_EQ(theirs[0].eventStatus, 0);
}

TEST(SwitchVICRequests, RunnableBoundCountsOnlyQueuedRunnableRequests)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, Requests::kRunnable + 1, true);
    for (uint32_t i = 0; i < Requests::kRunnable; ++i)
        queue.push(&requests[i]);
    EXPECT_FALSE(queue.admits(requests.back()));
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_TRUE(queue.admits(requests.back()));
}

TEST(SwitchVICRequests, ASurfaceIsAloneOnlyWhenNoListedOrRunningRequestTouchesIt)
{
    Requests queue;
    Conn conn;
    int source = 0, destination = 0, other = 0;
    auto requests = make(&conn, 1, true);
    requests[0].touched[0] = &destination;
    requests[0].touched[1] = &source;
    EXPECT_TRUE(queue.alone(&destination));
    queue.push(&requests[0]);
    EXPECT_FALSE(queue.alone(&destination));
    EXPECT_FALSE(queue.alone(&source));
    EXPECT_TRUE(queue.alone(&other));
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_FALSE(queue.alone(&destination)) << "the engine thread runs it";
    queue.finished();
    EXPECT_TRUE(queue.alone(&destination));
    EXPECT_TRUE(queue.alone(&source));
}

TEST(SwitchVICRequests, NothingOvertakesARequestTakingItsReservation)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, 2, true);
    requests[0].reserving = true;
    queue.push(&requests[0]);
    queue.push(&requests[1]);
    EXPECT_EQ(queue.pop(), nullptr);
    queue.reserved(&requests[0]);
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_EQ(queue.pop(), &requests[1]);
}

namespace {
// A destination's write reservation as switch_gpu_backing_conversion_reserve grants it, to one holder at a time.
struct Destination {
    std::mutex lock;
    std::condition_variable changed;
    const void *holder = nullptr;

    bool tryReserve(const void *who)
    {
        std::lock_guard held(lock);
        if (holder)
            return false;
        holder = who;
        return true;
    }

    void reserve(const void *who)
    {
        std::unique_lock held(lock);
        changed.wait(held, [&] { return !holder; });
        holder = who;
    }

    bool heldBy(const void *who)
    {
        std::lock_guard held(lock);
        return holder == who;
    }

    void release(const void *who)
    {
        std::lock_guard held(lock);
        if (holder == who) {
            holder = nullptr;
            changed.notify_all();
        }
    }
};

// TegraVICUserClient's queue and engine thread around SwitchVICRequests: queueJob decides under the queue's lock and
// reserves outside it; the engine thread pops, runs, and marks the run over.
struct Engine {
    Requests queue;
    std::mutex lock;
    std::condition_variable changed;
    Destination destination;
    std::vector<const Request *> ran;
    std::function<void(const Request &)> popped;

    void queueJob(Request *request, bool eventFree)
    {
        std::unique_lock held(lock);
        const bool reserve = eventFree && queue.alone(request->touched[0]);
        request->reserving = reserve;
        queue.push(request);
        changed.notify_all();
        held.unlock();
        if (!reserve)
            return;
        destination.tryReserve(request);
        held.lock();
        queue.reserved(request);
        changed.notify_all();
    }

    void ready(Request *request)
    {
        std::lock_guard held(lock);
        queue.ready(request, 0);
        changed.notify_all();
    }

    // A job's write of the destination: under the reservation it took when queued, else one it waits for (a record's
    // install takes one that way).
    void run(const Request &request)
    {
        if (popped)
            popped(request);
        if (!destination.heldBy(&request))
            destination.reserve(&request);
        destination.release(&request);
    }

    void serve(size_t jobs)
    {
        while (ran.size() < jobs) {
            std::unique_lock held(lock);
            Request *request = nullptr;
            changed.wait(held, [&] { return (request = queue.pop()) != nullptr; });
            held.unlock();
            run(*request);
            held.lock();
            queue.finished();
            ran.push_back(request);
        }
    }
};

// Runs the engine thread for `jobs` jobs; false if they were not all done in 5 s (the destination is then freed so
// the thread can end).
bool served(Engine &engine, size_t jobs, const Request &stuck)
{
    auto done = std::async(std::launch::async, [&] { engine.serve(jobs); });
    const bool ended = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!ended)
        engine.destination.release(&stuck);
    done.get();
    return ended;
}
}  // namespace

// Job A (with events) and then job B (without) write the same destination; A's events fire before the engine thread
// pops, so A runs first, and its record's install waits for the destination's reservation. B must not be holding it
// from when it was queued: B cannot run, and so release it, until A is done.
TEST(SwitchVICRequests, AJobQueuedBehindAnotherOnItsDestinationDoesNotStallIt)
{
    Engine engine;
    Conn conn;
    auto requests = make(&conn, 2, false);
    Request &a = requests[0], &b = requests[1];
    a.touched[0] = b.touched[0] = &engine.destination;
    engine.queueJob(&a, false);
    b.eventReady = true;
    engine.queueJob(&b, true);
    engine.ready(&a);
    EXPECT_TRUE(served(engine, 2, b)) << "A waited on B's reservation and B waited behind A";
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&a, &b}));
}

// The same with A already popped when B is queued: the engine thread's own job counts as touching the destination.
TEST(SwitchVICRequests, AJobQueuedWhileAnotherRunsOnItsDestinationDoesNotStallIt)
{
    Engine engine;
    Conn conn;
    auto requests = make(&conn, 2, true);
    Request &a = requests[0], &b = requests[1];
    a.touched[0] = b.touched[0] = &engine.destination;
    engine.popped = [&](const Request &request) {
        if (&request == &a)
            engine.queueJob(&b, true);
    };
    engine.queueJob(&a, false);
    EXPECT_TRUE(served(engine, 2, b)) << "A waited on B's reservation and B waited behind A";
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&a, &b}));
}
