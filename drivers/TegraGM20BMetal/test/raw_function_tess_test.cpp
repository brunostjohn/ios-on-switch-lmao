// SPDX-License-Identifier: MIT
// raw/runtime/raw_function_tess.h: a function's patch from its metallib entry's TESS tag, the byte Xcode's metal writes (its
// three forms below, from xcrun -sdk iphoneos metal and metallib), on a made-up universal metallib
#include "../raw/runtime/raw_function_tess.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

void tag(std::vector<uint8_t> &out, const char *name, const void *data, uint16_t length)
{
    out.insert(out.end(), name, name + 4);
    out.push_back(uint8_t(length));
    out.push_back(uint8_t(length >> 8));
    const auto *b = static_cast<const uint8_t *>(data);
    out.insert(out.end(), b, b + length);
}

template <class T> void put(std::vector<uint8_t> &out, size_t at, T v)
{
    memcpy(&out[at], &v, sizeof v);
}

void bigEndian(std::vector<uint8_t> &out, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

struct Entry {
    std::string name;
    int tess;  // -1: no TESS tag
};

std::vector<uint8_t> universalMetallib(const std::vector<Entry> &functions)
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4);
    put(list, 0, uint32_t(functions.size()));
    for (const Entry &f : functions) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", f.name.c_str(), uint16_t(f.name.size() + 1));
        const uint8_t type = 0;
        tag(e, "TYPE", &type, 1);
        if (f.tess >= 0) {
            const uint8_t tess = uint8_t(f.tess);
            tag(e, "TESS", &tess, 1);
        }
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    std::vector<uint8_t> file(0x100, 0);
    bigEndian(file, 0, 0xCAFEBABE);
    bigEndian(file, 4, 2);
    bigEndian(file, 8, 0x01000013);
    bigEndian(file, 16, 0x100);
    bigEndian(file, 20, 16);
    bigEndian(file, 28, 0x01000017);
    bigEndian(file, 36, 0x110);
    bigEndian(file, 40, uint32_t(slice.size()));
    file.resize(0x110, 0xEE);
    file.insert(file.end(), slice.begin(), slice.end());
    return file;
}

TEST(RawFunctionTess, DecodesTheBytesXcodeWrites)
{
    const raw::FunctionTess triangle = raw::decodeTess(0x0d), quad = raw::decodeTess(0x42), any = raw::decodeTess(0xfe);
    EXPECT_EQ(triangle.patchType, 1);
    EXPECT_EQ(triangle.controlPoints, 3);
    EXPECT_EQ(quad.patchType, 2);
    EXPECT_EQ(quad.controlPoints, 16);
    EXPECT_EQ(any.patchType, 2);
    EXPECT_EQ(any.controlPoints, -1);
}

TEST(RawFunctionTess, ReadsEachFunctionsOwnTag)
{
    const std::vector<uint8_t> file =
        universalMetallib({{"quad_vertex", -1}, {"tri_post", 0x0d}, {"quad_post", 0x42}, {"solid_fragment", -1}});
    raw::FunctionTess t;
    ASSERT_TRUE(raw::metallibFunctionTess(file.data(), file.size(), "tri_post", t));
    EXPECT_EQ(t.patchType, 1);
    EXPECT_EQ(t.controlPoints, 3);
    ASSERT_TRUE(raw::metallibFunctionTess(file.data(), file.size(), "quad_post", t));
    EXPECT_EQ(t.patchType, 2);
    EXPECT_EQ(t.controlPoints, 16);
    ASSERT_TRUE(raw::metallibFunctionTess(file.data(), file.size(), "quad_vertex", t));
    EXPECT_EQ(t.patchType, 0);
    EXPECT_EQ(t.controlPoints, -1);
    EXPECT_FALSE(raw::metallibFunctionTess(file.data(), file.size(), "missing", t));
}

TEST(RawFunctionTess, ACutListIsNoAnswer)
{
    const std::vector<uint8_t> file = universalMetallib({{"a", -1}, {"tri_post", 0x0d}});
    const std::vector<uint8_t> cut(file.begin(), file.end() - 6);
    raw::FunctionTess t;
    t.patchType = 7;
    EXPECT_FALSE(raw::metallibFunctionTess(cut.data(), cut.size(), "tri_post", t));
    EXPECT_EQ(t.patchType, 7);
}

}  // namespace
