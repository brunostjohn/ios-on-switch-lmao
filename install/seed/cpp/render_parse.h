// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "drivers/TegraGM20BMetal/cubic.h"
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/gm20b_cache.h"
#include "drivers/TegraGM20BMetal/pack/fetch_free.h"
#include "drivers/TegraGM20BMetal/written_textures.h"
#include "install/seed/cpp/offline_metal.h"

namespace switch_seed {

struct ProbeState {
    std::string key;
    std::string label;
    uint64_t fast = 0;
    RenderDescriptor d;
    std::shared_ptr<OfflineLibrary> library;
    std::string framework;
    unsigned vertexBuffer = 99;
    unsigned vertexStride = 0;
    gm20b::CubicBinds cubic;
    uint16_t xrSlots = 0;
    std::vector<gm20b::WrittenTexture> written;
    uint64_t tier = 0;
};

enum class FreeOutcome : uint8_t {
    None,
    Made,
    Fetches,
    Refused,
};

enum class NativeInputOutcome : uint8_t {
    Made,
    Refused,
    NoLibrary,
    NoFunction,
};

struct NativeStage {
    std::string name;
    std::string file;
    std::string library;
    std::string as;
    bool specialized = false;
    std::vector<std::string> constants;
};

struct NativeRenderInput {
    RenderDescriptor d;
    uint64_t options = 0;
    std::string file;
    std::string label;
    NativeStage vertex;
    NativeStage fragment;
    std::vector<std::pair<std::string, NativeStage>> linked;
};

std::vector<std::string> splitSpaces(const std::string &s);
std::vector<std::string> splitBy(const std::string &s, const std::string &by);
bool endsWith(const std::string &s, const std::string &suffix);
std::string libraryPathOfLine(const std::string &line);
std::string frameworkNameOfPath(const std::string &path);
std::vector<std::string> corpusLines(const std::string &text, const std::string &labelPrefix, bool includeOwn);
bool parseProbeState(LibraryStore &store, const std::shared_ptr<OfflineLibrary> &lib, const std::string &libRoot,
                     const std::string &line, ProbeState &s, const std::string &framework, std::string *why);
gm20b::ProbeTexturesCore probeTexturesOf(const std::string &line, bool ours);
FreeOutcome makeFetchFree(gm20b::DirectCore &direct, LibraryStore &store, const std::string &libRoot,
                          const ProbeState &state, const std::string &line, gm20b::ProbeTexturesCore textures,
                          probe::FetchFree &form, gm20b::RuntimeRecord &rec, std::map<uint32_t, uint32_t> *registers,
                          std::string &why, bool &barrier);

std::string unescapeToken(const std::string &token);
std::string nativeHostFile(const std::string &id, const std::string &qcPath, const std::string &storeRoot);
NativeStage parseNativeStage(const std::string &text, const std::string &qcPath, const std::string &storeRoot);
std::shared_ptr<OfflineFunction> loadNativeFunction(LibraryStore &store, const NativeStage &s,
                                                    NativeInputOutcome &outcome, std::string &why);
NativeInputOutcome parseNativeRenderInput(LibraryStore &store, const std::string &qcPath, const std::string &storeRoot,
                                          const std::string &line, NativeRenderInput &out, std::string &why);
bool nativeRenderCacheKey(const NativeRenderInput &input, uint64_t stack, gm20b::CacheKey &out);

}  // namespace switch_seed
