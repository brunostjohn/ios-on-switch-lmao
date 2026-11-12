// SPDX-License-Identifier: MIT
// What the system shader compiler compiles with: the plugin's own m2v frontend and NVK, opened as the runtime opens them (a
// device of this process's own, no shader cache on disk), and the installed pack's rules, so a record made here is the one
// the runtime would make in process (RawComplete.mm make, kernel).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "compiler_request.h"

namespace switch_compiler {

class Engine {
public:
    Engine();
    ~Engine();

    // The installed pack read: what the store is named by
    bool open(uint64_t stack, std::string &why);
    // The m2v device and NVK opened: what compiles
    bool openDevice(std::string &why);
    // The installed pack's rules (raw_record.h PackRules), as the store is named by them
    raw::Digest rules() const;
    // `p`'s record or refusal as the store keeps it (raw_cache.h's entry bytes); TegraGM20BCompilerABI.h's result
    int32_t compile(const Prepared &p, std::string &entry, std::string &why);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace switch_compiler
