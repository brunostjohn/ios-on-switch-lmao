// SPDX-License-Identifier: MIT
// The pack generator's records made directly (runtime/RawDirect.h): every state of a bucket's corpus compiled from m2v's
// translation and NVK's stage compile, as the runtime makes a state the pack lacks, with no pipeline, draw or capture.
// directlink.py makes the pack of them.
// usage: directpack <nvk dylib> <cache dir> <default.metallib> <corpus.log> <records.bin>
// (PROBE_OWN_LIB, PROBE_LIB_ROOT and PROBE_LABEL as probe.mm reads them)
//
// records.bin, little-endian, per state: "DREC", the outcome (0 made, 1 refused, 2 failed), the key and the reason (u32 length,
// bytes), then for a made state its PackPipeline (raw_pack.h; vs 0, fs 1, no blob), its key string, its registers (u32 count,
// method and value pairs: raw_emit's), its vertex and fragment programs (header + code, shader data, constant buffer types:
// each a u32 length and bytes), its constexpr sampler headers (u32 count, eight words each), which its entries number from 0,
// and its fragment program's split: u32 0, or 1 and its PackSplit (no program indices), its per-pixel program and its prologue
// (as the programs above; the prologue's header 80 bytes of zeros), its program variants, and the iterated blend program a
// fetch-free form names (u32 length and bytes: ca/fetchfree.py, iterated/table.py; empty for any other record). A record of
// one of a state's variants has the variant's name after its key (runtime/raw_pack.h variantNameAt); a state's fetch-free
// form is under the state's key (PipelineFetchFreeForm).
// Then per compute dispatch shape (the corpus's D lines, as the kernel probe read them: kernel_shapes.h): "DKRN", the outcome,
// its raw_compute_key.h key and the reason, then for a made kernel its PackKernel (raw_direct_compute.h; program 0), its code,
// shader data and constant buffer types, and its sampler headers, which its sampler entries number from 0 (slot 0x80 | n).
// PROBE_DATA_LIBS names the directory of the corpus's data: libraries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include "../runtime/raw_emit.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "direct_kernels.h"
#include "direct_states.h"
#include "fetch_free.h"

namespace {

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

struct Writer {
    FILE *f;

    void u32(uint32_t v)
    {
        fwrite(&v, 4, 1, f);
    }

    void bytes(const void *p, size_t n)
    {
        u32(uint32_t(n));
        fwrite(p, 1, n, f);
    }

    void text(const std::string &s)
    {
        bytes(s.data(), s.size());
    }

    template <typename T> void list(const std::vector<T> &v)
    {
        bytes(v.data(), v.size() * sizeof(T));
    }
};

// The record's head; false: no more follows (it was not made)
// A record field the pack holds none of (raw_pack.h's v21 fields): the runtime makes such a state at its creation, with its
// draws' binds and counts
const char *runtimeOnly(const raw::PackPipeline &p)
{
    if (raw::tessellates(p))
        return "a tessellation state, which the runtime makes";
    if (p.vertexStrideDynamic)
        return "a dynamic vertex buffer stride, which the runtime makes";
    for (const uint8_t stream : p.vertexStreams)
        if (stream & raw::kStreamBindStride)
            return "a dynamic vertex buffer stride, which the runtime makes";
    for (const auto &pulled : p.pulledStride)
        if (pulled[1])
            return "a pulled vertex buffer of a dynamic stride, which the runtime makes";
    if (p.amplificationMax || p.amplificationIdWord || p.amplificationCountRead)
        return "vertex amplification, which the runtime makes";
    return nullptr;
}

const char *runtimeOnly(const raw::PackKernel &k)
{
    for (const auto &stride : k.stageInStride)
        if (stride[1])
            return "a compute stage-in of a dynamic stride, which the runtime makes";
    return k.stageInRegionWord ? "a compute stage-in kernel, which the runtime makes" : nullptr;
}

bool head(Writer &w, const char *tag, const std::string &key, raw::CompileOutcome outcome, const std::string &why)
{
    fwrite(tag, 1, 4, w.f);
    w.u32(outcome == raw::CompileOutcome::Made ? 0 : outcome == raw::CompileOutcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    return outcome == raw::CompileOutcome::Made;
}

void record(Writer &w, const std::string &key, raw::CompileOutcome outcome, const std::string &why,
            const raw::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers,
            const std::string &iterated = "")
{
    if (!head(w, "DREC", key, outcome, why))
        return;
    w.bytes(&rec.pipeline, sizeof rec.pipeline);
    w.text(rec.keyString);
    std::vector<uint32_t> pairs;
    for (const auto &[m, v] : registers) {
        pairs.push_back(m);
        pairs.push_back(v);
    }
    w.list(pairs);
    auto program = [&](const raw::CapturedProgram &p) {
        w.list(p.image);
        w.list(p.data);
        w.list(p.cbufTypes);
        w.u32(p.gprs | uint32_t(p.boundTextures) << 8);
    };
    for (const raw::CapturedProgram &p : rec.programs)
        program(p);
    w.list(rec.samplers);
    w.u32(rec.split);
    if (rec.split) {
        w.bytes(&rec.splitRecord, sizeof rec.splitRecord);
        program(rec.splitPrograms[0]);
        program(rec.splitPrograms[1]);
    }
    w.u32(uint32_t(rec.variants.size()));
    for (const raw::RuntimeRecord::Variant &v : rec.variants) {
        w.u32(v.assume);
        program(v.program);
    }
    w.text(iterated);
}

// The rung a Core Animation state's fragment blend is a candidate for (rawpack.py's split tags its line " rung=<2|3>":
// ca/caset.py fetch_rung), marked on each color the state fetches and writes with no pipeline blend: what a device probe
// would have to confirm before such a fetch leaves the interlock
void markCandidates(const std::string &line, raw::PackPipeline &p)
{
    const size_t at = line.find(" rung=");
    const int rung = at == std::string::npos ? 0 : atoi(line.c_str() + at + 6);
    const uint8_t candidate = rung == 2 ? raw::kFetchCandidateBlend : rung == 3 ? raw::kFetchCandidateIterated : 0;
    const std::string head = line.substr(0, line.find(" | "));
    for (size_t c = head.find(" c"); candidate && c != std::string::npos; c = head.find(" c", c + 2)) {
        unsigned color = 0, x[9] = {};
        if (sscanf(head.c_str() + c, " c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &color, x, x + 1, x + 2, x + 3, x + 4, x + 5,
                   x + 6, x + 7, x + 8) != 10 ||
            color >= 8)
            continue;
        const bool replaces = x[3] == 1 && x[4] == 0 && x[5] == 0 && x[6] == 1 && x[7] == 0 && x[8] == 0;
        if ((p.fetchRung[color] & raw::kFetchRungMask) == raw::FetchInterlock && (!x[2] || replaces))
            p.fetchRung[color] |= candidate;
    }
}

// The depth and stencil test classes a state's draws ask its variant for (RawMetalCubic.mm drawnRecord): a state that fetches
// beside a depth or stencil attachment without the storage form, each class raw_fragment_tests.h FragmentTests names above Any
// (Pass, ReadOnly, DepthWrite, StencilWrite: 1-4)
std::vector<uint8_t> testClasses(const raw::PackPipeline &p, const std::string &key)
{
    unsigned depth = 0, stencil = 0;
    const size_t ds = key.find("ds=");
    if (!(p.flags & raw::PipelineFetches) || (p.flags & raw::PipelineStorageMetadata) || ds == std::string::npos ||
        sscanf(key.c_str() + ds, "ds=%u/%u", &depth, &stencil) != 2 || !(depth || stencil))
        return {};
    return {1, 2, 3, 4};
}

struct Counts {
    uint32_t made = 0, refused = 0, failed = 0, split = 0, variants = 0;
    uint32_t free[4] = {}, freeFetching = 0, freeRefused = 0;
};

// The state's fetch-free form (fetch_free.h), under the state's key: refused where the form still fetches (it reads
// another attachment), so the state keeps its storage form
void makeFetchFree(raw::Direct &direct, Writer &w, id<MTLLibrary> qc, const std::string &line, const std::string &key,
                   raw::ProbeTextures textures, Counts &n)
{
    probe::FetchFree form;
    probe::State s;
    if (!probe::fetchFree(line, form) || !probe::parseState(qc, form.line, s) || form.colour >= 8)
        return;
    std::map<uint32_t, uint32_t> registers;
    raw::RuntimeRecord rec;
    std::string why;
    // made under its own key, whose colour fields give its registers their blend, and filed under the state's
    const raw::CompileOutcome outcome =
        direct.make(s.d, s.key, rec, why, textures, &registers, true, raw::PackForm::Default);
    const bool fetches = outcome == raw::CompileOutcome::Made &&
                         (rec.pipeline.flags & (raw::PipelineFetches | raw::PipelineBarrierForm));
    if (outcome != raw::CompileOutcome::Made || fetches) {
        (fetches ? n.freeFetching : n.freeRefused)++;
        why = fetches ? "it still fetches" : why;
        fprintf(stderr, "directpack: no fetch-free form (%s): %s\n", why.c_str(), key.substr(0, 160).c_str());
        return;
    }
    rec.pipeline.flags |= raw::PipelineFetchFreeForm;
    rec.pipeline.fetchRung[form.colour] = form.rung == 2 ? raw::FetchBlend : raw::FetchIteratedBlend;
    n.free[form.rung]++;
    record(w, key, outcome, why, rec, registers, form.rung == 3 ? form.program : "");
}

// A state's records: its own, its barrier form where its own is the storage form and leaves the blend to the ROP (the runtime
// gives one that computes it none: raw_gpu.cpp Device::pipeline), and one per test class its draws can ask for (named
// " | tests=<class>" after its key, as the runtime names them)
void makeState(raw::Direct &direct, Writer &w, id<MTLLibrary> qc, const std::string &line, raw::ProbeTextures textures,
               const probe::State &s, Counts &n)
{
    const auto make = [&](const std::string &key, raw::PackForm form, raw::RuntimeRecord &rec) {
        std::map<uint32_t, uint32_t> registers;
        std::string why;
        raw::CompileOutcome outcome = direct.make(s.d, key, rec, why, textures, &registers, true, form);
        if (const char *runtime = outcome == raw::CompileOutcome::Made ? runtimeOnly(rec.pipeline) : nullptr) {
            outcome = raw::CompileOutcome::Refused;
            why = runtime;
        }
        markCandidates(line, rec.pipeline);
        record(w, key, outcome, why, rec, registers);
        return outcome;
    };
    raw::RuntimeRecord rec;
    const raw::CompileOutcome outcome = make(s.key, raw::PackForm::Default, rec);
    (outcome == raw::CompileOutcome::Made ? n.made : outcome == raw::CompileOutcome::Refused ? n.refused : n.failed)++;
    if (outcome != raw::CompileOutcome::Made)
        return;
    n.split += rec.split;
    if ((rec.pipeline.flags & raw::PipelineStorageMetadata) && !rec.pipeline.storageBlendMask) {
        raw::RuntimeRecord barrier;
        make(s.key, raw::PackForm::Barrier, barrier);
    }
    if (rec.pipeline.flags & raw::PipelineFetches)
        makeFetchFree(direct, w, qc, line, s.key, textures, n);
    for (const uint8_t tests : testClasses(rec.pipeline, s.key)) {
        raw::RuntimeRecord variant;
        textures.fragmentTests = tests;
        n.variants += make(s.key + " | tests=" + std::to_string(tests), raw::PackForm::Default, variant) ==
                      raw::CompileOutcome::Made;
    }
}

void kernel(Writer &w, const std::string &key, raw::CompileOutcome outcome, const std::string &why,
            const raw::ComputeRecord &rec)
{
    if (!head(w, "DKRN", key, outcome, why))
        return;
    w.bytes(&rec.kernel, sizeof rec.kernel);
    w.list(rec.code);
    w.list(rec.data);
    w.bytes(rec.cbufType, rec.cbufs);
    w.list(rec.samplers);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr,
                    "usage: directpack <nvk dylib> <cache dir> <default.metallib> <corpus.log> <records.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        NSError *error = nil;
        id<MTLLibrary> qc = dev ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error] : nil;
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        Writer w = {fopen(argv[5], "wb")};
        if (!qc || !text || !w.f) {
            fprintf(stderr, "directpack: %s\n", dev ? "library, corpus or output unread" : nmb_last_error());
            return 1;
        }
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB"); path && *path) {
            own = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [dev newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "directpack: own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        // every register raw_emit writes, over no baseline: directlink.py sets them over the context
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(std::move(rules));
        direct.makeVariants(true);
        Counts counts;
        probe::eachState(dev, qc, own, standIn, probe::corpusLines(text, own != nil),
                         [&](const std::string &line, bool ours, probe::State &s) {
                             raw::ProbeTextures textures = probe::probeTextures(line, ours);
                             textures.cubic = s.cubic;
                             makeState(direct, w, qc, line, textures, s, counts);
                         });
        uint32_t kernels[3] = {};
        probe::eachKernel(
            dev, text,
            [&](const probe::Kernel &k) {
                std::unique_ptr<raw::KernelDispatch> ask;
                probe::dispatchOf(k.shape, k.shape.threads, k.textures, ask);
                raw::ComputeRecord rec;
                std::string why;
                raw::CompileOutcome outcome = direct.kernel(k.descriptor, ask->dispatch(), k.key, rec, why);
                if (const char *runtime = outcome == raw::CompileOutcome::Made ? runtimeOnly(rec.kernel) : nullptr) {
                    outcome = raw::CompileOutcome::Refused;
                    why = runtime;
                }
                kernel(w, k.key, outcome, why, rec);
                kernels[uint32_t(outcome)]++;
            },
            [](const std::string &, const std::string &) {});
        fclose(w.f);
        fprintf(stderr,
                "directpack: %u states made (%u split, %u test variants), %u refused, %u failed; %u kernels made, %u "
                "refused, "
                "%u failed\n",
                counts.made, counts.split, counts.variants, counts.refused, counts.failed, kernels[0], kernels[1],
                kernels[2]);
        fprintf(stderr,
                "directpack: fetch-free forms: %u on rung 2, %u on rung 3; %u still fetch, %u otherwise refused\n",
                counts.free[2], counts.free[3], counts.freeFetching, counts.freeRefused);
        return 0;
    }
}
