// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "install/seed/cpp/switch_seed_cpp.h"

int main(int argc, char **argv)
{
    switch_seed_directpack_opts_t opts = {};
    opts.own_lib = std::getenv("PROBE_OWN_LIB");
    opts.lib_root = std::getenv("PROBE_LIB_ROOT");
    opts.data_libs = std::getenv("PROBE_DATA_LIBS");
    opts.label_prefix = std::getenv("PROBE_LABEL");

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if (!std::strcmp(argv[arg], "--kind") && arg + 1 < argc) {
            opts.kind = argv[arg + 1];
            arg += 2;
        } else if (!std::strcmp(argv[arg], "--own-lib") && arg + 1 < argc) {
            opts.own_lib = argv[arg + 1];
            arg += 2;
        } else if (!std::strcmp(argv[arg], "--lib-root") && arg + 1 < argc) {
            opts.lib_root = argv[arg + 1];
            arg += 2;
        } else if (!std::strcmp(argv[arg], "--data-libs") && arg + 1 < argc) {
            opts.data_libs = argv[arg + 1];
            arg += 2;
        } else if (!std::strcmp(argv[arg], "--label-prefix") && arg + 1 < argc) {
            opts.label_prefix = argv[arg + 1];
            arg += 2;
        } else {
            break;
        }
    }
    if (!opts.kind || argc - arg != 3) {
        std::fprintf(stderr, "usage: directpack_portable --kind <21H461|24B119> <default.metallib> <corpus.log> "
                             "<records.bin>\n");
        return 2;
    }
    opts.qc_metallib = argv[arg];
    opts.corpus_path = argv[arg + 1];
    opts.output_path = argv[arg + 2];

    char err[512] = {};
    const switch_seed_status_t st = switch_seed_directpack(&opts, err, sizeof err);
    if (st != SWITCH_SEED_OK) {
        if (err[0])
            std::fprintf(stderr, "%s\n", err);
        return st == SWITCH_SEED_ERR_INVALID_ARG ? 2 : 1;
    }
    return 0;
}
