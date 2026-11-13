// SPDX-License-Identifier: MIT
// The raw device's completion of the states and kernels its pack lacks (RawMetal.mm), of any library's functions: each asked
// once per process and per build. A state is looked up in the installed seed and this process's table when it is created,
// then in the system shader compiler's store, and otherwise asked of that compiler (raw_remote.h), which compiles each state
// once for every process and boot; its record is handed to the raw device, and for a state it refuses m2v draws it. A kernel
// the same, at the first dispatch of its key (raw_compute_key.h: its threadgroup shape and bindings are the dispatch's).
// Only a process the kernel serves no compiler (switch-no-bt on iOS 17), or a state the compiler does not take, is made
// here (RawDirect.h: m2v's translation and NVK's stage compile), each such compile counted and its reason logged once.
#pragma once
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include "raw_compiler.h"
#include "raw_compute_lookup.h"
#include "raw_cubic.h"
#include "raw_metallib.h"
#include "raw_obtained.h"
#include "raw_remote.h"
#include "raw_written_textures.h"

struct NmbDirectDispatch;

namespace raw {

class Device;
class Direct;
struct ComputeRecord;
struct ComputeState;
struct Pipeline;

class Completion {
public:
    // `cacheDir`: the process's own shader cache directory (null or empty: records kept in memory only)
    Completion(Device &device, const char *cacheDir, uint64_t stack, const char *seedDir = nullptr);
    ~Completion();

    struct Ask {
        // over m2v's functions, or with resolveDescriptor the facade's, whose m2v twins are made only for a state
        // compiled in this process (m2v's translation)
        MTLRenderPipelineDescriptor *descriptor;
        std::string key;                          // the pack key
        bool caTransform = false;                 // Pipeline::caTransform
        // its functions are not a framework's default library's: no pack names the key, so the record is kept under the
        // key and both functions' AIR (two apps' alike-named functions are not one state)
        bool own = false;
        // its cubic variant's binds (raw_cubic.h), whose name the key ends with; empty: the state itself
        CubicBinds cubic;
        uint8_t fragmentTests = 0;
        bool twoPlane = false;  // its " | xr2p" variant (raw_xr_pass.h)
        uint16_t xrSlots = 0;   // its " | xrs=" variant's sampled two-plane slots (raw_xr_pass.h)
        std::vector<WrittenTexture> written;  // its " | wtex=" variant's written textures (raw_written_textures.h)
        bool resolveDescriptor = false;
        // the descriptor over this facade's own functions, which a compile request names (nil: none can)
        MTLRenderPipelineDescriptor *facade = nil;
        uint32_t qos = 0;  // ask(): the asking thread's class, which the compiler runs it at
    };

    // The state's pipeline handed to `ready` once made, or null for a permanent refusal. Temporary allocation
    // failures keep the request pending and retry from retained compiler output.
    void ask(Ask a, std::function<void(Pipeline *)> ready);
    // The state's pipeline now, on the caller's thread: its record from this process or the disk (a state made in an earlier
    // boot is raw from its first draw), else made, as Metal makes a state at its creation. Null: refused, or its compile could
    // not run (*retry: ask() may make it later)
    Pipeline *makeNow(Ask a, bool *retry);

    // The kernel of compute key `key` (raw_compute_key.h) for `descriptor` (over m2v's functions) at `dispatch`, placed:
    // this process's, the disk's, or made now on the caller's thread, the dispatch that first needs it. Null: refused (named
    // once in the raw log), or temporarily unavailable. retryAtUs is the earliest next attempt after a transient failure.
    const ComputeState *kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                               const std::string &key, bool *retry = nullptr, uint64_t *retryAtUs = nullptr,
                               id facadeFunction = nil);

    void backgroundKernel(std::function<void()> work);

    Compiler::Stats stats() const;

    uint64_t compiledHere() const
    {
        return inProcess_.count();
    }

    std::string compilerStore() const
    {
        return remote_->storeIdentity();
    }

    const uint8_t *compilerStoreMapping(uint64_t &bytes) const
    {
        return remote_->storeMapping(bytes);
    }

    // What the calling thread's last makeNow got its record by
    static Obtained lastObtained();
    static void noteObtained(const Obtained &o);

private:
    struct Pending;
    void request(Ask asked, std::shared_ptr<Pending> pending);
    void place(const CacheKey &key, const RuntimeRecord *record, bool caTransform, std::shared_ptr<Pending> pending);

    struct Kernel {
        std::mutex lock;
        ComputeKernelBuild<ComputeRecord, ComputeState> build;
    };

    Device &device_;
    RecordCache cache_;
    std::unique_ptr<Compiler> compiler_;
    std::unique_ptr<Direct> direct_;
    std::once_flag directMade_;  // make() runs on the compile queue and on creating threads at once
    uint64_t stack_;
    dispatch_queue_t queue_, persistenceQueue_;
    dispatch_group_t pending_;
    std::atomic<bool> stopping_{false};
    std::mutex lock_;
    std::map<uint64_t, Ask> asked_;  // by CacheKey::name, what make() makes
    std::set<uint64_t> notTaken_;    // by CacheKey::name, the states the compiler did not take: made here
    std::mutex kernelLock_;          // map membership only; each kernel owns its single-flight lock
    std::map<uint64_t, std::shared_ptr<Kernel>> kernels_;

    std::unique_ptr<Remote> remote_;
    InProcess inProcess_;

    bool keyOf(Ask &a, CacheKey &k);
    Direct &direct();
    Compiler::Outcome make(const CacheKey &k, RuntimeRecord &record, std::string &why);

    // RawCompleteRemote.mm: the system shader compiler's side
    void compiledHere(InProcess::Why why);
    bool renderRequest(const Ask &a, const CacheKey &k, bool bytes, CompileRequest &r, Digest &name);
    RecordCache::Found fromStore(const Digest &name, const CacheKey &k, RuntimeRecord &record, std::string &why);
    // The state made by the compiler while this thread sleeps; false: not taken (made here instead)
    bool remoteNow(const Ask &a, const CacheKey &k, Compiler::State &state, RuntimeRecord &record);
    // The state asked of the compiler with `done` called once it answers; false: not asked
    bool remoteLater(const Ask &a, const CacheKey &k, std::function<void(const RuntimeRecord *)> done);
    // A kernel's record from the store or made by the compiler while this thread sleeps; Failed with `taken` false: not
    // taken (made here instead)
    Compiler::Outcome remoteKernel(id facadeFunction, MTLComputePipelineDescriptor *descriptor,
                                   const NmbDirectDispatch &dispatch, const CacheKey &k, ComputeRecord &record,
                                   std::string &why, bool &taken);
    void logRemote(const Ask &a, const CacheKey &k, RecordCache::Found found, const char *how, uint64_t waitedUs);
};

}  // namespace raw
