// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/compute_parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

namespace switch_seed {
namespace {

constexpr uint64_t kTextureType2D = 2;

bool parseSamplers(const std::string &text, std::vector<gm20b::BoundSampler> &out, std::string &why)
{
    std::istringstream in(text);
    std::string one;
    while (std::getline(in, one, ',')) {
        gm20b::BoundSampler b = {};
        unsigned long long f[9] = {};
        unsigned slot = 0, lo = 0, hi = 0;
        int normalized = 0, average = 0;
        if (std::sscanf(one.c_str(), "%u:%llu:%llu:%llu:%llu:%llu:%llu:%x:%x:%llu:%llu:%llu:%d:%d", &slot, &f[0], &f[1],
                        &f[2], &f[3], &f[4], &f[5], &lo, &hi, &f[6], &f[7], &f[8], &normalized, &average) != 14) {
            why = "an smp= field it cannot read";
            return false;
        }
        b.slot = slot;
        b.minFilter = f[0];
        b.magFilter = f[1];
        b.mipFilter = f[2];
        b.sAddress = f[3];
        b.tAddress = f[4];
        b.rAddress = f[5];
        b.maxAnisotropy = f[6];
        b.compare = f[7];
        b.border = f[8];
        std::memcpy(&b.lodMin, &lo, 4);
        std::memcpy(&b.lodMax, &hi, 4);
        b.normalized = normalized != 0;
        b.lodAverage = average != 0;
        out.push_back(b);
    }
    return true;
}

bool parseTextures(const std::string &text, std::vector<gm20b::BoundTexture> &out, std::string &why)
{
    std::istringstream in(text);
    std::string one;
    while (std::getline(in, one, ',')) {
        gm20b::BoundTexture t = {};
        if (!gm20b::parseComputeTexture(one, t)) {
            why = "a tex= field it cannot read";
            return false;
        }
        if (t.textureType != kTextureType2D || t.levels != 1 || t.arrayLength != 1) {
            why = "a texture of another type than a one-level 2D one";
            return false;
        }
        out.push_back(t);
    }
    return true;
}

bool headField(const std::string &line, const char *name, std::string &out)
{
    const std::string tag = std::string(" ") + name + "=";
    const size_t at = line.find(tag);
    if (at == std::string::npos || at >= line.find(" | "))
        return false;
    const size_t from = at + tag.size();
    const size_t end = line.find_first_of(" |", from);
    out = line.substr(from, end == std::string::npos ? std::string::npos : end - from);
    return true;
}

bool parseStageInputInto(const std::string &segment, ComputeDescriptor *out)
{
    const size_t inAt = segment.find(" in ");
    if (inAt == std::string::npos)
        return false;
    std::istringstream items(segment.substr(inAt + 4));
    std::string item;
    bool attributes = false, index = false;
    while (items >> item) {
        unsigned long long i = 0, f[6] = {};
        if (std::sscanf(item.c_str(), "a%llu=%llu:%llu:%llu:%llu:%llu:%llu", &i, f, f + 1, f + 2, f + 3, f + 4,
                        f + 5) == 7 &&
            i < 31 && f[2] < 31) {
            if (out) {
                out->hasStageInput = true;
                out->stageAttributes[i] = {uint64_t(f[0]), uint64_t(f[1]), uint64_t(f[2])};
                out->stageLayouts[f[2]] = {f[3] == UINT32_MAX ? UINT64_MAX : uint64_t(f[3]), uint64_t(f[4]),
                                           uint64_t(f[5])};
            }
            attributes = true;
        } else if (std::sscanf(item.c_str(), "i=%llu:%llu", f, f + 1) == 2) {
            if (out) {
                out->stageIndexType = uint64_t(f[0]);
                out->stageIndexBuffer = uint64_t(f[1]);
            }
            index = true;
        } else {
            return false;
        }
    }
    return attributes && index;
}

}  // namespace

gm20b::BoundSamplerDesc samplerDescOf(const gm20b::BoundSampler &b)
{
    gm20b::BoundSamplerDesc d;
    d.minFilter = b.minFilter;
    d.magFilter = b.magFilter;
    d.mipFilter = b.mipFilter;
    d.sAddressMode = b.sAddress;
    d.tAddressMode = b.tAddress;
    d.rAddressMode = b.rAddress;
    d.lodMinClamp = b.lodMin;
    d.lodMaxClamp = b.lodMax;
    d.maxAnisotropy = b.maxAnisotropy;
    d.compareFunction = b.compare;
    d.borderColor = b.border;
    d.normalizedCoordinates = b.normalized;
    d.lodAverage = b.lodAverage;
    return d;
}

bool parseComputeLine(const std::string &line, ComputeShape &s, std::string &why)
{
    uint32_t threadsForm = 0;
    if (std::sscanf(line.c_str(), "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", &s.threads[0], &s.threads[1],
                    &s.threads[2], &s.edge[0], &s.edge[1], &s.edge[2], &threadsForm) != 7) {
        why = "not a D line";
        return false;
    }
    if (threadsForm > 1) {
        why = "an invalid dispatch form";
        return false;
    }
    s.factsOnly =
        !s.threads[0] && !s.threads[1] && !s.threads[2] && !s.edge[0] && !s.edge[1] && !s.edge[2] && !threadsForm;
    if (!s.factsOnly) {
        for (uint32_t i = 0; i < 3; ++i) {
            if (!s.threads[i] || s.threads[i] > 1024 || s.edge[i] >= s.threads[i] || (!threadsForm && s.edge[i])) {
                why = "an invalid local size or edge";
                return false;
            }
            s.regionMode |= s.edge[i] != 0;
        }
    }
    std::string text;
    headField(line, "tgmem", s.threadgroupMemory);
    if ((headField(line, "tex", text) && !parseTextures(text, s.textures, why)) ||
        (headField(line, "smp", text) && !parseSamplers(text, s.samplers, why)))
        return false;
    if (s.factsOnly && (!s.threadgroupMemory.empty() || !s.textures.empty() || !s.samplers.empty())) {
        why = "a facts-only compute line cannot bind dispatch resources";
        return false;
    }
    const size_t state = line.find(" | form=");
    const size_t k = state == std::string::npos ? std::string::npos : line.find(" | k ", state);
    if (state == std::string::npos || k == std::string::npos) {
        why = "no state";
        return false;
    }
    if (line.find(" | l ", k) != std::string::npos || line.find(" | p ", k) != std::string::npos) {
        why = "linked functions";
        return false;
    }
    std::istringstream head(line.substr(state + 3, k - state - 3));
    std::string item;
    while (head >> item) {
        if (item.rfind("form=", 0) == 0)
            s.form = item.substr(5);
        else if (item.rfind("opt=", 0) == 0)
            s.options = std::strtoull(item.c_str() + 4, nullptr, 10);
        else if (item.rfind("max=", 0) == 0)
            s.maxThreads = uint32_t(std::strtoul(item.c_str() + 4, nullptr, 10));
        else if (item.rfind("tgm=", 0) == 0)
            s.multipleOfWidth = item.size() > 4 && item[4] == '1';
    }
    const size_t end = line.find(" | ", k + 5);
    if (const size_t in = line.find(" | in ", k); in != std::string::npos) {
        const size_t inEnd = line.find(" | ", in + 3);
        s.stageIn = line.substr(in, (inEnd == std::string::npos ? line.size() : inEnd) - in);
        if (s.form != "d" || !parseStageInputInto(s.stageIn, nullptr)) {
            why = "a stage input it cannot read";
            return false;
        }
    }
    std::istringstream fn(line.substr(k + 5, (end == std::string::npos ? line.size() : end) - k - 5));
    fn >> s.function;
    while (fn >> item && item != "|") {
        if (item.rfind("lib=", 0) == 0)
            s.library = item.substr(4);
        else if (item == "spec")
            s.specialized = true;
        else
            s.constants += (s.constants.empty() ? "" : " ") + item;
    }
    return !s.function.empty() && !s.library.empty();
}

void stateComputeFormatMaps(ComputeShape &s, uint8_t (*formatMap)(uint64_t))
{
    for (gm20b::BoundTexture &t : s.textures)
        if (!t.addressMap)
            t.addressMap = formatMap(t.pixelFormat);
}

std::string computeShapeKey(const ComputeShape &s, const gm20b::AirHash &air)
{
    return gm20b::computeKey(air, parseConstantsLine(s.constants).text() + s.stageIn, s.maxThreads, s.multipleOfWidth,
                             s.threads, s.threadgroupMemory,
                             gm20b::computeTextures(s.textures.data(), s.textures.size()),
                             gm20b::computeSamplers(s.samplers.data(), s.samplers.size()), s.regionMode);
}

ComputeDescriptor buildComputeDescriptor(const ComputeShape &s, const std::shared_ptr<OfflineFunction> &fn)
{
    ComputeDescriptor d;
    d.function = fn;
    if (s.form == "d") {
        d.maxThreads = s.maxThreads;
        d.multipleOfWidth = s.multipleOfWidth;
        if (!s.stageIn.empty())
            parseStageInputInto(s.stageIn, &d);
    }
    return d;
}

std::shared_ptr<OfflineFunction> loadDirectpackKernelFunction(LibraryStore &store, const std::string &libRoot,
                                                              const std::string &dataLibs, const ComputeShape &s,
                                                              gm20b::AirHash &air)
{
    std::string path;
    if (s.library.rfind("data:", 0) == 0) {
        const std::string sha = s.library.substr(5);
        if (!isSafeSha256Hex(sha))
            return nullptr;
        path = (dataLibs.empty() ? std::string(".") : dataLibs) + "/" + sha + ".metallib";
    } else if (!resolveContainedPath(libRoot, s.library, path)) {
        return nullptr;
    }
    std::string err;
    auto lib = store.loadFile(path, err);
    if (!lib)
        return nullptr;
    const bool specialized = s.specialized || !s.constants.empty();
    const ConstantSet cset = specialized ? parseConstantsLine(s.constants) : ConstantSet{};
    auto fn = store.makeFunction(lib, s.function, "", cset, specialized, err);
    if (!fn || !fn->airHash(air))
        return nullptr;
    return fn;
}

void buildDirectpackDispatch(const ComputeShape &s, const uint32_t threads[3],
                             std::unique_ptr<gm20b::KernelDispatchCore> &out)
{
    std::map<uint32_t, uint32_t> memory;
    std::istringstream text(s.threadgroupMemory);
    std::string pair;
    while (std::getline(text, pair, ',')) {
        const size_t colon = pair.find(':');
        if (colon != std::string::npos)
            memory[uint32_t(std::strtoul(pair.c_str(), nullptr, 10))] =
                uint32_t(std::strtoul(pair.c_str() + colon + 1, nullptr, 10));
    }
    uint32_t local[3] = {threads[0], threads[1], threads[2]};
    if (s.factsOnly) {
        local[0] = s.multipleOfWidth ? (s.maxThreads && s.maxThreads < 32 ? s.maxThreads : 32u) : 1u;
        local[1] = 1;
        local[2] = 1;
    }
    out = std::make_unique<gm20b::KernelDispatchCore>(local, memory, s.regionMode);
    for (const gm20b::BoundTexture &b : s.textures) {
        // Matches kernel_shapes.h textures() + direct_kernels.h dispatchOf(): default identity swizzle on the
        // bound descriptor while usage drops ShaderWrite when the corpus texture had a non-identity swizzle.
        const bool swizzled = b.swizzle[0] != 2 || b.swizzle[1] != 3 || b.swizzle[2] != 4 || b.swizzle[3] != 5;
        gm20b::BoundTextureDesc d;
        d.pixelFormat = b.pixelFormat;
        d.textureType = kTextureType2D;
        d.usage = 1u | (swizzled ? 0u : 2u);
        d.mipmapLevelCount = 1;
        d.arrayLength = 1;
        d.sampleCount = 1;
        out->texture(b.slot, d, false);
    }
    for (const gm20b::BoundSampler &b : s.samplers)
        out->sampler(b.slot, samplerDescOf(b));
}

bool buildNativeSeedDispatch(const ComputeShape &s, std::unique_ptr<gm20b::KernelDispatchCore> &out, std::string &why)
{
    std::map<uint32_t, uint32_t> memory;
    std::istringstream allocations(s.threadgroupMemory);
    std::string allocation;
    while (std::getline(allocations, allocation, ',')) {
        unsigned index = 0, size = 0;
        int used = 0;
        if (std::sscanf(allocation.c_str(), "%u:%u%n", &index, &size, &used) != 2 ||
            size_t(used) != allocation.size() || index >= 31 || !memory.emplace(index, size).second) {
            why = "invalid threadgroup memory binding";
            return false;
        }
    }
    out = std::make_unique<gm20b::KernelDispatchCore>(s.threads, memory, s.regionMode);
    for (const gm20b::BoundTexture &t : s.textures) {
        gm20b::BoundTextureDesc d;
        d.pixelFormat = t.pixelFormat;
        d.textureType = t.textureType;
        d.mipmapLevelCount = t.levels;
        d.arrayLength = t.arrayLength;
        d.sampleCount = 1;
        d.usage = 3u;
        out->texture(t.slot, d, t.planes);
    }
    for (const gm20b::BoundSampler &b : s.samplers)
        out->sampler(b.slot, samplerDescOf(b));
    return true;
}

}  // namespace switch_seed
