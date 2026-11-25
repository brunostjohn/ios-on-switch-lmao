// SPDX-License-Identifier: MIT
// Zero-on-free of Shared buffers' memory (gpu.h Scrub): a span freed beside live buffers of a VOL=1 write-combined chunk
// stays used until the copy engine has written zeros over it, so the create that takes it again does no CPU work (the
// free that empties a chunk goes back at once: Chunk::unscrubbed). Metal clears every buffer newBufferWithLength: makes,
// in every storage mode.
#include "lock.h"
#include "gpu.h"
#include "blit_encoder.h"

#include <algorithm>
#include <iterator>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// lock_ held: back in its chunk's free map, the chunk given back as an emptied write-combined one is (wcPoolFree)
void Device::scrubbedFree(const Scrub &s, DeferredFrees *deferred)
{
    std::vector<Chunk> &pool = volatileWcChunks_;
    if (s.chunk >= pool.size())
        return;
    Chunk &ch = pool[s.chunk];
    if (ch.park != ChunkPark::Live || ch.bo.va + s.offset != s.va)
        return;
    ch.used -= s.bytes;
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    if (ch.used || !deferred)
        return;
    for (const Chunk &other : pool)
        if (&other != &ch && other.park == ChunkPark::Live && other.bo.handle && !other.dedicated && !other.used) {
            deferred->add({ch.bo, 0, 0, nullptr, 0, nullptr, ch.bytes, ch.bo.mapped, RetireKind::WcShared});
            ch = Chunk();
            return;
        }
}

void Device::retireScrubs(DeferredFrees *deferred)
{
    if (scrubbing_.empty())
        return;
    const uint64_t generation = channelGeneration();
    for (auto it = scrubbing_.begin(); it != scrubbing_.end();) {
        if (it->generation != generation) {
            // its job was on a channel since lost, which may have stopped before it
            scrubWanted_.push_back(*it);
            scrubDue_.store(true, std::memory_order_relaxed);
        } else if (done(it->seqno)) {
            scrubbedFree(*it, deferred);
        } else {
            ++it;
            continue;
        }
        it = scrubbing_.erase(it);
    }
}

bool Device::scrubFreed(bool wait)
{
    std::vector<Scrub> batch;
    {
        DeferredFrees deferred(*this);
        std::lock_guard<gm20b::Mutex> l(lock_);
        retireScrubs(&deferred);
        batch.swap(scrubWanted_);
        scrubDue_.store(false, std::memory_order_relaxed);
    }
    if (batch.empty())
        return true;
    uint32_t seqno = 0;
    CommandBuffer *rc = begin();
    bool submitted = rc != nullptr;
    if (submitted) {
        // VOL=1 memory: the job ends with WFI + FB_FLUSH, and the zeros are in DRAM for the CPU's write-combined view
        rc->volatileWrites_ = true;
        for (const Scrub &s : batch)
            rc->useAddress(s.va, s.bytes, true);
        BlitEncoder *b = rc->blit();
        for (const Scrub &s : batch)
            b->fillWords(s.va, s.bytes, 0);
        b->end();
        submitted = rc->commit(seqno);
        if (!submitted)
            rc->abandon();
    }
    const uint64_t generation = channelGeneration();
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        for (Scrub &s : batch) {
            s.seqno = seqno;
            s.generation = generation;
        }
        std::vector<Scrub> &to = submitted ? scrubbing_ : scrubWanted_;
        to.insert(to.end(), batch.begin(), batch.end());
        if (!submitted)
            scrubDue_.store(true, std::memory_order_relaxed);
    }
    if (!submitted || !wait)
        return submitted;
    const bool waited = this->wait(seqno, generation);
    DeferredFrees deferred(*this);
    std::lock_guard<gm20b::Mutex> l(lock_);
    retireScrubs(&deferred);
    return waited;
}

}  // namespace gm20b
