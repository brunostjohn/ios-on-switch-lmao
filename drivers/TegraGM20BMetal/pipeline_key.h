// SPDX-License-Identifier: MIT
// A render pipeline state's key: what the runtime looks a state up in the pack by (TegraGM20BMetalLibrary.mm
// metal_pipeline_key), what the pack generator files its record under (pack/probe_states.h) and what the shader
// compiler checks a request's key by (TegraGM20BMetalCompileRequest.mm), all made here from the descriptor's fields alone
// (pipeline_fields.h) and the constant values its functions were made with, so no corpus line's text decides a pack
// key and a field a compile request does not carry cannot be in one.
// The journal's R-line fields in its order but the label (NativeBridgeMetalFrontend.mm journalRenderPipeline), each with
// a blank after it, then the stages as their library names them with their constants, the linked functions, and the
// framework where it is not QuartzCore; no trailing blank.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "pipeline_fields.h"

namespace gm20b {

// MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes)
inline size_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33:
        return 4;
    case 4:
    case 30:
    case 34:
        return 8;
    case 6:
    case 32:
    case 36:
        return 16;
    case 16:
    case 37:
    case 41:
        return 2;
    case 17:
    case 38:
    case 42:
        return 4;
    case 19:
    case 40:
    case 44:
        return 8;
    case 45:
    case 49:
    case 53:
        return 1;
    case 46:
    case 50:
    case 54:
        return 2;
    case 48:
    case 52:
    case 56:
        return 4;
    case 81:
    case 85:
        return 8;
    case 82:
    case 86:
        return 16;
    case 84:
    case 88:
        return 32;
    default:
        return 0;
    }
}

// "#<index>:<MTLDataType>:<hex bytes>" by index in ascending order, then "<name>:<MTLDataType>:<hex bytes>" by name;
// "" for no values. Takes any constant struct with {type, index, name, bytes, length} (such as NmbConstant).
template <class Constant> inline std::string constantsText(const Constant *values, size_t count)
{
    std::string text;
    if (!values || !count)
        return text;
    std::vector<const Constant *> indexed, named;
    for (size_t i = 0; i < count; i++) {
        if (values[i].name)
            named.push_back(&values[i]);
        else
            indexed.push_back(&values[i]);
    }
    std::stable_sort(indexed.begin(), indexed.end(), [](const Constant *a, const Constant *b) {
        return a->index < b->index;
    });
    std::stable_sort(named.begin(), named.end(), [](const Constant *a, const Constant *b) {
        return std::strcmp(a->name, b->name) < 0;
    });
    const auto append = [&](const Constant *v, bool isNamed) {
        const size_t expected = constantBytes(v->type);
        const size_t n = v->bytes ? std::min<size_t>(expected, size_t(v->length)) : 0;
        char head[48];
        if (isNamed) {
            if (!text.empty())
                text += ' ';
            text += v->name;
        } else {
            snprintf(head, sizeof head, "%s#%llu", text.empty() ? "" : " ", static_cast<unsigned long long>(v->index));
            text += head;
        }
        snprintf(head, sizeof head, ":%llu:", static_cast<unsigned long long>(v->type));
        text += head;
        const auto *raw = static_cast<const uint8_t *>(v->bytes);
        for (size_t i = 0; i < expected; i++) {
            const uint8_t b = i < n ? raw[i] : 0;
            snprintf(head, sizeof head, "%02x", b);
            text += head;
        }
    };
    for (const Constant *v : indexed)
        append(v, false);
    for (const Constant *v : named)
        append(v, true);
    return text;
}

// A stage's function: the library function it was made from and the text of the constants it was made with
struct StageKey {
    std::string function, constants;
};

// One linked function: " | <vl|vp|fl|fp> <library function>[ as=<its name>] <constants>"
inline std::string linkedSegment(const char *tag, const StageKey &f, const std::string &name)
{
    return std::string(" | ") + tag + " " + f.function + (name != f.function ? " as=" + name : "") + " " + f.constants;
}

// `linked`: the linked functions' segments in the descriptor's order; `framework`: the vertex function's
// (<name>.framework/default.metallib), "" for another library
inline std::string pipelineKey(const PipelineFields &f, const StageKey *vertex, const StageKey *fragment,
                               const std::string &linked, const std::string &framework)
{
    char head[160];
    snprintf(head, sizeof head, "samples=%llu topo=%llu ds=%llu/%llu ", (unsigned long long)f.samples,
             (unsigned long long)f.topology, (unsigned long long)f.depthFormat, (unsigned long long)f.stencilFormat);
    std::string key = head;
    if (f.alphaToCoverage)
        key += "alphacov=1 ";
    for (unsigned i = 0; i < 8; i++) {
        const ColorFields &c = f.color[i];
        if (!c.format)
            continue;
        snprintf(head, sizeof head, "c%u=%llu:%llu:%d:%llu:%llu:%llu:%llu:%llu:%llu ", i, (unsigned long long)c.format,
                 (unsigned long long)c.writeMask, int(c.blending), (unsigned long long)c.sourceRGB,
                 (unsigned long long)c.destinationRGB, (unsigned long long)c.operationRGB,
                 (unsigned long long)c.sourceAlpha, (unsigned long long)c.destinationAlpha,
                 (unsigned long long)c.operationAlpha);
        key += head;
    }
    for (unsigned i = 0; i < 31; i++) {
        const AttributeFields &a = f.attributes[i];
        if (!a.format)
            continue;
        // MTLBufferLayoutStrideDynamic as UINT32_MAX, which emit.cpp emitVertexInput reads back as the bind's
        const unsigned long long stride = a.stride == UINT64_MAX ? UINT32_MAX : a.stride;
        snprintf(head, sizeof head, "a%u=%llu:%llu:%llu:%llu:%llu:%llu ", i, (unsigned long long)a.format,
                 (unsigned long long)a.offset, (unsigned long long)a.buffer, stride, (unsigned long long)a.step,
                 (unsigned long long)a.rate);
        key += head;
    }
    // a tessellation state's tessellator state, and a vertex amplification count above Metal's default: no other key has
    // them
    if (f.patchType) {
        const TessellationFields &t = f.tessellation;
        snprintf(head, sizeof head, "t=%llu:%llu:%llu:%llu:%llu:%llu:%d ", (unsigned long long)t.partition,
                 (unsigned long long)t.factorFormat, (unsigned long long)t.factorStep, (unsigned long long)t.winding,
                 (unsigned long long)t.controlPointIndex, (unsigned long long)t.maxFactor, int(t.factorScale));
        key += head;
    }
    if (f.amplification > 1) {
        snprintf(head, sizeof head, "amp=%llu ", (unsigned long long)f.amplification);
        key += head;
    }
    if (f.textureWriteRounding == 2)
        key += "twr=2 ";
    // a state Metal does not rasterize: the driver draws it with the rasteriser off (stage_switches.h)
    if (!f.rasterization)
        key += "rast=0 ";
    key += " | v " + (vertex ? vertex->function : std::string("-")) + " " + (vertex ? vertex->constants : "");
    key += " | f " + (fragment ? fragment->function : std::string("-")) + " " + (fragment ? fragment->constants : "");
    key += linked;
    if (!framework.empty() && framework != "QuartzCore")
        key += " | l " + framework;
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    return key;
}

// The fast key, which the pack finds a state's records by (pack.h PackFastKey): what pipelineKey's text names, hashed as
// words with no text made. The descriptor's fields go in as pipelineKey prints them (a colour or attribute of no format,
// the tessellator without a patch, an amplification of 1 or less, a rounding other than 2, alpha-to-coverage off and
// rasterization on all left out, the dynamic stride as UINT32_MAX), each tagged so no two field lists hash alike; then
// each stage's stageHash, the linked functions' text's and the framework's. A hit is taken on the fast key alone without
// comparing the full key text: packed states cannot collide because the link refuses duplicates, and a state outside the
// pack collides with odds at most (fast-key rows) / 2^64 per creation.
inline uint64_t keyTextHash(const char *bytes, size_t length)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++)
        h = (h ^ uint8_t(bytes[i])) * 0x100000001b3ull;
    return h;
}

// A stage's part of the fast key (its function's: TegraGM20BMetalLibrary.mm makes it with the function)
inline uint64_t stageHash(const StageKey &k)
{
    const std::string named = k.function + '\0' + k.constants;
    return keyTextHash(named.data(), named.size());
}

constexpr uint64_t kKeyNoStage = 0x2d2d2d2d2d2d2d2dull;  // the fragment stage of a state with no fragment function

// The linked functions' text (linkedSegment's) and the framework, as the fast key takes them: 0 for none, QuartzCore's
// none
inline uint64_t linkedHash(const std::string &linked)
{
    return linked.empty() ? 0 : keyTextHash(linked.data(), linked.size());
}

inline uint64_t frameworkHash(const std::string &framework)
{
    return framework.empty() || framework == "QuartzCore" ? 0 : keyTextHash(framework.data(), framework.size());
}

class KeyWords {
public:
    void add(uint64_t w)
    {
        h_ = (h_ ^ w) * 0x9e3779b97f4a7c15ull;
        h_ ^= h_ >> 29;
    }

    uint64_t done() const
    {
        uint64_t h = h_ ^ (h_ >> 33);
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        return h ^ (h >> 33);
    }

private:
    uint64_t h_ = 0x243f6a8885a308d3ull;
};

inline uint64_t fastKey(const PipelineFields &f, uint64_t vertex, uint64_t fragment, uint64_t linked,
                        uint64_t framework)
{
    KeyWords k;
    keyFieldWords(f, [&k](uint64_t w) {
        k.add(w);
    });
    for (const uint64_t w : {uint64_t(0x700), vertex, fragment, linked, framework})
        k.add(w);
    return k.done();
}

inline uint64_t fastKey(const PipelineFields &f, const StageKey *vertex, const StageKey *fragment,
                        const std::string &linked, const std::string &framework)
{
    return fastKey(f, vertex ? stageHash(*vertex) : kKeyNoStage, fragment ? stageHash(*fragment) : kKeyNoStage,
                   linkedHash(linked), frameworkHash(framework));
}

}  // namespace gm20b
