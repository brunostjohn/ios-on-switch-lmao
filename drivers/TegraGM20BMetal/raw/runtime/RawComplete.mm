// SPDX-License-Identifier: MIT
// See RawComplete.h.
#import "RawComplete.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "RawDirect.h"
#include "RawMetalInternal.h"
#include "RawRenderKey.h"
#include "raw_compute.h"
#include "raw_gpu.h"

namespace raw {
namespace {

constexpr uint64_t kCacheCap = 8u << 20;

// A compute key (raw_compute_key.h) with its function constants moved last: a log line cut short keeps its shape and bindings
std::string constantsLast(const std::string &key)
{
    const size_t constants = key.find(" | "), shape = key.find(" | max=");
    if (constants == std::string::npos || shape == std::string::npos || shape < constants)
        return key;
    return key.substr(0, constants) + key.substr(shape) + " | constants" +
           key.substr(constants + 2, shape - constants - 2);
}

}  // namespace

Completion::Completion(Device &device, const char *cacheDir, uint64_t stack, const char *seedDir)
    : device_(device), stack_(stack),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.complete",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0))),
      splitQueue_(
          dispatch_queue_create("com.brunostjohn.switch.raw.split",
                                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0))),
      persistenceQueue_(
          dispatch_queue_create("com.brunostjohn.switch.raw.kernel-cache",
                                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0))),
      pending_(dispatch_group_create())
{
    const bool writable = cacheDir && *cacheDir && cache_.attach(cacheDir, stack_, kCacheCap);
    if (seedDir && *seedDir)
        cache_.attachSeed(seedDir, stack_);
    compiler_ = std::make_unique<Compiler>(&cache_, [this](const CacheKey &k, RuntimeRecord &record, std::string &why) {
        return make(k, record, why);
    });
    // the directory taken at once, for the lookups at state creation; its scan (another build's entries removed, the bytes
    // kept counted) on the queue, before any store there
    if (writable)
        dispatch_async(persistenceQueue_, ^{
            cache_.tidy();
        });
    else if (cacheDir && *cacheDir)
        NSLog(@"TegraGM20BMetal: raw records kept in memory only: %s takes no entry", cacheDir);
}

Completion::~Completion()
{
    stopping_.store(true, std::memory_order_release);
    compiler_->drain();
    dispatch_group_wait(pending_, DISPATCH_TIME_FOREVER);
    dispatch_sync(splitQueue_, ^{
                  });
    dispatch_sync(persistenceQueue_, ^{
                  });
    compiler_.reset();
}

// The state's key: the build, both functions' AIR (their entries' HASH tags in the metallibs m2v made them from) and the pack
// key, which a state of no framework's library gets both AIR hashes appended to, and a state linking functions each linked
// one's. False: a function the raw path cannot name the AIR of (made from no metallib's bytes, or not one of them)
bool Completion::keyOf(Ask &a, CacheKey &k)
{
    if (!renderCacheKey(a.descriptor, a.key, a.own, stack_, k))
        return false;
    a.key = k.key;
    return true;
}

struct Completion::Pending {
    Pending(dispatch_group_t group, std::function<void(Pipeline *)> ready)
        : group(group), ready(std::move(ready))
    {
        dispatch_group_enter(group);
    }
    ~Pending() { dispatch_group_leave(group); }
    dispatch_group_t group;
    std::function<void(Pipeline *)> ready;
    VariantPlacement<Pipeline> placement;
};

void Completion::ask(Ask asked, std::function<void(Pipeline *)> ready)
{
    request(std::move(asked), std::make_shared<Pending>(pending_, std::move(ready)));
}

void Completion::request(Ask asked, std::shared_ptr<Pending> pending)
{
    dispatch_async(queue_, ^{
        if (stopping_.load(std::memory_order_acquire)) {
            pending->ready(nullptr);
            return;
        }
        Ask a = asked;
        if (a.resolveDescriptor) {
            a.descriptor = twinDescriptor(a.descriptor);
            a.resolveDescriptor = false;
        }
        CacheKey k;
        if (!keyOf(a, k)) {
            pending->ready(nullptr);
            return;
        }
        {
            std::lock_guard<std::mutex> l(lock_);
            asked_[k.name()] = a;
        }
        const bool caTransform = a.caTransform;
        auto done = [this, k, asked, pending, caTransform](const RuntimeRecord *record, const std::string &) {
            if (record) {
                place(k, record, caTransform, pending);
                return;
            }
            uint64_t us = 0;
            if (!stopping_.load(std::memory_order_acquire) && compiler_->retryAfter(k, us))
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(us) * 1000), queue_, ^{
                    request(asked, pending);
                });
            else
                pending->ready(nullptr);
        };
        const RuntimeRecord *made = nullptr;
        const Compiler::State state = compiler_->ask(k, done, &made);
        if (state == Compiler::State::Ready && made)
            done(made, std::string());
        else if (state == Compiler::State::Refused || state == Compiler::State::Failed)
            done(nullptr, std::string());
    });
}

void Completion::place(const CacheKey &key, const RuntimeRecord *record, bool caTransform,
                       std::shared_ptr<Pending> pending)
{
    const CacheKey keptKey = key;
    dispatch_async(queue_, ^{
        if (stopping_.load(std::memory_order_acquire)) {
            pending->ready(nullptr);
            return;
        }
        const uint64_t delay = pending->placement.attempt([&](bool &retry) {
            std::string why;
            return device_.addRecord(*record, caTransform, why, &retry);
        }, [&](Pipeline *made) {
            if (made)
                splitLater(keptKey, *record);
            pending->ready(made);
        });
        if (delay)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay) * 1000), queue_, ^{
                place(keptKey, record, caTransform, pending);
            });
    });
}

Pipeline *Completion::makeNow(Ask a, bool *retry)
{
    *retry = false;
    CacheKey k;
    if (!keyOf(a, k))
        return nullptr;
    {
        std::lock_guard<std::mutex> l(lock_);
        asked_[k.name()] = a;
    }
    RuntimeRecord record;
    std::string why;
    const Compiler::State state = compiler_->makeNow(k, record, why);
    *retry = state == Compiler::State::Failed || state == Compiler::State::Pending;
    Pipeline *made = state == Compiler::State::Ready ? device_.addRecord(record, a.caTransform, why, retry) : nullptr;
    if (made)
        splitLater(k, record);
    return made;
}

Direct &Completion::direct()
{
    std::call_once(directMade_, [this] {
        direct_ = std::make_unique<Direct>(device_.pack());
    });
    return *direct_;
}

Compiler::Outcome Completion::make(const CacheKey &k, RuntimeRecord &record, std::string &why)
{
    Ask a;
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto it = asked_.find(k.name());
        if (it == asked_.end()) {
            why = "asked for no state";
            return Compiler::Outcome::Failed;
        }
        a = it->second;
    }
    ProbeTextures textures;
    textures.cubic = a.cubic;
    textures.fragmentTests = a.fragmentTests;
    const Compiler::Outcome outcome = direct().make(a.descriptor, a.key, record, why, textures);
    if (outcome != Compiler::Outcome::Made)
        once([NSString stringWithFormat:@"a state the pack lacks %s: %s | %.600s",
                                        outcome == Compiler::Outcome::Refused ? "is refused" : "was not made",
                                        why.c_str(), a.key.c_str()]);
    return outcome;
}

void Completion::backgroundKernel(std::function<void()> work)
{
    compiler_->enqueue([work = std::move(work)] {
        @autoreleasepool {
            work();
        }
    });
}

const ComputeState *Completion::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                                       const std::string &key, bool *retry, uint64_t *retryAtUs)
{
    CacheKey k;
    k.stack = stack_;
    k.key = "kernel | " + key;
    std::shared_ptr<Kernel> job;
    {
        std::lock_guard<std::mutex> mapLock(kernelLock_);
        auto &entry = kernels_[k.name()];
        if (!entry)
            entry = std::make_shared<Kernel>();
        job = entry;
    }
    std::lock_guard<std::mutex> hold(job->lock);
    using Build = ComputeKernelBuild<ComputeRecord, ComputeState>;
    std::string why;
    bool attempted = false;
    const auto result = job->build.make([] { return nowUs(); }, [&](ComputeRecord &record) {
        attempted = true;
        Compiler::Outcome outcome = Compiler::Outcome::Failed;
        switch (cache_.loadKernel(k, record, why)) {
        case RecordCache::Found::Record:
            outcome = Compiler::Outcome::Made;
            break;
        case RecordCache::Found::Refusal:
            outcome = Compiler::Outcome::Refused;
            break;
        case RecordCache::Found::None: {
#if SWITCH_RAW_DIAGNOSTICS
            const uint64_t began = nowUs();
#endif
            outcome = direct().kernel(descriptor, dispatch, key, record, why);
            if (outcome != Compiler::Outcome::Failed) {
                const auto saved = outcome == Compiler::Outcome::Made ? std::make_shared<ComputeRecord>(record) : nullptr;
                const CacheKey key = k;
                const std::string refusal = why;
                dispatch_async(persistenceQueue_, ^{
                    cache_.storeKernel(key, saved.get(), refusal);
                });
            }
#if SWITCH_RAW_DIAGNOSTICS
            if (outcome == Compiler::Outcome::Made)
                once([NSString
                    stringWithFormat:@"a kernel the pack lacks, made at its first dispatch of %u,%u,%u threads in "
                                     @"%llu us: %s %s",
                                     dispatch.threads[0], dispatch.threads[1], dispatch.threads[2],
                                     (unsigned long long)(nowUs() - began),
                                     descriptor.computeFunction.name.UTF8String ?: "-", constantsLast(key).c_str()]);
#endif
            break;
        }
        }
        return outcome == Compiler::Outcome::Made ? Build::Outcome::Made
               : outcome == Compiler::Outcome::Refused ? Build::Outcome::Refused : Build::Outcome::Failed;
    }, [&](const ComputeRecord &record) {
        attempted = true;
        const auto *state = placeCompute(device_, record);
        if (!state)
            why = "no room for its program or samplers";
        return state;
    });
    if (retry)
        *retry = result.retry;
    if (retryAtUs)
        *retryAtUs = result.retryAtUs;
    if (!result.state && attempted)
        once([NSString stringWithFormat:@"a kernel the pack lacks %s at %u,%u,%u threads: %s | %s %s",
                                        result.retry ? "was not made" : "is refused", dispatch.threads[0],
                                        dispatch.threads[1], dispatch.threads[2], why.c_str(),
                                        descriptor.computeFunction.name.UTF8String ?: "-", constantsLast(key).c_str()]);
    return result.state;
}

// The state made again with its fragment program's split, which only the same programs take (the record's layout is the
// whole program's). Kept on disk unless the compile could not run, so a later boot offers it at the state's creation.
void Completion::splitLater(const CacheKey &k, const RuntimeRecord &record)
{
    Ask a;
    {
        std::lock_guard<std::mutex> l(lock_);
        const auto it = asked_.find(k.name());
        if (record.splitTried || it == asked_.end() || !splitAsked_.insert(k.name()).second)
            return;
        a = it->second;
    }
    auto kept = std::make_shared<RuntimeRecord>(record);
    const CacheKey key = k;  // a block holds a reference parameter as the reference
    dispatch_async(splitQueue_, ^{
        RuntimeRecord made;
        std::string why;
        ProbeTextures textures;
        textures.cubic = a.cubic;
        textures.fragmentTests = a.fragmentTests;
        const Compiler::Outcome outcome = direct().make(a.descriptor, a.key, made, why, textures, nullptr, true);
        if (outcome == Compiler::Outcome::Failed)
            return;
        const bool same = outcome == Compiler::Outcome::Made && made.programs[0].id == kept->programs[0].id &&
                          made.programs[1].id == kept->programs[1].id;
        if (outcome == Compiler::Outcome::Made && !same)
            once([NSString
                stringWithFormat:@"a state's split compile made other programs than its own: %.600s", a.key.c_str()]);
        kept->splitTried = true;
        if (same && made.split) {
            kept->split = true;
            kept->splitRecord = made.splitRecord;
            kept->splitPrograms[0] = std::move(made.splitPrograms[0]);
            kept->splitPrograms[1] = std::move(made.splitPrograms[1]);
            device_.addSplit(*kept);
        }
        dispatch_async(persistenceQueue_, ^{
            cache_.store(key, kept.get(), std::string());
        });
    });
}

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
