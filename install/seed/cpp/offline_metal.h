// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/gm20b_cache.h"
#include "drivers/TegraGM20BMetal/metallib.h"
#include "drivers/TegraGM20BMetal/pipeline_fields.h"
#include "drivers/TegraGM20BMetal/pipeline_key.h"
#include "drivers/TegraGM20BMetal/plugin/MetallibEntries.h"
#include "drivers/TegraGM20BMetal/plugin/MetallibSlice.h"
#include "drivers/TegraGM20BMetal/translate/NativeMetalTranslate.h"

namespace switch_seed {

class MappedBytes {
public:
    MappedBytes() = default;
    MappedBytes(const MappedBytes &) = delete;
    MappedBytes &operator=(const MappedBytes &) = delete;
    ~MappedBytes();

    static std::shared_ptr<MappedBytes> openFile(const std::string &path, std::string &err);
    static std::shared_ptr<MappedBytes> copyBytes(const uint8_t *data, size_t length);

    const uint8_t *data() const
    {
        return data_;
    }

    size_t size() const
    {
        return size_;
    }

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    std::vector<uint8_t> owned_;
};

struct ConstantValue {
    uint64_t type = 0;
    uint64_t index = 0;
    std::string name;
    bool isNamed = false;
    std::array<uint8_t, 64> bytes = {};
    uint64_t length = 0;
};

struct ConstantSet {
    std::vector<ConstantValue> values;

    bool empty() const
    {
        return values.empty();
    }

    std::string text() const;
    bool toNmb(std::vector<NmbConstant> &out, std::string &why) const;
};

ConstantSet parseConstants(const std::vector<std::string> &items);
ConstantSet parseConstantsLine(const std::string &text);

class OfflineLibrary {
public:
    OfflineLibrary() = default;
    OfflineLibrary(const OfflineLibrary &) = delete;
    OfflineLibrary &operator=(const OfflineLibrary &) = delete;
    ~OfflineLibrary();

    static std::shared_ptr<OfflineLibrary> create(std::shared_ptr<MappedBytes> storage,
                                                  const switch_metal::AirSliceRule &rule, uint64_t scope,
                                                  std::string &err);

    NmbOfflineLibrary *handle() const
    {
        return handle_;
    }

    const uint8_t *sliceBytes() const
    {
        return sliceBytes_;
    }

    size_t sliceLength() const
    {
        return sliceLength_;
    }

    const std::set<std::string> &functionNames() const;
    bool airHash(const std::string &entry, gm20b::AirHash &out) const;

private:
    NmbOfflineLibrary *handle_ = nullptr;
    std::shared_ptr<MappedBytes> storage_;
    const uint8_t *sliceBytes_ = nullptr;
    size_t sliceLength_ = 0;
    mutable bool namesLoaded_ = false;
    mutable std::set<std::string> names_;
    mutable bool hashesLoaded_ = false;
    mutable bool hashesOk_ = false;
    mutable std::map<std::string, gm20b::AirHash> hashes_;
};

class OfflineFunction {
public:
    OfflineFunction(NmbOfflineFunction *handle, std::shared_ptr<OfflineLibrary> library, std::string name,
                    std::string libraryName);
    OfflineFunction(const OfflineFunction &) = delete;
    OfflineFunction &operator=(const OfflineFunction &) = delete;
    ~OfflineFunction();

    NmbOfflineFunction *handle() const
    {
        return handle_;
    }

    const std::shared_ptr<OfflineLibrary> &library() const
    {
        return library_;
    }

    const std::string &name() const
    {
        return name_;
    }

    const std::string &libraryFunctionName() const
    {
        return libraryName_;
    }

    uint64_t functionType() const
    {
        return functionType_;
    }

    switch_metal::FunctionTess tess() const;
    uint64_t patchType() const;
    bool airHash(gm20b::AirHash &out) const;

private:
    NmbOfflineFunction *handle_ = nullptr;
    std::shared_ptr<OfflineLibrary> library_;
    std::string name_;
    std::string libraryName_;
    uint64_t functionType_ = 0;
};

struct RenderDescriptor {
    std::string label;
    std::shared_ptr<OfflineFunction> vertex;
    std::shared_ptr<OfflineFunction> fragment;
    std::vector<std::shared_ptr<OfflineFunction>> vertexLinked;
    std::vector<std::shared_ptr<OfflineFunction>> vertexPrivateLinked;
    std::vector<std::shared_ptr<OfflineFunction>> fragmentLinked;
    std::vector<std::shared_ptr<OfflineFunction>> fragmentPrivateLinked;
    uint64_t samples = 1;
    uint64_t topology = 0;
    uint64_t depthFormat = 0;
    uint64_t stencilFormat = 0;
    bool rasterization = true;
    uint64_t amplification = 1;
    uint64_t textureWriteRounding = 0;
    bool alphaToCoverage = false;
    gm20b::TessellationFields tessellation = {0, 0, 0, 0, 0, 16, 0};
    gm20b::ColorFields color[8];
    bool hasVertexDescriptor = false;
    NmbVertexAttribute vertexAttributes[31] = {};
    NmbVertexLayout vertexLayouts[31];

    RenderDescriptor();

    gm20b::PipelineFields fields() const;
    std::vector<uint32_t> activeVertexAttributes() const;
    bool toNmb(NmbOfflineRenderPipelineDescriptor &p, std::vector<NmbOfflineFunction *> &linked) const;
    bool renderCacheKey(const std::string &key, bool own, uint64_t stack, gm20b::CacheKey &out) const;
};

struct ComputeDescriptor {
    std::shared_ptr<OfflineFunction> function;
    uint64_t maxThreads = 0;
    bool multipleOfWidth = false;
    bool indirect = false;
    uint64_t maxCallStackDepth = 1;
    bool addingBinaryFunctions = false;
    uint64_t mutability[31] = {};
    bool hasStageInput = false;
    NmbVertexAttribute stageAttributes[31] = {};
    NmbVertexLayout stageLayouts[31];
    uint64_t stageIndexType = 0;
    uint64_t stageIndexBuffer = 0;

    ComputeDescriptor();

    bool toNmb(NmbOfflineComputePipelineDescriptor &p) const;
};

class LibraryStore {
public:
    explicit LibraryStore(const switch_metal::AirSliceRule &rule, uint64_t sharedScope = 0)
        : rule_(rule), sharedScope_(sharedScope)
    {}

    std::shared_ptr<OfflineLibrary> loadFile(const std::string &path, std::string &err);
    std::shared_ptr<OfflineLibrary> loadMemory(const uint8_t *bytes, size_t length, std::string &err);

    std::shared_ptr<OfflineFunction> makeFunction(const std::shared_ptr<OfflineLibrary> &lib, const std::string &name,
                                                  const std::string &specializedName, const ConstantSet &constants,
                                                  bool specialized, std::string &err);

private:
    switch_metal::AirSliceRule rule_;
    uint64_t sharedScope_ = 0;
    std::map<std::string, std::shared_ptr<OfflineLibrary>> cache_;
};

gm20b::DirectTranslatorCore offlineTranslator();
bool kernelCompiles(const ComputeDescriptor &d, std::string &why);
bool kernelFacts(const ComputeDescriptor &d, uint32_t facts[3], std::string &why);
bool renderReflection(const RenderDescriptor &d, std::string &blob, std::string &why);
bool computeReflection(const ComputeDescriptor &d, std::string &blob, std::string &why);
bool readWholeFile(const std::string &path, std::string &out, std::string &err);
bool isSafeSha256Hex(std::string_view text);
bool resolveContainedPath(const std::string &root, std::string_view guestPath, std::string &out);

}  // namespace switch_seed
