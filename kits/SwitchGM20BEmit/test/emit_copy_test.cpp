// SPDX-License-Identifier: MIT
// switch_gm20b_copy_rect against the copy emitters the drivers have today, for the same copies: word for word against
// the raw path's emitCopy (raw_copy.h), method for method against the kernel's tgm_copy_rows and tgm_relayout_copy,
// whose launch takes two words where the immediate form takes one.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BCopyPush.h"
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_copy.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitCopy.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Methods;
using switch_gm20b_test::Words;

struct Side {
    uint64_t va;
    uint32_t pitch;
    bool tiled;
    uint32_t rowTexels, height, depth, layer, x, y;
    uint8_t blockHeight, blockDepth;
};

Side pitch(uint64_t va, uint32_t pitch)
{
    return {va, pitch, false, 0, 0, 0, 0, 0, 0, 0, 0};
}

Side tiled(uint64_t va, uint32_t rowTexels, uint32_t height, uint32_t depth, uint32_t layer, uint32_t x, uint32_t y,
           uint8_t blockHeight, uint8_t blockDepth)
{
    return {va, 0, true, rowTexels, height, depth, layer, x, y, blockHeight, blockDepth};
}

// raw's sideOf: a block-linear side's pitch is its row stride
raw::CopySurface rawSide(const Side &s, uint32_t bpp)
{
    raw::CopySurface c;
    c.va = s.va;
    c.pitch = s.tiled ? s.rowTexels * bpp : s.pitch;
    c.blockLinear = s.tiled;
    c.width = s.rowTexels;
    c.height = s.height;
    c.depth = s.depth;
    c.layer = s.layer;
    c.x = s.x;
    c.y = s.y;
    c.blockHeight = s.blockHeight;
    c.blockDepth = s.blockDepth;
    return c;
}

switch_gm20b_copy_surface kitSide(const Side &s, uint32_t bpp)
{
    switch_gm20b_copy_surface c = {};
    c.base_addr = s.va;
    c.bpp = bpp;
    c.tiled = s.tiled;
    c.is_3d = true;
    c.row_stride = s.tiled ? s.rowTexels * bpp : s.pitch;
    c.extent_el.height = s.height;
    c.extent_el.depth = s.depth;
    c.offset_el.x = s.tiled ? s.x : 0;
    c.offset_el.y = s.tiled ? s.y : 0;
    c.offset_el.z = s.layer;
    c.y_log2 = s.blockHeight;
    c.z_log2 = s.blockDepth;
    return c;
}

struct Case {
    const char *name;
    Side src, dst;
    uint32_t bpp, width, height;
    bool ordered;
};

const Case kCases[] = {
    {"pitch to pitch", pitch(0x1000000, 1024), pitch(0x2000040, 2048), 4, 256, 64, false},
    {"pitch to block-linear", pitch(0x1000000, 512), tiled(0x40000000, 256, 128, 1, 0, 8, 16, 4, 0), 4, 64, 32, true},
    {"block-linear to pitch, 8-byte texels", tiled(0x3400000000, 128, 64, 1, 0, 0, 8, 3, 0), pitch(0x5000, 1024), 8, 128,
     8, false},
    {"block-linear to block-linear, 3D", tiled(0x10000, 64, 64, 8, 3, 4, 4, 2, 1), tiled(0x20000, 64, 64, 8, 5, 0, 0, 2, 1),
     16, 32, 32, true},
    {"one-byte texels", pitch(0x7000, 4096), tiled(0x8000, 4096, 16, 1, 0, 0, 0, 1, 0), 1, 4096, 16, false},
    {"two-byte texels", tiled(0x9000, 640, 480, 1, 0, 32, 32, 4, 0), pitch(0xa000, 1280), 2, 320, 240, true},
};

TEST(EmitCopy, WordForWordTheRawPathsCopy)
{
    for (const Case &c : kCases) {
        std::vector<uint32_t> ours(SWITCH_GM20B_COPY_RECT_WORDS(1)), theirs(64);
        switch_gm20b_copy copy = {};
        copy.src = kitSide(c.src, c.bpp);
        copy.dst = kitSide(c.dst, c.bpp);
        copy.remap.comp_size = uint8_t(c.bpp < 4 ? c.bpp : 4);
        for (uint8_t i = 0; i < 4; i++)
            copy.remap.dst[i] = i;
        copy.extent_el.width = c.width;
        copy.extent_el.height = c.height;
        copy.extent_el.depth = copy.extent_el.array_len = 1;
        nv_push p;
        nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
        switch_gm20b_copy_rect(&p, &copy,
                               c.ordered ? NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED
                                         : NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED);
        ours.resize(nv_push_dw_count(&p));

        raw::Push r{theirs.data(), theirs.data() + theirs.size()};
        raw::emitCopy(r, rawSide(c.src, c.bpp), rawSide(c.dst, c.bpp), c.width, c.height, raw::copyTexelRemap(c.bpp), c.ordered);
        theirs.resize(size_t(r.at - theirs.data()));

        EXPECT_EQ(Words(ours), Words(theirs)) << c.name;
    }
}

TEST(EmitCopy, MethodForMethodTheKernelsRowCopy)
{
    std::vector<uint32_t> theirs(TGM_COPY_DWORDS), ours(SWITCH_GM20B_COPY_RECT_WORDS(1));
    theirs.resize(tgm_copy_rows(theirs.data(), 0x12340000, 4096, 0x56780000, 8192, 3840, 1080));

    switch_gm20b_copy copy = {};
    copy.src.base_addr = 0x12340000;
    copy.src.row_stride = 4096;
    copy.dst.base_addr = 0x56780000;
    copy.dst.row_stride = 8192;
    copy.src.bpp = copy.dst.bpp = 1;
    copy.extent_el.width = 3840;
    copy.extent_el.height = 1080;
    copy.extent_el.depth = copy.extent_el.array_len = 1;
    nv_push p;
    nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
    switch_gm20b_copy_rect(&p, &copy, NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
    ours.resize(nv_push_dw_count(&p));

    EXPECT_EQ(Methods(ours), Methods(theirs));
    EXPECT_EQ(ours.size() + 1, theirs.size()) << "the kernel's LAUNCH_DMA takes a header and a word";
}

TEST(EmitCopy, MethodForMethodTheKernelsRelayout)
{
    tgm_layout l = {};
    ASSERT_TRUE(tgm_layout_tiled(1280, 720, 4, &l));
    for (int toTiled = 0; toTiled < 2; toTiled++) {
        const uint64_t tiled = 0x80000000, rows = 0x90000000;
        const uint32_t pitch = 1280 * 4;
        std::vector<uint32_t> theirs(TGM_RELAYOUT_DWORDS), ours(SWITCH_GM20B_COPY_RECT_WORDS(1));
        theirs.resize(toTiled ? tgm_relayout_copy(theirs.data(), &l, rows, 0, tiled, 1, pitch)
                              : tgm_relayout_copy(theirs.data(), &l, tiled, 1, rows, 0, pitch));

        switch_gm20b_copy_surface block = {};
        block.base_addr = tiled;
        block.bpp = 4;
        block.tiled = true;
        block.row_stride = l.row_bytes;
        block.extent_el.height = l.height;
        block.extent_el.depth = 1;
        block.y_log2 = l.y_log2;
        switch_gm20b_copy_surface linear = {};
        linear.base_addr = rows;
        linear.bpp = 4;
        linear.row_stride = pitch;
        switch_gm20b_copy copy = {};
        copy.src = toTiled ? linear : block;
        copy.dst = toTiled ? block : linear;
        copy.remap.comp_size = 4;
        for (uint8_t i = 0; i < 4; i++)
            copy.remap.dst[i] = i;
        copy.extent_el.width = l.width;
        copy.extent_el.height = l.height;
        copy.extent_el.depth = copy.extent_el.array_len = 1;
        nv_push p;
        nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
        switch_gm20b_copy_rect(&p, &copy, NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
        ours.resize(nv_push_dw_count(&p));

        EXPECT_EQ(Methods(ours), Methods(theirs)) << (toTiled ? "to tiled" : "to rows");
    }
}

}  // namespace
