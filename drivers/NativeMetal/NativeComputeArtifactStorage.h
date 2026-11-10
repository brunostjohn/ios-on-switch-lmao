// SPDX-License-Identifier: MIT
#pragma once
#include "NativeMetalArtifactCache.h"
#include "bridge/NativeMetalComputeArtifact.h"

class NativeComputeArtifactStorage {
public:
    std::string cache, seed;

    NmbComputeArtifact *load(NmbDevice *device, const NmbComputePipelineDescriptor &descriptor, bool reflection,
                             const native_artifact_cache::Key &key) const
    {
        for (const auto *directory : {&cache, &seed}) {
            native_artifact_cache::Mapping mapped;
            if (mapped.load(*directory, key))
                if (auto *artifact = nmb_compute_artifact_import(device, &descriptor, reflection, mapped.data(),
                                                                 mapped.size()))
                    return artifact;
        }
        return nullptr;
    }

    static bool save(const NmbComputeArtifact *artifact, const native_artifact_cache::Key &key,
                     const std::string &directory)
    {
        struct Write {
            const native_artifact_cache::Key &key;
            const std::string &directory;
        } context{key, directory};
        return nmb_compute_artifact_serialize(
            artifact,
            [](const void *bytes, size_t size, void *opaque) {
                const auto &out = *static_cast<const Write *>(opaque);
                return native_artifact_cache::store(out.directory, out.key, bytes, size);
            },
            &context);
    }
};
