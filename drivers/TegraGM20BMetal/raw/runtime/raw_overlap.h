// SPDX-License-Identifier: MIT
// A framebuffer-fetching draw that writes a color it fetches must see, at each pixel, what its own earlier triangles wrote
// there (Metal's raster order). The fetch barrier orders draws, not the triangles of one draw, so such a draw is drawn as
// runs of consecutive triangles no two of which can cover the same pixel, with the barrier between runs.
//
// Two triangles cover no pixel centre in common when a line through an edge of one has the other on its far side, every
// vertex strictly but those exactly on the line. Maxwell's raster unit covers a pixel whose centre is inside the triangle
// or on one of its top or left edges (the top-left rule of D3D10 and later, which it implements whatever the API; NVK
// sets SET_WINDOW_ORIGIN upper left): a centre on a line is covered only by a triangle whose interior lies right of or
// below it there, so triangles on either side of a line never cover the same centre, along it or off it. A vertex is
// exactly on the line where the GPU computes its window position from the same inputs as the edge's ends: it is one of
// them (equal position bytes), or the edge is vertical (horizontal) because its ends share x (y), z and w, which alone
// make window x (y) when the matrix takes nothing of y (x) into x (y) or w, and the vertex shares them too. A triangle
// with two equal vertices has no area and covers nothing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "raw_fetch_order.h"
#include "raw_region.h"

namespace raw {

class TriangleRuns {
public:
    static constexpr uint32_t kMaxTriangles = kSequentialTriangles;

    // A list or strip needs no CPU geometry to preserve raster order one triangle at a time. An indexed strip needs
    // its restart positions, and instanced draws need a separate instance-order plan, so neither is admitted here.
    // An indexed list's runs are index ranges. Runs retain the original vertex IDs and provoking vertex; restarting
    // an odd strip triangle reverses winding.
    static bool sequential(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed,
                           std::vector<std::pair<uint32_t, uint32_t>> &runs,
                           std::array<bool, kMaxTriangles> &winding)
    {
        runs.clear();
        winding.fill(false);
        const bool strip = primitive == 4;
        if ((indexed && strip) || instances != 1 || (primitive != 3 && !strip))
            return false;
        const uint32_t triangles = strip ? (count > 2 ? count - 2 : 0) : count / 3;
        if (triangles > kMaxTriangles)
            return false;
        for (uint32_t i = 0; i < triangles; ++i) {
            const uint32_t first = strip ? i : i * 3;
            runs.emplace_back(first, first + 3);
            winding[i] = strip && (i & 1);
        }
        return true;
    }

    // False means an ordering proof is unavailable. Callers must use sequential primitives, interlock or refuse;
    // drawing an unknown self-fetching primitive whole would lose raster-order semantics.
    bool split(const DrawGeometry &geometry, bool strip, std::vector<std::pair<uint32_t, uint32_t>> &runs)
    {
        auto g = geometry;
        g.strip = strip;
        GeometryAnalysis analysis(g);
        return split(analysis, runs);
    }

    bool split(GeometryAnalysis &analysis, std::vector<std::pair<uint32_t, uint32_t>> &runs)
    {
        runs.clear();
        const auto &g = analysis.geometry();
        if (!g.bounded)
            return false;
        auto &snapshot = analysis.snapshot();
        const bool whole = !analysis.begin() && g.count == snapshot.source.count;
        if (whole && snapshot.runsEvaluated) {
            runs = snapshot.runs;
            return snapshot.runsValid;
        }
        auto finish = [&](bool valid) {
            if (!valid)
                runs.clear();
            if (whole) {
                snapshot.runsEvaluated = true;
                snapshot.runsValid = valid;
                snapshot.runs = runs;
            }
            return valid;
        };
        if (!snapshot.evaluate() || !gather(analysis))
            return finish(false);
        uint32_t runFirst = 0;
        for (uint32_t t = 1; t < count_; t++) {
            if (tri_[t].empty)
                continue;
            for (uint32_t u = runFirst; u < t; u++) {
                if (tri_[u].empty)
                    continue;
                if (!snapshot.takePair())
                    return finish(false);
                if (mayShare(tri_[u], tri_[t])) {
                    runs.push_back({runs.empty() ? 0 : tri_[runFirst].element, tri_[t - 1].element + 3});
                    runFirst = t;
                    break;
                }
            }
        }
        if (!runs.empty())
            runs.push_back({tri_[runFirst].element, g.count});
        return finish(true);
    }

    // Restarting a strip at an odd triangle reverses its assembly winding. Measure parity from this draw's
    // beginning (not a larger cached snapshot), resetting at every primitive restart.
    static std::array<bool, kMaxTriangles> winding(GeometryAnalysis &analysis,
                                                   const std::vector<std::pair<uint32_t, uint32_t>> &runs)
    {
        std::array<bool, kMaxTriangles> result = {};
        if (!analysis.geometry().strip || runs.empty())
            return result;
        const auto &points = analysis.snapshot().points;
        size_t run = 0;
        uint32_t first = 0;
        for (uint32_t i = 0; i < analysis.geometry().count && run < runs.size() && run < result.size(); ++i) {
            if (points[analysis.begin() + i].restart)
                first = i + 1;
            if (i == runs[run].first)
                result[run++] = !points[analysis.begin() + i].restart && ((i - first) & 1);
        }
        return result;
    }

private:
    using Vertex = GeometryVertex;
    bool exactX_ = false, exactY_ = false;

    struct Triangle {
        Vertex v[3];
        uint32_t element;
        bool empty;
        double x0, y0, x1, y1;
    };

    bool gather(GeometryAnalysis &analysis)
    {
        count_ = 0;
        const auto &g = analysis.geometry();
        const auto &snapshot = analysis.snapshot();
        exactX_ = snapshot.exactX;
        exactY_ = snapshot.exactY;
        Vertex window[3];
        uint32_t first = 0, have = 0;
        for (uint32_t e = 0; e < g.count; e++) {
            const auto &point = snapshot.points[analysis.begin() + e];
            if (point.restart) {
                have = 0;
                continue;
            }
            if (have == 3) {
                window[0] = window[1];
                window[1] = window[2];
                first++;
                have = 2;
            }
            if (!have)
                first = e;
            window[have++] = point;
            if (have < 3)
                continue;
            if (count_ == kMaxTriangles)
                return false;
            add(window, first);
            if (!g.strip)
                have = 0;
        }
        return true;
    }

    void add(const Vertex (&v)[3], uint32_t element)
    {
        Triangle &t = tri_[count_++];
        t.element = element;
        t.empty = same(v[0], v[1]) || same(v[1], v[2]) || same(v[0], v[2]);
        t.x0 = t.y0 = INFINITY;
        t.x1 = t.y1 = -INFINITY;
        for (int i = 0; i < 3; i++) {
            t.v[i] = v[i];
            t.x0 = std::min(t.x0, v[i].at.x - v[i].at.slack);
            t.x1 = std::max(t.x1, v[i].at.x + v[i].at.slack);
            t.y0 = std::min(t.y0, v[i].at.y - v[i].at.slack);
            t.y1 = std::max(t.y1, v[i].at.y + v[i].at.slack);
        }
    }

    static bool same(const Vertex &a, const Vertex &b)
    {
        return !memcmp(a.bits, b.bits, sizeof a.bits);
    }

    static bool share(const Vertex &a, const Vertex &b, int axis)
    {
        return a.bits[axis] == b.bits[axis] && a.bits[2] == b.bits[2] && a.bits[3] == b.bits[3];
    }

    // whether the GPU puts v exactly on the line through p and q
    bool onLine(const Vertex &v, const Vertex &p, const Vertex &q) const
    {
        if (same(v, p) || same(v, q))
            return true;
        for (int axis = 0; axis < 2; axis++)
            if ((axis ? exactY_ : exactX_) && share(p, q, axis) && share(v, p, axis))
                return true;
        return false;
    }

    bool mayShare(const Triangle &a, const Triangle &b) const
    {
        // a pixel's centre covered by both lies in both boxes, half a pixel in from each pixel's edge
        if (a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0)
            return false;
        for (int i = 0; i < 3; i++)
            if (separates(a, i, b) || separates(b, i, a))
                return false;
        return true;
    }

    // Whether the line through edge `i` of `a` (its vertices i and i + 1) has `b` on the far side from `a`'s third vertex
    bool separates(const Triangle &a, int i, const Triangle &b) const
    {
        const Vertex &p = a.v[i], &q = a.v[(i + 1) % 3], &r = a.v[(i + 2) % 3];
        const double dx = q.at.x - p.at.x, dy = q.at.y - p.at.y, length = std::hypot(dx, dy);
        if (!(length > 0))
            return false;
        auto side = [&](const Vertex &v) {
            return (dx * (v.at.y - p.at.y) - dy * (v.at.x - p.at.x)) / length;
        };
        auto error = [&](const Vertex &v) {
            const double along = std::hypot(v.at.x - p.at.x, v.at.y - p.at.y) / length;
            return v.at.slack + p.at.slack + q.at.slack + along * (p.at.slack + q.at.slack);
        };
        const double own = side(r);
        if (std::fabs(own) <= error(r))
            return false;
        for (const Vertex &v : b.v) {
            if (onLine(v, p, q))
                continue;
            if ((own > 0 ? side(v) : -side(v)) >= -error(v))
                return false;
        }
        return true;
    }

    Triangle tri_[kMaxTriangles];
    uint32_t count_ = 0;
};

}  // namespace raw
