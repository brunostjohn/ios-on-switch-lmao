// SPDX-License-Identifier: MIT
#import "RawRenderKey.h"
#include "NativeBridgeMetalFrontend.h"

namespace raw {
namespace {

bool functionAir(id<MTLFunction> function, FunctionSource source, AirHash &out)
{
    const void *bytes = nullptr;
    size_t length = 0;
    NSString *name = nil;
    std::map<std::string, AirHash> hashes;
    const bool found = source ? source(function, &bytes, &length, &name)
                              : native_bridge_metal_function_metallib(function, &bytes, &length) != nil;
    if (!found || !metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
        return false;
    if (!source)
        name = native_bridge_metal_function_library_name(function);
    const auto entry = hashes.find(name.UTF8String ?: "");
    if (entry == hashes.end())
        return false;
    out = entry->second;
    return true;
}

}  // namespace

bool renderCacheKey(MTLRenderPipelineDescriptor *descriptor, const std::string &key, bool own, uint64_t stack,
                    CacheKey &out, FunctionSource source)
{
    AirHash vertex, fragment;
    if (!functionAir(descriptor.vertexFunction, source, vertex) ||
        !functionAir(descriptor.fragmentFunction, source, fragment))
        return false;
    std::vector<LinkedStageAir> linked;
    const std::pair<char, MTLLinkedFunctions *> stages[] = {{'v', descriptor.vertexLinkedFunctions},
                                                            {'f', descriptor.fragmentLinkedFunctions}};
    for (const auto &[stage, functions] : stages)
        for (NSArray *list : {functions.functions, functions.privateFunctions})
            for (id<MTLFunction> function in list) {
                LinkedStageAir identity{stage, {}};
                if (!functionAir(function, source, identity.air))
                    return false;
                linked.push_back(identity);
            }
    out = renderCacheKey(stack, key, vertex, fragment, linked, own);
    return true;
}

}  // namespace raw
