// SPDX-License-Identifier: MIT
// record_digest.h over packs this test writes: a record digests by its content wherever the pack builder laid it out,
// a byte of its program moves its digest and names the section, and the sample does not depend on the records' bytes.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

namespace {

using namespace raw;

struct Built {
    std::vector<PackProgram> programs;
    std::vector<uint8_t> heap;
    std::vector<PackPipeline> pipelines;
    std::string strings = std::string(1, '\0');
};

// A pack of one state per key, each with two programs of `code` bytes; `gap` bytes of padding before the heap's first slot
// and the programs in reverse order, so two packs of the same records differ only in layout
std::string writePack(const char *name, const std::vector<uint64_t> &keys, uint32_t gap, uint8_t fsByte = 0x11)
{
    Built b;
    b.heap.assign(gap, 0xEE);
    for (size_t i = 0; i < keys.size(); i++) {
        for (int stage = 1; stage >= 0; stage--) {
            PackProgram p = {};
            p.heapOffset = uint32_t(b.heap.size());
            p.program = p.heapOffset + 0x30;
            p.bytes = 0x100;
            p.gprs = 8;
            std::vector<uint8_t> slot(p.bytes, uint8_t(stage ? fsByte : 0x22));
            slot[0] = uint8_t(keys[i]);
            b.heap.insert(b.heap.end(), slot.begin(), slot.end());
            b.programs.push_back(p);
        }
        PackPipeline pp = {};
        pp.keyHash = keys[i];
        pp.keyString = uint32_t(b.strings.size());
        b.strings += "state " + std::to_string(keys[i]) + std::string(1, '\0');
        pp.fs = uint32_t(b.programs.size() - 2);
        pp.vs = uint32_t(b.programs.size() - 1);
        b.pipelines.push_back(pp);
    }
    PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = kPackVersion;
    h.programs = uint32_t(b.programs.size());
    h.pipelines = uint32_t(b.pipelines.size());
    std::vector<uint8_t> file(sizeof h);
    const auto append = [&](PackSectionId id, const void *p, size_t n) {
        h.section[id] = {file.size(), n};
        file.insert(file.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
    };
    append(PackPrograms, b.programs.data(), b.programs.size() * sizeof(PackProgram));
    append(PackPipelines, b.pipelines.data(), b.pipelines.size() * sizeof(PackPipeline));
    append(PackStrings, b.strings.data(), b.strings.size());
    append(PackHeap, b.heap.data(), b.heap.size());
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/" + name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), file.size());
    return path;
}

std::vector<records::Record> load(const std::string &path)
{
    Pack pack;
    std::string err;
    EXPECT_TRUE(pack.load(path.c_str(), 0, err)) << err;
    return records::records(pack);
}

const records::Record &state(const std::vector<records::Record> &all, uint64_t key)
{
    for (const records::Record &r : all)
        if (r.key == key && r.kind == records::Kind::Default)
            return r;
    ADD_FAILURE() << "no state " << key;
    return all.front();
}

TEST(RecordDigest, ARecordDigestsTheSameWhereverItsProgramsAreLaidOut)
{
    const auto a = records::rows(load(writePack("a.bin", {5, 9}, 0)));
    const auto b = records::rows(load(writePack("b.bin", {9, 5}, 0x400)));
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) {
        EXPECT_EQ(a[i].key, b[i].key);
        EXPECT_EQ(a[i].digest, b[i].digest) << records::keyText(a[i].key);
    }
}

TEST(RecordDigest, AProgramByteMovesItsRecordAndNamesTheSection)
{
    const auto before = load(writePack("c.bin", {5}, 0));
    const auto after = load(writePack("d.bin", {5}, 0, 0x12));
    const records::Record &a = state(before, 5), &b = state(after, 5);
    EXPECT_NE(records::digest(a.sections), records::digest(b.sections));
    const auto was = records::sectionHashes(a), now = records::sectionHashes(b);
    EXPECT_NE(was.at("fs"), now.at("fs"));
    EXPECT_EQ(was.at("vs"), now.at("vs"));
    EXPECT_EQ(was.at("blob"), now.at("blob"));
}

TEST(RecordDigest, TheSampleIsPickedByKeyNotByContent)
{
    std::vector<uint64_t> keys;
    for (uint64_t k = 1; k <= 400; k++)
        keys.push_back(k * 0x9E3779B97F4A7C15ull);
    const auto a = records::pickSample(load(writePack("e.bin", keys, 0)), 40);
    const auto b = records::pickSample(load(writePack("f.bin", keys, 0, 0x13)), 40);
    EXPECT_EQ(a, b);
    EXPECT_GE(a.size(), 12u);
}

TEST(RecordDigest, RowsRoundTripThroughTheirFile)
{
    const auto rows = records::rows(load(writePack("g.bin", {3, 1, 2}, 0)));
    std::vector<records::Row> back;
    ASSERT_TRUE(records::decode(records::encode(rows), back));
    ASSERT_EQ(back.size(), rows.size());
    for (size_t i = 0; i < rows.size(); i++) {
        EXPECT_EQ(back[i].key, rows[i].key);
        EXPECT_EQ(back[i].kind, rows[i].kind);
        EXPECT_EQ(back[i].digest, rows[i].digest);
    }
}

}  // namespace
