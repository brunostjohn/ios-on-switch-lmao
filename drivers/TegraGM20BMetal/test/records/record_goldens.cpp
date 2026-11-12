// SPDX-License-Identifier: MIT
// Writes an IPSW kind's record goldens from a pack (golden_files.h):
//   bazel run //drivers/TegraGM20BMetal/test/records:record_goldens -- <pack.bin> <kind> [<out dir>]
// <out dir> defaults to the checkout's drivers/TegraGM20BMetal/test/records/testdata.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: record_goldens <pack.bin> <kind> [<out dir>]\n");
        return 2;
    }
    const char *workspace = getenv("BUILD_WORKSPACE_DIRECTORY");
    const std::string dir = argc > 3 ? argv[3]
                            : workspace ? std::string(workspace) + "/drivers/TegraGM20BMetal/test/records/testdata"
                                        : ".";
    raw::Pack pack;
    std::string err;
    if (!pack.load(argv[1], 0, err)) {
        fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const auto all = records::records(pack);
    const auto rows = records::encode(records::rows(all));
    const std::string base = dir + "/records_" + argv[2];
    std::ofstream(base + ".bin", std::ios::binary).write(reinterpret_cast<const char *>(rows.data()), rows.size());
    const auto sample = records::pickSample(all, 2000);
    std::ofstream(base + "_sample.tsv") << records::sampleText(all, sample);
    printf("%zu records, %zu sampled -> %s.bin, %s_sample.tsv\n", all.size(), sample.size(), base.c_str(),
           base.c_str());
    return 0;
}
