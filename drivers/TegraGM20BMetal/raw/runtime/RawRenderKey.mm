// SPDX-License-Identifier: MIT
#import "RawRenderKey.h"
#include "NativeBridgeMetalFrontend.h"

// Weak, so the pack generator links without the frontend
extern "C" id native_bridge_metal_function_metallib(id function, const void **bytes, size_t *length)
    __attribute__((weak_import));
extern "C" NSString *native_bridge_metal_function_library_name(id function) __attribute__((weak_import));

namespace raw {
namespace {

bool functionAir(id<MTLFunction> function, AirHash &out)
{
    const void *bytes = nullptr;
    size_t length = 0;
    std::string entry;
    std::map<std::string, AirHash> hashes;
    if (!functionLibrary(function, bytes, length, entry) ||
        !metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes))
        return false;
    const auto found = hashes.find(entry);
    if (found == hashes.end())
        return false;
    out = found->second;
    return true;
}

}  // namespace

bool functionLibrary(id<MTLFunction> function, const void *&bytes, size_t &length, std::string &entry)
{
    if ([(id)function conformsToProtocol:@protocol(RawLibraryFunction)]) {
        id<RawLibraryFunction> own = (id<RawLibraryFunction>)function;
        bytes = [own libraryBytes:&length];
        entry = own.libraryFunctionName.UTF8String ?: "";
        return bytes != nullptr;
    }
    if (!native_bridge_metal_function_metallib || !native_bridge_metal_function_metallib(function, &bytes, &length))
        return false;
    entry = native_bridge_metal_function_library_name(function).UTF8String ?: "";
    return true;
}

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
