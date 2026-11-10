// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_encode_profile.h"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks;

void check(bool value)
{
    if (!value)
        std::abort();
    ++checks;
}

void spin(uint64_t ticks)
{
    const uint64_t until = mach_absolute_time() + ticks;
    while (mach_absolute_time() < until) {
    }
}

void outsideAnEncode()
{
    raw::tEncodeProfile = nullptr;
    RAW_ENCODE_SPAN(Kernel);
    check(raw::tEncodeProfile == nullptr);
}

void nested()
{
    raw::EncodeProfile profile;
    raw::tEncodeProfile = &profile;
    {
        RAW_ENCODE_SPAN(Draw);
        {
            RAW_ENCODE_SPAN(Prepare);
            RAW_ENCODE_BEGIN(geometry, Geometry);
            spin(1000);
            RAW_ENCODE_END(geometry);
            spin(1000);
        }
        RAW_ENCODE_SPAN(Prepare);
        spin(10);
    }
    raw::tEncodeProfile = nullptr;
    const auto ticks = [&](raw::EncodePart p) {
        return profile.ticks[size_t(p)];
    };
    const auto calls = [&](raw::EncodePart p) {
        return profile.calls[size_t(p)];
    };
    check(calls(raw::EncodePart::Draw) == 1 && calls(raw::EncodePart::Prepare) == 2);
    check(calls(raw::EncodePart::Geometry) == 1);
    check(ticks(raw::EncodePart::Geometry) >= 1000);
    check(ticks(raw::EncodePart::Prepare) >= ticks(raw::EncodePart::Geometry) + 1000);
    check(ticks(raw::EncodePart::Draw) >= ticks(raw::EncodePart::Prepare));
    check(profile.longestPrepare >= 2000 && profile.longestPrepare <= ticks(raw::EncodePart::Prepare));
    check(calls(raw::EncodePart::Kernel) == 0);

    raw::EncodeTotals totals;
    totals.add(profile, 5);
    raw::EncodeProfile shorter;
    shorter.calls[size_t(raw::EncodePart::Kernel)] = 3;
    shorter.ticks[size_t(raw::EncodePart::Kernel)] = 11;
    shorter.longestPrepare = 1;
    totals.add(shorter, 1);
    check(totals.encodes.load() == 2 && totals.wallTicks.load() == 6);
    check(totals.calls[size_t(raw::EncodePart::Prepare)].load() == 2);
    check(totals.calls[size_t(raw::EncodePart::Kernel)].load() == 3);
    check(totals.ticks[size_t(raw::EncodePart::Kernel)].load() == 11);
    check(totals.longestPrepare.load() == profile.longestPrepare);
}
}  // namespace

int main()
{
    outsideAnEncode();
    nested();
    printf("encode profile: %u checks passed\n", checks);
}
