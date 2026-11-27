// SPDX-License-Identifier: MIT
// The device's direct compile of a state the pack lacks (direct.h), in process: the state's record made from m2v's
// translation of its functions and NVK's stage compile, at its creation, as the pack generator makes the pack's records. A state
// whose fragment function reads its color attachments uses its qualified ordered form when the native views support it.
#pragma once
#import <Metal/Metal.h>
#include <map>
#include <string>
#include <vector>
#include "direct_core.h"

namespace gm20b {

// A dispatch's shape and what it binds besides its buffers, as the direct compile of its kernel takes them
class KernelDispatch {
public:
    KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode = false);
    KernelDispatch(const KernelDispatch &) = delete;
    KernelDispatch &operator=(const KernelDispatch &) = delete;

    // `planes`: an RGB10A8_2P_XR10 texture held in Metal's two planes (BoundTexture::planes)
    void texture(uint32_t slot, MTLTextureDescriptor *d, bool planes = false);
    void sampler(uint32_t slot, MTLSamplerDescriptor *d);
    const NmbDirectDispatch &dispatch();

private:
    KernelDispatchCore core_;
};

// The textures a state is planned for, as the probe binds them (pack/probe.mm: BGRA8 but for the driver's own states that
// read codes, and the multisampled ones its resolves read)
struct ProbeTextures {
    MTLPixelFormat format = MTLPixelFormatBGRA8Unorm;
    NSUInteger samples = 1;
    CubicBinds cubic;  // the cubic samplers and the textures sampled through them (a state's cubic variant)
    uint8_t fragmentTests = 0;
    uint32_t bindStride = 0;  // the attributeStride every vertex buffer is bound at (0: none)
    uint16_t xrSlots = 0;     // fragment slots it samples two-plane RGB10A8_2P_XR10 at (its " | xrs=" variant)
    // the formats of the textures it writes at these slots (written_textures.h: its " | wtex=" variant)
    std::vector<WrittenTexture> written;
};

// What makes a state's or a kernel's translation: the offline compile (TegraGM20BMetalOffline.h), whose descriptors hold its own
// functions, or a host tool's own. False with `why`: the translator refused it.
struct DirectTranslator {
    bool (*render)(id descriptor, bool barrier, NSUInteger textureFormat, NSUInteger textureSamples,
                   const NmbDirectBinds *binds, bool (*each)(const NmbDirectState *, void *), void *ctx,
                   const char **why) = nullptr;
    bool (*compute)(id descriptor, const NmbDirectDispatch *dispatch, bool (*each)(const NmbDirectKernel *, void *),
                    void *ctx, const char **why) = nullptr;
};

class Direct {
public:
    // Records under `rules` alone, translated by `translator`
    Direct(PackRules rules, bool rankArtifacts, DirectTranslator translator);

    // The record of the state `descriptor` (of m2v's functions) describes, whose pack key is `key`; `registers`: as
    // directRecord's; `split`: with its fragment program's split (directRecord). A state of bind-stride vertex streams
    // gets its stride twin (RuntimeRecord::strideTwin) with it.
    CompileOutcome make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                        std::string &why, ProbeTextures textures = {},
                        std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false,
                        PackForm requested = PackForm::Default);

    // The shader compiler's: a stage compiled from the same inputs as an earlier one is that one (StageCompile::memo)
    void keepStages(StageMemo *memo)
    {
        core_.keepStages(memo);
    }

    // The pack generator's: records also hold the program variants directRecord makes (`variants`)
    void makeVariants(bool on)
    {
        core_.makeVariants(on);
    }

    // Why the last make's storage form was refused where it made the barrier form instead; empty where it made none
    const std::string &storageRefusal() const
    {
        return core_.storageRefusal();
    }

    // Whether the last make's state is one directRecord splits (splitsFragment)
    bool splittable() const
    {
        return core_.splittable();
    }

    // The pack generator's: each make keeps m2v's bound probe draw of the state it made (boundDraw)
    void keepBoundDraws(bool on)
    {
        core_.keepBoundDraws(on);
    }

    const BoundDraw &boundDraw() const
    {
        return core_.boundDraw();
    }

    // The pack generator's: the registers of the last make's stride twin (RuntimeRecord::strideTwin), as `registers`
    // holds the record's
    const std::map<uint32_t, uint32_t> &strideTwinRegisters() const
    {
        return core_.strideTwinRegisters();
    }

    // The record of `descriptor`'s kernel at a dispatch of `dispatch`'s shape and bindings, whose compute key is `key`
    // (direct_compute.h)
    CompileOutcome kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                          const std::string &key, ComputeRecord &out, std::string &why);

private:
    DirectTranslator translator_;
    DirectCore core_;
};

}  // namespace gm20b
