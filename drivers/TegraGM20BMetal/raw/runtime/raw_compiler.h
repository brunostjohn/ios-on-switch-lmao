// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "raw_cache.h"
#include "raw_stats.h"

namespace raw {

class CompileSlots;

// The states this process completes: each asked state made once, by the disk cache or `make` (the probe and record), with per-key exclusion
// on bounded user-initiated workers, or on the creating thread (makeNow). A later ask of a state answers its result or
// that it is still being made. A refusal sticks; a probe that could not run is tried again by a later ask, kRetries times at
// most, each after a wait four times the last's (the first `retryUs`).
class Compiler {
public:
    enum class Outcome : uint8_t {
        Made,
        Refused,  // the state's own reason (kept on disk: the same probe would refuse it again)
        Failed    // the probe could not run (not kept: a later process may)
    };

    using Make = std::function<Outcome(const CacheKey &k, RuntimeRecord &record, std::string &why)>;
    // Called on the queue once the state is made (`record` null: refused, `why`)
    using Done = std::function<void(const RuntimeRecord *record, const std::string &why)>;

    static constexpr uint32_t kRetries = 3;

    Compiler(RecordCache *cache, Make make, uint64_t retryUs = 1000000);
    ~Compiler();

    enum class State : uint8_t {
        Pending,
        Ready,
        Refused,
        Failed
    };

    // The state's result so far; the first ask of it queues its making. Every ask made while it is being made has its `done`
    // called once it is
    State ask(const CacheKey &k, Done done, const RuntimeRecord **record = nullptr);
    // The state's record or refusal if it is made or on disk, on the caller's thread: what a state's creation takes at once (the
    // disk's entry is kept as the state's result). None: not made yet (being made, or never asked)
    RecordCache::Found lookup(const CacheKey &k, RuntimeRecord &record, std::string &why);
    // The state made now, on the caller's thread (sharing the bounded compile slots with queued work), as a queued ask would: Ready with
    // its record, Refused, Failed (a later ask tries again), or Pending (the queue is making it)
    State makeNow(const CacheKey &k, RuntimeRecord &record, std::string &why);
    // A state whose probe failed and may be tried again: true, and in *us how long until an ask tries it
    bool retryAfter(const CacheKey &k, uint64_t &us) const;
    void drain();  // host tests: waits for everything asked so far
    void enqueue(std::function<void()> work);

    struct Stats {
#if SWITCH_RAW_DIAGNOSTICS
        uint64_t hits = 0, misses = 0, refusals = 0, failures = 0, makeUs = 0;
        uint64_t lookupHits = 0, lookupUs = 0;  // hits found by lookup() at a state's creation, and their cost
#endif
    };

    Stats stats() const;

private:
    struct Entry {
        State state = State::Pending;
        std::unique_ptr<RuntimeRecord> record;
        std::string why;
        uint32_t failures = 0;
        uint64_t failedUs = 0;
        std::vector<Done> waiters;  // the asks made while it was being made
    };

    struct Job;
    struct Store;
    struct Task;

    RecordCache *cache_;
    Make make_;
    uint64_t retryUs_;
    void *queue_;
    void *persistence_;
    std::unique_ptr<CompileSlots> slots_;  // at most two cold compiles; unrelated warm keys never take a slot
    mutable std::mutex lock_;
    std::condition_variable idle_;
    uint64_t queued_ = 0;
    std::map<uint64_t, Entry> entries_;
#if SWITCH_RAW_DIAGNOSTICS
    Stats stats_;
#endif
    static void run(void *job);
    static void store(void *job);
    static void runTask(void *task);
    Outcome produce(const CacheKey &k, RuntimeRecord &record, std::string &why, RecordCache::Found &found);
    std::vector<Done> settle(const CacheKey &k, Outcome outcome, RecordCache::Found found,
                             std::unique_ptr<RuntimeRecord> record, const std::string &why, uint64_t began,
                             const RuntimeRecord **made);
    void queue(const CacheKey &k);
    bool retryable(const Entry &e, uint64_t &wait) const;
};

}  // namespace raw
