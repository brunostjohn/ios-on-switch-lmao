/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Translated System files a deploy replaces without the install (kernel/switch/vfs/pretranslated.c). The tree on Data
 * is placed once, by the install, and nothing changes it afterwards. A boot package whose translation of a file differs
 * names that file in /chosen/switch-pretranslated-updates (tools/package/adt_edit.py), and //deploy:sd puts its bytes at
 * /private/var/Switch/pretranslated-updates/<System path>. At capture the kernel points the tree's entry for that file
 * at the update and its CDHash. Each update stands only if the boot package's CDHash list pins its CDHash and the file
 * is signed as it (its SHA-256 CodeDirectory hashes to it), so a deploy of another build's boot package never serves this
 * one's files and wrong bytes leave the tree's file in place.
 *
 * Little-endian: "SWUP", u32 version (1), u32 entry count, u32 string bytes; then per entry, in path order, u32 the
 * offset of its path in the string table and 20 bytes the CDHash; then the string table, the entries' NUL-terminated
 * absolute System paths, sorted by bytes and unique, every byte belonging to one path.
 */
#pragma once

#include "switch_pretranslated.h"

#define SWITCH_PT_UPDATES_MAGIC   0x50555753u
#define SWITCH_PT_UPDATES_VERSION 1u
#define SWITCH_PT_UPDATES_HEADER  16u
#define SWITCH_PT_UPDATES_RECORD  24u
#define SWITCH_PT_MAX_UPDATES     64u

struct switch_pt_updates {
    const uint8_t *records;
    const char *strings;
    uint32_t count;
    uint32_t string_bytes;
};

struct switch_pt_update {
    const char *path;
    uint32_t length;
    const uint8_t *cdhash;
};

static inline struct switch_pt_update switch_pt_update_at(const struct switch_pt_updates *updates, uint32_t i)
{
    const uint8_t *record = updates->records + (size_t)SWITCH_PT_UPDATES_RECORD * i;
    struct switch_pt_update update;
    update.path = updates->strings + switch_pt_u32(record);
    update.length = (uint32_t)__builtin_strlen(update.path);
    update.cdhash = record + 4;
    return update;
}

/* No property is no updates (count 0); a property that is there must be whole and in order. */
static inline enum switch_pt_error switch_pt_updates_parse(const uint8_t *raw, size_t size,
                                                           struct switch_pt_updates *out)
{
    out->records = NULL;
    out->strings = NULL;
    out->count = 0;
    out->string_bytes = 0;
    if (size == 0) {
        return SWITCH_PT_OK;
    }
    if (size < SWITCH_PT_UPDATES_HEADER) {
        return SWITCH_PT_SHORT;
    }
    const uint32_t count = switch_pt_u32(raw + 8), strings = switch_pt_u32(raw + 12);
    if (switch_pt_u32(raw) != SWITCH_PT_UPDATES_MAGIC || switch_pt_u32(raw + 4) != SWITCH_PT_UPDATES_VERSION ||
        count == 0 || count > SWITCH_PT_MAX_UPDATES) {
        return SWITCH_PT_HEADER_BAD;
    }
    if ((uint64_t)SWITCH_PT_UPDATES_HEADER + (uint64_t)SWITCH_PT_UPDATES_RECORD * count + strings != size) {
        return SWITCH_PT_SIZE_BAD;
    }
    const uint8_t *records = raw + SWITCH_PT_UPDATES_HEADER;
    const char *table = (const char *)(records + (size_t)SWITCH_PT_UPDATES_RECORD * count);
    uint32_t end = 0;
    const char *previous = NULL;
    uint32_t previous_length = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (switch_pt_u32(records + (size_t)SWITCH_PT_UPDATES_RECORD * i) != end) {
            return SWITCH_PT_RECORD_BAD;
        }
        const char *path = table + end;
        uint32_t length = 0;
        while (end + length < strings && path[length] != '\0') {
            ++length;
        }
        if (end + length >= strings || !switch_pt_path_ok(path, length)) {
            return SWITCH_PT_PATH_BAD;
        }
        if (previous && switch_pt_bytes_cmp(previous, previous_length, path, length) >= 0) {
            return SWITCH_PT_ORDER_BAD;
        }
        previous = path;
        previous_length = length;
        end += length + 1;
    }
    if (end != strings) {
        return SWITCH_PT_SIZE_BAD;
    }
    out->records = records;
    out->strings = table;
    out->count = count;
    out->string_bytes = strings;
    return SWITCH_PT_OK;
}

/* The update's path below the update tree: the System path without its leading slash. */
static inline const char *switch_pt_update_relative(const struct switch_pt_update *update)
{
    return update->path + 1;
}

/*
 * Points every record of the System file `system_id` (several for its hard links) at the Data file `data_id` with this
 * CDHash, in the index's own bytes (`raw`, which `index` was parsed from), before the tables are built from it. The
 * Data id the records named before is returned in *replaced, for the caller to keep protected. Returns the records
 * changed: 0 when the tree has no such file.
 */
static inline uint32_t switch_pt_index_point(uint8_t *raw, const struct switch_pt_index *index, uint64_t system_id,
                                             uint64_t data_id, const uint8_t *cdhash, uint64_t *replaced)
{
    uint32_t changed = 0;
    uint8_t *records = raw + SWITCH_PT_HEADER;
    for (uint32_t i = 0; i < index->count; ++i) {
        uint8_t *record = records + (size_t)SWITCH_PT_RECORD * i;
        if (switch_pt_u64(record) != system_id) {
            continue;
        }
        if (!changed) {
            *replaced = switch_pt_u64(record + 8);
        }
        __builtin_memcpy(record + 8, &data_id, sizeof(data_id));
        __builtin_memcpy(record + 24, cdhash, SWITCH_PT_CDHASH);
        ++changed;
    }
    return changed;
}

/* A thin 64-bit Mach-O's code signature, from its header and load commands (`head`, the file's first `n` bytes): the
 * signature's file offset and size, or false. Translation leaves each file one arm64 slice. */
#define SWITCH_PT_MH_MAGIC_64         0xfeedfacfu
#define SWITCH_PT_LC_CODE_SIGNATURE   0x1du
#define SWITCH_PT_MAX_SIGNATURE_BYTES (40u << 20)

static inline bool switch_pt_macho_signature(const uint8_t *head, size_t n, uint32_t *offset, uint32_t *size)
{
    if (n < 32 || switch_pt_u32(head) != SWITCH_PT_MH_MAGIC_64) {
        return false;
    }
    const uint32_t commands = switch_pt_u32(head + 16), bytes = switch_pt_u32(head + 20);
    if ((uint64_t)32 + bytes > n) {
        return false;
    }
    uint64_t at = 32;
    for (uint32_t i = 0; i < commands; ++i) {
        if (at + 8 > 32 + (uint64_t)bytes) {
            return false;
        }
        const uint32_t cmd = switch_pt_u32(head + at), cmdsize = switch_pt_u32(head + at + 4);
        if (cmdsize < 8 || at + cmdsize > 32 + (uint64_t)bytes) {
            return false;
        }
        if (cmd == SWITCH_PT_LC_CODE_SIGNATURE && cmdsize >= 16) {
            *offset = switch_pt_u32(head + at + 8);
            *size = switch_pt_u32(head + at + 12);
            return *size >= 12 && *size <= SWITCH_PT_MAX_SIGNATURE_BYTES;
        }
        at += cmdsize;
    }
    return false;
}

static inline uint32_t switch_pt_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#define SWITCH_PT_CS_SUPERBLOB      0xfade0cc0u
#define SWITCH_PT_CS_CODEDIRECTORY  0xfade0c02u
#define SWITCH_PT_CS_SLOT_CD        0u
#define SWITCH_PT_CS_SLOT_ALTERNATE 0x1000u
#define SWITCH_PT_CS_HASH_SHA256    2u

/* The SHA-256 CodeDirectory in an embedded signature (`blob`, `n` bytes; big-endian, as codesign writes it), the one a
 * trust cache entry of hash type 2 names: its offset and length, or false. */
static inline bool switch_pt_code_directory(const uint8_t *blob, size_t n, uint32_t *offset, uint32_t *length)
{
    if (n < 12 || switch_pt_be32(blob) != SWITCH_PT_CS_SUPERBLOB || switch_pt_be32(blob + 4) > n) {
        return false;
    }
    const uint32_t count = switch_pt_be32(blob + 8);
    if ((uint64_t)12 + (uint64_t)8 * count > n) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t type = switch_pt_be32(blob + 12 + 8 * i), at = switch_pt_be32(blob + 16 + 8 * i);
        if (type != SWITCH_PT_CS_SLOT_CD &&
            (type < SWITCH_PT_CS_SLOT_ALTERNATE || type >= SWITCH_PT_CS_SLOT_ALTERNATE + 5)) {
            continue;
        }
        if ((uint64_t)at + 40 > n || switch_pt_be32(blob + at) != SWITCH_PT_CS_CODEDIRECTORY) {
            return false;
        }
        const uint32_t cd = switch_pt_be32(blob + at + 4);
        if (cd < 40 || (uint64_t)at + cd > n) {
            return false;
        }
        if (blob[at + 37] == SWITCH_PT_CS_HASH_SHA256) {
            *offset = at;
            *length = cd;
            return true;
        }
    }
    return false;
}
