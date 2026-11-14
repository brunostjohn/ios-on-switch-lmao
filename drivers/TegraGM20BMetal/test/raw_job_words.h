// SPDX-License-Identifier: MIT
// The jobs of command buffers as text that does not depend on where their setBytes landed. A command buffer takes its
// setBytes chunks from a pool a utility queue grows ahead and completions refill after waking waitUntilCompleted
// (RawMetalCommandBuffer.mm), so which chunk, and whether one coalesced chunk or two, varies from run to run; every
// chunk alive is in the device's resource set, so in every job's uses. Each address in a chunk reads as its offset in
// the command buffer's chunks laid end to end, and the BOs made since the first command buffer began read in a job's
// uses as one name.
#pragma once
#include "raw_kernel_model.h"
#import <Metal/Metal.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

@interface NSObject (SwitchRawJobWords)
- (NSArray<NSNumber *> *)switchRawBytesChunks;        // RawMetalCommandBuffer.mm (debug builds)
- (NSArray<NSNumber *> *)switchRawBytesChunkLengths;  // the same chunks' lengths
@end

namespace job_words {

struct Chunk {
    uint64_t va = 0, bytes = 0, at = 0;  // at: its first byte's offset in the run
};

// A command buffer's jobs, from `firstJob` to the next run's, and its setBytes chunks in the order it filled them
struct Run {
    size_t firstJob = 0;
    uint64_t firstHandle = 0;  // the model's next BO handle as it was recorded
    std::vector<Chunk> chunks;
};

// `cb` recorded and not yet committed: its jobs are the model's next ones
inline Run run(id<MTLCommandBuffer> cb)
{
    Run r;
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        r.firstJob = kernel_model::model.jobs.size();
        r.firstHandle = kernel_model::model.nextHandle;
    }
    NSArray<NSNumber *> *vas = [(id)cb switchRawBytesChunks], *lengths = [(id)cb switchRawBytesChunkLengths];
    uint64_t at = 0;
    for (NSUInteger i = 0; i < vas.count && i < lengths.count; i++) {
        r.chunks.push_back({vas[i].unsignedLongLongValue, lengths[i].unsignedLongLongValue, at});
        at += lengths[i].unsignedLongLongValue;
    }
    return r;
}

inline const Chunk *chunkAt(const Run &r, uint64_t va)
{
    for (const Chunk &c : r.chunks)
        if (va >= c.va && va - c.va < c.bytes)
            return &c;
    return nullptr;
}

// The model's jobs of `runs` (in order, the last one's to the model's last job): one line a job, its words in hex with each
// (high, low) address pair in a chunk as "bytes+<offset in the run>", then its uses, those of BOs made since the first
// run began as "made"
inline std::string words(const std::vector<Run> &runs)
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    const auto &jobs = kernel_model::model.jobs;
    std::string out;
    char text[48];
    for (size_t r = 0; r < runs.size(); r++) {
        const Run &run = runs[r];
        const size_t end = r + 1 < runs.size() ? runs[r + 1].firstJob : jobs.size();
        for (size_t j = run.firstJob; j < end && j < jobs.size(); j++) {
            const auto &w = jobs[j].words;
            snprintf(text, sizeof text, "cb %zu job %zu %zu:", r, j - run.firstJob, w.size());
            out += text;
            for (size_t i = 0; i < w.size(); i++) {
                const uint64_t va = i + 1 < w.size() ? uint64_t(w[i]) << 32 | w[i + 1] : 0;
                if (const Chunk *c = w[i] ? chunkAt(run, va) : nullptr) {
                    snprintf(text, sizeof text, " bytes+%llx", (unsigned long long)(c->at + va - c->va));
                    out += text;
                    i++;
                    continue;
                }
                snprintf(text, sizeof text, " %08x", w[i]);
                out += text;
            }
            out += " | uses";
            bool madeNamed = false;
            for (const TegraGM20BAbiSurfaceUse &u : jobs[j].uses) {
                if (u.handle >= runs[0].firstHandle) {
                    if (!madeNamed)
                        out += " made";
                    madeNamed = true;
                    continue;
                }
                snprintf(text, sizeof text, " %llx/%x", (unsigned long long)u.handle, u.access);
                out += text;
            }
            out += "\n";
        }
    }
    return out;
}

inline uint64_t fnv(const std::string &text)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

}  // namespace job_words
