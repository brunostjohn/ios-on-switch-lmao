// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/switch_seed_cpp.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/emit.h"
#include "drivers/TegraGM20BMetal/plugin/MetallibSlice.h"
#include "drivers/TegraGM20BMetal/plugin/TwoPlaneStandIn.h"
#include "drivers/TegraGM20BMetal/record_parts.h"
#include "drivers/TegraGM20BMetal/utf8_path.h"
#include "drivers/TegraGM20BMetal/xr_pass.h"
#include "drivers/TegraGM20BMetal/zeta.h"
#include "install/seed/cpp/compute_parse.h"
#include "install/seed/cpp/directpack_writer.h"
#include "install/seed/cpp/offline_metal.h"
#include "install/seed/cpp/render_parse.h"

namespace {

using switch_seed::RecordWriter;
using switch_seed::writeHead;
using switch_seed::writeKernel;
using switch_seed::writeRecord;

void setErr(char *buf, size_t cap, const std::string &msg) noexcept
{
    if (!buf || !cap)
        return;
    const size_t n = msg.size() < cap - 1 ? msg.size() : cap - 1;
    std::memcpy(buf, msg.data(), n);
    buf[n] = '\0';
}

void markCandidates(const std::string &line, gm20b::PackPipeline &p)
{
    const size_t at = line.find(" rung=");
    const int rung = at == std::string::npos ? 0 : std::atoi(line.c_str() + at + 6);
    const uint8_t candidate = rung == 2 ? gm20b::kFetchCandidateBlend : rung == 3 ? gm20b::kFetchCandidateIterated : 0;
    const std::string head = line.substr(0, line.find(" | "));
    for (size_t c = head.find(" c"); candidate && c != std::string::npos; c = head.find(" c", c + 2)) {
        unsigned color = 0, x[9] = {};
        if (std::sscanf(head.c_str() + c, " c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &color, x, x + 1, x + 2, x + 3, x + 4,
                        x + 5, x + 6, x + 7, x + 8) != 10 ||
            color >= 8)
            continue;
        const bool replaces = x[3] == 1 && x[4] == 0 && x[5] == 0 && x[6] == 1 && x[7] == 0 && x[8] == 0;
        if ((p.fetchRung[color] & gm20b::kFetchRungMask) == gm20b::FetchInterlock && (!x[2] || replaces))
            p.fetchRung[color] |= candidate;
    }
}

std::vector<uint8_t> testClasses(const gm20b::PackPipeline &p, const std::string &key)
{
    unsigned depth = 0, stencil = 0;
    const size_t ds = key.find("ds=");
    if (!(p.flags & gm20b::PipelineFetches) || (p.flags & gm20b::PipelineStorageMetadata) || ds == std::string::npos ||
        std::sscanf(key.c_str() + ds, "ds=%u/%u", &depth, &stencil) != 2 || !(depth || stencil))
        return {};
    std::vector<uint8_t> classes = {1, 2};
    if (gm20b::zetaHasDepth(depth) || gm20b::zetaHasDepth(stencil))
        classes.push_back(3);
    if (gm20b::zetaHasStencil(gm20b::testedZeta(depth)) || gm20b::zetaHasStencil(stencil))
        classes.push_back(4);
    return classes;
}

struct Counts {
    uint32_t made = 0, refused = 0, failed = 0, dropped = 0, split = 0, variants = 0, sampled = 0;
    uint32_t free[4] = {}, freeFetching = 0, freeRefused = 0, freeBarrier = 0;
    uint32_t written = 0, twins = 0;
    uint32_t reflected[2] = {}, unreflected[2] = {};
};

void writeReflection(RecordWriter &w, bool compute, const std::string &key, bool made, const std::string &blob,
                     const std::string &why, Counts &n)
{
    if (!made) {
        if (!n.unreflected[compute]++)
            std::fprintf(stderr, "directpack: no reflection of %s: %s\n", key.c_str(), why.c_str());
        return;
    }
    n.reflected[compute]++;
    w.raw("DRFL", 4);
    w.u32(compute ? 1u : 0u);
    w.text(key);
    w.text(blob);
}

void writeFetchFree(gm20b::DirectCore &direct, switch_seed::LibraryStore &store, const std::string &libRoot,
                    RecordWriter &w, const switch_seed::ProbeState &s, const std::string &line,
                    gm20b::ProbeTexturesCore textures, Counts &n)
{
    probe::FetchFree form;
    gm20b::RuntimeRecord rec;
    std::map<uint32_t, uint32_t> registers;
    std::string why;
    bool barrier = false;
    const switch_seed::FreeOutcome outcome =
        switch_seed::makeFetchFree(direct, store, libRoot, s, line, textures, form, rec, &registers, why, barrier);
    if (outcome == switch_seed::FreeOutcome::None)
        return;
    if (outcome != switch_seed::FreeOutcome::Made) {
        const bool fetches = outcome == switch_seed::FreeOutcome::Fetches;
        (fetches ? n.freeFetching : n.freeRefused)++;
        why = fetches ? "it still fetches" : why;
        std::fprintf(stderr, "directpack: no fetch-free form (%s): %s\n", why.c_str(), s.key.substr(0, 160).c_str());
        return;
    }
    rec.pipeline.flags |= gm20b::PipelineFetchFreeForm;
    rec.pipeline.fetchRung[form.colour] = form.rung == 2 ? gm20b::FetchBlend : gm20b::FetchIteratedBlend;
    n.free[form.rung]++;
    n.freeBarrier += barrier ? 1u : 0u;
    writeRecord(w, s.key, gm20b::CompileOutcome::Made, why, rec, registers, direct.boundDraw(),
                direct.strideTwinRegisters(), form.rung == 3 ? form.program : "");
}

void makeState(gm20b::DirectCore &direct, switch_seed::LibraryStore &store, const std::string &libRoot, RecordWriter &w,
               const std::string &line, gm20b::ProbeTexturesCore textures, const switch_seed::ProbeState &s, Counts &n)
{
    const std::vector<uint32_t> attrs = s.d.activeVertexAttributes();
    const auto make = [&](const std::string &key, gm20b::PackForm form, const gm20b::ProbeTexturesCore &t,
                          gm20b::RuntimeRecord &rec) {
        std::map<uint32_t, uint32_t> registers;
        std::string why;
        const gm20b::CompileOutcome outcome = direct.make(&s.d, attrs, key, rec, why, t, &registers, true, form);
        markCandidates(line, rec.pipeline);
        writeRecord(w, key, outcome, why, rec, registers, direct.boundDraw(), direct.strideTwinRegisters());
        n.twins += (outcome == gm20b::CompileOutcome::Made && rec.strideTwin) ? 1u : 0u;
        return outcome;
    };
    const auto forms = [&](gm20b::ProbeTexturesCore t, const char *suffix) {
        gm20b::RuntimeRecord rec;
        const gm20b::CompileOutcome outcome = make(s.key + suffix, gm20b::PackForm::Default, t, rec);
        if (outcome != gm20b::CompileOutcome::Made)
            return outcome;
        n.split += rec.split ? 1u : 0u;
        if (rec.pipeline.flags & gm20b::PipelineStorageMetadata) {
            gm20b::RuntimeRecord barrier;
            make(s.key + suffix, gm20b::PackForm::Barrier, t, barrier);
        }
        if (!*suffix && (rec.pipeline.flags & gm20b::PipelineFetches))
            writeFetchFree(direct, store, libRoot, w, s, line, t, n);
        for (const uint8_t tests : testClasses(rec.pipeline, s.key)) {
            gm20b::RuntimeRecord variant;
            t.fragmentTests = tests;
            n.variants += make(s.key + " | tests=" + std::to_string(tests) + suffix, gm20b::PackForm::Default, t,
                               variant) == gm20b::CompileOutcome::Made
                              ? 1u
                              : 0u;
        }
        return outcome;
    };
    const std::string sampled =
        (s.xrSlots ? gm20b::xrSampledVariant(s.xrSlots) : std::string()) + gm20b::writtenVariant(s.written);
    textures.xrSlots = s.xrSlots;
    textures.written = s.written;
    const gm20b::CompileOutcome outcome = forms(textures, sampled.c_str());
    (outcome == gm20b::CompileOutcome::Made      ? n.made
     : outcome == gm20b::CompileOutcome::Refused ? n.refused
                                                 : n.failed)++;
    n.sampled += (s.xrSlots && outcome == gm20b::CompileOutcome::Made) ? 1u : 0u;
    n.written += (!s.written.empty() && outcome == gm20b::CompileOutcome::Made) ? 1u : 0u;
}

}  // namespace

extern "C" switch_seed_status_t switch_seed_directpack(const switch_seed_directpack_opts_t *opts, char *err_buf,
                                                       size_t err_cap)
{
    try {
        if (!opts || !opts->kind || !opts->qc_metallib || !opts->corpus_path || !opts->output_path) {
            setErr(err_buf, err_cap, "missing required directpack options");
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        const switch_metal::AirSliceRule *sliceRule = switch_metal::airSliceRuleOf(opts->kind);
        if (!sliceRule) {
            setErr(err_buf, err_cap, std::string("unknown IPSW kind: ") + opts->kind);
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        const auto isCancelled = [opts]() -> bool {
            return opts->cancel_check && opts->cancel_check(opts->cancel_ctx) != 0;
        };

        switch_seed::LibraryStore store(*sliceRule, opts->scope);
        std::string err;
        auto qc = store.loadFile(opts->qc_metallib, err);
        std::string corpusText;
        const bool readOk = switch_seed::readWholeFile(opts->corpus_path, corpusText, err);
        RecordWriter w(gm20b::fopenUtf8(opts->output_path, L"wb", "wb"));
        if (!qc || !readOk || !w.f) {
            setErr(err_buf, err_cap, err.empty() ? "directpack: library, corpus or output unread" : err);
            return SWITCH_SEED_ERR_IO;
        }

        std::shared_ptr<switch_seed::OfflineLibrary> own, standIn;
        if (opts->own_lib && *opts->own_lib) {
            own = store.loadFile(opts->own_lib, err);
            if (own)
                standIn = store.loadMemory(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary, err);
            if (!own || !standIn) {
                setErr(err_buf, err_cap, "directpack: own library: " + err);
                return SWITCH_SEED_ERR_IO;
            }
        }

        const std::string libRoot = opts->lib_root ? opts->lib_root : "";
        const std::string dataLibs = opts->data_libs ? opts->data_libs : ".";
        const std::string labelPrefix = opts->label_prefix ? opts->label_prefix : "com.apple.coreanimation.";

        gm20b::PackRules rules;
        rules.varying = gm20b::emittableMethods();
        gm20b::DirectCore direct(std::move(rules), false, switch_seed::offlineTranslator());
        if (!direct.configureKind(opts->kind)) {
            setErr(err_buf, err_cap, std::string("unsupported kind: ") + opts->kind);
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        direct.makeVariants(true);
        direct.keepBoundDraws(true);

        Counts counts;
        std::set<std::string> reflected;
        std::set<std::string> seenRender;
        const std::vector<std::string> lines = switch_seed::corpusLines(corpusText, labelPrefix, own != nullptr);
        for (const std::string &marked : lines) {
            if (isCancelled()) {
                setErr(err_buf, err_cap, "cancelled");
                return SWITCH_SEED_ERR_CANCELLED;
            }
            switch_seed::ProbeState s;
            const bool ours = marked.rfind("@O ", 0) == 0;
            const bool framework = marked.rfind("@L ", 0) == 0;
            const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
            const std::string line = ours ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
            const std::string path = framework ? marked.substr(3, pathEnd - 3) : "";
            std::shared_ptr<switch_seed::OfflineLibrary> library =
                !ours ? qc : (line.find("| f two_plane_") != std::string::npos ? standIn : own);
            if (framework) {
                std::string resolved;
                std::string loadErr;
                if (!switch_seed::resolveContainedPath(libRoot, path, resolved))
                    library = nullptr;
                else
                    library = store.loadFile(resolved, loadErr);
                if (!library) {
                    writeHead(w, "DREC", line, gm20b::CompileOutcome::Refused,
                              "its library " + path + " did not load from the crawl (PROBE_LIB_ROOT)");
                    counts.dropped++;
                    continue;
                }
            }
            const std::string name = framework ? switch_seed::frameworkNameOfPath(path) : "";
            std::string why;
            if (!switch_seed::parseProbeState(store, library, libRoot, line, s, name, &why)) {
                writeHead(w, "DREC", line, gm20b::CompileOutcome::Refused, why);
                counts.dropped++;
                continue;
            }
            s.library = library;
            s.framework = name;
            if (framework && name.empty()) {
                const std::string cubic = s.cubic.empty() ? std::string() : s.cubic.key();
                gm20b::CacheKey airKeyed;
                if (!s.d.renderCacheKey(s.key.substr(0, s.key.size() - cubic.size()), true, 0, airKeyed)) {
                    writeHead(w, "DREC", line, gm20b::CompileOutcome::Refused,
                              "its functions' AIR keys no state (renderCacheKey)");
                    counts.dropped++;
                    continue;
                }
                s.key = airKeyed.key + cubic;
            }
            const std::string dedupKey = s.key + (s.xrSlots ? gm20b::xrSampledVariant(s.xrSlots) : std::string()) +
                                         gm20b::writtenVariant(s.written);
            if (!seenRender.insert(dedupKey).second)
                continue;
            gm20b::ProbeTexturesCore textures = switch_seed::probeTexturesOf(line, ours);
            textures.cubic = s.cubic;
            makeState(direct, store, libRoot, w, line, textures, s, counts);
            const std::string stateKey = s.key.substr(0, gm20b::variantNameAt(s.key));
            const uint64_t keys[3] = {s.fast, gm20b::packKeyHash(stateKey), s.tier};
            w.raw("DFKY", 4);
            w.bytes(keys, sizeof keys);
            if (reflected.insert(stateKey).second) {
                std::string blob, rWhy;
                const bool made = switch_seed::renderReflection(s.d, blob, rWhy);
                writeReflection(w, false, stateKey, made, blob, rWhy, counts);
            }
            if (!w.ok) {
                setErr(err_buf, err_cap, std::string("directpack write failed: ") + std::strerror(w.err));
                return SWITCH_SEED_ERR_IO;
            }
        }

        uint32_t kernels[3] = {}, kernelsDropped = 0;
        std::set<std::string> seenKernels;
        size_t pos = 0;
        while (pos <= corpusText.size()) {
            if (isCancelled()) {
                setErr(err_buf, err_cap, "cancelled");
                return SWITCH_SEED_ERR_CANCELLED;
            }
            const size_t nl = corpusText.find('\n', pos);
            const std::string line = corpusText.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? corpusText.size() + 1 : nl + 1;
            if (line.rfind("D ok=1 ", 0) != 0)
                continue;
            switch_seed::ComputeShape shape;
            std::string why;
            gm20b::AirHash air{};
            std::shared_ptr<switch_seed::OfflineFunction> fn;
            if (!switch_seed::parseComputeLine(line, shape, why) ||
                !(fn = switch_seed::loadDirectpackKernelFunction(store, libRoot, dataLibs, shape, air))) {
                writeHead(w, "DKRN", line, gm20b::CompileOutcome::Refused,
                          why.empty() ? "no function " + shape.function : why);
                kernelsDropped++;
                continue;
            }
            switch_seed::stateComputeFormatMaps(shape, nmb_kernel_texture_map);
            const switch_seed::ComputeDescriptor desc = switch_seed::buildComputeDescriptor(shape, fn);
            if (!switch_seed::kernelCompiles(desc, why)) {
                writeHead(w, "DKRN", line, gm20b::CompileOutcome::Refused, why);
                kernelsDropped++;
                continue;
            }
            const switch_seed::ComputeShape original = shape;
            switch_seed::eachRegionShape(original, [&](const switch_seed::ComputeShape &region, uint8_t) {
                const std::string key = switch_seed::computeShapeKey(region, air);
                if (!seenKernels.insert(key).second)
                    return;
                std::unique_ptr<gm20b::KernelDispatchCore> ask;
                switch_seed::buildDirectpackDispatch(region, region.threads, ask);
                gm20b::ComputeRecord rec;
                std::string kWhy;
                uint32_t facts[3] = {};
                const gm20b::CompileOutcome outcome = !switch_seed::kernelFacts(desc, facts, kWhy)
                                                          ? gm20b::CompileOutcome::Refused
                                                          : direct.kernel(&desc, ask->dispatch(), key, rec, kWhy);
                writeKernel(w, key, outcome, kWhy, rec, facts);
                kernels[uint32_t(outcome)]++;
                if (reflected.insert(key.substr(0, key.find(" | tg="))).second) {
                    std::string blob, unmade;
                    const bool made = switch_seed::computeReflection(desc, blob, unmade);
                    writeReflection(w, true, key, made, blob, unmade, counts);
                }
            });
            if (!w.ok) {
                setErr(err_buf, err_cap, std::string("directpack write failed: ") + std::strerror(w.err));
                return SWITCH_SEED_ERR_IO;
            }
        }

        if (!w.close()) {
            setErr(err_buf, err_cap, std::string("directpack close failed: ") + std::strerror(w.err));
            return SWITCH_SEED_ERR_IO;
        }
        std::fprintf(stderr,
                     "directpack: %u states made (%u split, %u test variants, %u sampled two-plane variants, %u "
                     "written-format variants), %u refused, %u failed, %u corpus lines making no state; %u kernels "
                     "made, %u refused, %u failed, %u dispatch lines making no kernel\n",
                     counts.made, counts.split, counts.variants, counts.sampled, counts.written, counts.refused,
                     counts.failed, counts.dropped, kernels[0], kernels[1], kernels[2], kernelsDropped);
        std::fprintf(stderr,
                     "directpack: fetch-free forms: %u on rung 2, %u on rung 3 (%u reading other colours behind a "
                     "barrier); %u still fetch, %u otherwise refused; %u stride twins\n",
                     counts.free[2], counts.free[3], counts.freeBarrier, counts.freeFetching, counts.freeRefused,
                     counts.twins);
        std::fprintf(stderr, "directpack: reflections: %u render (%u none), %u compute (%u none)\n",
                     counts.reflected[0], counts.unreflected[0], counts.reflected[1], counts.unreflected[1]);
        return SWITCH_SEED_OK;
    } catch (const std::exception &e) {
        setErr(err_buf, err_cap, std::string("directpack exception: ") + e.what());
        return SWITCH_SEED_ERR_COMPILE;
    } catch (...) {
        setErr(err_buf, err_cap, "directpack: unknown C++ exception");
        return SWITCH_SEED_ERR_COMPILE;
    }
}
