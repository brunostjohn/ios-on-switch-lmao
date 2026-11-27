// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/render_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "drivers/TegraGM20BMetal/pipeline_key.h"
#include "drivers/TegraGM20BMetal/utf8_path.h"

namespace switch_seed {
namespace {

constexpr const char *kQuartzCorePath = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";

std::string nativeFrameworkOf(const NativeStage &stage)
{
    if (stage.library.empty() || stage.library == kQuartzCorePath)
        return "QuartzCore";
    return frameworkNameOfPath(stage.library);
}

gm20b::StageKey nativeStageKey(const NativeStage &stage)
{
    return {stage.name, parseConstants(stage.constants).text()};
}

}  // namespace

std::string unescapeToken(const std::string &token)
{
    const auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(token.size());
    for (size_t i = 0; i < token.size(); i++) {
        if (token[i] == '%') {
            if (i + 2 >= token.size())
                return "";
            const int hi = hexVal(token[i + 1]);
            const int lo = hexVal(token[i + 2]);
            if (hi < 0 || lo < 0)
                return "";
            const unsigned char byte = static_cast<unsigned char>((hi << 4) | lo);
            if (byte == 0)
                return "";
            out += char(byte);
            i += 2;
        } else {
            out += token[i];
        }
    }
    return out;
}

std::string nativeHostFile(const std::string &id, const std::string &qcPath, const std::string &storeRoot)
{
    if (id.empty())
        return qcPath;
    std::string path = unescapeToken(id);
    if (path.empty())
        return "";
    if (path.rfind("/System/Cryptexes/OS/", 0) == 0)
        path = path.substr(20);
    if (path.rfind("/var/", 0) == 0)
        path = "/private" + path;
    if (path == kQuartzCorePath)
        return qcPath;
    if (path.rfind("data:", 0) == 0) {
        const std::string sha = path.substr(5);
        if (!isSafeSha256Hex(sha))
            return "";
        return storeRoot + "/data/" + sha + ".metallib";
    }
    std::string resolved;
    if (!resolveContainedPath(storeRoot + "/root", path, resolved))
        return "";
    return resolved;
}

NativeStage parseNativeStage(const std::string &text, const std::string &qcPath, const std::string &storeRoot)
{
    NativeStage s;
    const std::vector<std::string> tokens = splitSpaces(text);
    if (tokens.empty())
        return s;
    s.name = tokens[0];
    std::string id;
    for (size_t i = 1; i < tokens.size(); i++) {
        if (tokens[i].rfind("lib=", 0) == 0)
            id = tokens[i].substr(4);
        else if (tokens[i].rfind("as=", 0) == 0)
            s.as = tokens[i].substr(3);
        else if (tokens[i] == "spec")
            s.specialized = true;
        else
            s.constants.push_back(tokens[i]);
    }
    s.library = unescapeToken(id);
    s.file = nativeHostFile(id, qcPath, storeRoot);
    s.specialized = s.specialized || !s.constants.empty();
    return s;
}

std::shared_ptr<OfflineFunction> loadNativeFunction(LibraryStore &store, const NativeStage &s,
                                                    NativeInputOutcome &outcome, std::string &why)
{
    const auto wp = gm20b::fsPathUtf8(s.file);
    std::error_code ec;
    if (wp.empty() || !std::filesystem::is_regular_file(wp, ec) || ec) {
        outcome = NativeInputOutcome::NoLibrary;
        return nullptr;
    }
    std::string libErr;
    auto lib = store.loadFile(s.file, libErr);
    if (!lib) {
        std::fprintf(stderr, "library %s: %s\n", s.file.c_str(), libErr.c_str());
        outcome = NativeInputOutcome::NoLibrary;
        return nullptr;
    }
    if (!lib->functionNames().count(s.name)) {
        outcome = NativeInputOutcome::NoFunction;
        return nullptr;
    }
    const ConstantSet cset = parseConstants(s.constants);
    const bool specialized = !s.as.empty() || s.specialized;
    auto fn = store.makeFunction(lib, s.name, s.as, cset, specialized, why);
    outcome = fn ? NativeInputOutcome::Made : NativeInputOutcome::Refused;
    return fn;
}

NativeInputOutcome parseNativeRenderInput(LibraryStore &store, const std::string &qcPath, const std::string &storeRoot,
                                          const std::string &line, NativeRenderInput &out, std::string &why)
{
    out = {};
    why.clear();
    const std::vector<std::string> parts = splitBy(line, " | ");
    if (parts.size() < 3 || parts[1].rfind("v ", 0) != 0 || parts[2].rfind("f ", 0) != 0)
        return NativeInputOutcome::Refused;
    out.vertex = parseNativeStage(parts[1].substr(2), qcPath, storeRoot);
    out.fragment = parseNativeStage(parts[2].substr(2), qcPath, storeRoot);
    out.file = out.vertex.file;
    RenderDescriptor &d = out.d;
    for (const std::string &h : splitSpaces(parts[0])) {
        if (h.rfind("label=", 0) == 0) {
            out.label = h.substr(6);
            if (out.label != "-")
                d.label = out.label;
        } else if (h.rfind("samples=", 0) == 0) {
            d.samples = std::strtoull(h.c_str() + 8, nullptr, 10);
        } else if (h.rfind("topo=", 0) == 0) {
            d.topology = std::strtoull(h.c_str() + 5, nullptr, 10);
        } else if (h.rfind("opt=", 0) == 0) {
            out.options = std::strtoull(h.c_str() + 4, nullptr, 10);
        } else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            std::sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthFormat = a;
            d.stencilFormat = b;
        } else if (h.size() > 1 && h[0] == 'c' && std::isdigit(static_cast<unsigned char>(h[1]))) {
            unsigned i = 0, x[9] = {};
            if (std::sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5,
                            x + 6, x + 7, x + 8) != 10 ||
                i >= 8)
                continue;
            d.color[i] = {x[0], x[1], x[2] ? 1u : 0u, x[3], x[4], x[5], x[6], x[7], x[8]};
        } else if (h.size() > 1 && h[0] == 'a' && std::isdigit(static_cast<unsigned char>(h[1]))) {
            unsigned i = 0, x[6] = {};
            if (std::sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 ||
                i >= 31 || x[2] >= 31)
                continue;
            d.hasVertexDescriptor = true;
            d.vertexAttributes[i] = {x[0], x[1], x[2]};
            d.vertexLayouts[x[2]] = {x[3], x[4], x[5]};
        }
    }
    NativeInputOutcome outcome = NativeInputOutcome::Made;
    d.vertex = loadNativeFunction(store, out.vertex, outcome, why);
    if (out.fragment.name != "-" && outcome == NativeInputOutcome::Made)
        d.fragment = loadNativeFunction(store, out.fragment, outcome, why);
    if (outcome == NativeInputOutcome::Made) {
        for (size_t i = 3; i < parts.size(); i++) {
            const std::string tag = parts[i].substr(0, 3);
            if (tag != "vl " && tag != "vp " && tag != "fl " && tag != "fp ")
                continue;
            const NativeStage source = parseNativeStage(parts[i].substr(3), qcPath, storeRoot);
            auto fn = loadNativeFunction(store, source, outcome, why);
            if (outcome != NativeInputOutcome::Made)
                return outcome;
            out.linked.emplace_back(tag.substr(0, 2), source);
            auto &list = tag[0] == 'v' ? (tag[1] == 'l' ? d.vertexLinked : d.vertexPrivateLinked)
                                       : (tag[1] == 'l' ? d.fragmentLinked : d.fragmentPrivateLinked);
            list.push_back(std::move(fn));
        }
    }
    return outcome;
}

bool nativeRenderCacheKey(const NativeRenderInput &input, uint64_t stack, gm20b::CacheKey &out)
{
    const gm20b::StageKey vertex = nativeStageKey(input.vertex);
    const gm20b::StageKey fragment = nativeStageKey(input.fragment);
    const std::string vertexFramework = nativeFrameworkOf(input.vertex);
    bool packable = !vertexFramework.empty() && vertexFramework == nativeFrameworkOf(input.fragment);
    std::string linked;
    for (const char *tag : {"vl", "vp", "fl", "fp"}) {
        for (const auto &[kind, source] : input.linked) {
            if (kind == tag) {
                linked +=
                    gm20b::linkedSegment(tag, nativeStageKey(source), source.as.empty() ? source.name : source.as);
                packable &= !nativeFrameworkOf(source).empty();
            }
        }
    }
    const std::string key = gm20b::pipelineKey(input.d.fields(), &vertex, &fragment, linked, vertexFramework);
    return input.d.renderCacheKey(key, !packable, stack, out);
}

}  // namespace switch_seed
