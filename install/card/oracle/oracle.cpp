// SPDX-License-Identifier: GPL-3.0-or-later
// The kernel's own SD rules over an installer test's card, for Rust: setup()'s root rule (SwitchSdRoot.h) as setup()
// runs it, and the shared volume's selection (SwitchSdVolume.cpp) given a boot loader reference page that the kernel
// tests' model card (sd_card_model.h) builds for this card's capacity and sector 0.
#include <stdint.h>
#include <string.h>

#include <vector>

#include "drivers/NativeSwitchBlock/SwitchSdRoot.h"
#include "drivers/NativeSwitchBlock/test/sd_card_model.h"

extern "C" {
typedef int (*switch_card_oracle_read)(void *, uint64_t, uint8_t[512]);

struct switch_card_oracle_root {
    uint64_t first, count;
    uint32_t found;
    uint8_t unique;
};

struct switch_card_oracle_files {
    uint64_t first, count;
    uint8_t exfat;
};

// 0 with *out filled, 1 when setup() refuses the header, 2 when its entry sectors are out of range, -1 on a read error.
int switch_card_oracle_root(switch_card_oracle_read read, void *context, struct switch_card_oracle_root *out)
{
    uint8_t header[512];
    if (read(context, 1, header))
        return -1;
    SwitchSdGptArray array;
    if (!switchSdGptArray(header, &array))
        return 1;
    const uint32_t sectors = switchSdGptArraySectors(array);
    if (!sectors)
        return 2;
    std::vector<uint8_t> entries(kSwitchSdGptArrayBytes);
    for (uint32_t s = 0; s < sectors; ++s) {
        if (read(context, array.lba + s, entries.data() + static_cast<size_t>(s) * 512))
            return -1;
    }
    const SwitchSdRoot root = switchSdFindRoot(entries.data(), array, switch_sd_test::apfsType.data(), "iOS");
    out->first = root.window.first;
    out->count = root.window.count;
    out->found = root.found;
    out->unique = root.unique() ? 1 : 0;
    return 0;
}

// switch_sd_files_select's status (SWITCH_GPT_*), with *out filled on SWITCH_GPT_OK. `capacity` must be a multiple
// of 1024 sectors, as an SDHC/SDXC card's CSD gives it.
int switch_card_oracle_files(switch_card_oracle_read read, void *context, uint64_t capacity, uint64_t root_first,
                             uint64_t root_sectors, struct switch_card_oracle_files *out)
{
    switch_sd_test::Card model;
    model.setCapacity(capacity);
    if (read(context, 0, model.sectors[0].data()))
        return SWITCH_GPT_IO;
    model.handoffMbr();
    switch_gpt_workspace work{};
    switch_sd_files_slice slice{};
    const int status =
        switch_sd_files_select(read, context, model.page.data(), root_first, root_sectors, &work, &slice);
    if (status == SWITCH_GPT_OK) {
        out->first = slice.first_sector;
        out->count = slice.sector_count;
        out->exfat = slice.exfat ? 1 : 0;
    }
    return status;
}

const char *switch_card_oracle_files_reason(int status)
{
    return switchSdFilesReason(status);
}

void switch_card_oracle_apfs_type(uint8_t out[16])
{
    memcpy(out, switch_sd_test::apfsType.data(), 16);
}

void switch_card_oracle_fat32_vbr(uint8_t out[512], uint32_t length, uint8_t cluster)
{
    switch_sd_test::Sector sector;
    switch_sd_test::writeFat32Vbr(sector, length, cluster);
    memcpy(out, sector.data(), 512);
}
}
