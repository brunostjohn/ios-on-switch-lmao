// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/record_parts.h"

namespace switch_seed {

struct RecordWriter {
    FILE *f = nullptr;
    bool ok = true;
    int err = 0;

    explicit RecordWriter(FILE *file) : f(file)
    {}

    RecordWriter(const RecordWriter &) = delete;
    RecordWriter &operator=(const RecordWriter &) = delete;

    ~RecordWriter()
    {
        if (f)
            std::fclose(f);
    }

    void raw(const void *p, size_t n)
    {
        if (!ok || !n)
            return;
        if (std::fwrite(p, 1, n, f) != n) {
            ok = false;
            err = errno;
        }
    }

    void u32(uint32_t v)
    {
        raw(&v, 4);
    }

    void bytes(const void *p, size_t n)
    {
        u32(uint32_t(n));
        if (n)
            raw(p, n);
    }

    void text(const std::string &s)
    {
        bytes(s.data(), s.size());
    }

    template <typename T> void list(const std::vector<T> &v)
    {
        bytes(v.data(), v.size() * sizeof(T));
    }

    bool close()
    {
        if (!f)
            return ok;
        if (ok && std::fflush(f) != 0) {
            ok = false;
            err = errno;
        }
        const int rc = std::fclose(f);
        f = nullptr;
        if (rc != 0 && ok) {
            ok = false;
            err = errno;
        }
        return ok;
    }
};

inline bool writeHead(RecordWriter &w, const char *tag, const std::string &key, gm20b::CompileOutcome outcome,
                      const std::string &why)
{
    w.raw(tag, 4);
    w.u32(outcome == gm20b::CompileOutcome::Made ? 0 : outcome == gm20b::CompileOutcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    return outcome == gm20b::CompileOutcome::Made;
}

inline void writeRecord(RecordWriter &w, const std::string &key, gm20b::CompileOutcome outcome, const std::string &why,
                        const gm20b::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers,
                        const gm20b::BoundDraw &bound, const std::map<uint32_t, uint32_t> &twinRegisters,
                        const std::string &iterated = "")
{
    if (!writeHead(w, "DREC", key, outcome, why))
        return;
    gm20b::PackPipeline pipeline = rec.pipeline;
    const std::string twinKey = gm20b::strideTwinKey(key);
    pipeline.strideTwin = rec.strideTwin ? gm20b::packKeyHash(twinKey) : 0;
    w.bytes(&pipeline, sizeof pipeline);
    w.text(rec.keyString);
    std::vector<uint32_t> pairs;
    pairs.reserve(registers.size() * 2);
    for (const auto &[m, v] : registers) {
        pairs.push_back(m);
        pairs.push_back(v);
    }
    w.list(pairs);
    auto program = [&](const gm20b::CapturedProgram &p) {
        w.list(p.image);
        w.list(p.data);
        w.list(p.cbufTypes);
        w.list(p.farUbos);
        w.u32(p.gprs | uint32_t(p.boundTextures) << 8 | gm20b::programFacts(p) << 16);
    };
    for (const gm20b::CapturedProgram &p : rec.programs)
        program(p);
    w.list(rec.samplers);
    w.u32(rec.split ? 1u : 0u);
    if (rec.split) {
        w.bytes(&rec.splitRecord, sizeof rec.splitRecord);
        program(rec.splitPrograms[0]);
        program(rec.splitPrograms[1]);
    }
    w.u32(uint32_t(rec.variants.size()));
    for (const gm20b::RuntimeRecord::Variant &v : rec.variants) {
        w.u32(v.assume);
        program(v.program);
        w.u32(gm20b::variantSetReads(rec, v.program));
    }
    w.text(iterated);
    w.list(rec.spill);
    w.u32(bound.pushMask);
    w.bytes(bound.push, sizeof bound.push);
    w.list(bound.spill[0]);
    w.list(bound.spill[1]);
    const bool tess = gm20b::tessellates(rec.pipeline);
    w.u32(tess ? 2u : 0u);
    for (uint32_t k = 0; tess && k < 2; k++)
        program(rec.tessPrograms[k]);
    for (const auto *list : {&rec.extras, &rec.restore}) {
        std::vector<uint32_t> words;
        for (const auto &[m, v] : *list)
            words.insert(words.end(), {m, v});
        w.list(words);
    }
    w.list(rec.far);
    if (!rec.strideTwin)
        return;
    gm20b::RuntimeRecord twin = *rec.strideTwin;
    twin.pipeline.flags |= rec.pipeline.flags & gm20b::PipelineFetchFreeForm;
    for (uint32_t c = 0; c < 8; c++)
        twin.pipeline.fetchRung[c] =
            (rec.pipeline.flags & gm20b::PipelineFetchFreeForm)
                ? rec.pipeline.fetchRung[c]
                : uint8_t(twin.pipeline.fetchRung[c] | (rec.pipeline.fetchRung[c] & ~gm20b::kFetchRungMask));
    writeRecord(w, twinKey, gm20b::CompileOutcome::Made, "", twin, twinRegisters, bound, {}, iterated);
}

inline void writeKernel(RecordWriter &w, const std::string &key, gm20b::CompileOutcome outcome, const std::string &why,
                        const gm20b::ComputeRecord &rec, const uint32_t facts[3])
{
    if (!writeHead(w, "DKRN", key, outcome, why))
        return;
    w.bytes(&rec.kernel, sizeof rec.kernel);
    w.list(rec.code);
    w.list(rec.data);
    w.bytes(rec.cbufType, rec.cbufs);
    w.list(rec.samplers);
    w.list(rec.spill);
    w.bytes(facts, 3 * sizeof *facts);
}

}  // namespace switch_seed
