// SPDX-License-Identifier: MIT
// See TegraGM20BMetalDirect.h.
#import "TegraGM20BMetalDirect.h"

namespace gm20b {
namespace {

struct ObjcRequest {
    id descriptor;
    DirectTranslator translator;
};

bool objcRender(const void *descriptor, bool barrier, uint64_t textureFormat, uint32_t textureSamples,
                const NmbDirectBinds *binds, uint32_t, bool (*each)(const NmbDirectState *, void *), void *ctx,
                const char **why)
{
    const auto &req = *static_cast<const ObjcRequest *>(descriptor);
    return req.translator.render(req.descriptor, barrier, NSUInteger(textureFormat), NSUInteger(textureSamples), binds,
                                 each, ctx, why);
}

bool objcCompute(const void *descriptor, const NmbDirectDispatch *dispatch,
                 bool (*each)(const NmbDirectKernel *, void *), void *ctx, const char **why)
{
    const auto &req = *static_cast<const ObjcRequest *>(descriptor);
    return req.translator.compute(req.descriptor, dispatch, each, ctx, why);
}

}  // namespace

Direct::Direct(PackRules rules, bool rankArtifacts, DirectTranslator translator)
    : translator_(translator),
      core_(std::move(rules), rankArtifacts,
            {translator.render ? &objcRender : nullptr, translator.compute ? &objcCompute : nullptr})
{}

CompileOutcome Direct::make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                            std::string &why, ProbeTextures textures, std::map<uint32_t, uint32_t> *registers,
                            bool split, PackForm requested)
{
    // The function's reflection marks active an attribute a function constant gates (CoreRE's vsShadow texcoord, under
    // kEnableAlphaCutoutShadows), which a state that leaves the constant undefined or false does not read: m2v links
    // only what the specialised stage reads and refuses a state whose descriptor lacks one of those
    // (pipeline/link/vertex.rs), so an attribute the descriptor leaves unset is not one the stage reads
    MTLVertexDescriptor *vertexDescriptor = descriptor.vertexDescriptor;
    std::vector<uint32_t> attributes;
    for (MTLVertexAttribute *a in descriptor.vertexFunction.vertexAttributes)
        if (a.isActive &&
            (!vertexDescriptor || vertexDescriptor.attributes[a.attributeIndex].format != MTLVertexFormatInvalid))
            attributes.push_back(uint32_t(a.attributeIndex));
    const ObjcRequest request{descriptor, translator_};
    ProbeTexturesCore coreTextures{uint64_t(textures.format),  uint64_t(textures.samples), std::move(textures.cubic),
                                   textures.fragmentTests,     textures.bindStride,        textures.xrSlots,
                                   std::move(textures.written)};
    return core_.make(&request, attributes, key, out, why, std::move(coreTextures), registers, split, requested);
}

KernelDispatch::KernelDispatch(const uint32_t threads[3], const std::map<uint32_t, uint32_t> &memory, bool regionMode)
    : core_(threads, memory, regionMode)
{}

void KernelDispatch::texture(uint32_t slot, MTLTextureDescriptor *d, bool planes)
{
    const MTLTextureSwizzleChannels w = d.swizzle;
    BoundTextureDesc desc;
    desc.pixelFormat = d.pixelFormat;
    desc.textureType = d.textureType;
    desc.usage = d.usage;
    desc.mipmapLevelCount = d.mipmapLevelCount;
    desc.arrayLength = d.arrayLength;
    desc.sampleCount = d.sampleCount;
    desc.swizzle[0] = uint8_t(w.red);
    desc.swizzle[1] = uint8_t(w.green);
    desc.swizzle[2] = uint8_t(w.blue);
    desc.swizzle[3] = uint8_t(w.alpha);
    core_.texture(slot, desc, planes);
}

void KernelDispatch::sampler(uint32_t slot, MTLSamplerDescriptor *d)
{
    BoundSamplerDesc desc;
    desc.minFilter = d.minFilter;
    desc.magFilter = d.magFilter;
    desc.mipFilter = d.mipFilter;
    desc.sAddressMode = d.sAddressMode;
    desc.tAddressMode = d.tAddressMode;
    desc.rAddressMode = d.rAddressMode;
    desc.borderColor = d.borderColor;
    desc.normalizedCoordinates = d.normalizedCoordinates;
    desc.maxAnisotropy = d.maxAnisotropy;
    desc.compareFunction = d.compareFunction;
    desc.lodAverage = d.lodAverage;
    desc.lodMinClamp = d.lodMinClamp;
    desc.lodMaxClamp = d.lodMaxClamp;
    core_.sampler(slot, desc);
}

const NmbDirectDispatch &KernelDispatch::dispatch()
{
    return core_.dispatch();
}

CompileOutcome Direct::kernel(MTLComputePipelineDescriptor *descriptor, const NmbDirectDispatch &dispatch,
                              const std::string &key, ComputeRecord &out, std::string &why)
{
    const ObjcRequest request{descriptor, translator_};
    return core_.kernel(&request, dispatch, key, out, why);
}

}  // namespace gm20b
