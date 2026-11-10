// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <functional>
#include <vector>

namespace raw {

struct RetirementWrites {
    struct Target {
        void *buffer = nullptr;
        void *texture = nullptr;
    };
    std::vector<void *> buffers, textures;

    template <class Visit, class Resolve> static RetirementWrites capture(Visit visit, Resolve resolve)
    {
        RetirementWrites result;
        visit([&](void *object) {
            const Target target = resolve(object);
            if (target.buffer)
                result.buffers.push_back(target.buffer);
            if (target.texture)
                result.textures.push_back(target.texture);
        });
        for (auto *objects : {&result.buffers, &result.textures}) {
            std::sort(objects->begin(), objects->end(), std::less<void *>{});
            objects->erase(std::unique(objects->begin(), objects->end()), objects->end());
        }
        return result;
    }
};

} // namespace raw
