// SPDX-License-Identifier: MIT
// Indirect dispatch commands on the raw channel (raw_icb_compute.h): a command region's QMD, root table and set 0 as a
// dispatch's (raw_compute.cpp), in memory the command keeps, and an execute of commands by reference. A dispatch of an
// indirect grid, and a stage-in region read from a buffer, are the dispatch's own QMD and root table with the words the
// GPU reads patched in by the compute class's inline engine, its data the buffer's words fetched by the channel after the
// method header, as NVK's pre-Turing nvk_CmdDispatchIndirect2KHR writes them: no CPU reads the words.
#include "raw_compute.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "raw_icb_compute.h"
#include "raw_kit.h"
#include "raw_residual.h"

#include <cstring>

namespace raw {
namespace {

constexpr uint8_t kConstexprSamplerSlot = 0x80;
constexpr uint32_t kRootBytes = 0x800, kRootGroupCount = 12, kRootPush = 0x700;
constexpr uint32_t kQmdRasterWidth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_WIDTH),
                   kQmdRasterHeightDepth = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CTA_RASTER_HEIGHT);
constexpr uint32_t kSetReference = 0x0050;  // NV906F_SET_REFERENCE: the fetch after it sees what the engines wrote

}  // namespace

void ComputeEncoder::inlineFrom(uint64_t dst, uint64_t src, uint32_t bytes)
{
    room(8);
    p_.inc(NVB1C0_LINE_LENGTH_IN, {bytes, 1, uint32_t(dst >> 32), uint32_t(dst)}, SUBC_COMPUTE);
    *p_.at++ = Push::hdr(5, SUBC_COMPUTE, NVB1C0_LAUNCH_DMA, 1 + (bytes + 3) / 4);
    *p_.at++ = NVDEF(NVB1C0, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) | NVDEF(NVB1C0, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY);
    cb_.segment(start_, p_.at);
    cb_.segments_.back().flags |= TegraGM20BPushIncomplete;
    cb_.segments_.append({src, (bytes + 3) / 4, TegraGM20BPushNoPrefetch}, false);
    start_ = p_.at;
}

void ComputeEncoder::setStageInRegionIndirect(const Buffer *region, uint64_t offset)
{
    stageInRegion_ = region;
    stageInRegionOffset_ = offset;
}

// The work before has written and its stores reached L2, and the channel fetches what follows only then: words the GPU
// wrote that a continuation fetches are there (NVK's pre-Turing indirect-read barrier)
static void fetchAfterWrites(Push &p)
{
    p.m(NVB1C0_WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    p.m(NVB1C0_INVALIDATE_SHADER_CACHES, NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE), SUBC_COMPUTE);
    p.m(kSetReference, 0, SUBC_COMPUTE);
}

void ComputeEncoder::stageInFromMemory(const PackKernel &r, uint64_t rootVa, bool fetched)
{
    if (!stageInRegion_ || !r.stageInRegionWord || r.stageInRegionWord + 7u > 32)
        return;
    const uint64_t src = stageInRegion_->va + stageInRegionOffset_,
                   dst = rootVa + kRootPush + 4 * (r.stageInRegionWord - 1u);
    cb_.use(*stageInRegion_, false);
    if (stageInRegion_->hostVisible && !stageInRegion_->wc && stageInRegion_->cpu)
        cb_.clean(stageInRegion_->cpu + stageInRegionOffset_, 24);
    if (!fetched) {
        room(8);
        fetchAfterWrites(p_);
    }
    inlineFrom(dst, src, 12);
    inlineFrom(dst + 16, src + 12, 12);
}

bool ComputeEncoder::dispatchIndirect(const Buffer *arguments, uint64_t offset, const char **why)
{
    constexpr uint32_t kArgumentBytes = 12;
    if (!arguments || !arguments->va || (offset & 3) || offset > arguments->length ||
        arguments->length - offset < kArgumentBytes) {
        *why = "indirect dispatch arguments outside their buffer or not four-byte aligned";
        return false;
    }
    if (!state_) {
        *why = "a dispatch without a kernel";
        return false;
    }
    const PackKernel &r = *state_->rec;
    if (r.regionWord >= 0) {
        *why = "an indirect dispatch of a kernel compiled for the regions of an edge";
        return false;
    }
    if (state_->heapMissing) {
        *why = "no memory for the tables a kernel reads argument buffers through";
        return false;
    }
    if (!prepareDescriptors(why))
        return false;
    cb_.buffersWritten_ = true;
    if (serial_ && dispatched_)
        barrier();
    dispatched_ = true;
    slotTableVa_ = 0;
    const uint32_t none[3] = {};
    const uint32_t region[8] = {};
    uint32_t root[kRootBytes / 4] = {};
    rootWords(r, none, none, setVa_, residualMask_, residuals_, region, root);
    argumentRoot(r, root);
    stageInWords(r, stageIn_, root + kRootPush / 4);
    uint64_t rootVa = 0, qmdVa = 0;
    memcpy(dev_.alloc(dev_.upload_, kRootBytes, 256, rootVa), root, sizeof root);
    rootVa_ = 0;
    uint32_t qmd[kQmdWords];
    if (!qmdWords(*state_, none, rootVa, setVa_, qmd, why))
        return false;
    memcpy(dev_.alloc(dev_.upload_, sizeof qmd, 256, qmdVa), qmd, sizeof qmd);
    RAW_DIAGNOSTIC(dev_.stats.uploadBytes += kRootBytes + sizeof qmd);
    cb_.use(*arguments, false);
    if (arguments->hostVisible && !arguments->wc && arguments->cpu)
        cb_.clean(arguments->cpu + offset, kArgumentBytes);
    const bool looping = cb_.loops_.open();
    room(8 + (looping ? SWITCH_GM20B_INLINE_WORDS_WORDS(kQmdWords) : 0));
    if (looping)
        kit(p_, switch_gm20b_compute_inline_words, qmdVa, qmd, kQmdWords);
    fetchAfterWrites(p_);
    stageInFromMemory(r, rootVa, true);
    const uint64_t at = arguments->va + offset;
    inlineFrom(rootVa + kRootGroupCount, at, kArgumentBytes);
    inlineFrom(qmdVa + 4 * kQmdRasterWidth, at, 4);
    inlineFrom(qmdVa + 4 * kQmdRasterHeightDepth, at + 4, 2);
    inlineFrom(qmdVa + 4 * kQmdRasterHeightDepth + 2, at + 8, 2);
    room(8 + Predication::kGuardWords + Predication::kReleaseWords);
    cb_.predication_.guard(p_);
    p_.m(NVB1C0_SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
    p_.m(NVB1C0_SEND_SIGNALING_PCAS_B,
         NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) | NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE),
         SUBC_COMPUTE);
    cb_.predication_.release(p_);
    RAW_DIAGNOSTIC(dev_.stats.dispatches++);
    return true;
}

bool ComputeEncoder::icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                               const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                               const IcbBuffer (&buffers)[31], const StageIn &stageIn, uint8_t *block, uint64_t va,
                               const char **why)
{
    if (const char *refusal = icbRefusal(k)) {
        *why = refusal;
        return false;
    }
    const PackKernel &r = *k.rec;
    const uint32_t setBytes = icb::computeSetBytes(r.setBytes);
    const uint64_t rootVa = va + icb::kComputeQmdBytes, setVa = rootVa + icb::kComputeRootBytes;
    uint32_t *qmd = reinterpret_cast<uint32_t *>(block);
    uint32_t *root = qmd + icb::kComputeQmdBytes / 4;
    uint32_t *set = root + icb::kComputeRootBytes / 4;
    memset(root, 0, icb::kComputeRootBytes);
    memset(set, 0, setBytes);
    std::array<uint32_t, 32> residuals{};
    uint32_t residualMask = 0;
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == KernelBuffer) {
            const IcbBuffer &b = e.slot < 31 ? buffers[e.slot] : IcbBuffer{};
            BufferDescriptor desc;
            if (!bufferDescriptor(e, b.va, b.size, desc)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            if (desc.word >= 0) {
                residuals[desc.word] = desc.residual;
                residualMask |= 1u << desc.word;
            }
            at[0] = uint32_t(desc.base);
            at[1] = uint32_t(desc.base >> 32);
            at[2] = b.va ? desc.range : 0;
        } else if (e.kind == KernelSampler) {
            uint32_t tsc = dev.neutralTsc_;
            if (e.slot & kConstexprSamplerSlot) {
                const uint32_t index = e.slot & ~kConstexprSamplerSlot;
                if (index >= dev.constexprCount_) {
                    *why = "a kernel's constexpr sampler the pack holds no header of";
                    return false;
                }
                tsc = dev.constexprTsc_[index];
            }
            at[0] |= tsc << 20;
        }
    }
    uint32_t words[8] = {};
    if (r.regionWord >= 0 && !computeRegionWords(region.base, grid, nominal, words)) {
        *why = "a threadgroup m2v's dispatch region cannot name";
        return false;
    }
    const uint32_t base[3] = {};  // an indirect dispatch is never cut into pieces
    rootWords(r, base, region.groups.data(), setVa, residualMask, residuals, words, root);
    stageInWords(r, stageIn, root + kRootPush / 4);
    return qmdWords(k, region.groups.data(), rootVa, setVa, qmd, why);
}

void ComputeEncoder::inheritBuffers(IcbBuffer (&out)[31], std::array<uint32_t, 31> &strides)
{
    strides = stageIn_.strides;
    for (uint32_t slot = 0; slot < 31; slot++) {
        const Bound &b = bufs_[slot];
        out[slot] = {b.va, b.size};
        if (b.buffer) {
            cb_.use(*b.buffer, true);
            if (b.buffer->hostVisible && !b.buffer->wc && b.buffer->cpu)
                cb_.clean(b.buffer->cpu + b.offset, b.size);
        } else if (b.va) {
            cb_.useAddress(b.va, b.size, true);
        }
    }
}

bool ComputeEncoder::executeSlots(const IcbExecute &x, const char **why)
{
    if (!x.count)
        return true;
    const uint64_t slotBytes = icb::kComputeSlotWords * 4, offset = uint64_t(x.at) * slotBytes,
                   bytes = uint64_t(x.count) * slotBytes;
    if (!x.slots || !x.slots->va || offset > x.slots->length || x.slots->length - offset < bytes) {
        *why = "indirect command buffer commands outside its memory";
        return false;
    }
    if (x.range && (!x.range->va || (x.rangeOffset & 3) || x.rangeOffset > x.range->length ||
                    x.range->length - x.rangeOffset < 8)) {
        *why = "an indirect command buffer's execution range outside its buffer or not four-byte aligned";
        return false;
    }
    if (x.binds && x.binds->length < uint64_t(x.at + x.count) * icb::kComputeBindSlotWords * 4) {
        *why = "indirect command buffer bind slots outside its memory";
        return false;
    }
    cb_.buffersWritten_ = true;
    cb_.use(*x.slots, false);
    if (serial_ && dispatched_)
        barrier();
    dispatched_ = true;
    if (x.binds) {
        IcbBuffer inherited[31];
        std::array<uint32_t, 31> strides;
        inheritBuffers(inherited, strides);
        uint64_t va[31] = {};
        uint32_t size[31] = {}, stride[31] = {};
        for (uint32_t s = 0; s < 31; s++) {
            if ((x.alignedSlots >> s & 1) && (inherited[s].va & 15)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            // a bind without a stride reads at 0, as Metal's does (raw_stage_in.h)
            stride[s] = strides[s] == kNoBindStride ? 0 : strides[s];
            if ((x.strideSlots >> s & 1) && stride[s] > icb::kComputeStrideLimit) {
                *why = "a bind stride past the 24 bits an execute's scratch word holds";
                return false;
            }
            va[s] = inherited[s].va;
            size[s] = inherited[s].size;
        }
        cb_.use(*x.binds, false);
        room(8 + 3 * 31);
        icb::computeBindHead(p_, va, size, stride, x.bindSlots | x.strideSlots);
        cb_.segment(start_, p_.at);
        cb_.segments_.append({x.binds->va + uint64_t(x.at) * icb::kComputeBindSlotWords * 4,
                              x.count * icb::kComputeBindSlotWords, TegraGM20BPushNoPrefetch},
                             false);
        start_ = p_.at;
        room(4);
        icb::computeBindTail(p_);
    }
    room(16);
    icb::computeExecuteHead(p_, x.written || x.binds, x.range != nullptr, x.first, x.count);
    if (x.range) {
        cb_.use(*x.range, false);
        if (x.range->hostVisible && !x.range->wc && x.range->cpu)
            cb_.clean(x.range->cpu + x.rangeOffset, 8);
        cb_.segment(start_, p_.at);
        cb_.segments_.append({x.range->va + x.rangeOffset, 2, TegraGM20BPushNoPrefetch}, false);
        start_ = p_.at;
    }
    cb_.segment(start_, p_.at);
    cb_.segments_.append({x.slots->va + offset, uint32_t(bytes / 4), TegraGM20BPushNoPrefetch}, false);
    start_ = p_.at;
    room(4);
    icb::computeExecuteTail(p_);
    RAW_DIAGNOSTIC(dev_.stats.dispatches += x.count);
    return true;
}

}  // namespace raw
