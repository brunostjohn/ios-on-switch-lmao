// SPDX-License-Identifier: MIT
// The fetch barrier's region test: whether a fetching draw's pixels can meet those a draw since the last barrier wrote.
// Each draw keeps what its vertices need (raw_bounds.h) and the CPU reads them only when a fetching draw asks, newest
// first, until one meets it. A draw whose pixels it cannot bound (not Core Animation's transform, its matrix or vertices
// not readable here, too many vertices, a vertex at or behind w = 0) may cover its whole scissor.
//
// Skipping the barrier where no written pixel is fetched is exact on uncompressed (pitch and generic block-linear)
// targets: a texel's bytes change only where the ROP writes that pixel, so a fetch of any other pixel reads the same
// bytes whether or not the earlier writes have landed, from the texture cache or from L2.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

#include "raw_bounds.h"

namespace raw {

struct DrawGeometry {
    bool bounded = false;  // Core Animation's transform over triangles, its inputs where the CPU can read them
    const uint8_t *matrix = nullptr;  // the float4x4 at vertex buffer 2
    PositionStream positions;
    uint64_t positionBytes = 0;        // bound from positions.base
    uint32_t first = 0, count = 0;     // the first vertex and the vertex count, or the base vertex and the index count
    const uint8_t *indices = nullptr;  // 2- or 4-byte indices
    uint32_t indexSize = 0;
    WindowViewport viewport;
    PixelBox limit;  // its scissor within its viewport within the render area
};

class WrittenRegion {
public:
    static constexpr uint32_t kMaxDraws = 16, kMaxVertices = 96;

    class Analysis {
    public:
        explicit Analysis(const DrawGeometry &geometry) : geometry_(geometry) {}

        const DrawGeometry &geometry() const
        {
            return geometry_;
        }

        bool evaluated() const
        {
            return evaluated_;
        }

        PixelBox cachedBox() const
        {
            return valid_ ? pixels_ : geometry_.limit;
        }

        bool bounds(PixelBox &out)
        {
            if (!evaluated_) {
                evaluated_ = true;
                if (geometry_.bounded && geometry_.count <= kMaxVertices) {
                    float matrix[16];
                    memcpy(matrix, geometry_.matrix, sizeof(matrix));
                    bounds_.emplace(matrix, geometry_.viewport);
                    valid_ = addVertices(geometry_, *bounds_, added_);
                    if (valid_)
                        pixels_ = bounds_->pixels(geometry_.limit);
                }
            }
            if (valid_)
                out = pixels_;
            return valid_;
        }

        bool covers(bool list, uint32_t w, uint32_t h)
        {
            const DrawGeometry &g = geometry_;
            if (!g.bounded || g.count != (list ? 6u : 4u) || g.limit.x0 || g.limit.y0 || g.limit.x1 < w ||
                g.limit.y1 < h)
                return false;
            PixelBox box, certain, possible;
            return bounds(box) && added_ == g.count && bounds_->rectangle(list, certain, possible) &&
                   !certain.x0 && !certain.y0 && certain.x1 >= w && certain.y1 >= h;
        }

    private:
        const DrawGeometry &geometry_;
        std::optional<DrawBounds> bounds_;
        PixelBox pixels_;
        uint32_t added_ = 0;
        bool evaluated_ = false, valid_ = false;
    };

    void clear()
    {
        count_ = 0;
        overflow_ = false;
    }

    void add(const DrawGeometry &g)
    {
        add(g, g.limit, !g.bounded);
    }

    void add(const Analysis &analysis)
    {
        const DrawGeometry &g = analysis.geometry();
        add(g, analysis.cachedBox(), analysis.evaluated() || !g.bounded);
    }

    // Whether `g`'s pixels may meet a pixel written since the last clear
    bool meets(const DrawGeometry &g)
    {
        Analysis analysis(g);
        return meets(analysis);
    }

    bool meets(Analysis &analysis)
    {
        const DrawGeometry &g = analysis.geometry();
        if (g.limit.empty())
            return false;
        if (overflow_)
            return true;
        PixelBox box;
        if (!analysis.bounds(box))
            box = g.limit;
        for (uint32_t i = count_; i-- > 0;) {
            Draw &d = draws_[i];
            if (!d.known) {
                d.known = true;
                if (!bounds(d.geometry, d.box))
                    d.box = d.geometry.limit;
            }
            if (d.box.overlaps(box))
                return true;
        }
        return false;
    }

    static bool bounds(const DrawGeometry &g, PixelBox &out)
    {
        Analysis analysis(g);
        return analysis.bounds(out);
    }

    // Whether the draw covers every pixel of [0, w) x [0, h) for sure: all its vertices one axis-aligned rectangle (a
    // triangle `list` of 6, or a strip of 4 with no restart), within a scissor and viewport that hold the whole area
    static bool covers(const DrawGeometry &g, bool list, uint32_t w, uint32_t h)
    {
        Analysis analysis(g);
        return analysis.covers(list, w, h);
    }

    // Every vertex of `g` into `b` but a strip's restarts (`added` counts those taken); false: one past the bound bytes,
    // or one that leaves no box (raw_bounds.h add)
    static bool addVertices(const DrawGeometry &g, DrawBounds &b, uint32_t &added)
    {
        const uint64_t reach = g.positions.offset + g.positions.components * sizeof(float);
        for (uint32_t i = 0; i < g.count; i++) {
            uint32_t v = g.first + i;
            if (g.indices) {
                uint32_t x;
                if (g.indexSize == 2) {
                    uint16_t h;
                    memcpy(&h, g.indices + 2 * i, 2);
                    x = h == 0xffff ? ~0u : h;
                } else {
                    memcpy(&x, g.indices + 4 * i, 4);
                }
                if (x == ~0u)
                    continue;  // a strip's restart
                v = g.first + x;
            }
            if (uint64_t(v) * g.positions.stride + reach > g.positionBytes || !b.add(g.positions, v))
                return false;
            added++;
        }
        return true;
    }

private:
    void add(const DrawGeometry &g, const PixelBox &box, bool known)
    {
        if (g.limit.empty())
            return;
        if (count_ == kMaxDraws) {
            overflow_ = true;
            return;
        }
        draws_[count_++] = {g, box, known};
    }

    struct Draw {
        DrawGeometry geometry;
        PixelBox box;
        bool known;
    };

    std::array<Draw, kMaxDraws> draws_;
    uint32_t count_ = 0;
    bool overflow_ = false;
};

}  // namespace raw
