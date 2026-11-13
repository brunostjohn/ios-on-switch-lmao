// SPDX-License-Identifier: MIT
#include "raw_surface_note.h"

#include <gtest/gtest.h>
#include <cstring>

namespace {

IOSurfaceRef makeSurface(int width, int height)
{
    int bytes = 4;
    OSType format = 'BGRA';
    CFNumberRef w = CFNumberCreate(nullptr, kCFNumberIntType, &width);
    CFNumberRef h = CFNumberCreate(nullptr, kCFNumberIntType, &height);
    CFNumberRef b = CFNumberCreate(nullptr, kCFNumberIntType, &bytes);
    CFNumberRef f = CFNumberCreate(nullptr, kCFNumberIntType, &format);
    const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat,
                          kIOSurfaceName};
    const void *values[] = {w, h, b, f, CFSTR("CA layer")};
    CFDictionaryRef d = CFDictionaryCreate(nullptr, keys, values, 5, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(d);
    for (CFNumberRef n : {w, h, b, f})
        CFRelease(n);
    return s;
}

TEST(RawSurfaceNote, TakesScalarsAndCallersAndHoldsNoReference)
{
    IOSurfaceRef s = makeSurface(1280, 720);
    ASSERT_NE(s, nullptr);
    const raw::SurfaceNote note = raw::surfaceNote(s, __builtin_frame_address(0));
    EXPECT_EQ(note.surface, IOSurfaceGetID(s));
    EXPECT_EQ(note.width, 1280u);
    EXPECT_EQ(note.height, 720u);
    EXPECT_EQ(note.format, uint32_t('BGRA'));
    EXPECT_NE(note.site.pc[0], 0u);

    char line[512];
    raw::describeSurfaceNote(note, line, sizeof line);
    EXPECT_NE(strstr(line, "1280x720 'BGRA'"), nullptr) << line;
    EXPECT_NE(strstr(line, "use count 0 in use 0"), nullptr) << line;
    EXPECT_NE(strstr(line, "name CA layer"), nullptr) << line;

    CFRelease(s);
    raw::describeSurfaceNote(note, line, sizeof line);
    EXPECT_NE(strstr(line, "not found"), nullptr) << line;
}

}  // namespace
