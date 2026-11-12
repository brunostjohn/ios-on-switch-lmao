// SPDX-License-Identifier: BSD-2-Clause-Patent
// What each NVENC job reads that another engine's job wrote: VIC's input picture, or a native picture's NVDEC and VIC
// producers. The job's stream waits for them by syncpoint, and a producer its engine cancelled reaches its syncpoint
// all the same (the CPU releases it), so the job's Wait answers with the producer's failure instead of the frame coded
// from memory nobody wrote. The cancelled job is recorded failed before that release; a semantic failure counts once
// it is retired.
#pragma once
#include <IOKit/IOLocks.h>
#include "../TegraGM20B/SwitchGpuBacking.h"

class TegraNVENCInputs {
public:
    static constexpr uint32_t kJobs = 16;  // the queue's 4 and the fences a client may still wait for after them
    static constexpr uint32_t kProducers = 5;

    bool start()
    {
        lock_ = IOLockAlloc();
        return lock_ != nullptr;
    }

    // Under the engine lock, once the job is queued and before its fence is handed out.
    void record(uint32_t fence, const switch_gpu_backing_token *producers, uint32_t count)
    {
        Job job = {fence, 0, {}};
        for (uint32_t i = 0; i < count && job.count < kProducers; ++i)
            if (producers[i].engine == SWITCH_GPU_ENGINE_VIC || producers[i].engine == SWITCH_GPU_ENGINE_NVDEC)
                job.producers[job.count++] = producers[i];
        IOLockLock(lock_);
        jobs_[fence % kJobs] = job;
        IOLockUnlock(lock_);
    }

    // Once the job's fence is reached: 0, or the first failure of what it read. A job kJobs fences back is no longer
    // known and reads as 0.
    int failure(uint32_t fence) const
    {
        IOLockLock(lock_);
        const Job job = jobs_[fence % kJobs];
        IOLockUnlock(lock_);
        if (job.fence != fence)
            return 0;
        for (uint32_t i = 0; i < job.count; ++i) {
            const int physical = switch_gpu_backing_token_physical_status(&job.producers[i], 0);
            const int result = physical < 0 ? physical : switch_gpu_backing_token_status(&job.producers[i]);
            if (result < 0 && result != SWITCH_GPU_BACKING_STALE)
                return result;
        }
        return 0;
    }

private:
    struct Job {
        uint32_t fence, count;
        switch_gpu_backing_token producers[kProducers];
    };

    IOLock *lock_ = nullptr;
    Job jobs_[kJobs] = {};
};
