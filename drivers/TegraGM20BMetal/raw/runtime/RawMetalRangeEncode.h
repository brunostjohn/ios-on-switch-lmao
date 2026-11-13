// SPDX-License-Identifier: MIT
// A raw range of a command buffer's tape onto the raw path in three steps: open (the raw command buffer, the range's first
// contents and resources, the planar images its draws sample), step (its commands up to a position, as many times as it
// is given more) and seal (its encoders ended, the tail, the kernel's job). Only the thread that owns the device's
// encoding (the submit worker) runs it.
#pragma once
#import "RawMetalInternal.h"
#import "RawMetalCounters.h"
#import "RawMetalFresh.h"
#include <array>
#include <deque>
#include <optional>
#include <set>
#include <vector>
#include "raw_compute.h"
#include "raw_pass_fusion.h"
#include "raw_turned_pass.h"

class RMRangeEncode {
public:
    // `whole`: [from, to) is the whole range, its resources and first contents taken at once; else they are taken as each
    // encoder is stepped into, and `to` is only where the range will end at most. False: *why (and *refusedEncoder where
    // one encoder is to blame); nothing of it was submitted.
    bool open(RMCommandBuffer *cb, size_t from, size_t to, bool whole, const char **why, size_t *refusedEncoder);
    bool step(size_t until);
    // False: *why, or `stale` (a native surface changed: encode it again)
    bool seal(size_t to, bool &stale);
    // What open() took that no seal() submitted goes back
    void reset();

    size_t position() const
    {
        return at_;
    }

private:
    // tRawEncoding (RawMetalPrivateComp.mm) while a step of it runs
    struct Encoding {
        Encoding()
        {
            tRawEncoding = true;
        }

        ~Encoding()
        {
            tRawEncoding = false;
        }
    };

    struct Resolve {
        uint32_t attachment;
        __unsafe_unretained RMTexture *into;
        bool keepSamples;  // MTLStoreActionStoreAndMultisampleResolve
        uint32_t level, slice;
    };

    // A depth attachment's resolve (raw_samples.h depthResolveState): a pass of the depth resolve state of its format and
    // filter into the resolve texture, writing each pixel's depth whatever it held
    struct DepthResolve {
        __unsafe_unretained RMTexture *into = nil;
        uint32_t filter = 0;
    };

    struct StencilResolve {  // CommandBuffer::stencilResolve's pass (raw_stencil_bits.h)
        __unsafe_unretained RMTexture *into = nil;
        raw::StencilSample pick = raw::StencilSample::Zero;
    };

    struct Conversion {
        __unsafe_unretained RMTexture *t;
        uint32_t seed;
        uint64_t writes, nativeGeneration;
    };

    bool fail(RMPred p, const char *w, uint64_t a = 0, uint64_t b = 0);
    bool refused();
    bool resources(size_t from, size_t to);
    bool conversions();
    int standInPass(RMTexture *t, bool store);
    void standInBackings();
    bool storeStandIns();
    bool resolvePasses();
    void refresh(size_t pass);
    bool standIns(size_t pass);
    bool command(size_t at);
    bool beginPass(size_t at);
    bool endEncoder(size_t at);
    bool endEncoders();

    __unsafe_unretained RMCommandBuffer *cb_ = nil;
    raw::Device *d_ = nullptr;
    raw::CommandBuffer *rc_ = nullptr;
    const char **why_ = nullptr;
    size_t *refusedEncoder_ = nullptr;
    size_t from_ = 0, to_ = 0, at_ = 0;
    bool whole_ = true;
    uint64_t generation_ = 0;
    std::optional<FreshWrites> firstContents_;
    std::optional<RMCounterEmit> counters_;

    raw::RenderEncoder *re_ = nullptr;
    raw::BlitEncoder *be_ = nullptr;
    std::optional<raw::ComputeEncoder> ce_;
    bool serialCompute_ = false, computeUnit_ = false;
    raw::StateCursor stateCursor_;
    raw::FusionPass fused_;
    size_t fusedAt_ = SIZE_MAX;
    size_t renderBegin_ = SIZE_MAX;
    bool continuing_ = false;
    size_t encoder_ = SIZE_MAX;  // the encoder being encoded, to blame for a refusal (SIZE_MAX: none)

    // A planar render target's stand-in passes (raw::Texture::planarStandIn)
    std::vector<__unsafe_unretained RMTexture *> standInLoads_, planesWritten_;
    std::vector<std::pair<__unsafe_unretained RMTexture *, uint32_t>> standInStores_;  // and their attachment slots
    std::array<const raw::Texture *, 8> backings_{};
    std::vector<Resolve> resolves_;
    std::deque<raw::Texture> resolveTargets_;  // the encoder reads them until its end()
    raw::TurnedPass turned_;                   // the open pass's turn copies (raw_turned_pass.h)
    DepthResolve depthResolve_;
    StencilResolve stencilResolve_;
    std::vector<Conversion> converted_;  // planar images converted here, at their surfaces' seeds and write counts
    std::set<RMTexture *> fresh_;
    raw::Texture levelTargets_[8];  // the open pass's level targets: the encoder reads them until its end()
    raw::Texture zetaPart_;         // and its depth/stencil attachment's layer
    std::set<RMTexture *> standInFresh_;
    std::deque<raw::Texture> codes_;  // the XR conversions' sources: the codes' headers (read until the encoder's end)
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t levelPasses_ = 0;
#endif
};

// The submit worker's one range encode
RMRangeEncode &rmRangeEncode();
