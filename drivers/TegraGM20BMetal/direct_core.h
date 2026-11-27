// SPDX-License-Identifier: MIT
// Pure C++17 core of the direct compile (direct.h, direct_compute.h), shared between the Objective-C++ driver facade
// (TegraGM20BMetalDirect.{h,mm}) and desktop seed builders.
#pragma once
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "compute_key.h"
#include "compute_record.h"
#include "cubic.h"
#include "direct.h"
#include "direct_abi.h"
#include "direct_compute.h"
#include "pack.h"
#include "record_parts.h"
#include "stage_textures.h"
#include "written_textures.h"

namespace gm20b {

struct BoundTextureDesc {
    uint64_t pixelFormat = 0;
    uint64_t textureType = 2;
    uint64_t usage = 0;
    uint64_t mipmapLevelCount = 1;
    uint64_t arrayLength = 1;
    uint64_t sampleCount = 1;
    uint8_t swizzle[4] = {2, 3, 4, 5};
};

struct BoundSamplerDesc {
    uint64_t minFilter = 0;
    uint64_t magFilter = 0;
    uint64_t mipFilter = 0;
    uint64_t sAddressMode = 0;
    uint64_t tAddressMode = 0;
    uint64_t rAddressMode = 0;
    uint64_t borderColor = 0;
    bool normalizedCoordinates = true;
    uint64_t maxAnisotropy = 1;
    uint64_t compareFunction = 0;
    bool lodAverage = false;
    float lodMinClamp = 0.0f;
    float lodMaxClamp = FLT_MAX;
};

class KernelDispatchCore {
public:
    KernelDispatchCore(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode = false)
    {
        std::memcpy(dispatch_.threads, threads, sizeof dispatch_.threads);
        dispatch_.regionMode = regionMode;
        for (const auto &[index, length] : memory)
            if (index < 31)
                dispatch_.memory[index] = length;
    }

    KernelDispatchCore(const KernelDispatchCore &) = delete;
    KernelDispatchCore &operator=(const KernelDispatchCore &) = delete;

    void texture(uint32_t slot, const BoundTextureDesc &d, bool planes = false)
    {
        const uint32_t swizzle = NMB_DIRECT_BOUND_SWIZZLE_STATED |
                                 ((uint32_t(d.swizzle[0]) & 7u) << NMB_DIRECT_BOUND_SWIZZLE_SHIFT) |
                                 ((uint32_t(d.swizzle[1]) & 7u) << (NMB_DIRECT_BOUND_SWIZZLE_SHIFT + 3u)) |
                                 ((uint32_t(d.swizzle[2]) & 7u) << (NMB_DIRECT_BOUND_SWIZZLE_SHIFT + 6u)) |
                                 ((uint32_t(d.swizzle[3]) & 7u) << (NMB_DIRECT_BOUND_SWIZZLE_SHIFT + 9u));
        textures_.push_back({slot, (planes ? NMB_DIRECT_BOUND_TWO_PLANES : 0u) | swizzle, d.pixelFormat, d.textureType,
                             d.usage, d.mipmapLevelCount, d.arrayLength, d.sampleCount});
    }

    void sampler(uint32_t slot, const BoundSamplerDesc &d)
    {
        samplers_.push_back({slot,
                             {d.minFilter, d.magFilter, d.mipFilter, d.sAddressMode, d.tAddressMode, d.rAddressMode,
                              d.borderColor, d.normalizedCoordinates, d.maxAnisotropy, d.compareFunction, d.lodAverage,
                              d.lodMinClamp, d.lodMaxClamp}});
    }

    const NmbDirectDispatch &dispatch()
    {
        dispatch_.textures = textures_.data();
        dispatch_.textureCount = uint32_t(textures_.size());
        dispatch_.samplers = samplers_.data();
        dispatch_.samplerCount = uint32_t(samplers_.size());
        return dispatch_;
    }

private:
    NmbDirectDispatch dispatch_ = {};
    std::vector<NmbDirectBoundTexture> textures_;
    std::vector<NmbDirectBoundSampler> samplers_;
};

struct ProbeTexturesCore {
    uint64_t format = 80;
    uint64_t samples = 1;
    CubicBinds cubic;
    uint8_t fragmentTests = 0;
    uint32_t bindStride = 0;
    uint16_t xrSlots = 0;
    std::vector<WrittenTexture> written;
};

struct DirectTranslatorCore {
    bool (*render)(const void *descriptor, bool barrier, uint64_t textureFormat, uint32_t textureSamples,
                   const NmbDirectBinds *binds, uint32_t stageTextures, bool (*each)(const NmbDirectState *, void *),
                   void *ctx, const char **why) = nullptr;
    bool (*compute)(const void *descriptor, const NmbDirectDispatch *dispatch,
                    bool (*each)(const NmbDirectKernel *, void *), void *ctx, const char **why) = nullptr;
};

// m2v's own instantiation of a state's probe draw that binds every buffer slot at its probe offset (pack/probe_bind.h,
// NmbDirectState draws[3]): its root push words and each stage's argument table, what a host check
// (harness/boundcheck.py) compares the words the driver writes for that draw with
struct BoundDraw {
    uint32_t pushMask = 0;
    uint32_t push[32] = {};
    std::vector<uint32_t> spill[2];
};

namespace detail {

inline uint32_t ctz32(uint32_t mask)
{
#if defined(_MSC_VER) && !defined(__clang__)
    uint32_t bit = 0;
    while ((mask & 1u) == 0u) {
        mask >>= 1;
        bit++;
    }
    return bit;
#else
    return uint32_t(__builtin_ctz(mask));
#endif
}

struct DirectAsked {
    const StageCompile &compile;
    const PackRules &rules;
    const std::string &key;
    const std::vector<uint32_t> &attributes;
    RuntimeRecord &out;
    std::map<uint32_t, uint32_t> *registers;
    bool split;
    bool variants;
    BoundDraw *bound;
    uint16_t xrSlots;
    uint32_t stageTextures;
    const char *kindName;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
    bool splittable = false;
};

inline bool directMade(const NmbDirectState *state, void *ctx) noexcept
{
    DirectAsked &a = *static_cast<DirectAsked *>(ctx);
    try {
        a.splittable = splitsFragment(*state);
        a.outcome = directRecord(*state, a.compile, a.rules, a.key, a.attributes, a.out, a.why, a.registers, a.split,
                                 a.variants, a.xrSlots, a.stageTextures, a.kindName);
        if (a.bound) {
            const NmbDirectDraw &d = state->draws[3];
            a.bound->pushMask = d.pushMask;
            std::memcpy(a.bound->push, d.push, sizeof a.bound->push);
            for (uint32_t k = 0; k < 2; k++)
                a.bound->spill[k].assign(d.spill[k], d.spill[k] + d.spillWords[k]);
        }
    } catch (const std::exception &e) {
        a.outcome = CompileOutcome::Failed;
        try {
            a.why = e.what();
        } catch (...) {
        }
    } catch (...) {
        a.outcome = CompileOutcome::Failed;
        try {
            a.why = "C++ exception in render compile callback";
        } catch (...) {
        }
    }
    return true;
}

struct DirectAskedKernel {
    const StageCompile &compile;
    const std::string &key;
    ComputeRecord &out;
    std::string why;
    CompileOutcome outcome = CompileOutcome::Failed;
};

inline bool directMadeKernel(const NmbDirectKernel *kernel, void *ctx) noexcept
{
    DirectAskedKernel &a = *static_cast<DirectAskedKernel *>(ctx);
    try {
        a.outcome = directKernel(*kernel, a.compile, a.key, a.out, a.why);
    } catch (const std::exception &e) {
        a.outcome = CompileOutcome::Failed;
        try {
            a.why = e.what();
        } catch (...) {
        }
    } catch (...) {
        a.outcome = CompileOutcome::Failed;
        try {
            a.why = "C++ exception in compute compile callback";
        } catch (...) {
        }
    }
    return true;
}

inline bool stateKeyMaps(const std::string &key, std::vector<NmbDirectBoundTexture> &textures)
{
    const size_t tex = key.rfind(" | tex=");
    if (tex == std::string::npos)
        return false;
    const size_t from = tex + 7, to = key.find(" | ", from);
    const std::string items = key.substr(from, to == std::string::npos ? std::string::npos : to - from);
    std::map<uint32_t, uint8_t> maps;
    for (size_t at = 0; at < items.size();) {
        const size_t comma = std::min(items.find(',', at), items.size());
        BoundTexture item;
        if (!parseComputeTexture(items.substr(at, comma - at), item))
            return false;
        maps[item.slot] = item.addressMap;
        at = comma + 1;
    }
    for (const NmbDirectBoundTexture &t : textures)
        if (!maps.count(t.slot))
            return false;
    for (NmbDirectBoundTexture &t : textures)
        t.planes |= NMB_DIRECT_BOUND_MAP_STATED | uint32_t(maps[t.slot]) << NMB_DIRECT_BOUND_MAP_SHIFT;
    return true;
}

}  // namespace detail

class DirectCore {
public:
    DirectCore(PackRules rules, bool rankArtifacts, DirectTranslatorCore translator)
        : rules_(std::move(rules)), translator_(translator), compile_(StageCompile::linked(rankArtifacts))
#ifdef SWITCH_METAL_KIND
          ,
          stageTextures_(kStageTextures), kindName_(SWITCH_METAL_KIND)
#endif
    {}

    bool configureKind(std::string_view kind);

    uint32_t stageTextures() const
    {
        return stageTextures_;
    }

    const std::string &kindName() const
    {
        return kindName_;
    }

    CompileOutcome make(const void *descriptor, const std::vector<uint32_t> &attributes, const std::string &key,
                        RuntimeRecord &out, std::string &why, ProbeTexturesCore textures = {},
                        std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false,
                        PackForm requested = PackForm::Default)
    {
        storageRefusal_.clear();
        splittable_ = false;
        if (!translator_.render || !StageCompile::abiMatches()) {
            why = translator_.render ? "the stage compile's structs are not direct_nvk.h's" : "no translator";
            return CompileOutcome::Failed;
        }
        if (!stageTextures_) {
            why = "no render stage texture slots configured";
            return CompileOutcome::Failed;
        }
        const auto barrierForm = [&](bool twin) {
            const CompileOutcome made = form(descriptor, true, textures, key, attributes, out, why, registers, split);
            if (made != CompileOutcome::Made)
                return made;
            dropUnreadFetches(out);
            if (twin || (out.pipeline.flags & PipelineFetches))
                out.pipeline.flags |= PipelineBarrierForm;
            return strideTwin(descriptor, textures, key, attributes, out, why, split);
        };
        if (requested == PackForm::Barrier)
            return barrierForm(true);
        CompileOutcome outcome = form(descriptor, false, textures, key, attributes, out, why, registers, split);
        if (outcome == CompileOutcome::Made) {
            const bool storage = out.pipeline.flags & PipelineStorageMetadata;
            dropUnreadFetches(out);
            if (!storage || (out.pipeline.storageMask && loadsSurface(out)))
                return strideTwin(descriptor, textures, key, attributes, out, why, split);
            storageRefusal_ = out.pipeline.storageMask ? "storage form: its program loads no colour"
                                                       : "storage form: its program reads and writes no colour";
            return barrierForm(false);
        }
        if (outcome != CompileOutcome::Refused || why.rfind("storage form", 0) != 0)
            return outcome;
        storageRefusal_ = why;
        return barrierForm(false);
    }

    void keepStages(StageMemo *memo)
    {
        compile_.memo = memo;
    }

    void makeVariants(bool on)
    {
        variants_ = on;
    }

    const std::string &storageRefusal() const
    {
        return storageRefusal_;
    }

    bool splittable() const
    {
        return splittable_;
    }

    void keepBoundDraws(bool on)
    {
        keepBound_ = on;
    }

    const BoundDraw &boundDraw() const
    {
        return bound_;
    }

    const std::map<uint32_t, uint32_t> &strideTwinRegisters() const
    {
        return twinRegisters_;
    }

    CompileOutcome kernel(const void *descriptor, const NmbDirectDispatch &dispatch, const std::string &key,
                          ComputeRecord &out, std::string &why)
    {
        if (!translator_.compute || !StageCompile::abiMatches()) {
            why =
                translator_.compute ? "the stage compile's structs are not direct_nvk.h's" : "no translator of kernels";
            return CompileOutcome::Failed;
        }
        std::vector<NmbDirectBoundTexture> textures(dispatch.textures, dispatch.textures + dispatch.textureCount);
        const bool mapsStated = detail::stateKeyMaps(key, textures);
        NmbDirectDispatch stated = dispatch;
        stated.textures = textures.data();
        detail::DirectAskedKernel a = {compile_, key, out, {}};
        const char *refused = nullptr;
        if (!translator_.compute(descriptor, mapsStated ? &stated : &dispatch, detail::directMadeKernel, &a,
                                 &refused)) {
            why = std::string("m2v refused its translation: ") + (refused ? refused : "");
            const bool fails =
                refused && (!std::strncmp(refused, "retry: ", 7) || !std::strncmp(refused, "fails: ", 7));
            return fails ? CompileOutcome::Failed : CompileOutcome::Refused;
        }
        why = a.why;
        if (a.outcome == CompileOutcome::Made)
            out.kernel.mapsStated = mapsStated;
        return a.outcome;
    }

private:
    PackRules rules_;
    DirectTranslatorCore translator_;
    StageCompile compile_;
    uint32_t stageTextures_ = 0;
    std::string kindName_;
    bool variants_ = false;
    std::string storageRefusal_;
    bool splittable_ = false;
    bool keepBound_ = false;
    BoundDraw bound_;
    std::map<uint32_t, uint32_t> twinRegisters_;

    CompileOutcome form(const void *descriptor, bool barrier, ProbeTexturesCore textures, const std::string &key,
                        const std::vector<uint32_t> &attributes, RuntimeRecord &out, std::string &why,
                        std::map<uint32_t, uint32_t> *registers, bool split)
    {
        detail::DirectAsked a = {compile_,
                                 rules_,
                                 key,
                                 attributes,
                                 out,
                                 registers,
                                 split,
                                 variants_,
                                 keepBound_ ? &bound_ : nullptr,
                                 textures.xrSlots,
                                 stageTextures_,
                                 kindName_.empty() ? nullptr : kindName_.c_str(),
                                 {}};
        if (keepBound_)
            bound_ = {};
        std::vector<NmbDirectCubicSampler> cubic;
        std::vector<NmbDirectTextureFormat> formats;
        for (const CubicBinds::Sampler &c : textures.cubic.samplers)
            cubic.push_back({c.slot, c.s, c.t, c.r, c.border, c.normalized, c.stage == kCubicVertex});
        for (const CubicBinds::Texture &t : textures.cubic.textures)
            formats.push_back({t.slot, t.format, t.stage == kCubicVertex});
        for (const WrittenTexture &t : textures.written)
            formats.push_back({t.slot, t.format, false});
        // Two-plane XR10 slots are rewritten into paired-plane textures after translation and must not receive
        // m2v's 8-bit UNORM software linear-filter lowering (MTLPixelFormatASTC_4x4_LDR -> TexelFormat::OTHER).
        for (uint32_t mask = textures.xrSlots; mask; mask &= mask - 1) {
            const uint32_t slot = detail::ctz32(mask);
            const bool listed = std::any_of(formats.begin(), formats.end(), [&](const NmbDirectTextureFormat &f) {
                return !f.vertex && f.slot == slot;
            });
            if (!listed)
                formats.push_back({slot, 204u, false});
        }
        const NmbDirectBinds binds = {cubic.data(),           uint32_t(cubic.size()),
                                      formats.data(),         uint32_t(formats.size()),
                                      textures.fragmentTests, textures.bindStride};
        const bool plain = formats.empty() && cubic.empty() && !textures.fragmentTests && !textures.bindStride;
        const char *refused = nullptr;
        if (!translator_.render(descriptor, barrier, textures.format, uint32_t(textures.samples),
                                plain ? nullptr : &binds, stageTextures_, detail::directMade, &a, &refused)) {
            why = std::string("m2v refused its translation: ") + (refused ? refused : "");
            return refused && !std::strncmp(refused, "retry: ", 7) ? CompileOutcome::Failed : CompileOutcome::Refused;
        }
        why = a.why;
        splittable_ = a.splittable;
        if (a.outcome == CompileOutcome::Made && !compiledFor(out.pipeline, textures.written)) {
            why = "a written texture's variant m2v compiled for another format than the one bound";
            return CompileOutcome::Refused;
        }
        return a.outcome;
    }

    CompileOutcome strideTwin(const void *descriptor, ProbeTexturesCore textures, const std::string &key,
                              const std::vector<uint32_t> &attributes, RuntimeRecord &out, std::string &why, bool split)
    {
        if (!bindStrideStreams(out.pipeline))
            return CompileOutcome::Made;
        textures.bindStride = kStreamStrideField + 1;
        auto twin = std::make_shared<RuntimeRecord>();
        std::string twinWhy;
        const bool barrier = (out.pipeline.flags & PipelineBarrierForm) || !storageRefusal_.empty();
        const BoundDraw bound = bound_;
        twinRegisters_.clear();
        const CompileOutcome outcome = form(descriptor, barrier, textures, key, attributes, *twin, twinWhy,
                                            keepBound_ ? &twinRegisters_ : nullptr, split);
        bound_ = bound;
        if (outcome == CompileOutcome::Failed) {
            why = "its stride twin: " + twinWhy;
            return outcome;
        }
        if (outcome != CompileOutcome::Made || bindStrideStreams(twin->pipeline))
            return CompileOutcome::Made;
        dropUnreadFetches(*twin);
        twin->pipeline.flags |= out.pipeline.flags & PipelineBarrierForm;
        twin->pipeline.keyHash = packKeyHash(strideTwinKey(key));
        out.pipeline.strideTwin = twin->pipeline.keyHash;
        out.strideTwin = std::move(twin);
        return CompileOutcome::Made;
    }
};

}  // namespace gm20b
