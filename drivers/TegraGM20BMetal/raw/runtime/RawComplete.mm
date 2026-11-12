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

#if SWITCH_RAW_DIAGNOSTICS
// The compiling thread's class, from its pthread TSD. No thread CPU clock or thread_info: system sandboxes, such as
// backboardd's, kill a process that makes thread_selfusage (syscall 482, the thread clock's); some refuse thread_info.
const char *threadClass()
{
    const qos_class_t c = qos_class_self();
    return c == QOS_CLASS_USER_INTERACTIVE ? "UI"
           : c == QOS_CLASS_USER_INITIATED ? "IN"
           : c == QOS_CLASS_DEFAULT        ? "DF"
           : c == QOS_CLASS_UTILITY        ? "UT"
           : c == QOS_CLASS_BACKGROUND     ? "BG"
                                           : "fixed";
}

// One runtime compile's wall time and its thread's class, for the line that names it
class CompileCost {
public:
    CompileCost() : wallUs_(nowUs())
    {}

    std::string text() const
    {
        return std::to_string((nowUs() - wallUs_) / 1000) + " ms (" + threadClass() + ")";
    }

private:
    uint64_t wallUs_;
};
#endif

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
    const raw::Kernel &kernel = device_.kernel();
    remote_ = std::make_unique<Remote>(
        KernelTransport{kernel.connection(), kernel.tunnelled(), kernel.tunnelSelector()}, stack_);
    Remote *remote = remote_.get();
    device_.runSlotsFrom([remote](const uint8_t *slot, uint32_t size, const uint8_t *&mapping, uint64_t &bytes) {
        return remote->findSlot(slot, size, mapping, bytes);
    });
    if (!remote_->serving())
        os_log_error(rawLog(),
                     "raw: %{public}s has no shader compiler this boot: the states its pack lacks are made here",
                     getprogname());
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
    device_.runSlotsFrom(nullptr);
    stopping_.store(true, std::memory_order_release);
    compiler_->drain();
    dispatch_group_wait(pending_, DISPATCH_TIME_FOREVER);
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
    asked.qos = uint32_t(qos_class_self());
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
        bool remote = remote_->serving();
        if (!remote && remote_->declined()) {
            pending->ready(nullptr);
            return;
        }
        {
            std::lock_guard<std::mutex> l(lock_);
            asked_[k.name()] = a;
            remote &= !notTaken_.count(k.name());
        }
        const bool caTransform = a.caTransform;
        if (remote && remoteLater(a, k, [this, k, caTransform, pending](const RuntimeRecord *record) {
                if (record)
                    place(k, record, caTransform, pending);
                else
                    pending->ready(nullptr);
            }))
            return;
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
            pending->ready(made);
        });
        if (delay)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(delay) * 1000), queue_, ^{
                place(keptKey, record, caTransform, pending);
            });
    });
}

Pipeline *Completion::makeNow(Ask a, std::string &failure)
{
    failure.clear();
    CacheKey k;
    if (!keyOf(a, k))
        return nullptr;
    {
        std::lock_guard<std::mutex> l(lock_);
        asked_[k.name()] = a;
    }
    RuntimeRecord record;
    std::string why;
    Compiler::State state = Compiler::State::Refused;
    RAW_DIAGNOSTIC(noteObtained({"table", 0, uint32_t(qos_class_self()), 0}));
    switch (compiler_->lookup(k, record, why)) {
    case RecordCache::Found::Record:
        state = Compiler::State::Ready;
        break;
    case RecordCache::Found::Refusal:
        state = Compiler::State::Refused;
        break;
    case RecordCache::Found::None:
        if (!remote_->serving()) {
            if (remote_->declined()) {
                failure = "no shader compiler comes this boot: it declined, or its launcher stopped starting it";
                return nullptr;
            }
            compiledHere(InProcess::NoCompiler);
            state = madeHere(k, record, why);
            break;
        }
        switch (remoteNow(a, k, state, record)) {
        case Asked::Answered:
            break;
        case Asked::Here: {
            {
                std::lock_guard<std::mutex> l(lock_);
                notTaken_.insert(k.name());
            }
            state = madeHere(k, record, why);
            break;
        }
        case Asked::Gone:
            failure = "no shader compiler comes this boot: it declined, or its launcher stopped starting it";
            return nullptr;
        }
        break;
    }
    if (state == Compiler::State::Failed) {
        failure = "the state's compile could not run" + (why.empty() ? std::string() : ": " + why);
        return nullptr;
    }
    if (state != Compiler::State::Ready)
        return nullptr;
    bool heapFull = false;
    Pipeline *made = device_.addRecord(record, a.caTransform, why, &heapFull);
    if (!made && heapFull)
        failure = why;
    return made;
}

Compiler::State Completion::madeHere(const CacheKey &k, RuntimeRecord &record, std::string &why)
{
    RAW_DIAGNOSTIC(noteObtained({"here", 0, uint32_t(qos_class_self()), 0}));
    const Compiler::State state = compiler_->makeNow(k, record, why);
    return state == Compiler::State::Failed ? compiler_->makeNow(k, record, why, true) : state;
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
#if SWITCH_RAW_DIAGNOSTICS
    const CompileCost cost;
#endif
    const Compiler::Outcome outcome = direct().make(a.descriptor, a.key, record, why, textures);
#if SWITCH_RAW_DIAGNOSTICS
    os_log(
        rawLog(),
        "raw: a state the pack lacks %{public}s in %{public}s after %llu ms for a compile slot: %016llx %{public}.400s",
        outcome == Compiler::Outcome::Made      ? "made"
        : outcome == Compiler::Outcome::Refused ? "refused"
                                                : "not made",
        cost.text().c_str(), (unsigned long long)(Compiler::slotWaitUs() / 1000), (unsigned long long)k.name(),
        a.key.c_str());
#endif
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
                                       const std::string &key, bool *retry, uint64_t *retryAtUs, id facadeFunction)
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
            bool taken = false;
            if (remote_->serving())
                outcome = remoteKernel(facadeFunction, descriptor, dispatch, k, record, why, taken);
            else
                compiledHere(InProcess::NoCompiler);
            if (taken)
                break;
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
                                     @"%llu us (%s): %s %s",
                                     dispatch.threads[0], dispatch.threads[1], dispatch.threads[2],
                                     (unsigned long long)(nowUs() - began), threadClass(),
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

Compiler::Stats Completion::stats() const
{
    return compiler_->stats();
}

}  // namespace raw
