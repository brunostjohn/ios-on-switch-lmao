// SPDX-License-Identifier: MIT
// The raw GM20B path's compute work: Metal kernels as MAXWELL_COMPUTE_B launches on the raw channel, beside its 3D and copy
// work. The pack's context init binds the class on subchannel 1 with the raw heap as its program region and the raw texture
// and sampler pools (raw_pack.h PackInit), so a dispatch is a QMD in the upload ring and SEND_PCAS: no compute state lives in
// the channel between dispatches.
//
// A kernel is NVK's work for one Metal kernel at one key (raw_compute_key.h: m2v compiles the threadgroup shape and memory
// lengths into the program, and picks its variant by the textures and samplers bound): NAK's program, the QMD NVK filled for
// it (nak_fill_qmd), which of its constant buffers are NVK's root table, set 0 and shader data, and where set 0 takes each
// Metal buffer, texture and sampler (raw_pack.h PackKernel). A dispatch writes those as NVK's nvk_cmd_upload_qmd does: its
// own root table (the cs group count, the set's address, the push words with m2v's DispatchRegion field) and set 0 in the
// upload ring, and a QMD of the template with the program's heap offset, the grid and the constant buffers' addresses.
//
// Order. A compute encoder is ordered after the work encoded before it and before the work after it on the GPU, as Metal orders
// a serial encoder: a wait for idle where it starts (the render and copy work before it has written), between its dispatches
// a shader-data flush that waits for the one before, and where it ends a wait for idle with the texture and constant caches
// invalidated (what the 3D and copy engines read next was written through L2).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "raw_band_rates.h"
#include "raw_compute_record.h"
#include "raw_dispatch_bands.h"
#include "raw_dispatch_regions.h"
#include "raw_gpu.h"
#include "raw_loop_copies.h"

namespace raw {

constexpr uint32_t kQmdWords = 64;

// A kernel ready to dispatch: its record and where its program lies in the heap
struct ComputeState {
    const PackKernel *rec = nullptr;
    uint32_t instructions = 1;
    uint8_t cbufType[8] = {};
    uint32_t program = 0;  // QMD PROGRAM_OFFSET: from the program region (the heap's start)
    uint64_t data = 0;     // its shader data's address, 0: none
    PackKernel placed =
        {};  // a runtime record's, its samplers numbered among the device's (placeCompute): rec points here
    mutable BandRate rate;  // what its dispatches cost the GPU, learned from its measured ones (raw_band_rates.h)
};

// The pack's kernel of key `keyHash` (computeKeyHash), its program placed at its first use; null: not in the pack, or no
// room in the heap
const ComputeState *computeState(Device &dev, uint64_t keyHash);
// A record of no pack, its program placed at its first use (null: no room)
const ComputeState *placeCompute(Device &dev, const ComputeRecord &r);

class ComputeEncoder {
public:
    ComputeEncoder(CommandBuffer &cb, Device &dev);

    void setState(const ComputeState *k)
    {
        state_ = k;
    }

    // MTLDispatchTypeSerial: each dispatch after the encoder's first waits for the one before, its writes visible
    void setSerial(bool serial)
    {
        serial_ = serial;
    }

    void setBytes(uint32_t slot, const void *bytes, uint32_t length);
    void setBytesAt(uint32_t slot, uint64_t va, uint32_t length,
                    const Buffer *allocation = nullptr);  // bytes already where the GPU reads them
    void setBuffer(uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(uint32_t slot, uint64_t offset);
    void setTexture(uint32_t slot, const Texture *t);
    void setSampler(uint32_t slot, const Sampler *s);

    // dispatchThreadgroups: `groups` threadgroups of `threads` threads, the shape the kernel's record was made at. False:
    // refused (*why), nothing encoded
    bool dispatch(const uint32_t groups[3], const uint32_t threads[3], const char **why);
    bool dispatchRegion(const ComputeRegion &region, const std::array<uint32_t, 3> &grid,
                        const std::array<uint32_t, 3> &nominal, bool first, bool last, const char **why);
    // memoryBarrier...: the dispatches before have written, and their writes are visible to the ones after
    void barrier();
    // A predication record (raw_predication.h): the word at `wordVa` for encodeStartIf; Misused: nothing encoded
    Predication::Answer startIf(uint64_t wordVa, PredicateCompare compare, uint32_t reference);
    Predication::Answer startElse();
    Predication::Answer endIf();
    // Loops (raw_predication.h, raw_loop_copies.h): a While's word at `wordVa`, a DoWhile's at its end; `tag` names the
    // loop in the record its end leaves where it would run past its copies (Device::loopContinue). Misused: a loop the
    // raw path cannot lay down (deeper than LoopCopies::kMaxDepth, a body of more than kMaxSegments segments or one that
    // left the encoder, no copies within the entry budget), nothing more encoded
    Predication::Answer startWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference);
    Predication::Answer endWhile(uint32_t tag);
    Predication::Answer startDoWhile();
    Predication::Answer endDoWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference, uint32_t tag);
    // A counter sample on the compute class now: right after the encoder's opening wait for idle, its start
    void timestamp(uint64_t va);
    void end();
#if defined(SWITCH_RAW_HARNESS_MARKS)
    // The bench's QMD chains (hw_test_app/bench qmdchain): each launch's QMD as built, before its upload, to patch;
    // false holds the launch back, the QMD then only a copy another QMD names dependent
    std::function<bool(uint32_t *qmd)> benchQmd;
#endif

    // A buffer an indirect dispatch command binds at a slot (va 0: none)
    struct IcbBuffer {
        uint64_t va = 0;
        uint32_t size = 0;
    };

    // One region of an indirect dispatch command of kernel `k` (raw_icb_compute.h): its QMD, root table and set 0 written at
    // `block` (GPU address `va`, icb::computeRegionBytes(k.rec->setBytes) long). False: refused (*why), as a dispatch is.
    static bool icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                          const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                          const IcbBuffer (&buffers)[31], uint8_t *block, uint64_t va, const char **why);
    // A command of a buffer that inherits the encoder's buffers takes the ones this encoder binds now, each used as written
    void inheritBuffers(IcbBuffer (&out)[31]);
    // Memory of this command buffer's upload ring the GPU reads, 256-byte aligned
    uint8_t *upload(uint32_t bytes, uint64_t &va);

    struct IcbExecute {
        const Buffer *slots;  // the commands' words, icb::kComputeSlotWords each, command `first`'s at slot `at`
        uint32_t at, first, count;
        const Buffer *range;  // the execution range the GPU reads at rangeOffset; null: [first, first + count)
        uint64_t rangeOffset;
        bool written;  // a blit wrote the words on the GPU
    };

    // The commands by reference, ordered after this encoder's work before them as a dispatch is
    bool executeSlots(const IcbExecute &x, const char **why);

private:
    static void rootWords(const PackKernel &r, const uint32_t base[3], const uint32_t groups[3], uint64_t setVa,
                          uint32_t residualMask, const std::array<uint32_t, 32> &residuals, const uint32_t region[8],
                          uint32_t *root);
    static bool qmdWords(const ComputeState &k, const uint32_t groups[3], uint64_t rootVa, uint64_t setVa,
                         uint32_t *qmd, const char **why);

    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
        uint64_t resourceVa = 0;

        bool operator==(const Bound &b) const
        {
            return va == b.va && size == b.size && buffer == b.buffer && offset == b.offset && resourceVa == b.resourceVa;
        }
    };

    CommandBuffer &cb_;
    Device &dev_;
    Push p_;
    uint32_t *start_ = nullptr;
    const ComputeState *state_ = nullptr;
    Bound bufs_[31];
    const Texture *textures_[31] = {};
    const Sampler *samplers_[16] = {};
    bool serial_ = false, dispatched_ = false;
    uint64_t apiWork_ = 0;
    uint64_t bindingRevision_ = 0, describedRevision_ = 0;
    const ComputeState *describedState_ = nullptr;
    std::vector<uint32_t> descriptorWords_;
    std::array<uint32_t, 32> residuals_{};
    uint32_t residualMask_ = 0;
    uint64_t setVa_ = 0, rootVa_ = 0;
    std::array<uint32_t, 3> grid_{}, base_{};
    std::array<uint32_t, 8> region_{};
    void bindBuffer(uint32_t slot, const Bound &binding);
    bool prepareDescriptors(const char **why);
    void room(uint32_t words);
    bool launch(const std::array<uint32_t, 3> &grid, const DispatchPiece &piece, const std::array<uint32_t, 8> &region,
                const char **why);
    void cutBatch();
    bool measureDispatch(uint64_t units, DrawTiming &t);
    void stampDispatch(const DrawTiming &t, bool end);
    LoopStream stream();
    Predication::Answer startLoop(bool doWhile, uint64_t wordVa, const PredicateTest *test);
    Predication::Answer endLoop(uint64_t wordVa, const PredicateTest *test, uint32_t tag);
};

}  // namespace raw
