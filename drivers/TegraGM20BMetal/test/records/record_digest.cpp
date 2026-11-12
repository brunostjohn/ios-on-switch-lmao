// SPDX-License-Identifier: MIT
// See record_digest.h.
#include "drivers/TegraGM20BMetal/test/records/record_digest.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>

namespace records {
namespace {

using namespace raw;

template <class T> void put(std::vector<uint8_t> &out, const T &value)
{
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}

void put(std::vector<uint8_t> &out, const void *p, size_t n)
{
    const auto *b = static_cast<const uint8_t *>(p);
    out.insert(out.end(), b, b + n);
}

void put(std::vector<uint8_t> &out, const std::string &s)
{
    put(out, s.data(), s.size() + 1);
}

// The program's record with its heap placement left out, then its slot's bytes (header, code, shader data)
void program(std::vector<uint8_t> &out, const Pack &pack, uint32_t index)
{
    if (uint64_t(index + 1) * sizeof(PackProgram) > pack.bytes(PackPrograms))
        return put(out, uint32_t(0xFFFFFFFF));
    PackProgram p = pack.section<PackProgram>(PackPrograms)[index];
    const uint32_t at = p.heapOffset;
    p.program -= p.heapOffset;
    p.heapOffset = 0;
    put(out, p);
    if (uint64_t(at) + p.bytes <= pack.bytes(PackHeap))
        put(out, pack.section<uint8_t>(PackHeap) + at, p.bytes);
}

void clearDispatchWords(uint32_t (&q)[64])
{
    q[8] = q[12] = q[13] = 0;
    for (int i = 0; i < 8; i++) {
        q[29 + 2 * i] = 0;
        q[30 + 2 * i] &= ~0xFFu;
    }
}

uint32_t traits(const Pack &pack, const PackPipeline &pp)
{
    uint32_t t = 0, rung = 0;
    if (pp.flags & PipelineFetches)
        t |= TraitFetches;
    for (uint32_t e = 0; e < pp.entries && e < kPackEntries; e++)
        if (pp.entry[e].kind == EntryTexture3d)
            t |= TraitTexture3d;
    if (pack.split(pp.fs))
        t |= TraitSplit;
    if (pp.iterated)
        t |= TraitIterated;
    if (pp.variantCount[0] || pp.variantCount[1])
        t |= TraitVariants;
    if (pp.storageMask)
        t |= TraitStorage;
    for (uint8_t r : pp.fetchRung)
        rung = std::max<uint32_t>(rung, r & kFetchRungMask);
    return t | rung << TraitRungShift;
}

Record pipeline(const Pack &pack, const PackPipeline &pp)
{
    Record r;
    r.label = pack.string(pp.keyString);
    r.key = pp.keyHash;
    r.kind = Kind(uint8_t(recordForm(pp)));
    std::vector<uint8_t> key;
    put(key, r.label);
    if (pp.flags & PipelineVariantRecord) {
        const std::string name = pack.string(pp.variantName);
        put(key, name);
        const uint64_t mixed[3] = {pp.keyHash, Pack::fnv(name.data(), name.size()), uint64_t(r.kind)};
        r.kind = Kind::Variant;
        r.key = Pack::fnv(mixed, sizeof mixed);
        r.label += name;
    }
    r.traits = traits(pack, pp);
    r.sections.push_back({"key", key});

    PackPipeline flat = pp;
    flat.keyString = flat.vs = flat.fs = flat.blob = flat.variants = flat.variantName = 0;
    flat.iterated = pp.iterated ? 1 : 0;
    std::vector<uint8_t> record;
    put(record, flat);
    r.sections.push_back({"record", record});

    for (const auto &[name, index] : {std::pair{"vs", pp.vs}, std::pair{"fs", pp.fs}}) {
        std::vector<uint8_t> bytes;
        program(bytes, pack, index);
        r.sections.push_back({name, bytes});
    }

    std::vector<uint8_t> blob;
    if (uint64_t(pp.blob + pp.blobDwords) * 4 <= pack.bytes(PackBlob))
        put(blob, pack.section<uint32_t>(PackBlob) + pp.blob, size_t(pp.blobDwords) * 4);
    r.sections.push_back({"blob", blob});

    std::vector<uint8_t> variants;
    for (uint32_t stage = 0; stage < 2; stage++) {
        uint32_t count = 0;
        const PackVariant *v = pack.variants(pp, stage, count);
        put(variants, count);
        for (uint32_t i = 0; i < count; i++) {
            put(variants, v[i].assume);
            program(variants, pack, v[i].program);
        }
    }
    r.sections.push_back({"variants", variants});

    std::vector<uint8_t> iterated;
    if (const PackIteratedBlend *it = pack.iterated(pp))
        put(iterated, *it);
    r.sections.push_back({"iterated", iterated});

    std::vector<uint8_t> split;
    if (const PackSplit *s = pack.split(pp.fs)) {
        PackSplit flatSplit = *s;
        flatSplit.whole = flatSplit.fs = flatSplit.prologue = 0;
        clearDispatchWords(flatSplit.qmd);
        put(split, flatSplit);
        program(split, pack, s->fs);
        program(split, pack, s->prologue);
    }
    r.sections.push_back({"split", split});
    return r;
}

Record kernel(const Pack &pack, const PackKernel &k)
{
    Record r;
    r.label = pack.string(k.keyString);
    r.key = k.keyHash;
    r.kind = Kind::Kernel;
    std::vector<uint8_t> key;
    put(key, r.label);
    r.sections.push_back({"key", key});
    PackKernel flat = k;
    flat.keyString = flat.program = 0;
    clearDispatchWords(flat.qmd);
    std::vector<uint8_t> record;
    put(record, flat);
    r.sections.push_back({"record", record});
    std::vector<uint8_t> code;
    program(code, pack, k.program);
    r.sections.push_back({"program", code});
    return r;
}

Record whole(const Pack &pack, uint64_t key, const char *name, PackSectionId id)
{
    Record r;
    r.key = key;
    r.kind = Kind::Pack;
    r.label = name;
    std::vector<uint8_t> bytes;
    if (id == PackInit) {
        const PackInitMethod *m = pack.section<PackInitMethod>(PackInit);
        for (uint64_t i = 0; i < pack.bytes(PackInit) / sizeof(PackInitMethod); i++) {
            PackInitMethod flat = m[i];
            if (flat.reloc != RelocNone)
                flat.data = 0;
            put(bytes, flat);
        }
    } else {
        put(bytes, pack.section<uint8_t>(id), pack.bytes(id));
    }
    r.sections.push_back({name, bytes});
    return r;
}

}  // namespace

std::vector<Record> records(const Pack &pack)
{
    std::vector<Record> out;
    const PackPipeline *p = pack.section<PackPipeline>(PackPipelines);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++)
        out.push_back(pipeline(pack, p[i]));
    const PackKernel *k = pack.section<PackKernel>(PackKernels);
    for (uint64_t i = 0; i < pack.bytes(PackKernels) / sizeof(PackKernel); i++)
        out.push_back(kernel(pack, k[i]));
    out.push_back(whole(pack, 0, "baseline", PackBaseline));
    out.push_back(whole(pack, 1, "init", PackInit));
    out.push_back(whole(pack, 2, "samplers", PackSamplers));
    out.push_back(whole(pack, 3, "archives", PackArchives));
    std::stable_sort(out.begin(), out.end(), [](const Record &a, const Record &b) {
        return a.key != b.key ? a.key < b.key : a.kind < b.kind;
    });
    return out;
}

std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &bytes)
{
    std::array<uint8_t, 32> out;
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), out.data());
    return out;
}

Digest digest(const std::vector<Section> &sections)
{
    std::vector<uint8_t> all;
    for (const Section &s : sections) {
        put(all, s.name);
        put(all, uint64_t(s.bytes.size()));
        all.insert(all.end(), s.bytes.begin(), s.bytes.end());
    }
    const auto full = sha256(all);
    Digest d;
    std::copy_n(full.begin(), d.size(), d.begin());
    return d;
}

std::string hex(const uint8_t *bytes, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

}  // namespace records
