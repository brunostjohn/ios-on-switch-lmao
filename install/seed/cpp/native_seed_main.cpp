// SPDX-License-Identifier: GPL-3.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "install/seed/cpp/switch_seed_cpp.h"

namespace {

bool parseHexStack16(const char *s, uint64_t &out)
{
    if (!s || std::strlen(s) != 16)
        return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return false;
        v = (v << 4) | uint64_t(d);
    }
    out = v;
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--restamp")) {
        uint64_t stack = 0;
        if (argc < 5 || !parseHexStack16(argv[2], stack)) {
            std::fprintf(stderr, "usage: native_seed_portable --restamp stack output input...\n");
            return 2;
        }
        std::vector<const char *> inputs;
        for (int i = 4; i < argc; ++i)
            inputs.push_back(argv[i]);
        char err[512] = {};
        const switch_seed_status_t st =
            switch_seed_native_restamp(stack, argv[3], inputs.data(), inputs.size(), err, sizeof err);
        if (st != SWITCH_SEED_OK) {
            if (err[0])
                std::fprintf(stderr, "native-seed: %s\n", err);
            return st == SWITCH_SEED_ERR_INVALID_ARG ? 2 : 1;
        }
        return 0;
    }

    const char *kind = nullptr;
    const char *libsStore = nullptr;
    int first = 1;
    while (first < argc) {
        if (!std::strcmp(argv[first], "--kind") && first + 1 < argc) {
            kind = argv[first + 1];
            first += 2;
        } else if (!std::strcmp(argv[first], "--libs") && first + 1 < argc) {
            libsStore = argv[first + 1];
            first += 2;
        } else {
            break;
        }
    }
    if (!kind || argc - first < 4) {
        std::fprintf(stderr, "usage: native_seed_portable --kind <21H461|24B119> [--libs store] QuartzCore.metallib "
                             "stack output corpus... [--derived corpus...]\n");
        return 2;
    }
    char *end = nullptr;
    errno = 0;
    const uint64_t stack = std::strtoull(argv[first + 1], &end, 16);
    if (errno || !*argv[first + 1] || !end || *end) {
        std::fprintf(stderr, "native-seed: the stack key %s is not hexadecimal\n", argv[first + 1]);
        return 2;
    }

    std::vector<const char *> corpora;
    std::vector<const char *> derived;
    bool inDerived = false;
    for (int arg = first + 3; arg < argc; ++arg) {
        if (!std::strcmp(argv[arg], "--derived")) {
            inDerived = true;
            continue;
        }
        (inDerived ? derived : corpora).push_back(argv[arg]);
    }

    switch_seed_native_opts_t opts = {};
    opts.kind = kind;
    opts.libs_store = libsStore;
    opts.qc_metallib = argv[first];
    opts.stack_key = stack;
    opts.output_dir = argv[first + 2];
    opts.corpora = corpora.data();
    opts.corpus_count = corpora.size();
    opts.derived = derived.data();
    opts.derived_count = derived.size();

    char err[512] = {};
    const switch_seed_status_t st = switch_seed_native_bucket(&opts, err, sizeof err);
    if (st != SWITCH_SEED_OK) {
        if (err[0])
            std::fprintf(stderr, "native-seed: %s\n", err);
        return st == SWITCH_SEED_ERR_INVALID_ARG ? 2 : 1;
    }
    return 0;
}
