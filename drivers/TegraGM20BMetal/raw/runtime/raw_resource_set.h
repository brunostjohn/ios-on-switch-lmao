// SPDX-License-Identifier: MIT
// The raw device's persistent resource set (drivers/TegraGM20B/TegraGM20BJobABI.h, methods 31-33): every BO of the device
// and every page range it imported of its own memory, registered once, which a job names by the set's id and generation
// instead of one use each. The kernel keeps out (and later takes out) what another process, engine or a CPU lease or
// CpuComplete reaches; that BO stays a use of every job that touches it.
//
// Registration (add, free, renew) holds the set's lock and makes the set again with every member, which ends the
// generation; submitters never take it while the set is unchanged: each keeps a SetView, refreshed when the version moved.
// A member is freed while it is still a member (the kernel keeps the set's pending jobs as its users), and leaves the view
// before its VA is given back, so no view names an address another BO may be bound at next.
#pragma once
#include "raw_lock.h"
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <vector>
#include "raw_kernel.h"
#include "raw_surfaces.h"

namespace raw {

struct SetMember {
    uint64_t handle = 0, va = 0, bytes = 0;
    uint32_t access = 0;  // TegraGM20BSurfaceRead, or TegraGM20BSurfaceWrite: the set's jobs may write it
    bool always = false;  // every job of the device uses it (its control memory, rings, shader heap, local memory)
};

// A member a recorded use skipped: named as a use by handle should it leave the set before the job is submitted
struct SetUse {
    uint64_t handle;
    uint32_t access;
};

struct SetView {
    uint32_t set = 0, generation = 0, version = 0;
    uint32_t removals = 0;           // members gone so far (freed, or taken out by the kernel)
    std::vector<SetMember> members;  // by va
    std::vector<SetMember> outside;  // `always` BOs the kernel keeps out of the set: a use of every job

    // The member [va, va + bytes) lies in, else null
    const SetMember *find(uint64_t va, uint64_t bytes) const;

    bool holds(uint64_t va, uint64_t bytes) const
    {
        return find(va, bytes) != nullptr;
    }

    bool holds(const Bo &bo) const
    {
        return bo.handle && holds(bo.vaBase ? bo.vaBase : bo.va, bo.bytes);
    }

    bool named(uint64_t handle) const;
};

class ResourceSet {
public:
    struct Candidate {
        const Bo *bo;
        uint32_t access;
    };

    // The set made again with these BOs among its members (`always`: every job uses them); each the kernel refuses stays out
    // (a use of the jobs that touch it). False when any was.
    bool add(Kernel &k, std::initializer_list<Candidate> candidates, bool always = false);

    bool add(Kernel &k, const Bo &bo, uint32_t access, bool always = false)
    {
        return add(k, {{&bo, access}}, always);
    }

    // A read member a job is about to write becomes a written one (the set made again). Members start as read where the CPU
    // may lease them, so a CPU read lease of a member the GPU never wrote does not wait for the set's jobs.
    void promote(Kernel &k, uint64_t handle);

    // Kernel::free of `bo`, a member or not; a member leaves the set first.
    void free(Kernel &k, Bo &bo);

    // A job naming `generation` came back kIOReturnNotReady. True: the set's generation had ended (its mirror word says so:
    // a member freed, reached from outside or by the CPU, or a channel killed) and a newer one is current now, without
    // what the kernel took out. False: the generation was current, so something else of the job was stale (a native view);
    // or the set could not be made again (the kernel's error is the Kernel's).
    bool renew(Kernel &k, uint32_t generation);

    // `v` as the set is now; one load when nothing changed since it was taken
    void view(SetView &v) const;

private:
    bool registerLocked(Kernel &k);
    void insertLocked(const Bo &bo, uint32_t access, bool always);
    bool eraseLocked(uint64_t handle);

    mutable raw::Mutex lock_;
    std::vector<SetMember> members_;  // by va
    std::vector<SetMember> outside_;
    uint32_t set_ = 0, generation_ = 0, removals_ = 0;
    const volatile uint32_t *mirror_ = nullptr;
    std::atomic<uint32_t> version_{0};
};

// One job of `channel`, naming the set when `named` (the view's generation, made again and the job submitted again while
// the kernel finds it ended). `uses` (null: the job's own), what no member holds: it gains the set's `outside` BOs, and
// each of `recorded` (taken under `removals`) the set no longer holds. False: refused; `result` the kern_return_t.
bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno,
                 int *result = nullptr, SurfaceList *uses = nullptr, const std::vector<SetUse> *recorded = nullptr,
                 uint32_t removals = 0);

}  // namespace raw
