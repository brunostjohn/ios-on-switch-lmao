// SPDX-License-Identifier: MIT
// Rules resolve.twod and resolve.exact: where an 8-bit unorm MSAA resolve is the 2D engine's 2:1 box of the sample grid
// (raw_resolve_twod.cpp) instead of the pack's resolve pass.
//
// The engine stores the samples' mean by the ROP's unorm rule (x1-units UNITS.md: cut to n+4 fractional bits, nearest,
// ties down), where Metal's resolve is (sum + n/2) / n, ties up; no 902d method sets the rounding (cl902d.h). Read
// directly (resolve.twod) a half-code mean lands one code under Metal's. Read through the complement (resolve.exact:
// every sample c made 255 - c in place, the box read under ROP3 NOTSRCCOPY) a tie lands on the other side: 255 -
// down((255n - s) / n) = up(s / n) for every sum s, so one rounding of the mean is Metal's rule exactly. Two rounded
// 2:1 steps are not (pair sums 1 and 0 give 1 where Metal gives 0): 4x waits on bench resolvetie2's reading of the
// engine, 2x has one step only.
#pragma once
#include <cstdint>

namespace raw {

// What can read a resolve texture's texels as data
struct ResolveTarget {
    bool sharedStorage = false;  // its storage, its parent's or its heap's, is CPU-visible
    bool imported = false;       // over an IOSurface or no-copy pages: the CPU or another process maps them
    bool overBuffer = false;     // over a buffer's bytes
    bool storage = false;        // ShaderWrite usage: a kernel or pass reads and writes it as data
    bool readOut = false;  // a blit copied out of it (into a buffer or a texture), in this command buffer or before
};

constexpr bool resolveTargetReadable(const ResolveTarget &t)
{
    return t.sharedStorage || t.imported || t.overBuffer || t.storage || t.readOut;
}

struct ResolveAsk {
    uint16_t format = 0;             // MTLPixelFormat of the resolve texture
    uint8_t samples = 0;             // the attachment's
    bool readable = false;           // resolveTargetReadable
    bool wholeTarget = false;        // the pass's render area is the resolve texture's whole level 0, slice 0
    bool shadowTarget = false;       // a pitch target the render hardware does not take (Texture::shadowTarget)
    bool compressedSamples = false;  // the multisampled image the pass rendered has comptags
};

// resolve.twod: within one code of Metal (gpu.x1.twod_resolve on the chip, T-v2: none beyond), so taken only where a
// one-code error cannot be seen as data, as fetch.blend is
constexpr bool twodResolveAcceptable(const ResolveAsk &a)
{
    const bool unorm8 = a.format == 70 || a.format == 80;  // RGBA8Unorm, BGRA8Unorm
    return unorm8 && (a.samples == 2 || a.samples == 4) && !a.readable && a.wholeTarget && !a.shadowTarget;
}

// 4x read as one rounding of the four samples' mean: resolvetie2's rt-*-4x-prod cells at 0 down-rounding misses and
// its 4x sandwich cells at 0 against Metal flip it
constexpr bool twodResolveSingleRounding4x()
{
    return false;
}

// The complement writes the multisampled image in place: a compressed one is written by the 2D engine through a
// multisample kind no open driver writes with it, unproven, so those keep resolve.twod
constexpr bool twodResolveExact(const ResolveAsk &a)
{
    return twodResolveAcceptable(a) && !a.compressedSamples && (a.samples == 2 || twodResolveSingleRounding4x());
}

enum ResolveRoute : uint8_t {
    ResolveByTwod,
    ResolveByComplement,
    ResolveByPass
};

constexpr uint32_t resolveSampleSlot(uint32_t samples)
{
    return samples == 2 ? 0 : samples == 4 ? 1 : 2;
}

}  // namespace raw
