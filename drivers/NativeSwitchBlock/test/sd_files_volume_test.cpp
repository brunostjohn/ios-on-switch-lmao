// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <map>
#include "../SwitchSdVolumeInternal.h"

namespace {
using Sector = std::array<uint8_t, 512>;
constexpr uint64_t capacity = UINT64_C(1) << 32;
constexpr uint32_t fatFirst = 65536, fatSize = 524288, rootFirst = 2048, rootSize = 16384;

void put(uint8_t *p, uint64_t n, unsigned bytes = 4)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

void mbrEntry(uint8_t *p, uint8_t type, uint32_t first, uint32_t length)
{
    p[4] = type;
    put(p + 8, first);
    put(p + 12, length);
}

class Card {
public:
    std::array<uint8_t, SWITCH_SD_REFERENCE_BYTES> page{};
    std::map<uint64_t, Sector> sectors;
    switch_gpt_workspace work{};
    switch_sd_files_slice slice{};
    uint32_t reads = 0;

    Card()
    {
        auto &mbr = sectors[0];
        mbrEntry(mbr.data() + 446, 0x0c, fatFirst, fatSize);
        mbrEntry(mbr.data() + 462, 0xee, 1, UINT32_MAX);
        put(mbr.data() + 510, 0xaa55, 2);
        constexpr std::array<uint8_t, 16> basic = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
                                                   0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
        auto &entries = sectors[2];
        memcpy(entries.data(), basic.data(), 16);
        entries[16] = 2;
        put(entries.data() + 32, fatFirst, 8);
        put(entries.data() + 40, fatFirst + fatSize - 1, 8);
        for (unsigned i = 0; i < 8; ++i)
            entries[56 + i * 2] = static_cast<uint8_t>("hos_data"[i]);
        entries[128] = 0xef;
        entries[144] = 3;
        put(entries.data() + 160, rootFirst, 8);
        put(entries.data() + 168, rootFirst + rootSize - 1, 8);
        syncTables();

        constexpr std::array<uint32_t, 10> header = {
            SWITCH_SD_REFERENCE_MAGIC, 2, 0x500, 512, 512, 0x700b0000, 0, 1, 1, 0};
        for (unsigned i = 0; i < header.size(); ++i)
            put(page.data() + 4 * i, header[i]);
        put(page.data() + 0x2c, 1);
        auto *r = page.data() + SWITCH_SD_CARD_OFFSET;
        constexpr std::array<uint32_t, 8> fixed = {SWITCH_SD_CARD_MAGIC, 1, 256, 1, 0x700b0000, 1, 512, 1};
        for (unsigned i = 0; i < fixed.size(); ++i)
            put(r + 4 * i, fixed[i]);
        put(r + 0x20, capacity, 8);
        put(r + 0x28, capacity * 512, 8);
        put(r + 0x30, 0xc0000000);
        put(r + 0x34, 1);
        put(r + 0x38, 0x80000000);
        put(r + 0x40, 1u << 30);
        put(r + 0x44, (9u << 16) | 0x3f);
        put(r + 0x48, 0xffff0000);
        put(r + 0x50, 0x1b534d31);
        put(r + 0x60, 0xf0100000, 8);
        put(r + 0x68, 0xf0100100, 8);
        put(r + 0x70, 0xf0100200, 8);
        constexpr std::array<uint8_t, 32> firmware = {0xb4, 0x92, 0xa4, 0x01, 0x75, 0x80, 0xca, 0x11, 0xc9, 0xcd, 0x49,
                                                      0x07, 0x5a, 0x89, 0x9a, 0x65, 0xf6, 0x20, 0x69, 0xd3, 0x75, 0x06,
                                                      0x15, 0x21, 0x7f, 0xc1, 0x03, 0x08, 0x95, 0x4c, 0x9f, 0xaa};
        memcpy(r + 0x80, firmware.data(), firmware.size());
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
        handoffMbr();
        auto *b = sectors[fatFirst].data();
        put(b + 11, 512, 2);
        b[13] = 4;
        put(b + 14, 32, 2);
        b[16] = 2;
        put(b + 32, fatSize);
        put(b + 36, 1024);
        put(b + 44, 2);
        put(b + 510, 0xaa55, 2);
    }

    void syncTables()
    {
        sectors[capacity - 33] = sectors[2];
        for (bool backup : {false, true}) {
            auto &header = sectors[backup ? capacity - 1 : 1];
            header.fill(0);
            auto *h = header.data();
            memcpy(h, "EFI PART", 8);
            put(h + 8, 0x10000);
            put(h + 12, 92);
            put(h + 24, backup ? capacity - 1 : 1, 8);
            put(h + 32, backup ? 1 : capacity - 1, 8);
            put(h + 40, 34, 8);
            put(h + 48, capacity - 34, 8);
            h[56] = 1;
            put(h + 72, backup ? capacity - 33 : 2, 8);
            put(h + 80, 2);
            put(h + 84, 128);
            put(h + 88, switch_sd_crc32(sectors[2].data(), 256));
            put(h + 16, switch_sd_crc32(h, 92));
        }
    }

    void handoffMbr()
    {
        auto &mbr = sectors[0];
        memcpy(page.data() + 512, mbr.data(), 512);
        uint32_t c = switch_sd_crc32(mbr.data(), 512);
        for (unsigned i = 0; i < 4; ++i)
            page[0x28 + i] = static_cast<uint8_t>(c >> (24 - 8 * i));
    }

    void exfat(uint8_t type, unsigned shift = 9)
    {
        sectors[0][450] = type;
        handoffMbr();
        auto &v = sectors[fatFirst];
        v.fill(0);
        auto *b = v.data();
        memcpy(b + 3, "EXFAT   ", 8);
        const uint32_t length = fatSize >> (shift - 9);
        put(b + 72, length, 8);
        put(b + 80, 24);
        put(b + 84, 1024);
        put(b + 88, 2048);
        put(b + 92, (length - 2048) / 4);
        put(b + 96, 2);
        put(b + 104, 0x100, 2);
        b[108] = static_cast<uint8_t>(shift);
        b[109] = 2;
        b[110] = 1;
        put(b + 510, 0xaa55, 2);
    }

    static int read(void *context, uint64_t lba, uint8_t out[512])
    {
        auto &card = *static_cast<Card *>(context);
        ++card.reads;
        auto it = card.sectors.find(lba);
        if (it == card.sectors.end())
            return -1;
        memcpy(out, it->second.data(), 512);
        return 0;
    }

    int select(uint64_t root = rootFirst)
    {
        return switch_sd_files_select(read, this, page.data(), root, rootSize, &work, &slice);
    }
};
}  // namespace

TEST(SdFilesVolume, BootCardIdentitySurvivesOfflineAndAbsentSharedPartitions)
{
    Card card;
    auto *record = card.page.data() + SWITCH_SD_CARD_OFFSET;
    put(record + 0x54, 0x32333435);
    put(record + 0x58, 0x36123456);
    put(record + 0x5c, 0x789abcff);
    put(record + 0xf0, switch_sd_crc32(record, 0xf0));
    const std::array<uint8_t, 16> expected = {0x1b, 0x53, 0x4d, 0x31, 0x32, 0x33, 0x34, 0x35,
                                              0x36, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x00};
    ASSERT_EQ(card.select(), SWITCH_GPT_OK);
    std::array<uint8_t, 16> cid{};
    card.sectors[fatFirst][510] = 0;
    ASSERT_EQ(card.select(), SWITCH_GPT_PROBE);
    ASSERT_TRUE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);

    memset(card.sectors[2].data(), 0, 128);
    card.syncTables();
    ASSERT_NE(card.select(), SWITCH_GPT_OK);
    cid.fill(0);
    ASSERT_TRUE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);

    card.page[SWITCH_SD_REFERENCE_COMMIT_OFFSET] = 0;
    EXPECT_FALSE(switch_sd_handoff_cid(card.page.data(), cid.data()));
    EXPECT_EQ(cid, expected);
}

TEST(SdFilesVolume, SelectsTheHekateWindowAndFirmwareIdentity)
{
    Card c;
    ASSERT_EQ(c.select(), SWITCH_GPT_OK);
    EXPECT_EQ(c.slice.first_sector, fatFirst);
    EXPECT_FALSE(c.slice.exfat);
    EXPECT_EQ(c.slice.sector_count, fatSize);
    EXPECT_EQ(c.slice.unique_guid[0], 2);
    EXPECT_EQ(c.slice.cid[0], 0x1b);
    EXPECT_EQ(c.slice.cid[1], 0x53);
    for (uint8_t mirror : {0x07, 0x0c}) {
        for (unsigned shift : {9u, 12u}) {
            c.exfat(mirror, shift);
            EXPECT_EQ(c.select(), SWITCH_GPT_OK);
            EXPECT_TRUE(c.slice.exfat);
        }
    }
}

TEST(SdFilesVolume, RefusesDamagedTablesRootMismatchAndWrongCard)
{
    Card c;
    c.slice.first_sector = 123;
    EXPECT_NE(c.select(rootFirst + 1), SWITCH_GPT_OK);
    c.sectors[capacity - 33][16]++;
    EXPECT_EQ(c.select(), SWITCH_GPT_MISMATCH);
    c.syncTables();
    c.sectors[0][100]++;
    EXPECT_EQ(c.select(), SWITCH_GPT_MISMATCH);
    c.page[0x2c] = 0;
    c.reads = 0;
    EXPECT_NE(c.select(), SWITCH_GPT_OK);
    EXPECT_EQ(c.reads, 0u);
    EXPECT_EQ(c.slice.first_sector, 123u);
}

TEST(SdFilesVolume, RequiresUniqueAuthorizedPartition)
{
    for (unsigned field : {0u, 56u, 57u, 73u}) {
        Card c;
        c.sectors[2][field] ^= 1;
        c.syncTables();
        EXPECT_NE(c.select(), SWITCH_GPT_OK);
    }
    for (unsigned bit : {1u, 60u, 62u, 63u}) {
        Card c;
        put(c.sectors[2].data() + 48, UINT64_C(1) << bit, 8);
        c.syncTables();
        EXPECT_NE(c.select(), SWITCH_GPT_OK);
    }
    Card c;
    put(c.sectors[2].data() + 160, fatFirst, 8);
    put(c.sectors[2].data() + 168, fatFirst + 22, 8);
    c.syncTables();
    EXPECT_EQ(c.select(), SWITCH_GPT_OVERLAP);
}

TEST(SdFilesVolume, RejectsMbrAliasesAndWrongVolumeType)
{
    for (uint32_t offset : {0u, 1u}) {
        Card c;
        mbrEntry(c.sectors[0].data() + 478, 0xe0, fatFirst + offset, fatSize);
        c.handoffMbr();
        EXPECT_EQ(c.select(), SWITCH_GPT_MBR);
    }
    Card c;
    c.sectors[0][450] = 0x0b;
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_OK);
    c.sectors[0][450] = 7;
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.exfat(0x0b);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.sectors[0][450] = 12;
    put(c.sectors[0].data() + 458, fatSize - 1);
    c.handoffMbr();
    EXPECT_EQ(c.select(), SWITCH_GPT_MBR);
}

TEST(SdFilesVolume, BoundsMetadataInLogicalSectors)
{
    Card c;
    put(c.sectors[fatFirst].data() + 32, fatSize + 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    put(c.sectors[fatFirst].data() + 32, fatSize);
    put(c.sectors[fatFirst].data() + 36, 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    for (unsigned offset : {48u, 84u, 88u, 92u, 96u, 108u, 109u, 110u, 511u}) {
        c.exfat(7);
        memset(c.sectors[fatFirst].data() + offset, 0xff, 1);
        if (offset == 84 || offset == 88)
            put(c.sectors[fatFirst].data() + offset, 0xffffff00);
        if (offset == 92 || offset == 96)
            put(c.sectors[fatFirst].data() + offset, UINT32_MAX);
        EXPECT_EQ(c.select(), SWITCH_GPT_PROBE) << offset;
    }
    c.exfat(7);
    put(c.sectors[fatFirst].data() + 92, 1);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
    c.exfat(7, 12);
    put(c.sectors[fatFirst].data() + 72, fatSize, 8);
    EXPECT_EQ(c.select(), SWITCH_GPT_PROBE);
}

TEST(SdFilesVolume, WriteProtectedCardNeverPublishesAWritableWindow)
{
    for (uint32_t bit : {0x1000u, 0x2000u}) {
        Card c;
        uint8_t *r = c.page.data() + SWITCH_SD_CARD_OFFSET;
        put(r + 0x4c, bit);
        put(r + 0xf0, switch_sd_crc32(r, 0xf0));
        EXPECT_NE(c.select(), SWITCH_GPT_OK);
        EXPECT_EQ(c.reads, 0u);
    }
}
