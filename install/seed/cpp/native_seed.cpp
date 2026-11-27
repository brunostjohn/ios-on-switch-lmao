// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/switch_seed_cpp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/emit.h"
#include "drivers/TegraGM20BMetal/gm20b_cache.h"
#include "drivers/TegraGM20BMetal/plugin/MetallibSlice.h"
#include "drivers/TegraGM20BMetal/utf8_path.h"
#include "install/seed/cpp/compute_parse.h"
#include "install/seed/cpp/json_min.h"
#include "install/seed/cpp/native_seed_io.h"
#include "install/seed/cpp/offline_metal.h"
#include "install/seed/cpp/render_parse.h"

namespace {

using namespace switch_seed;

bool sameKey(const gm20b::CacheKey &a, const gm20b::CacheKey &b)
{
    return a.stack == b.stack && a.key == b.key && !std::memcmp(a.vertexAir, b.vertexAir, sizeof a.vertexAir) &&
           !std::memcmp(a.fragmentAir, b.fragmentAir, sizeof a.fragmentAir);
}

const char *outcomeText(NativeInputOutcome outcome)
{
    switch (outcome) {
    case NativeInputOutcome::Made:
        return "made";
    case NativeInputOutcome::Refused:
        return "refused";
    case NativeInputOutcome::NoLibrary:
        return "missing-library";
    case NativeInputOutcome::NoFunction:
        return "missing-function";
    }
    return "refused";
}

struct CompiledRender {
    gm20b::CacheKey key;
    std::string status;
    std::string reason;
    std::string artifact;
    bool hasArtifact = false;
    std::vector<JsonValue> slots;
    bool hasSlots = false;
};

bool compileRender(gm20b::DirectCore &direct, gm20b::RecordCache &cache, const NativeRenderInput &input,
                   const std::string &output, CompiledRender &result)
{
    gm20b::RuntimeRecord record;
    std::string why;
    const std::vector<uint32_t> attrs = input.d.activeVertexAttributes();
    const auto outcome = direct.make(&input.d, attrs, result.key.key, record, why, {}, nullptr, true);
    result.status = outcome == gm20b::CompileOutcome::Made      ? "made"
                    : outcome == gm20b::CompileOutcome::Refused ? "refused"
                                                                : "failed";
    result.reason = why;
    if (outcome != gm20b::CompileOutcome::Made)
        return outcome != gm20b::CompileOutcome::Failed;
    const std::string relative = "rec-" + hex16(result.key.stack) + "/" + hex16(result.key.name()) + ".rec";
    gm20b::RuntimeRecord loaded;
    if (!cache.store(result.key, &record, "") ||
        cache.load(result.key, loaded, why) != gm20b::RecordCache::Found::Record ||
        loaded.keyString != record.keyString || !setFileMode(output + "/" + relative, 0644) ||
        !writeRecordSlots(record, result.key.stack, output, result.slots, result.hasSlots)) {
        result.status = "failed";
        result.reason = "native record write/readback failed";
        return false;
    }
    result.artifact = relative;
    result.hasArtifact = true;
    return true;
}

JsonValue compileComputeRow(LibraryStore &store, const std::string &qcPath, const std::string &storeRoot,
                            const std::string &line, uint64_t stack, gm20b::DirectCore &direct,
                            gm20b::RecordCache &cache, const std::string &output, const ComputeShape *region = nullptr)
{
    JsonValue row = JsonValue::makeObject();
    row.object["kind"] = JsonValue::makeString("compute");
    row.object["status"] = JsonValue::makeString("refused");
    row.object["reason"] = JsonValue::makeString("");
    std::string why;
    ComputeShape shape;
    if (!parseComputeLine(line, shape, why)) {
        row.object["reason"] = JsonValue::makeString(why);
        return row;
    }
    if (region)
        shape = *region;
    stateComputeFormatMaps(shape, nmb_kernel_texture_map);
    const size_t start = line.find(" | k ") + 5;
    const size_t end = line.find(" | ", start);
    const NativeStage stage = parseNativeStage(
        line.substr(start, end == std::string::npos ? std::string::npos : end - start), qcPath, storeRoot);
    row.object["kernelLibrary"] = JsonValue::makeString(stage.library);
    NativeInputOutcome inputStatus = NativeInputOutcome::Refused;
    std::string fnErr;
    auto function = loadNativeFunction(store, stage, inputStatus, fnErr);
    if (!function) {
        row.object["status"] = JsonValue::makeString(outcomeText(inputStatus));
        row.object["reason"] = JsonValue::makeString(fnErr.empty() ? "compute function unavailable" : fnErr);
        return row;
    }
    gm20b::AirHash air{};
    if (!function->airHash(air)) {
        row.object["reason"] = JsonValue::makeString("native AIR identity unavailable");
        return row;
    }
    std::unique_ptr<gm20b::KernelDispatchCore> dispatch;
    if (!buildNativeSeedDispatch(shape, dispatch, why)) {
        row.object["reason"] = JsonValue::makeString(why);
        return row;
    }
    const std::string dispatchKey = computeShapeKey(shape, air);
    gm20b::CacheKey key;
    key.stack = stack;
    key.key = "kernel | " + dispatchKey;
    identifyManifestRow(row, key);
    gm20b::ComputeRecord record;
    const auto found = cache.loadKernel(key, record, why);
    const ComputeDescriptor desc = buildComputeDescriptor(shape, function);
    const auto outcome = found == gm20b::RecordCache::Found::Record
                             ? gm20b::CompileOutcome::Made
                             : direct.kernel(&desc, dispatch->dispatch(), dispatchKey, record, why);
    row.object["status"] = JsonValue::makeString(outcome == gm20b::CompileOutcome::Made      ? "made"
                                                 : outcome == gm20b::CompileOutcome::Refused ? "refused"
                                                                                             : "failed");
    row.object["reason"] = JsonValue::makeString(why);
    if (outcome != gm20b::CompileOutcome::Made)
        return row;
    const std::string relative = "rec-" + hex16(stack) + "/" + hex16(key.name()) + ".rec";
    gm20b::ComputeRecord loaded;
    if (!cache.storeKernel(key, &record, "") ||
        cache.loadKernel(key, loaded, why) != gm20b::RecordCache::Found::Record ||
        gm20b::encodeKernelEntry(key, &record, "") != gm20b::encodeKernelEntry(key, &loaded, "") ||
        !setFileMode(output + "/" + relative, 0644)) {
        row.object["status"] = JsonValue::makeString("failed");
        row.object["reason"] = JsonValue::makeString("native compute record write/readback failed");
        return row;
    }
    row.object["artifact"] = JsonValue::makeString(relative);
    return row;
}

std::vector<JsonValue> compileComputeRows(LibraryStore &store, const std::string &qcPath, const std::string &storeRoot,
                                          const std::string &line, uint64_t stack, gm20b::DirectCore &direct,
                                          gm20b::RecordCache &cache, const std::string &output)
{
    ComputeShape shape;
    std::string why;
    if (!parseComputeLine(line, shape, why))
        return {compileComputeRow(store, qcPath, storeRoot, line, stack, direct, cache, output)};
    if (shape.factsOnly)
        return {};
    std::vector<JsonValue> rows;
    eachRegionShape(shape, [&](const ComputeShape &region, uint8_t mask) {
        JsonValue row = compileComputeRow(store, qcPath, storeRoot, line, stack, direct, cache, output, &region);
        row.object["region"] = JsonValue::makeUint(mask);
        std::vector<JsonValue> local;
        local.push_back(JsonValue::makeUint(region.threads[0]));
        local.push_back(JsonValue::makeUint(region.threads[1]));
        local.push_back(JsonValue::makeUint(region.threads[2]));
        row.object["local"] = JsonValue::makeArray(std::move(local));
        row.object["regionMode"] = JsonValue::makeBool(region.regionMode);
        rows.push_back(std::move(row));
    });
    return rows;
}

}  // namespace

extern "C" switch_seed_status_t switch_seed_native_bucket(const switch_seed_native_opts_t *opts, char *err_buf,
                                                          size_t err_cap)
{
    try {
        if (!opts || !opts->kind || !opts->qc_metallib || !opts->output_dir ||
            (opts->corpus_count == 0 && opts->derived_count == 0)) {
            setErrBuf(err_buf, err_cap, "missing required native-seed options");
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        const switch_metal::AirSliceRule *sliceRule = switch_metal::airSliceRuleOf(opts->kind);
        if (!sliceRule) {
            setErrBuf(err_buf, err_cap, std::string("unknown IPSW kind: ") + opts->kind);
            return SWITCH_SEED_ERR_INVALID_ARG;
        }
        const auto isCancelled = [opts]() -> bool {
            return opts->cancel_check && opts->cancel_check(opts->cancel_ctx) != 0;
        };
        const std::string output = opts->output_dir;
        const std::string qcPath = opts->qc_metallib;
        const std::string storeRoot = opts->libs_store ? opts->libs_store : "";
        const uint64_t stack = opts->stack_key;

        gm20b::RecordCache cache;
        if (!openSeedOutput(output, stack, cache)) {
            setErrBuf(err_buf, err_cap, "failed to open output directory " + output);
            return SWITCH_SEED_ERR_IO;
        }

        LibraryStore store(*sliceRule, opts->scope);
        gm20b::PackRules rules;
        rules.varying = gm20b::emittableMethods();
        gm20b::DirectCore direct(rules, true, offlineTranslator());
        if (!direct.configureKind(opts->kind)) {
            setErrBuf(err_buf, err_cap, std::string("unsupported kind: ") + opts->kind);
            return SWITCH_SEED_ERR_INVALID_ARG;
        }

        std::vector<JsonValue> rows;
        std::map<uint64_t, CompiledRender> compiled;
        bool failed = false;

        const auto processList = [&](const char *const *paths, size_t count, bool derived) -> switch_seed_status_t {
            for (size_t i = 0; i < count; ++i) {
                if (isCancelled()) {
                    setErrBuf(err_buf, err_cap, "cancelled");
                    return SWITCH_SEED_ERR_CANCELLED;
                }
                if (!paths || !paths[i]) {
                    setErrBuf(err_buf, err_cap, "null corpus path");
                    return SWITCH_SEED_ERR_INVALID_ARG;
                }
                const std::string corpusPath = paths[i];
                const auto wcorpus = gm20b::fsPathUtf8(corpusPath);
                std::ifstream stream(wcorpus);
                if (wcorpus.empty() || !stream) {
                    std::fprintf(stderr, "cannot read %s\n", corpusPath.c_str());
                    setErrBuf(err_buf, err_cap, "cannot read " + corpusPath);
                    return SWITCH_SEED_ERR_IO;
                }
                std::string line;
                size_t lineNumber = 0;
                while (std::getline(stream, line)) {
                    if (isCancelled()) {
                        setErrBuf(err_buf, err_cap, "cancelled");
                        return SWITCH_SEED_ERR_CANCELLED;
                    }
                    ++lineNumber;
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if ((line.rfind("R ", 0) != 0 && line.rfind("D ", 0) != 0) ||
                        line.find(" ok=1 ") == std::string::npos)
                        continue;
                    if (line[0] == 'D') {
                        for (JsonValue &row :
                             compileComputeRows(store, qcPath, storeRoot, line, stack, direct, cache, output)) {
                            row.object["source"] = JsonValue::makeString(corpusPath);
                            row.object["line"] = JsonValue::makeUint(lineNumber);
                            row.object["derived"] = JsonValue::makeBool(derived);
                            row.object["input"] = JsonValue::makeString(line);
                            const JsonValue *st = row.field("status");
                            if (st && st->isString() && st->text == "failed")
                                failed = true;
                            rows.push_back(std::move(row));
                        }
                        continue;
                    }
                    NativeRenderInput input;
                    std::string why;
                    const auto outcome = parseNativeRenderInput(store, qcPath, storeRoot, line, input, why);
                    JsonValue row = JsonValue::makeObject();
                    row.object["source"] = JsonValue::makeString(corpusPath);
                    row.object["kind"] = JsonValue::makeString("render");
                    row.object["line"] = JsonValue::makeUint(lineNumber);
                    row.object["derived"] = JsonValue::makeBool(derived);
                    row.object["input"] = JsonValue::makeString(line);
                    row.object["status"] = JsonValue::makeString(outcomeText(outcome));
                    row.object["reason"] = JsonValue::makeString(why);
                    row.object["vertexLibrary"] = JsonValue::makeString(input.vertex.library);
                    row.object["fragmentLibrary"] = JsonValue::makeString(input.fragment.library);
                    if (outcome == NativeInputOutcome::Made) {
                        gm20b::CacheKey key;
                        if (!nativeRenderCacheKey(input, stack, key)) {
                            row.object["status"] = JsonValue::makeString("refused");
                            row.object["reason"] = JsonValue::makeString("native AIR identity unavailable");
                        } else {
                            const auto found = compiled.find(key.name());
                            if (found != compiled.end() && !sameKey(found->second.key, key)) {
                                row.object["status"] = JsonValue::makeString("failed");
                                row.object["reason"] = JsonValue::makeString("native record name collision");
                                failed = true;
                            } else {
                                if (found == compiled.end()) {
                                    CompiledRender result;
                                    result.key = key;
                                    if (!compileRender(direct, cache, input, output, result))
                                        failed = true;
                                    compiled.emplace(key.name(), std::move(result));
                                }
                                const CompiledRender &result = compiled.at(key.name());
                                identifyManifestRow(row, result.key);
                                row.object["status"] = JsonValue::makeString(result.status);
                                row.object["reason"] = JsonValue::makeString(result.reason);
                                if (result.hasArtifact)
                                    row.object["artifact"] = JsonValue::makeString(result.artifact);
                                if (result.hasSlots)
                                    row.object["slots"] = JsonValue::makeArray(result.slots);
                            }
                        }
                    }
                    rows.push_back(std::move(row));
                }
                if (!stream.eof()) {
                    reportSeedError("read", corpusPath, errno);
                    setErrBuf(err_buf, err_cap, "read error on " + corpusPath);
                    return SWITCH_SEED_ERR_IO;
                }
            }
            return SWITCH_SEED_OK;
        };

        if (const auto st = processList(opts->corpora, opts->corpus_count, false); st != SWITCH_SEED_OK)
            return st;
        if (const auto st = processList(opts->derived, opts->derived_count, true); st != SWITCH_SEED_OK)
            return st;
        if (!writeSeedManifest(output, stack, std::move(rows))) {
            setErrBuf(err_buf, err_cap, "failed to write manifest.json");
            return SWITCH_SEED_ERR_IO;
        }
        if (failed) {
            std::fprintf(stderr, "native-seed: native records failed: their rows in %s/manifest.json\n",
                         output.c_str());
            setErrBuf(err_buf, err_cap, "native records failed");
            return SWITCH_SEED_ERR_COMPILE;
        }
        return SWITCH_SEED_OK;
    } catch (const std::exception &e) {
        setErrBuf(err_buf, err_cap, std::string("native-seed exception: ") + e.what());
        return SWITCH_SEED_ERR_COMPILE;
    } catch (...) {
        setErrBuf(err_buf, err_cap, "native-seed: unknown C++ exception");
        return SWITCH_SEED_ERR_COMPILE;
    }
}
