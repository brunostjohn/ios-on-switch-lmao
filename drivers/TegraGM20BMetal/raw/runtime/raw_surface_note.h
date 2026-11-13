// SPDX-License-Identifier: MIT
// Debug builds, the render server: a texture it will draw into over an IOSurface of half a screen or more, with what
// Core Animation's pools decide reuse by (use count, purgeable state, name) and the caller, so a new surface every frame
// of an animation names its maker (boot 21: a new 1280x720 one on every app-close frame). The render thread takes the
// surface's id, size, format, seed and its caller frames; the kernel calls and dladdr run on a utility queue, which reads
// the surface by its id and holds no reference to it in between.
#pragma once
#include "SwitchFrameTimeline.h"

#if SWITCH_FRAME_TIMELINE
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <cstdio>

namespace raw {

struct SurfaceNote {
    uint32_t surface = 0, width = 0, height = 0, format = 0, seed = 0;
    switch_frame::Site site;
};

inline SurfaceNote surfaceNote(IOSurfaceRef s, const void *frame)
{
    return {IOSurfaceGetID(s),          uint32_t(IOSurfaceGetWidth(s)), uint32_t(IOSurfaceGetHeight(s)),
            IOSurfaceGetPixelFormat(s), IOSurfaceGetSeed(s),            switch_frame::callers(frame)};
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
inline IOSurfaceRef lookUpSurface(uint32_t id)
{
    return IOSurfaceLookup(id);
}
#pragma clang diagnostic pop

// The surface's line as it is now: "not found" once no process holds it
inline void describeSurfaceNote(const SurfaceNote &note, char *out, size_t size)
{
    char state[128] = "not found";
    if (IOSurfaceRef s = lookUpSurface(note.surface)) {
        uint32_t purgeable = 0;
        IOSurfaceSetPurgeable(s, kIOSurfacePurgeableKeepCurrent, &purgeable);
        char name[64] = "-";
        if (CFTypeRef value = IOSurfaceCopyValue(s, kIOSurfaceName)) {
            if (CFGetTypeID(value) == CFStringGetTypeID())
                CFStringGetCString(static_cast<CFStringRef>(value), name, sizeof name, kCFStringEncodingUTF8);
            CFRelease(value);
        }
        snprintf(state, sizeof state, "use count %d in use %d purgeable %u name %s", IOSurfaceGetUseCount(s),
                 IOSurfaceIsInUse(s), purgeable, name);
        CFRelease(s);
    }
    char site[switch_frame::kSiteFrames * 48];
    switch_frame::describe(note.site, site, sizeof site);
    const uint32_t f = note.format;
    snprintf(out, size, "raw: compositor target over IOSurface %u %ux%u '%c%c%c%c' seed %u %s at %s", note.surface,
             note.width, note.height, char(f >> 24), char(f >> 16), char(f >> 8), char(f), note.seed, state, site);
}

inline void logSurfaceNote(void *context)
{
    const SurfaceNote *note = static_cast<const SurfaceNote *>(context);
    char line[512];
    describeSurfaceNote(*note, line, sizeof line);
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "frame");
    os_log(log, "%{public}s", line);
    delete note;
}

inline void postSurfaceNote(IOSurfaceRef s, const void *frame)
{
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), new SurfaceNote(surfaceNote(s, frame)),
                     logSurfaceNote);
}

}  // namespace raw
#endif
