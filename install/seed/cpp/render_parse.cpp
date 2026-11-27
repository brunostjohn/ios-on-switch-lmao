// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/render_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include "drivers/TegraGM20BMetal/pipeline_key.h"
#include "drivers/TegraGM20BMetal/record_parts.h"
#include "install/seed/cpp/own_states_list.h"

namespace switch_seed {
namespace {

constexpr uint64_t kPixelFormatBGRA8Unorm = 80;
constexpr uint64_t kPixelFormatDepth32Float = 252;
constexpr uint64_t kPixelFormatR32Uint = 53;
constexpr uint64_t kPixelFormatRGBA16Uint = 113;
constexpr uint64_t kPixelFormatRG32Uint = 103;
constexpr uint64_t kPixelFormatRGBA16Float = 115;
constexpr uint64_t kPixelFormatRGBA32Float = 125;

gm20b::ProbeTexturesCore probeTex(uint64_t format, uint64_t samples)
{
    gm20b::ProbeTexturesCore out;
    out.format = format;
    out.samples = samples;
    return out;
}

bool isSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Replaces matches of `( | [vf][lp] \S+) lib=` with `$1 from=`.
std::string renameLinkedLibToFrom(const std::string &line)
{
    std::string out = line;
    size_t pos = 0;
    while ((pos = out.find(" | ", pos)) != std::string::npos) {
        if (pos + 6 < out.size() && (out[pos + 3] == 'v' || out[pos + 3] == 'f') &&
            (out[pos + 4] == 'l' || out[pos + 4] == 'p') && out[pos + 5] == ' ') {
            size_t fnEnd = pos + 6;
            while (fnEnd < out.size() && !isSpaceChar(out[fnEnd]))
                fnEnd++;
            if (fnEnd > pos + 6 && out.compare(fnEnd, 5, " lib=") == 0) {
                out.replace(fnEnd, 5, " from=");
                pos = fnEnd + 6;
                continue;
            }
        }
        pos += 3;
    }
    return out;
}

// Removes all matches of ` lib=\S+`.
std::string stripLibTokens(const std::string &line)
{
    std::string out;
    out.reserve(line.size());
    size_t at = 0;
    while (at < line.size()) {
        const size_t found = line.find(" lib=", at);
        if (found == std::string::npos) {
            out.append(line, at, std::string::npos);
            break;
        }
        size_t end = found + 5;
        while (end < line.size() && !isSpaceChar(line[end]))
            end++;
        if (end > found + 5) {
            out.append(line, at, found - at);
            at = end;
        } else {
            out.append(line, at, end - at);
            at = end;
        }
    }
    return out;
}

bool parseCubicBinds(const std::string &segment, gm20b::CubicBinds &out)
{
    const std::vector<std::string> tokens = splitSpaces(segment);
    for (size_t i = 1; i < tokens.size(); i++) {
        const std::string &t = tokens[i];
        if (t.empty())
            return false;
        const bool vertex = t[0] == 'v';
        const uint8_t stage = vertex ? gm20b::kCubicVertex : gm20b::kCubicFragment;
        const size_t colon = t.find(':');
        const size_t at = vertex ? 1 : 0;
        if (colon == std::string::npos || colon < at + 2)
            return false;
        const uint8_t slot = uint8_t(std::atoi(t.c_str() + at + 1));
        const std::string value = t.substr(colon + 1);
        if (t[at] == 's' && value.size() == 5 && (value[4] == 'n' || value[4] == 'p'))
            out.samplers.push_back({slot, uint8_t(value[0] - '0'), uint8_t(value[1] - '0'), uint8_t(value[2] - '0'),
                                    uint8_t(value[3] - '0'), value[4] == 'n', stage});
        else if (t[at] == 't')
            out.textures.push_back({slot, uint32_t(std::strtoul(value.c_str(), nullptr, 10)), stage});
        else
            return false;
    }
    return !out.empty();
}

std::shared_ptr<OfflineFunction> makeProbeStageFunction(LibraryStore &store, const std::shared_ptr<OfflineLibrary> &lib,
                                                        const std::vector<std::string> &stage, gm20b::StageKey &key)
{
    if (stage.empty() || stage[0] == "-")
        return nullptr;
    key.function = stage[0];
    std::string err;
    if (stage.size() == 1)
        return store.makeFunction(lib, stage[0], "", {}, false, err);
    const std::vector<std::string> items(stage.begin() + 1, stage.end());
    const ConstantSet cset = parseConstants(items);
    key.constants = cset.text();
    return store.makeFunction(lib, stage[0], "", cset, true, err);
}

bool addProbeLinkedFunction(LibraryStore &store, const std::shared_ptr<OfflineLibrary> &own, const std::string &libRoot,
                            RenderDescriptor &d, const std::string &segment, std::string &linked)
{
    const std::vector<std::string> tokens = splitSpaces(segment);
    if (tokens.size() < 2)
        return false;
    std::shared_ptr<OfflineLibrary> library = own;
    std::vector<std::string> values;
    std::string specName;
    std::string effectiveName = tokens[1];
    std::string err;
    for (size_t i = 2; i < tokens.size(); i++) {
        if (tokens[i].rfind("from=", 0) == 0) {
            std::string resolved;
            if (!resolveContainedPath(libRoot, tokens[i].substr(5), resolved))
                return false;
            library = store.loadFile(resolved, err);
        } else if (tokens[i].rfind("as=", 0) == 0) {
            specName = tokens[i].substr(3);
            effectiveName = specName;
        } else {
            values.push_back(tokens[i]);
        }
    }
    gm20b::StageKey key{tokens[1], ""};
    ConstantSet cset;
    if (!values.empty()) {
        cset = parseConstants(values);
        key.constants = cset.text();
    }
    const bool specialized = !values.empty() || !specName.empty();
    auto fn = store.makeFunction(library, tokens[1], specName, cset, specialized, err);
    if (!fn)
        return false;
    auto &list = tokens[0][0] == 'v' ? (tokens[0][1] == 'l' ? d.vertexLinked : d.vertexPrivateLinked)
                                     : (tokens[0][1] == 'l' ? d.fragmentLinked : d.fragmentPrivateLinked);
    list.push_back(std::move(fn));
    linked += gm20b::linkedSegment(tokens[0].c_str(), key, effectiveName);
    return true;
}

uint32_t fetchedColours(const gm20b::RuntimeRecord &r)
{
    const gm20b::SetEntries entries = gm20b::entriesOf(r);
    uint32_t colours = 0;
    for (uint32_t k = 0; k < entries.size(); k++)
        if (entries[k].kind == gm20b::EntryFetch && entries[k].slot < 8)
            colours |= 1u << entries[k].slot;
    return colours;
}

bool writesNone(const std::string &line, uint32_t colours)
{
    const std::string head = line.substr(0, line.find(" | "));
    for (uint32_t c = 0; c < 8; c++) {
        if (!(colours >> c & 1))
            continue;
        const size_t at = head.find(" c" + std::to_string(c) + "=");
        unsigned format = 0, mask = 1;
        if (at == std::string::npos || std::sscanf(head.c_str() + at + 4, "%u:%u:", &format, &mask) != 2 || mask)
            return false;
    }
    return true;
}

}  // namespace

std::vector<std::string> splitSpaces(const std::string &s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(' ', at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

std::vector<std::string> splitBy(const std::string &s, const std::string &by)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find(by, at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + by.size();
    }
    return out;
}

bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && !s.compare(s.size() - suffix.size(), suffix.size(), suffix);
}

std::string libraryPathOfLine(const std::string &line)
{
    const size_t at = line.find(" lib=");
    if (at == std::string::npos)
        return "";
    const size_t end = line.find(' ', at + 5);
    const std::string path = line.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
    const bool framework = endsWith(path, ".framework/default.metallib");
    const bool resources =
        path.find(".bundle/") != std::string::npos || path.find(".framework/Resources/") != std::string::npos;
    const bool bundle = path.rfind("/System/Library/", 0) == 0 && endsWith(path, ".metallib") && resources;
    const bool app = path.rfind("/Applications/Switch", 0) == 0 && endsWith(path, ".app/default.metallib") &&
                     path.find('/', 14) == path.size() - 17;
    if ((!framework && !bundle && !app) || path.find("/QuartzCore.framework/") != std::string::npos)
        return "";
    return path;
}

std::string frameworkNameOfPath(const std::string &path)
{
    if (!endsWith(path, ".framework/default.metallib"))
        return "";
    const size_t end = path.rfind(".framework/");
    const size_t slash = path.rfind('/', end);
    const size_t begin = slash == std::string::npos ? 0 : slash + 1;
    return path.substr(begin, end - begin);
}

std::vector<std::string> corpusLines(const std::string &text, const std::string &labelPrefix, bool includeOwn)
{
    std::vector<std::string> lines;
    const std::string labelTag = " label=" + labelPrefix;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (l.rfind("R ", 0) != 0 || l.find(" ok=1 ") == std::string::npos)
            continue;
        if (const std::string path = libraryPathOfLine(l); !path.empty()) {
            const std::string kept = renameLinkedLibToFrom(l);
            lines.push_back("@L " + path + " " + stripLibTokens(kept));
            continue;
        }
        if (!labelPrefix.empty() && l.find(labelTag) == std::string::npos)
            continue;
        lines.push_back(stripLibTokens(l));
    }
    if (includeOwn)
        appendOwnStateLines(lines);
    return lines;
}

bool parseProbeState(LibraryStore &store, const std::shared_ptr<OfflineLibrary> &lib, const std::string &libRoot,
                     const std::string &line, ProbeState &s, const std::string &framework, std::string *why)
{
    const auto refuse = [why](const std::string &reason) {
        if (why)
            *why = reason;
        return false;
    };
    const size_t bar1 = line.find(" | v ");
    const size_t bar2 = line.find(" | f ");
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return refuse("no vertex or fragment segment");
    const size_t bar3 = line.find(" | ", bar2 + 5);
    const std::vector<std::string> head = splitSpaces(line.substr(0, bar1));
    const std::vector<std::string> v = splitSpaces(line.substr(bar1 + 5, bar2 - bar1 - 5));
    const std::vector<std::string> f =
        splitSpaces(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    RenderDescriptor d;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) {
            s.label = h.substr(6);
            if (s.label != "-")
                d.label = s.label;
        } else if (h.rfind("samples=", 0) == 0) {
            d.samples = std::strtoull(h.c_str() + 8, nullptr, 10);
        } else if (h == "rast=0") {
            d.rasterization = false;
        } else if (h.rfind("topo=", 0) == 0) {
            d.topology = std::strtoull(h.c_str() + 5, nullptr, 10);
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
        } else if (h.rfind("t=", 0) == 0 && h.find(':') != std::string::npos) {
            unsigned long long t[6] = {};
            int scale = 0;
            if (std::sscanf(h.c_str() + 2, "%llu:%llu:%llu:%llu:%llu:%llu:%d", t, t + 1, t + 2, t + 3, t + 4, t + 5,
                            &scale) != 7)
                return refuse("its tessellator field " + h + " does not parse");
            d.tessellation = {t[0], t[1], t[2], t[3], t[4], t[5], scale ? 1u : 0u};
        } else if (h.rfind("amp=", 0) == 0) {
            d.amplification = std::strtoull(h.c_str() + 4, nullptr, 10);
        } else if (h == "tier=1") {
            s.tier = 1;
        } else if (h == "twr=2") {
            d.textureWriteRounding = 2;
        } else if (h == "alphacov=1") {
            d.alphaToCoverage = true;
        } else if (h.size() > 1 && h[0] == 'a' && std::isdigit(static_cast<unsigned char>(h[1]))) {
            unsigned i = 0, x[6] = {};
            unsigned long long stride = 0;
            if (std::sscanf(h.c_str(), "a%u=%u:%u:%u:%llu:%u:%u", &i, x, x + 1, x + 2, &stride, x + 4, x + 5) != 7 ||
                i >= 31 || x[2] >= 31)
                continue;
            d.hasVertexDescriptor = true;
            d.vertexAttributes[i] = {x[0], x[1], x[2]};
            const bool dynamic = stride == UINT64_MAX || stride == UINT32_MAX;
            x[3] = dynamic ? UINT32_MAX : unsigned(stride);
            d.vertexLayouts[x[2]] = {dynamic ? UINT64_MAX : uint64_t(stride), x[4], x[5]};
            s.vertexBuffer = x[2];
            s.vertexStride = x[3];
        }
    }
    gm20b::StageKey vertex, fragment;
    d.vertex = makeProbeStageFunction(store, lib, v, vertex);
    d.fragment = makeProbeStageFunction(store, lib, f, fragment);
    if (!d.vertex)
        return refuse("its library makes no vertex function " + (v.empty() ? std::string("-") : v[0]));
    if (!d.fragment && !f.empty() && f[0] != "-")
        return refuse("its library makes no fragment function " + f[0]);
    std::string linked;
    for (size_t at = bar3; at != std::string::npos && at + 3 < line.size();) {
        const size_t next = line.find(" | ", at + 3);
        const std::string segment = line.substr(at + 3, (next == std::string::npos ? line.size() : next) - at - 3);
        const std::string tag = segment.substr(0, 3);
        const bool isLinked = tag == "vl " || tag == "vp " || tag == "fl " || tag == "fp ";
        if ((isLinked && !addProbeLinkedFunction(store, lib, libRoot, d, segment, linked)) ||
            (segment.rfind("cubic ", 0) == 0 && !parseCubicBinds(segment, s.cubic)) ||
            (segment.rfind("xrs=", 0) == 0 &&
             !(s.xrSlots = uint16_t(std::strtoul(segment.c_str() + 4, nullptr, 10)))) ||
            (segment.rfind("wtex=", 0) == 0 && !gm20b::parseWritten(segment, s.written)))
            return refuse("its segment \"" + segment.substr(0, 80) + "\" makes nothing");
        at = next;
    }
    const gm20b::PipelineFields fields = d.fields();
    s.key = gm20b::pipelineKey(fields, &vertex, d.fragment ? &fragment : nullptr, linked, framework) +
            (s.cubic.empty() ? std::string() : s.cubic.key());
    s.fast = gm20b::fastKey(fields, &vertex, d.fragment ? &fragment : nullptr, linked, framework);
    s.d = std::move(d);
    return true;
}

gm20b::ProbeTexturesCore probeTexturesOf(const std::string &line, bool ours)
{
    if (ours && line.find("| v resolve_vert | f resolve_depth") != std::string::npos)
        return probeTex(kPixelFormatDepth32Float, 4);
    if (ours && line.find("| f stencil_bits_ms_") != std::string::npos)
        return probeTex(kPixelFormatR32Uint, 4);
    if (ours && line.find("| v resolve_vert | f resolve_") != std::string::npos)
        return probeTex(kPixelFormatBGRA8Unorm, 4);

    struct Entry {
        const char *fragment;
        uint64_t format;
        uint64_t samples;
    };

    const Entry sets[] = {{"f xr_bgr10_frag", kPixelFormatR32Uint, 1},
                          {"f xr_bgr10_srgb_frag", kPixelFormatR32Uint, 1},
                          {"f xr_bgra10_frag", kPixelFormatRGBA16Uint, 1},
                          {"f xr_bgra10_srgb_frag", kPixelFormatRGBA16Uint, 1},
                          {"f xr_planes_frag", kPixelFormatRG32Uint, 1},
                          {"f xr_planes_srgb_frag", kPixelFormatRG32Uint, 1},
                          {"f xr_two_planes_frag", kPixelFormatR32Uint, 1},
                          {"f two_plane_store", kPixelFormatRGBA16Float, 1},
                          {"f xr_two_planes_srgb_frag", kPixelFormatR32Uint, 1},
                          {"f ycbcr_store_", kPixelFormatRGBA32Float, 1},
                          {"f ycbcr_packed_", kPixelFormatR32Uint, 1},
                          {"f turn_copy", kPixelFormatR32Uint, 1},
                          {"f turn_masked", kPixelFormatR32Uint, 1},
                          {"f turn_reach", kPixelFormatR32Uint, 1},
                          {"f turn_depth", kPixelFormatDepth32Float, 1},
                          {"f stencil_bits_", kPixelFormatR32Uint, 1},
                          {"f fmt_store_", kPixelFormatRGBA32Float, 1},
                          {"f xr_target_", kPixelFormatRGBA16Float, 1},
                          {"f fmt_load_rgb9e5", kPixelFormatR32Uint, 1},
                          {"f fmt_mip_", kPixelFormatR32Uint, 1}};
    for (const Entry &e : sets)
        if (line.find(std::string("| ") + e.fragment) != std::string::npos)
            return probeTex(e.format, e.samples);
    return {};
}

FreeOutcome makeFetchFree(gm20b::DirectCore &direct, LibraryStore &store, const std::string &libRoot,
                          const ProbeState &state, const std::string &line, gm20b::ProbeTexturesCore textures,
                          probe::FetchFree &form, gm20b::RuntimeRecord &rec, std::map<uint32_t, uint32_t> *registers,
                          std::string &why, bool &barrier)
{
    ProbeState s;
    barrier = false;
    if (!probe::fetchFree(line, form) ||
        !parseProbeState(store, state.library, libRoot, form.line, s, state.framework, nullptr) || form.colour >= 8)
        return FreeOutcome::None;
    const std::vector<uint32_t> attrs = s.d.activeVertexAttributes();
    gm20b::CompileOutcome outcome =
        direct.make(&s.d, attrs, s.key, rec, why, textures, registers, true, gm20b::PackForm::Default);
    const auto readsOthersOnly = [&] {
        const uint32_t fetched = fetchedColours(rec);
        return fetched && !(fetched >> form.colour & 1) && writesNone(form.line, fetched);
    };
    if (outcome != gm20b::CompileOutcome::Made)
        return FreeOutcome::Refused;
    if (!(rec.pipeline.flags & (gm20b::PipelineFetches | gm20b::PipelineBarrierForm)))
        return FreeOutcome::Made;
    if (!readsOthersOnly())
        return FreeOutcome::Fetches;
    barrier = true;
    outcome = direct.make(&s.d, attrs, s.key, rec, why, textures, registers, true, gm20b::PackForm::Barrier);
    if (outcome != gm20b::CompileOutcome::Made)
        return FreeOutcome::Refused;
    return (rec.pipeline.flags & gm20b::PipelineStorageMetadata) || !readsOthersOnly() ? FreeOutcome::Fetches
                                                                                       : FreeOutcome::Made;
}

}  // namespace switch_seed
