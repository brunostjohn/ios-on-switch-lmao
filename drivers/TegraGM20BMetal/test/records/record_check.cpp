// SPDX-License-Identifier: MIT
// A pack's records against its kind's goldens (golden_files.h). With --sample: each sampled record section by section, so
// a moved record names what moved. Without: every record's digest, a moved one by its key. Run as a test:
//   record_check <pack.bin> <records_<kind>.bin> <records_<kind>_sample.tsv> <kind> [--sample]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

namespace {

std::string slurp(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void regenerate(const char *kind)
{
    printf("a pack whose records moved on purpose: bazel run //drivers/TegraGM20BMetal/test/records:record_goldens -- "
           "<its pack.bin> %s\n",
           kind);
}

int sample(const std::vector<records::Record> &all, const std::string &text, const char *kind)
{
    std::map<records::RecordId, records::SectionHashes> golden;
    if (!records::parseSample(text, golden)) {
        printf("the sample file does not parse\n");
        return 1;
    }
    std::map<records::RecordId, const records::Record *> byId;
    for (const records::Record &r : all)
        byId[{r.key, r.kind}] = &r;
    int moved = 0;
    for (const auto &[id, sections] : golden) {
        const auto found = byId.find(id);
        if (found == byId.end()) {
            if (moved++ < 40)
                printf("%s kind %d: not in the pack\n", records::keyText(id.first).c_str(), int(id.second));
            continue;
        }
        const records::SectionHashes now = records::sectionHashes(*found->second);
        for (const auto &[name, hash] : sections) {
            const auto at = now.find(name);
            if (at != now.end() && at->second == hash)
                continue;
            if (moved++ < 40)
                printf("%s kind %d %s: section %s moved\n", records::keyText(id.first).c_str(), int(id.second),
                       found->second->label.substr(0, 80).c_str(), name.c_str());
        }
    }
    printf("%zu sampled records, %d moved sections\n", golden.size(), moved);
    if (moved)
        regenerate(kind);
    return moved ? 1 : 0;
}

int corpus(const std::vector<records::Record> &all, const std::string &bytes, const char *kind)
{
    std::vector<records::Row> golden;
    if (!records::decode({bytes.begin(), bytes.end()}, golden)) {
        printf("the digest file is not whole rows\n");
        return 1;
    }
    const auto now = records::rows(all);
    std::map<records::RecordId, records::Digest> want, got;
    for (const auto &r : golden)
        want[{r.key, r.kind}] = r.digest;
    for (const auto &r : now)
        got[{r.key, r.kind}] = r.digest;
    int moved = 0, missing = 0, extra = 0;
    for (const auto &[id, d] : want) {
        const auto at = got.find(id);
        const bool gone = at == got.end();
        if (gone ? ++missing : at->second != d ? ++moved : 0) {
            if (moved + missing <= 40)
                printf("%s kind %d: %s\n", records::keyText(id.first).c_str(), int(id.second),
                       gone ? "not in the pack" : "moved");
        }
    }
    for (const auto &[id, d] : got)
        if (!want.count(id) && ++extra <= 10)
            printf("%s kind %d: not in the goldens\n", records::keyText(id.first).c_str(), int(id.second));
    printf("%zu records: %d moved, %d missing, %d new\n", want.size(), moved, missing, extra);
    if (moved || missing || extra) {
        printf("rerun the dump on both packs for the moved records' sections (record_goldens writes them)\n");
        regenerate(kind);
    }
    return moved || missing || extra ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: record_check <pack.bin> <records.bin> <sample.tsv> <kind> [--sample]\n");
        return 2;
    }
    raw::Pack pack;
    std::string err;
    if (!pack.load(argv[1], 0, err)) {
        printf("%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const auto all = records::records(pack);
    return argc > 5 && std::string(argv[5]) == "--sample" ? sample(all, slurp(argv[3]), argv[4])
                                                          : corpus(all, slurp(argv[2]), argv[4]);
}
