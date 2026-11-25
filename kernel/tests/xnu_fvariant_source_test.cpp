// xnu-12377's shared region pager fills the pinned dyld function-variant slots (switch/fvariant.h) after both slide
// paths, including pages with no rebases, and every slide info carries the table; xnu-10063 is untouched.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;

namespace {

size_t Find(const std::string &text, const std::string &what, size_t from = 0)
{
    const size_t at = text.find(what, from);
    EXPECT_NE(at, std::string::npos) << what;
    return at;
}

TEST(XnuFvariantSource, PagerFillsAfterEachSlidePath)
{
    const std::string pager = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region_pager.c");
    const std::string request = Between(pager, "\nshared_region_pager_data_request(", "\n}\n");

    const size_t part = Find(request, "shared_region_pager_slide_part(pager, dst_vaddr");
    const size_t partOk = Find(request, "shared_region_pager_slid++;", part);
    const size_t partFill = Find(request, "switch_fvariant_fill_page(", partOk);
    EXPECT_LT(partFill, Find(request, "goto part_done;", part));

    const size_t page = Find(request, "kr = vm_shared_region_slide_page(pager->srp_slide_info");
    const size_t pageOk = Find(request, "shared_region_pager_slid++;", page);
    const size_t pageFill = Find(request, "switch_fvariant_fill_page(pager->srp_slide_info", pageOk);
    EXPECT_NE(request.find("offset_in_sliding_range + slide_start_address, slide_info_page_size", pageFill),
              std::string::npos);
    // The fill runs in the pager, after vm_shared_region_slide_page() returned: its v5 NO_REBASE early return cannot
    // skip it.
    EXPECT_EQ(Between(pager, "\nshared_region_pager_slide_part(", "\n}\n").find("switch_fvariant"), std::string::npos);
}

TEST(XnuFvariantSource, EverySlideInfoCarriesItsCachesSlots)
{
    const std::string region = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region.c");
    const std::string slide = Between(region, "\nvm_shared_region_slide_mapping(\n", "\n}\n");
    const size_t alloc = Find(slide, "si = kalloc_type(struct vm_shared_region_slide_info");
    const size_t set = Find(slide, "si->si_switch_fvariant = switch_fvariant_for_region(sr);", alloc);
    EXPECT_LT(set, Find(slide, "#if __has_feature(ptrauth_calls)", alloc));

    const std::string header = SourceText("../xnu-12377.121.6/osfmk/vm/vm_shared_region_xnu.h");
    EXPECT_NE(Between(header, "typedef struct vm_shared_region_slide_info {", "} *vm_shared_region_slide_info_t;")
                  .find("const struct switch_fvariant_cache *si_switch_fvariant;"),
              std::string::npos);
}

TEST(XnuFvariantSource, OnlyTheIos27TreeHasIt)
{
    for (const char *file :
         {"../xnu-10063.141.1/osfmk/vm/vm_shared_region_pager.c", "../xnu-10063.141.1/osfmk/vm/vm_shared_region.c"}) {
        EXPECT_EQ(SourceText(file).find("switch_fvariant"), std::string::npos) << file;
    }
    const std::string objects = SourceText("../switch/objects.bzl");
    EXPECT_NE(objects.find(R"(("osfmk/vm/vm_shared_region.c", "vm/fvariant.c", [], ["12377.121.6"]))"),
              std::string::npos);
}

}  // namespace
