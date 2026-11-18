// SPDX-License-Identifier: MIT
// See compiler_engine.h.
#import <Metal/Metal.h>
#import "compiler_engine.h"

#include <dlfcn.h>
#include <os/log.h>
#include <mutex>
#include <vector>
#include "TegraGM20BCompilerABI.h"
#include "compiler_facts.h"
#include "compiler_libraries.h"
#include "compiler_record.h"
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalOffline.h"
#include "../pack.h"

namespace switch_compiler {
namespace {

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "compiler");
    return l;
}

// <Extensions>/TegraGM20BMetal.bundle/<name>, and with `up` <Extensions>/<name>: what the runtime opens beside the plugin
std::string besidePlugin(const char *name, bool up = false)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&logger), &info) || !info.dli_fname)
        return std::string();
    std::string path = info.dli_fname;
    for (int i = 0; i < (up ? 2 : 1); ++i)
        path.erase(path.rfind('/') == std::string::npos ? 0 : path.rfind('/'));
    return path + "/" + name;
}

gm20b::Digest rulesDigest(const gm20b::PackRules &r)
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
    return gm20b::sha256(bytes.data(), bytes.size());
}

}  // namespace

struct Engine::State {
    // The idle compilers: taken and given back around a compile, never held across one
    std::mutex idle;
    std::vector<std::unique_ptr<gm20b::Direct>> compilers;
    EngineInputs inputs;
    uint64_t stack = 0;
    gm20b::Pack pack;
    gm20b::Digest rules{};
    std::shared_ptr<LibraryCache> libraries;
    std::unique_ptr<CodeGen> codeGen;
};

Engine::Engine() : state_(std::make_unique<State>())
{}

Engine::~Engine() = default;

EngineInputs EngineInputs::installed()
{
    EngineInputs inputs = {besidePlugin("TegraGM20BMetal.pack/pack.bin", true), "", {}};
    inputs.codeGen.seed = besidePlugin("TegraGM20BMetal.seed/shared/msl", true);
    return inputs;
}

bool Engine::open(uint64_t stack, const EngineInputs &inputs, std::string &why)
{
    State &s = *state_;
    s.stack = stack;
    s.inputs = inputs;
    if (!s.pack.load(inputs.pack.c_str(), stack, why))
        return why = "the pack: " + why, false;
    s.rules = rulesDigest(gm20b::PackRules::of(s.pack));
    s.libraries = std::make_shared<LibraryCache>(inputs.systemRoot);
    s.codeGen = std::make_unique<CodeGen>(inputs.codeGen);
    return true;
}

std::unique_ptr<gm20b::Direct> Engine::takeCompiler()
{
    State &s = *state_;
    {
        std::lock_guard<std::mutex> l(s.idle);
        if (!s.compilers.empty()) {
            std::unique_ptr<gm20b::Direct> compiler = std::move(s.compilers.back());
            s.compilers.pop_back();
            return compiler;
        }
    }
#if SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    auto compiler = std::make_unique<gm20b::Direct>(gm20b::PackRules::of(s.pack), false, offline::translator());
#if SWITCH_DEBUG
    os_log(logger(), "SwitchShaderCompiler: a compiler made for a compile no idle one could take in %llu ms",
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    return compiler;
}

void Engine::giveBack(std::unique_ptr<gm20b::Direct> compiler)
{
    State &s = *state_;
    std::lock_guard<std::mutex> l(s.idle);
    s.compilers.push_back(std::move(compiler));
}

gm20b::Digest Engine::rules() const
{
    return state_->rules;
}

int32_t Engine::compile(const Prepared &p, std::string &entry, std::string &why)
{
    if (p.request.kind == gm20b::RequestKind::Facts)
        return makeFacts(p, state_->inputs.systemRoot, entry, why);
    if (gm20b::functionAttributes(p.request))
        return makeAttributes(p, state_->inputs.systemRoot, entry, why);
    if (gm20b::bufferEncoders(p.request))
        return makeBufferEncoders(p, state_->inputs.systemRoot, entry, why);
    if (p.request.kind == gm20b::RequestKind::Library)
        return state_->codeGen->make(p, entry, why);
    State &s = *state_;
    if (p.request.kind == gm20b::RequestKind::Reflection) {
        const std::string &root = s.inputs.systemRoot;
        const Libraries offline = [&root](const gm20b::RequestLibrary &l, std::string &failed) {
            return offlineLibrary(l, root, failed);
        };
        return makeReflectionEntry(p, offline, s.stack, entry, why);
    }
    std::unique_ptr<gm20b::Direct> compiler = takeCompiler();
    LibraryCache &cache = *s.libraries;
    const Libraries libraries = [&cache](const gm20b::RequestLibrary &l, std::string &failed) {
        return cache.library(l, failed);
    };
    cache.began();
    const int32_t result = makeEntry(p, libraries, *compiler, s.stack, entry, why);
    cache.ended();
    giveBack(std::move(compiler));
    return result;
}

}  // namespace switch_compiler
