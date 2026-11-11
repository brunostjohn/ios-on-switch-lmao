// SPDX-License-Identifier: MIT
// Event operations no raw range's job carries, in jobs of no work on the raw channel (raw_event_job.h). Submit thread only.
#pragma once
#import "RawMetalObjects.h"

// The head range is a kernel event's signal no job carries yet, with the kernel ordering this channel after m2v's: the
// scheduler submits it once the m2v work before it is scheduled (m2vClear), not once that work completed
bool signalOrderedOnGpu(RMCommandBuffer *cb, const RMSegment &range);

enum class RMEventJob {
    None,       // nothing for a job: a signal of an event the kernel does not hold (its value is set on the CPU)
    Submitted,  // the head range's signal and the ones right after it went in jobs
    Retry,      // not submitted now (the kernel refused the Order, or the command buffer failed): visit it again
};

// At a head signal range no job carries: it and the leasable signals right after it
RMEventJob submitEventJobs(RMCommandBuffer *cb);

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's metal.native.shared_event: the signal jobs made since the device opened
uint64_t eventJobsSubmitted();
#endif
