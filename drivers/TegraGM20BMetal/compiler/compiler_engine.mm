// SPDX-License-Identifier: MIT
// See compiler_engine.h.
#import <Metal/Metal.h>
#import "compiler_engine.h"

#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <os/log.h>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BCompilerABI.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawDirect.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/RawRenderKey.h"
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_pack.h"

namespace switch_compiler {
namespace {

constexpr size_t kDataLibraries = 16;

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "compiler");
    return l;
}

void missing(const char *cls, const char *selector)
{
    os_log_error(logger(), "SwitchShaderCompiler: unimplemented -[%{public}s %{public}s]", cls, selector);
}

// <Extensions>/TegraGM20BMetal.bundle/<name>, and with `up` <Extensions>/<name>: what the runtime opens beside the plugin
std::string besidePlugin(const char *name, bool up = false)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&missing), &info) || !info.dli_fname)
        return std::string();
    std::string path = info.dli_fname;
    for (int i = 0; i < (up ? 2 : 1); ++i)
        path.erase(path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    return path + "/" + name;
}

uint64_t acceleratorId()
{
    uint64_t id = 0;
    if (io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"))) {
        IORegistryEntryGetRegistryEntryID(service, &id);
        IOObjectRelease(service);
    }
    return id;
}

id<MTLFunction> function(id<MTLLibrary> library, const raw::RequestStage &s, std::string &why)
{
    MTLFunctionConstantValues *values = nil;
    if (s.specialized && !(values = raw::constantValuesOf(s.constants)))
        return why = "constants it cannot read: " + s.constants, nil;
    if (!s.specialized && !s.constants.empty())
        return why = "constants of a function made without them", nil;
    NSError *error = nil;
    NSString *name = @(s.function.c_str());
    id<MTLFunction> made = nil;
    if (s.name != s.function) {
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = name;
        d.specializedName = @(s.name.c_str());
        d.constantValues = values;
        made = [library newFunctionWithDescriptor:d error:&error];
    } else {
        made = values ? [library newFunctionWithName:name constantValues:values error:&error]
                      : [library newFunctionWithName:name];
    }
    if (!made)
        why = "no function " + s.function + (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
    else if (raw::constantsText(values) != s.constants)
        return why = "constants that do not read back as given", nil;
    return made;
}

int32_t resultOf(raw::CompileOutcome outcome)
{
    return outcome == raw::CompileOutcome::Made      ? TegraGM20BCompileMade
           : outcome == raw::CompileOutcome::Refused ? TegraGM20BCompileRefused
                                                     : TegraGM20BCompileFailed;
}

raw::Digest rulesDigest(const raw::PackRules &r)
{
    std::string bytes;
    const auto word = [&](uint32_t v) {
        bytes.append(reinterpret_cast<const char *>(&v), sizeof v);
    };
    word(uint32_t(r.varying.size()));
    for (uint32_t v : r.varying)
        word(v);
    for (const auto *registers : {&r.baseline, &r.context}) {
        word(uint32_t(registers->size()));
        for (const auto &[method, value] : *registers) {
            word(method);
            word(value);
        }
    }
    return raw::sha256(bytes.data(), bytes.size());
}

}  // namespace

struct Engine::State {
    id<MTLDevice> device = nil;
    uint64_t stack = 0;
    raw::Pack pack;
    raw::Digest rules{};
    std::unique_ptr<raw::Direct> direct;
    std::mutex lock;
    std::map<std::string, id<MTLLibrary>> system;
    std::map<raw::Digest, id<MTLLibrary>> data;
    std::deque<raw::Digest> dataOrder;

    id<MTLLibrary> library(const raw::RequestLibrary &l, std::string &why)
    {
        std::string real;
        if (l.system && !SystemLibraries::resolve(l.path, real, why))
            return nil;
        const raw::Digest digest = l.system ? raw::Digest{} : raw::sha256(l.bytes.data(), l.bytes.size());
        {
            std::lock_guard<std::mutex> g(lock);
            if (l.system) {
                if (const auto found = system.find(real); found != system.end())
                    return found->second;
            } else if (const auto found = data.find(digest); found != data.end()) {
                return found->second;
            }
        }
        NSError *error = nil;
        id<MTLLibrary> made = nil;
        if (l.system) {
            made = [device newLibraryWithURL:[NSURL fileURLWithPath:@(real.c_str())] error:&error];
        } else {
            dispatch_data_t bytes =
                dispatch_data_create(l.bytes.data(), l.bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            made = [device newLibraryWithData:bytes error:&error];
        }
        if (!made) {
            why = std::string("m2v refused the library ") + (l.path.empty() ? "of its data" : l.path) +
                  (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
            return nil;
        }
        std::lock_guard<std::mutex> g(lock);
        if (l.system) {
            system[real] = made;
        } else if (data.emplace(digest, made).second) {
            dataOrder.push_back(digest);
            if (dataOrder.size() > kDataLibraries) {
                data.erase(dataOrder.front());
                dataOrder.pop_front();
            }
        }
        return made;
    }

    bool functions(const raw::CompileRequest &r, std::vector<id<MTLFunction>> &out, std::string &why)
    {
        std::vector<id<MTLLibrary>> libraries;
        for (const raw::RequestLibrary &l : r.libraries) {
            id<MTLLibrary> made = library(l, why);
            if (!made)
                return false;
            libraries.push_back(made);
        }
        for (const raw::RequestStage &s : r.stages) {
            id<MTLFunction> f = function(libraries[s.library], s, why);
            if (!f)
                return false;
            out.push_back(f);
        }
        return true;
    }

    // `p`'s descriptor over this device's functions, checked to key as `p` was keyed
    MTLRenderPipelineDescriptor *descriptor(const Prepared &p, std::string &why)
    {
        const raw::CompileRequest &r = p.request;
        std::vector<id<MTLFunction>> made;
        if (!functions(r, made, why))
            return nil;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        raw::applyFields(r.fields, d);
        d.vertexFunction = made[0];
        d.fragmentFunction = made[1];
        NSMutableDictionary<NSString *, NSMutableArray *> *linked = [NSMutableDictionary dictionary];
        for (size_t i = 2; i < r.stages.size(); ++i) {
            NSString *tag = @(r.stages[i].tag.c_str());
            if (!linked[tag])
                linked[tag] = [NSMutableArray array];
            [linked[tag] addObject:made[i]];
        }
        d.vertexLinkedFunctions.functions = linked[@"vl"];
        d.vertexLinkedFunctions.privateFunctions = linked[@"vp"];
        d.fragmentLinkedFunctions.functions = linked[@"fl"];
        d.fragmentLinkedFunctions.privateFunctions = linked[@"fp"];
        bool own = false;
        raw::requestPackKey(r, own);
        raw::CacheKey runtime;
        if (!raw::renderCacheKey(d, p.packKey, own, stack, runtime) || runtime.key != p.key.key ||
            memcmp(runtime.vertexAir, p.key.vertexAir, sizeof runtime.vertexAir) ||
            memcmp(runtime.fragmentAir, p.key.fragmentAir, sizeof runtime.fragmentAir))
            return why = "functions m2v keys otherwise than their libraries", nil;
        return d;
    }

    int32_t render(const Prepared &p, std::string &entry, std::string &why)
    {
        MTLRenderPipelineDescriptor *d = descriptor(p, why);
        if (!d)
            return TegraGM20BCompileUnavailable;
        raw::ProbeTextures textures;
        textures.cubic = p.request.cubic;
        textures.fragmentTests = p.request.fragmentTests;
        raw::RuntimeRecord record;
        const raw::CompileOutcome outcome = direct->make(d, p.packKey, record, why, textures);
        if (outcome != raw::CompileOutcome::Failed)
            entry = raw::encodeEntry(p.key, outcome == raw::CompileOutcome::Made ? &record : nullptr, why);
        return resultOf(outcome);
    }

    int32_t kernel(const Prepared &p, std::string &entry, std::string &why)
    {
        std::vector<id<MTLFunction>> made;
        if (!functions(p.request, made, why))
            return TegraGM20BCompileUnavailable;
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = made[0];
        d.maxTotalThreadsPerThreadgroup = p.request.kernel.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = p.request.kernel.multipleOfWidth;
        const NmbDirectDispatch dispatch = p.request.kernel.dispatch();
        raw::ComputeRecord record;
        const raw::CompileOutcome outcome = direct->kernel(d, dispatch, p.packKey, record, why);
        if (outcome != raw::CompileOutcome::Failed)
            entry = raw::encodeKernelEntry(p.key, outcome == raw::CompileOutcome::Made ? &record : nullptr, why);
        return resultOf(outcome);
    }
};

Engine::Engine() : state_(std::make_unique<State>())
{}

Engine::~Engine() = default;

bool Engine::open(uint64_t stack, std::string &why)
{
    State &s = *state_;
    s.stack = stack;
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
    const std::string pack = besidePlugin("TegraGM20BMetal.raw/pack.bin", true);
    if (!s.pack.load(pack.c_str(), stack, why))
        return why = "the raw pack: " + why, false;
    s.rules = rulesDigest(raw::PackRules::of(s.pack));
    return true;
}

bool Engine::openDevice(std::string &why)
{
    State &s = *state_;
    const std::string loader = besidePlugin("libvulkan_nouveau.dylib");
    @try {
        s.device = native_bridge_metal_open(loader.c_str(), nullptr, acceleratorId(), missing);
    } @catch (NSException *exception) {
        s.device = nil;
    }
    if (!s.device)
        return why = std::string("no m2v device: ") + nmb_last_error(), false;
    s.direct = std::make_unique<raw::Direct>(s.pack);
    return true;
}

raw::Digest Engine::rules() const
{
    return state_->rules;
}

int32_t Engine::compile(const Prepared &p, std::string &entry, std::string &why)
{
    @autoreleasepool {
        return p.request.kind == raw::RequestKind::Kernel ? state_->kernel(p, entry, why)
                                                          : state_->render(p, entry, why);
    }
}

}  // namespace switch_compiler
