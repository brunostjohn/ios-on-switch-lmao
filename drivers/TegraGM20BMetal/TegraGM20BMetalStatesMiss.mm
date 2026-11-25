// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalStatesKey.h"
#include "lock.h"

#include <mutex>
#include <unordered_set>

void gm20bNoteMiss(const char *kind, uint64_t keyHash, const std::string &key, NSString *label)
{
    static gm20b::Mutex lock;
    static std::unordered_set<uint64_t> named;
    {
        std::lock_guard<gm20b::Mutex> l(lock);
        if (!named.insert(keyHash).second)
            return;
    }
    os_log_error(gm20bLog(),
                 "SwitchMetal: %{public}s: a %{public}s state the pack lacks %016llx (%{public}s): %{public}s",
                 getprogname(), kind, (unsigned long long)keyHash, label.UTF8String ?: "-", key.c_str());
}
