// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/compute_key.h"
#include "drivers/TegraGM20BMetal/direct_core.h"
#include "drivers/TegraGM20BMetal/metallib.h"
#include "install/seed/cpp/offline_metal.h"

namespace switch_seed {

struct ComputeShape {
    uint32_t threads[3] = {};
    uint32_t edge[3] = {};
    bool regionMode = false;
    bool factsOnly = false;
    std::string form;
    uint64_t options = 0;
    uint32_t maxThreads = 0;
    bool multipleOfWidth = false;
    std::string function;
    std::string library;
    std::string constants;
    bool specialized = false;
    std::string threadgroupMemory;
    std::vector<gm20b::BoundTexture> textures;
    std::vector<gm20b::BoundSampler> samplers;
    std::string stageIn;
};

template <typename Each> void eachRegionShape(const ComputeShape &original, Each each)
{
    for (uint8_t mask = 0; mask < 8; ++mask) {
        ComputeShape shape = original;
        bool present = true;
        for (uint8_t axis = 0; axis < 3; ++axis) {
            if (mask & (1u << axis)) {
                present &= original.edge[axis] != 0;
                shape.threads[axis] = original.edge[axis];
            }
        }
        if (present)
            each(shape, mask);
    }
}

bool parseComputeLine(const std::string &line, ComputeShape &s, std::string &why);
void stateComputeFormatMaps(ComputeShape &s, uint8_t (*formatMap)(uint64_t));
std::string computeShapeKey(const ComputeShape &s, const gm20b::AirHash &air);
ComputeDescriptor buildComputeDescriptor(const ComputeShape &s, const std::shared_ptr<OfflineFunction> &fn);
gm20b::BoundSamplerDesc samplerDescOf(const gm20b::BoundSampler &b);

std::shared_ptr<OfflineFunction> loadDirectpackKernelFunction(LibraryStore &store, const std::string &libRoot,
                                                              const std::string &dataLibs, const ComputeShape &s,
                                                              gm20b::AirHash &air);
void buildDirectpackDispatch(const ComputeShape &s, const uint32_t threads[3],
                             std::unique_ptr<gm20b::KernelDispatchCore> &out);
bool buildNativeSeedDispatch(const ComputeShape &s, std::unique_ptr<gm20b::KernelDispatchCore> &out, std::string &why);

}  // namespace switch_seed
