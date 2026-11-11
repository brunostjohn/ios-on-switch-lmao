// SPDX-License-Identifier: MIT
// The raw device's persistent resource set (drivers/TegraGM20B/TegraGM20BJobABI.h, methods 31-33): BOs that no other
// process, engine or CPU lease reaches, registered once, which a job names by the set's id and generation instead of one
// use each. The kernel refuses imports, IOSurfaces, native views, read-only and CPU-tracked (cached) BOs; those stay uses
// of every job that touches them.
//
// Registration (add, free, renew) holds the set's lock and makes the set again with every member, which ends the
// generation; submitters never take it while the set is unchanged: each keeps a SetView, refreshed when the version moved.
// A member is freed while it is still a member (the kernel keeps the set's pending jobs as its users), and leaves the view
// before its VA is given back, so no view names an address another BO may be bound at next.
#pragma once
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <vector>
#include "raw_kernel.h"

namespace raw {

struct SetMember {
    uint64_t handle = 0, va = 0, bytes = 0;
    uint32_t access = 0;  // TegraGM20BSurfaceRead, or TegraGM20BSurfaceWrite: the set's jobs may write it
};

struct SetView {
    uint32_t set = 0, generation = 0, version = 0;
    std::vector<SetMember> members;  // by va

    // [va, va + bytes) lies in one member
    bool holds(uint64_t va, uint64_t bytes) const;

    bool holds(const Bo &bo) const
    {
        return bo.handle && holds(bo.vaBase ? bo.vaBase : bo.va, bo.bytes);
    }
};

class ResourceSet {
public:
    struct Candidate {
        const Bo *bo;
        uint32_t access;
    };

    // The set made again with these BOs among its members; each the kernel refuses is left out (a use of the jobs that touch
    // it). False when any was left out.
    bool add(Kernel &k, std::initializer_list<Candidate> candidates);

    bool add(Kernel &k, const Bo &bo, uint32_t access)
    {
        return add(k, {{&bo, access}});
    }

    // Kernel::free of `bo`, a member or not; a member leaves the set first.
    void free(Kernel &k, Bo &bo);

    // A job naming `generation` came back kIOReturnNotReady. True: the set's generation had ended (its mirror word says so)
    // and a newer one is current now. False: the generation was current, so something else of the job was stale (a native
    // view); or the set could not be made again (the kernel's error is the Kernel's).
    bool renew(Kernel &k, uint32_t generation);

    // `v` as the set is now; one load when nothing changed since it was taken
    void view(SetView &v) const;

private:
    bool registerLocked(Kernel &k);
    void insertLocked(const Bo &bo, uint32_t access);
    void eraseLocked(uint64_t handle);

    mutable std::mutex lock_;
    std::vector<SetMember> members_;  // by va
    uint32_t set_ = 0, generation_ = 0;
    const volatile uint32_t *mirror_ = nullptr;
    std::atomic<uint32_t> version_{0};
};

// One job of `channel`, naming the set when `named` (the view's generation, made again and the job submitted again while
// the kernel finds it ended). False: refused; `result` the kern_return_t.
bool submitInSet(Kernel &k, ResourceSet &set, SetView &view, bool named, KernelJob job, uint32_t &seqno,
                 int *result = nullptr);

}  // namespace raw
