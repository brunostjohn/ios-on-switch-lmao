// SPDX-License-Identifier: MIT
#import "RawRenderKey.h"
#include "NativeBridgeMetalFrontend.h"

namespace raw {
namespace {

bool functionAir(id<MTLFunction> function, AirHash &out)
{
    const void *bytes = nullptr;
    size_t length = 0;
    std::map<std::string, AirHash> hashes;
    id library = native_bridge_metal_function_metallib(function, &bytes, &length);
    if (!library || !metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
        return false;
    const auto found = hashes.find(native_bridge_metal_function_library_name(function).UTF8String ?: "");
    if (found == hashes.end())
        return false;
    out = found->second;
    return true;
}

}  // namespace

bool renderCacheKey(MTLRenderPipelineDescriptor *descriptor, const std::string &key, bool own, uint64_t stack,
                    CacheKey &out)
{
    AirHash vertex, fragment;
    if (!functionAir(descriptor.vertexFunction, vertex) || !functionAir(descriptor.fragmentFunction, fragment))
        return false;
    std::vector<LinkedStageAir> linked;
    const std::pair<char, MTLLinkedFunctions *> stages[] = {{'v', descriptor.vertexLinkedFunctions},
                                                            {'f', descriptor.fragmentLinkedFunctions}};
    for (const auto &[stage, functions] : stages)
        for (NSArray *list : {functions.functions, functions.privateFunctions})
            for (id<MTLFunction> function in list) {
                LinkedStageAir identity{stage, {}};
                if (!functionAir(function, identity.air))
                    return false;
                linked.push_back(identity);
            }
    out = renderCacheKey(stack, key, vertex, fragment, linked, own);
    return true;
}

}  // namespace raw
