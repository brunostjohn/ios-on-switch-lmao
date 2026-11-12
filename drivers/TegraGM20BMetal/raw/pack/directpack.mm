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
// (as the programs above; the prologue's header 80 bytes of zeros). A record of one of a state's variants has the variant's name
// after its key (runtime/raw_pack.h variantNameAt).
// Then per compute dispatch shape (the corpus's D lines, as the kernel probe read them: kernel_shapes.h): "DKRN", the outcome,
// its raw_compute_key.h key and the reason, then for a made kernel its PackKernel (raw_direct_compute.h; program 0), its code,
// shader data and constant buffer types, and its sampler headers, which its sampler entries number from 0 (slot 0x80 | n).
// PROBE_DATA_LIBS names the directory of the corpus's data: libraries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include "../runtime/raw_emit.h"
#include "../runtime/raw_xr_pass.h"
#include "../runtime/raw_zeta.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "direct_kernels.h"
#include "direct_states.h"

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
bool head(Writer &w, const char *tag, const std::string &key, raw::CompileOutcome outcome, const std::string &why)
{
    fwrite(tag, 1, 4, w.f);
    w.u32(outcome == raw::CompileOutcome::Made ? 0 : outcome == raw::CompileOutcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    return outcome == raw::CompileOutcome::Made;
}

void record(Writer &w, const std::string &key, raw::CompileOutcome outcome, const std::string &why,
            const raw::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers)
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
// (Pass, ReadOnly, DepthWrite, StencilWrite: 1-4) that its attachments' formats can reach (fragmentTests answers DepthWrite
// only of a zeta with depth, StencilWrite only of one with stencil)
std::vector<uint8_t> testClasses(const raw::PackPipeline &p, const std::string &key)
{
    unsigned depth = 0, stencil = 0;
    const size_t ds = key.find("ds=");
    if (!(p.flags & raw::PipelineFetches) || (p.flags & raw::PipelineStorageMetadata) || ds == std::string::npos ||
        sscanf(key.c_str() + ds, "ds=%u/%u", &depth, &stencil) != 2 || !(depth || stencil))
        return {};
    std::vector<uint8_t> classes = {1, 2};
    if (raw::zetaHasDepth(depth) || raw::zetaHasDepth(stencil))
        classes.push_back(3);
    if (raw::zetaHasStencil(depth) || raw::zetaHasStencil(stencil))
        classes.push_back(4);
    return classes;
}

struct Counts {
    uint32_t made = 0, refused = 0, failed = 0, split = 0, variants = 0, planes = 0, planesRefused = 0, sampled = 0;
};

// Whether a state's key names an RGB10A8_2P_XR10 colour ("c<j>=550:")
bool twoPlaneColors(const std::string &key)
{
    const std::string head = key.substr(0, key.find(" | "));
    for (size_t at = head.find(" c"); at != std::string::npos; at = head.find(" c", at + 2))
        if (head.compare(at + 4, 4, "550:") == 0)
            return true;
    return false;
}

// A state's records: its own, its barrier form where its own is the storage form and leaves the blend to the ROP (the runtime
// gives one that computes it none: raw_gpu.cpp Device::pipeline), and one per test class its draws can ask for (named
// " | tests=<class>" after its key, as the runtime names them); then, for a state of RGB10A8_2P_XR10 colours, the same of its
// two-plane form (raw_xr_pass.h), each named " | xr2p" after
void makeState(raw::Direct &direct, Writer &w, const std::string &line, raw::ProbeTextures textures,
               const probe::State &s, Counts &n)
{
    const auto make = [&](const std::string &key, raw::PackForm form, const raw::ProbeTextures &t,
                          raw::RuntimeRecord &rec) {
        std::map<uint32_t, uint32_t> registers;
        std::string why;
        const raw::CompileOutcome outcome = direct.make(s.d, key, rec, why, t, &registers, true, form);
        markCandidates(line, rec.pipeline);
        record(w, key, outcome, why, rec, registers);
        return outcome;
    };
    const auto forms = [&](raw::ProbeTextures t, const char *suffix) {
        raw::RuntimeRecord rec;
        const raw::CompileOutcome outcome = make(s.key + suffix, raw::PackForm::Default, t, rec);
        if (outcome != raw::CompileOutcome::Made)
            return outcome;
        n.split += rec.split;
        if ((rec.pipeline.flags & raw::PipelineStorageMetadata) && !rec.pipeline.storageBlendMask) {
            raw::RuntimeRecord barrier;
            make(s.key + suffix, raw::PackForm::Barrier, t, barrier);
        }
        for (const uint8_t tests : testClasses(rec.pipeline, s.key)) {
            raw::RuntimeRecord variant;
            t.fragmentTests = tests;
            n.variants += make(s.key + " | tests=" + std::to_string(tests) + suffix, raw::PackForm::Default, t,
                               variant) == raw::CompileOutcome::Made;
        }
        return outcome;
    };
    // a line of a sampled two-plane variant (" | xrs=<slots>") names that variant's records alone
    const std::string sampled = s.xrSlots ? raw::xrSampledVariant(s.xrSlots) : std::string();
    textures.xrSlots = s.xrSlots;
    const raw::CompileOutcome outcome = forms(textures, sampled.c_str());
    (outcome == raw::CompileOutcome::Made ? n.made : outcome == raw::CompileOutcome::Refused ? n.refused : n.failed)++;
    n.sampled += s.xrSlots && outcome == raw::CompileOutcome::Made;
    if (outcome != raw::CompileOutcome::Made || !twoPlaneColors(s.key))
        return;
    raw::ProbeTextures planes = textures;
    planes.twoPlane = true;
    const std::string planesName = raw::kXrPlanesVariant + sampled;
    (forms(planes, planesName.c_str()) == raw::CompileOutcome::Made ? n.planes : n.planesRefused)++;
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
                             makeState(direct, w, line, textures, s, counts);
                         });
        uint32_t kernels[3] = {};
        probe::eachKernel(
            dev, text,
            [&](const probe::Kernel &k) {
                std::unique_ptr<raw::KernelDispatch> ask;
                probe::dispatchOf(k.shape, k.shape.threads, k.textures, ask);
                raw::ComputeRecord rec;
                std::string why;
                const raw::CompileOutcome outcome = direct.kernel(k.descriptor, ask->dispatch(), k.key, rec, why);
                kernel(w, k.key, outcome, why, rec);
                kernels[uint32_t(outcome)]++;
            },
            [](const std::string &, const std::string &) {});
        fclose(w.f);
        fprintf(stderr,
                "directpack: %u states made (%u split, %u test variants, %u two-plane forms, %u refused, %u sampled two-plane "
                "variants), %u refused, %u failed; %u kernels made, %u refused, %u failed\n",
                counts.made, counts.split, counts.variants, counts.planes, counts.planesRefused, counts.sampled,
                counts.refused, counts.failed, kernels[0], kernels[1], kernels[2]);
        return 0;
    }
}
