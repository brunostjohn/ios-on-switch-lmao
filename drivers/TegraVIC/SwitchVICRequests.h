// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// IOSurfaceAccelerator requests waiting for VIC's engine thread. One whose events have not fired counts against its own
// connection, a runnable one against the shared bound, so a client whose events never fire stalls only itself. The
// caller holds one lock across every call.
//
// A request names the surfaces it reads or writes (`touched`, null where none). It may take its destination's write
// reservation while queued only if no listed request, and not the one the engine thread runs, touches that surface:
// held by a later request, it stalls an earlier one that runs first on the surface (pop takes the first runnable one),
// which waits on it from the engine thread. `reserving` marks one taking it, which nothing behind it overtakes.
template <class Request> class SwitchVICRequests {
public:
    static constexpr uint32_t kRunnable = 16, kWaitingPerConnection = 16;

    bool admits(const Request &request) const
    {
        return request.eventReady ? runnable < kRunnable : request.conn->waiting < kWaitingPerConnection;
    }

    void push(Request *request)
    {
        request->next = nullptr;
        request->listed = true;
        if (tail)
            tail->next = request;
        else
            head = request;
        tail = request;
        if (request->eventReady)
            ++runnable;
        else
            ++request->conn->waiting;
    }

    // Its events fired (status 0) or failed. Called before the request is listed, it only marks it.
    void ready(Request *request, int status)
    {
        if (!request->eventStatus)
            request->eventStatus = status;
        if (request->eventReady)
            return;
        request->eventReady = true;
        if (request->listed) {
            --request->conn->waiting;
            ++runnable;
        }
    }

    // Runs as a failure: the engine thread finishes it with status and gives VIC nothing.
    void fail(Request *request, int status)
    {
        request->eventStatus = status;
        ready(request, status);
    }

    bool alone(const void *surface) const
    {
        if (!surface || running[0] == surface || running[1] == surface)
            return false;
        for (const auto *request = head; request; request = request->next) {
            if (request->touched[0] == surface || request->touched[1] == surface)
                return false;
        }
        return true;
    }

    void reserved(Request *request)
    {
        request->reserving = false;
    }

    // The popped request's run is over: its surfaces are not touched any more.
    void finished()
    {
        running[0] = running[1] = nullptr;
    }

    template <class Connection> void cancel(const Connection *conn, int status)
    {
        for (auto *request = head; request; request = request->next)
            if (request->conn == conn)
                fail(request, status);
    }

    Request *pop()
    {
        Request *previous = nullptr;
        for (auto *request = head; request; previous = request, request = request->next) {
            if (request->reserving)
                return nullptr;
            if (!request->eventReady)
                continue;
            (previous ? previous->next : head) = request->next;
            if (tail == request)
                tail = previous;
            request->next = nullptr;
            request->listed = false;
            --runnable;
            running[0] = request->touched[0];
            running[1] = request->touched[1];
            return request;
        }
        return nullptr;
    }

private:
    Request *head = nullptr, *tail = nullptr;
    uint32_t runnable = 0;
    const void *running[2] = {};
};
