// SPDX-License-Identifier: MIT
// A raw pack's records as content: every record (a state's form, a variant record, a compute kernel, and the pack's own
// baseline, context init, samplers and archives) as named sections of canonical bytes, and one digest of them. Canonical
// bytes leave out where the pack builder laid things out (program heap offsets and indices, string offsets) and take in
// what those pointed at, and a kernel's QMD loses its dispatch words as the runtime clears them (clearDispatchWords), so
// two packs digest the same exactly where each record carries the same bytes.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_pack.h"

namespace records {

enum class Kind : uint8_t {
    Default = 0,
    Barrier = 1,
    FetchFree = 2,
    Variant = 3,  // a state's PipelineVariantRecord: its key is fnv(keyHash, fnv(variant name), its form)
    Kernel = 4,
    Pack = 5  // the pack's own sections: key 0 baseline, 1 context init, 2 samplers, 3 archives
};

struct Section {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct Record {
    uint64_t key = 0;
    Kind kind = Kind::Default;
    std::string label;  // the state key's text, for messages
    uint32_t traits = 0;  // Trait bits, which pick a sample that covers every kind of record
    std::vector<Section> sections;
};

enum Trait : uint32_t {
    TraitFetches = 1,
    TraitTexture3d = 2,
    TraitSplit = 4,
    TraitIterated = 8,
    TraitVariants = 16,
    TraitStorage = 32,
    TraitRungShift = 8  // bits 8-11: the highest fetch rung of its colour attachments
};

using Digest = std::array<uint8_t, 16>;

// The pack's records in (key, kind) order
std::vector<Record> records(const raw::Pack &pack);

Digest digest(const std::vector<Section> &sections);
std::array<uint8_t, 32> sha256(const std::vector<uint8_t> &bytes);

std::string hex(const uint8_t *bytes, size_t n);

}  // namespace records
