// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/gm20b_cache.h"
#include "install/seed/cpp/json_min.h"

namespace switch_seed {

void setErrBuf(char *buf, size_t cap, const std::string &msg);
bool reportSeedError(const char *step, const std::string &path, int err = 0);
bool setFileMode(const std::string &path, unsigned mode);
bool writeFileAtomic(const std::string &path, const void *data, size_t size, unsigned mode);
std::string hex16(uint64_t value);
std::string airHex(const uint8_t *air);
bool parseHexBytes(const std::string &text, uint8_t *out, size_t bytes);
bool parseHexStack(const std::string &text, uint64_t &out);
void identifyManifestRow(JsonValue &row, const gm20b::CacheKey &key);
bool openSeedOutput(const std::string &output, uint64_t stack, gm20b::RecordCache &cache);
bool writeSeedManifest(const std::string &output, uint64_t stack, std::vector<JsonValue> rows);
bool writeRecordSlots(const gm20b::RuntimeRecord &record, uint64_t stack, const std::string &output,
                      std::vector<JsonValue> &slots, bool &hasSlots);

}  // namespace switch_seed
