// SPDX-License-Identifier: MIT
// A blit encoder's commands of the tape onto the copy engine (and the 2D engine's mip chains): see encodeRaw.
#import "RawMetalInternal.h"

// The copy engine's copies of those rows into or out of plane z: one copy of every row where they lie a pitch apart, else one
// per row
static void copyRows(raw::BlitEncoder *be, uint64_t bufferVa, const BufferRows &r, uint64_t offset,
                     uint64_t bytesPerRow, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                     uint32_t h, bool intoTexture, uint8_t aspect)
{
    if (r.uniform) {
        be->copyBufferTexels(bufferVa + r.first, uint32_t(r.pitch), t, x, y, z, w, h, intoTexture, aspect);
        return;
    }
    for (uint32_t k = 0; k < h; k++)
        be->copyBufferTexels(bufferVa + bufferRow(r, offset, bytesPerRow, k), w * raw::bufferUnit(t, aspect), t, x,
                             y + k, z, w, 1, intoTexture, aspect);
}

// A copy side's z (raw::BlitEncoder): an array's or cube's layer, else a 3D texture's plane
static uint32_t layerOrPlane(const raw::Texture &t, uint32_t slice, uint32_t z)
{
    return t.layers > 1 ? slice : z;
}

// A buffer<->texture copy of `planes` planes from plane z, `image` bytes apart in the buffer (each plane's rows decided at
// record: bufferPlanes); the buffer's lines cleaned before, or invalidated for the CPU after where the copy writes it
static void copyPlanes(raw::BlitEncoder *be, raw::CommandBuffer *rc, const raw::Buffer &b, uint64_t offset,
                       uint64_t row, uint64_t image, const raw::Texture &t, uint32_t x, uint32_t y, uint32_t z,
                       uint32_t w, uint32_t h, uint32_t planes, bool intoTexture, uint8_t aspect)
{
    const uint32_t unit = raw::bufferUnit(t, aspect);
    for (uint32_t k = 0; k < std::max(planes, 1u); k++) {
        const uint64_t at = offset + k * image;
        BufferRows rows;
        bufferRows(at, row, h, w * unit, unit, intoTexture, rows);
        const uint64_t first = bufferRow(rows, at, row, 0);
        if (!b.wc)
            rc->clean(b.cpu + first, bufferRow(rows, at, row, h - 1) - first + w * unit, !intoTexture);
        copyRows(be, b.va, rows, at, row, t, x, y, z + k, w, h, intoTexture, aspect);
    }
}

void encodeBlit(RMCommandBuffer *cb, const Cmd &c, raw::CommandBuffer *rc, raw::BlitEncoder *be,
                std::set<RMTexture *> &fresh, std::set<RMTexture *> &standInFresh)
{
    switch (c.op) {
    case OpCopyTexture: {
        RMTexture *s = (__bridge RMTexture *)c.obj, *t = (__bridge RMTexture *)c.obj2;
        useTexture(rc, s, false);
        useTexture(rc, t, true);
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        const uint64_t x = c.x, o = extra(cb, x + 10), sl = extra(cb, x + 1), dl = extra(cb, x + 9);
        const uint32_t sx = uint32_t(extra(cb, x + 2)), sy = uint32_t(extra(cb, x + 3)),
                       sz = uint32_t(extra(cb, x + 4)), w = uint32_t(extra(cb, x + 5)), h = uint32_t(extra(cb, x + 6)),
                       d = uint32_t(extra(cb, x + 7)), dx = uint32_t(o), dy = uint32_t(o >> 32),
                       dz = uint32_t(extra(cb, x + 11)), ss = uint32_t(extra(cb, x)), ds = uint32_t(extra(cb, x + 8));
        raw::Texture fromLevel, toLevel;
        if (sl)
            raw::chainLevelImage(s->_raw, uint32_t(sl), fromLevel);
        if (dl)
            raw::chainLevelImage(t->_raw, uint32_t(dl), toLevel);
        for (uint32_t k = 0; k < std::max(d, 1u); k++)
            be->copyTexels(sl ? fromLevel : s->_raw, sx, sy, layerOrPlane(s->_raw, ss, sz + k), dl ? toLevel : t->_raw,
                           dx, dy, layerOrPlane(t->_raw, ds, dz + k), w, h);
        break;
    }
    case OpMipmaps: {  // a chain's levels from level 0 (decided at record)
        RMTexture *t = (__bridge RMTexture *)c.obj;
        useTexture(rc, t, true);
        be->generate(t->_raw);
        break;
    }
    case OpCopyBuffer: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj, *t = (__bridge RMBuffer *)c.obj2;
        useBuffer(rc, s, false);
        useBuffer(rc, t, true);
        const uint64_t n = extra(cb, c.x + 1), dof = extra(cb, c.x);
        if (!s->_raw.wc)
            rc->clean(s->_raw.cpu + c.c, n);
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + dof, n, true);
        be->copyBytes(s->_raw.va + c.c, t->_raw.va + dof, n);
        break;
    }
    case OpFillBuffer: {
        RMBuffer *t = (__bridge RMBuffer *)c.obj;
        const uint64_t n = extra(cb, c.x);
        useBuffer(rc, t, true);
        if (!t->_raw.wc)
            rc->clean(t->_raw.cpu + c.c, n, true);
        be->fillBytes(t->_raw.va + c.c, n, uint8_t(c.a));
        break;
    }
    case OpCopyBufferToTexture: {
        RMBuffer *s = (__bridge RMBuffer *)c.obj;
        RMTexture *t = (__bridge RMTexture *)c.obj2;
        useBuffer(rc, s, false);
        useTexture(rc, t, true);
        const uint64_t x = c.x, o = extra(cb, x + 8), so = extra(cb, x), row = extra(cb, x + 1),
                       image = extra(cb, x + 2);
        const uint64_t w = extra(cb, x + 3), h = extra(cb, x + 4), d = extra(cb, x + 5), dl = extra(cb, x + 7),
                       dz = layerOrPlane(t->_raw, uint32_t(extra(cb, x + 6)), uint32_t(extra(cb, x + 9)));
        if (!fresh.empty())
            fresh.erase(t);
        standInFresh.erase(t);
        raw::Texture level;
        if (dl)
            raw::chainLevelImage(t->_raw, uint32_t(dl), level);
        copyPlanes(be, rc, s->_raw, so, row, image, dl ? level : t->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(dz),
                   uint32_t(w), uint32_t(h), uint32_t(d), true, uint8_t(c.a));
        break;
    }
    case OpCopyTextureToBuffer: {
        RMTexture *s = (__bridge RMTexture *)c.obj;
        RMBuffer *t = (__bridge RMBuffer *)c.obj2;
        useTexture(rc, s, false);
        useBuffer(rc, t, true);
        const uint64_t x = c.x, o = extra(cb, x + 2),
                       sz = layerOrPlane(s->_raw, uint32_t(extra(cb, x)), uint32_t(extra(cb, x + 3))),
                       w = extra(cb, x + 4), h = extra(cb, x + 5), d = extra(cb, x + 6), dof = extra(cb, x + 7),
                       row = extra(cb, x + 8), image = extra(cb, x + 9), sl = extra(cb, x + 1);
        raw::Texture level;
        if (sl)
            raw::chainLevelImage(s->_raw, uint32_t(sl), level);
        copyPlanes(be, rc, t->_raw, dof, row, image, sl ? level : s->_raw, uint32_t(o), uint32_t(o >> 32), uint32_t(sz),
                   uint32_t(w), uint32_t(h), uint32_t(d), false, uint8_t(c.a));
        break;
    }
    default:
        break;
    }
}
