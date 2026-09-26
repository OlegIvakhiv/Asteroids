/**
 * @file FieldGeom.hpp
 * @brief Canvas-to-SFML translation kit: the drawing vocabulary the design
 *        lab pages use, rebuilt as baked triangle lists.
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * The asteroid, scrap and citadel designs were authored in HTML canvas, which
 * gives you mitered strokes of any width, radial and linear gradients, and
 * clip(). SFML gives you triangles and 1px lines. Porting the designs by eye
 * would lose exactly the details that make them read, so this file rebuilds
 * the canvas calls those pages actually use, one for one:
 *
 *   ctx.stroke()          strokePath()    mitered quads, optional round caps
 *   ctx.fill()            fillPoly()      ear-clipped (concave OK)
 *   createRadialGradient  radialGradient() concentric bands, per-vertex colour
 *   createLinearGradient  fillQuadGradient() subdivided so mid stops survive
 *   ctx.clip()            clipMeshInto()  triangle x triangle Sutherland-Hodgman
 *   setLineDash           dashLine()
 *   mulberry32            Rng             bit-identical to the JS generator
 *
 * Everything writes into a Mesh: one flat sf::Vertex triangle list in painter
 * order, so a whole object is ONE draw call and the design's layering
 * (fill, then its stroke, then the next plate over both) is preserved.
 *
 * ============================================================================
 * THIN LINES
 * ============================================================================
 * Canvas antialiases a 0.4px line into a faint smear. A 0.4px quad in SFML
 * either vanishes or flickers as it rotates. Mesh::minWidth clamps every
 * stroke to a floor and pays for the extra width with alpha, so the line
 * keeps the same visual weight the design gave it.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EnemyArchetypes.hpp"   // enemyarch::geom::triangulate / convexHull
#include <SFML/Graphics.hpp>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace fieldgeom {

    using V2 = sf::Vector2f;
    inline constexpr float PI = 3.14159265358979f;
    inline constexpr float TAU = 6.28318530717959f;

    // ========================================================================
    // RNG -- mulberry32, the same generator the design pages use.
    // ========================================================================
    // Same seed, same object. The citadel is built from the design page's own
    // seed (0xC17ADE1), so its core, craters, fissures and armour ring come
    // out as the exact shapes that were signed off -- not look-alikes.
    struct Rng {
        uint32_t a;
        explicit Rng(uint32_t seed) : a(seed) {}
        float operator()() {
            a += 0x6D2B79F5u;
            uint32_t t = (a ^ (a >> 15)) * (1u | a);
            t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
            return static_cast<float>(static_cast<double>(t ^ (t >> 14)) / 4294967296.0);
        }
    };

    // ========================================================================
    // COLOUR
    // ========================================================================
    inline std::uint8_t u8(float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 255.f)));
    }
    /// rgba(r,g,b,a) with CSS semantics: channels 0-255, alpha 0-1.
    inline sf::Color rgba(float r, float g, float b, float a = 1.f) {
        return sf::Color(u8(r), u8(g), u8(b), u8(a * 255.f));
    }
    /// #rrggbb
    inline sf::Color hex(uint32_t h, float a = 1.f) {
        return rgba(static_cast<float>((h >> 16) & 255),
            static_cast<float>((h >> 8) & 255),
            static_cast<float>(h & 255), a);
    }
    inline sf::Color withAlpha(sf::Color c, float mul) {
        c.a = u8(c.a * mul);
        return c;
    }
    inline sf::Color lerp(sf::Color a, sf::Color b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return sf::Color(u8(a.r + (b.r - a.r) * t), u8(a.g + (b.g - a.g) * t),
            u8(a.b + (b.b - a.b) * t), u8(a.a + (b.a - a.a) * t));
    }

    struct Stop { float t; sf::Color c; };

    /// Colour of a gradient at t, CSS rules: clamped at both ends.
    inline sf::Color sample(const std::vector<Stop>& s, float t) {
        if (s.empty()) return sf::Color::Transparent;
        if (t <= s.front().t) return s.front().c;
        if (t >= s.back().t) return s.back().c;
        for (size_t i = 1; i < s.size(); ++i) {
            if (t <= s[i].t) {
                const float span = std::max(1e-6f, s[i].t - s[i - 1].t);
                return lerp(s[i - 1].c, s[i].c, (t - s[i - 1].t) / span);
            }
        }
        return s.back().c;
    }

    // ========================================================================
    // VECTOR HELPERS
    // ========================================================================
    inline float len(V2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
    inline V2 norm(V2 v) { const float l = len(v); return l > 1e-6f ? V2(v.x / l, v.y / l) : V2(0.f, 0.f); }
    inline V2 perp(V2 v) { return { -v.y, v.x }; }
    inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
    inline V2 pol(float r, float a) { return { std::cos(a) * r, std::sin(a) * r }; }
    inline V2 rot(V2 p, float c, float s) { return { p.x * c - p.y * s, p.x * s + p.y * c }; }

    inline float radiusOf(const std::vector<V2>& pts) {
        float m = 0.f;
        for (const auto& p : pts) m = std::max(m, p.x * p.x + p.y * p.y);
        return std::sqrt(m);
    }
    inline V2 centroid(const std::vector<V2>& pts) {
        V2 c(0.f, 0.f);
        if (pts.empty()) return c;
        for (const auto& p : pts) c += p;
        return c / static_cast<float>(pts.size());
    }
    struct Box { float minX, minY, maxX, maxY; float w() const { return maxX - minX; } float h() const { return maxY - minY; } };
    inline Box boundsOf(const std::vector<V2>& pts) {
        Box b{ 1e9f, 1e9f, -1e9f, -1e9f };
        for (const auto& p : pts) {
            b.minX = std::min(b.minX, p.x); b.minY = std::min(b.minY, p.y);
            b.maxX = std::max(b.maxX, p.x); b.maxY = std::max(b.maxY, p.y);
        }
        return b;
    }
    inline bool inside(V2 q, const std::vector<V2>& poly) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const V2& a = poly[i]; const V2& b = poly[j];
            if (((a.y > q.y) != (b.y > q.y)) &&
                (q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y + 1e-9f) + a.x))
                in = !in;
        }
        return in;
    }

    // ========================================================================
    // MESH
    // ========================================================================
    struct Mesh {
        std::vector<sf::Vertex> v;
        /// Narrowest stroke that is allowed to exist, in this mesh's units.
        /// Narrower strokes are widened to it and faded to compensate.
        float minWidth = 0.f;

        void tri(V2 a, V2 b, V2 c, sf::Color ca, sf::Color cb, sf::Color cc) {
            v.push_back(sf::Vertex{ a, ca });
            v.push_back(sf::Vertex{ b, cb });
            v.push_back(sf::Vertex{ c, cc });
        }
        void tri(V2 a, V2 b, V2 c, sf::Color col) { tri(a, b, c, col, col, col); }
        void quad(V2 a, V2 b, V2 c, V2 d, sf::Color col) { tri(a, b, c, col); tri(a, c, d, col); }
        void append(const Mesh& o) { v.insert(v.end(), o.v.begin(), o.v.end()); }
        bool empty() const { return v.empty(); }
        size_t size() const { return v.size(); }

        /// Apply the thin-line floor. Returns the width to draw at and fades
        /// `col` by however much was added.
        float lineWidth(float w, sf::Color& col) const {
            if (minWidth > 0.f && w < minWidth) {
                col = withAlpha(col, std::max(0.f, w) / minWidth);
                return minWidth;
            }
            return w;
        }
    };

    // ========================================================================
    // FILLS
    // ========================================================================
    inline void fillPoly(Mesh& m, const std::vector<V2>& pts, sf::Color c) {
        if (pts.size() < 3 || c.a == 0) return;
        const auto t = enemyarch::geom::triangulate(pts);
        for (size_t i = 0; i + 2 < t.size(); i += 3) m.tri(t[i], t[i + 1], t[i + 2], c);
    }

    /// Fill with a per-point colour function (gradients). Triangles are the
    /// ear-clipped polygon, so this is only exact when the gradient is linear
    /// across each triangle -- see fillQuadGradient for anything with a
    /// mid stop.
    template <class F>
    inline void fillPolyShaded(Mesh& m, const std::vector<V2>& pts, F colorAt) {
        if (pts.size() < 3) return;
        const auto t = enemyarch::geom::triangulate(pts);
        for (size_t i = 0; i + 2 < t.size(); i += 3)
            m.tri(t[i], t[i + 1], t[i + 2], colorAt(t[i]), colorAt(t[i + 1]), colorAt(t[i + 2]));
    }

    /// A quad (any convex four points, ring order) subdivided bilinearly, so a
    /// three-stop linear gradient keeps its middle stop instead of being
    /// flattened into a two-colour ramp between the corners.
    template <class F>
    inline void fillQuadGradient(Mesh& m, const V2 q[4], F colorAt, int sub = 4) {
        auto at = [&](float u, float w) {
            const V2 top = q[0] + (q[1] - q[0]) * u;
            const V2 bot = q[3] + (q[2] - q[3]) * u;
            return top + (bot - top) * w;
            };
        for (int i = 0; i < sub; ++i) {
            for (int j = 0; j < sub; ++j) {
                const float u0 = i / static_cast<float>(sub), u1 = (i + 1) / static_cast<float>(sub);
                const float w0 = j / static_cast<float>(sub), w1 = (j + 1) / static_cast<float>(sub);
                const V2 a = at(u0, w0), b = at(u1, w0), c = at(u1, w1), d = at(u0, w1);
                m.tri(a, b, c, colorAt(a), colorAt(b), colorAt(c));
                m.tri(a, c, d, colorAt(a), colorAt(c), colorAt(d));
            }
        }
    }

    inline void disc(Mesh& m, V2 c, float r, sf::Color col, int seg = 12) {
        if (r <= 0.f || col.a == 0) return;
        for (int i = 0; i < seg; ++i) {
            const float a0 = TAU * i / seg, a1 = TAU * (i + 1) / seg;
            m.tri(c, c + pol(r, a0), c + pol(r, a1), col);
        }
    }

    inline void rect(Mesh& m, float x, float y, float w, float h, sf::Color c) {
        m.quad({ x, y }, { x + w, y }, { x + w, y + h }, { x, y + h }, c);
    }

    /**
     * @brief createRadialGradient(c, r0, c, r1) filling a disc of radius `R`.
     *
     * Built as concentric bands with a ring at every stop, so the colour is
     * exact at each stop and linear between them -- which is exactly what the
     * canvas does. Inside r0 the first stop colour holds; past r1 the last.
     */
    inline void radialGradient(Mesh& m, V2 c, float r0, float r1, float R,
        const std::vector<Stop>& stops, int seg = 28)
    {
        if (R <= 0.f) return;
        const float span = std::max(1e-4f, r1 - r0);
        std::vector<float> radii{ 0.f };
        if (r0 > 0.f && r0 < R) radii.push_back(r0);
        for (const auto& s : stops) {
            const float r = r0 + s.t * span;
            if (r > 0.f && r < R) radii.push_back(r);
        }
        radii.push_back(R);
        std::sort(radii.begin(), radii.end());
        radii.erase(std::unique(radii.begin(), radii.end(),
            [](float a, float b) { return std::fabs(a - b) < 1e-4f; }), radii.end());

        auto colAt = [&](float r) { return sample(stops, (r - r0) / span); };
        for (size_t k = 0; k + 1 < radii.size(); ++k) {
            const float ra = radii[k], rb = radii[k + 1];
            const sf::Color ca = colAt(ra), cb = colAt(rb);
            if (ca.a == 0 && cb.a == 0) continue;
            for (int i = 0; i < seg; ++i) {
                const float a0 = TAU * i / seg, a1 = TAU * (i + 1) / seg;
                const V2 p0 = c + pol(rb, a0), p1 = c + pol(rb, a1);
                if (ra <= 0.f) { m.tri(c, p0, p1, ca, cb, cb); continue; }
                const V2 q0 = c + pol(ra, a0), q1 = c + pol(ra, a1);
                m.tri(q0, p0, p1, ca, cb, cb);
                m.tri(q0, p1, q1, ca, cb, ca);
            }
        }
    }

    // ========================================================================
    // STROKES
    // ========================================================================

    /**
     * @brief Canvas-style stroke: centred on the path, mitered joins.
     *
     * Offsets are computed once per VERTEX (not per segment), so adjacent
     * quads share an edge and a translucent stroke never double-blends at a
     * corner -- which is what a canvas stroke looks like, and what naive
     * per-segment quads get wrong.
     */
    inline void strokePath(Mesh& m, std::vector<V2> pts, bool closed, float w,
        sf::Color col, bool roundCaps = false, float miterLimit = 4.f)
    {
        // Drop duplicate points: a zero-length segment has no direction.
        pts.erase(std::unique(pts.begin(), pts.end(),
            [](V2 a, V2 b) { return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f; }),
            pts.end());
        if (closed && pts.size() > 2 && len(pts.front() - pts.back()) < 1e-5f) pts.pop_back();
        const size_t n = pts.size();
        if (n < 2 || col.a == 0) return;
        w = m.lineWidth(w, col);
        const float h = w * 0.5f;

        std::vector<V2> L(n), R(n);
        for (size_t i = 0; i < n; ++i) {
            const bool hasPrev = closed || i > 0;
            const bool hasNext = closed || i + 1 < n;
            const V2 dPrev = hasPrev ? norm(pts[i] - pts[(i + n - 1) % n]) : V2();
            const V2 dNext = hasNext ? norm(pts[(i + 1) % n] - pts[i]) : V2();
            V2 off;
            if (!hasPrev) off = perp(dNext) * h;
            else if (!hasNext) off = perp(dPrev) * h;
            else {
                const V2 n1 = perp(dPrev), n2 = perp(dNext);
                V2 mv = n1 + n2;
                const float ml = len(mv);
                if (ml < 1e-4f) off = n1 * h;
                else {
                    mv = mv / ml;
                    const float d = std::max(1e-3f, dot(mv, n1));
                    off = mv * std::min(h / d, h * miterLimit);
                }
            }
            L[i] = pts[i] + off;
            R[i] = pts[i] - off;
        }
        const size_t segs = closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            const size_t j = (i + 1) % n;
            m.quad(L[i], L[j], R[j], R[i], col);
        }
        if (roundCaps && !closed) {
            disc(m, pts.front(), h, col, 8);
            disc(m, pts.back(), h, col, 8);
        }
    }

    inline void strokeLine(Mesh& m, V2 a, V2 b, float w, sf::Color c, bool roundCaps = false) {
        strokePath(m, { a, b }, false, w, c, roundCaps);
    }
    inline void strokeLoop(Mesh& m, const std::vector<V2>& pts, float w, sf::Color c) {
        strokePath(m, pts, true, w, c);
    }

    /// setLineDash([on, off]) along one segment.
    inline void dashLine(Mesh& m, V2 a, V2 b, float w, sf::Color c, float on, float off) {
        const float L = len(b - a);
        if (L < 1e-4f) return;
        const V2 d = (b - a) / L;
        for (float s = 0.f; s < L; s += on + off) {
            const float e = std::min(L, s + on);
            strokeLine(m, a + d * s, a + d * e, w, c);
        }
    }

    /// Points on an ellipse arc. `ccw` follows the canvas `anticlockwise`
    /// flag: sweep DOWN from a0 to a1 rather than up.
    inline std::vector<V2> arcPts(V2 c, float rx, float ry, float a0, float a1,
        bool ccw = false, int seg = 16)
    {
        std::vector<V2> out;
        float sweep;
        if (!ccw) { sweep = a1 - a0; while (sweep < 0.f) sweep += TAU; }
        else { sweep = a1 - a0; while (sweep > 0.f) sweep -= TAU; }
        if (std::fabs(sweep) < 1e-5f) sweep = ccw ? -TAU : TAU;
        out.reserve(static_cast<size_t>(seg) + 1);
        for (int i = 0; i <= seg; ++i) {
            const float a = a0 + sweep * i / seg;
            out.push_back({ c.x + std::cos(a) * rx, c.y + std::sin(a) * ry });
        }
        return out;
    }

    // ========================================================================
    // CLIPPING -- ctx.clip() as geometry
    // ========================================================================

    /// Sutherland-Hodgman against a CONVEX clip polygon of either winding.
    inline std::vector<V2> clipConvex(const std::vector<V2>& subject, const std::vector<V2>& clip) {
        std::vector<V2> out = subject;
        if (clip.size() < 3) return {};
        const float sgn = enemyarch::geom::signedArea(clip) >= 0.f ? 1.f : -1.f;
        for (size_t e = 0; e < clip.size() && !out.empty(); ++e) {
            const V2 A = clip[e], B = clip[(e + 1) % clip.size()];
            auto side = [&](V2 p) { return sgn * ((B.x - A.x) * (p.y - A.y) - (B.y - A.y) * (p.x - A.x)); };
            std::vector<V2> in = std::move(out);
            out.clear();
            for (size_t i = 0; i < in.size(); ++i) {
                const V2 P = in[i], Q = in[(i + 1) % in.size()];
                const float sp = side(P), sq = side(Q);
                if (sp >= 0.f) out.push_back(P);
                if ((sp >= 0.f) != (sq >= 0.f)) {
                    const float t = sp / (sp - sq);
                    out.push_back(P + (Q - P) * t);
                }
            }
        }
        return out;
    }

    /// Triangulated clip region, with each triangle's bounding box cached.
    struct ClipRegion {
        std::vector<V2> tris;
        std::vector<Box> boxes;
    };
    inline ClipRegion makeClip(const std::vector<V2>& poly) {
        ClipRegion c;
        c.tris = enemyarch::geom::triangulate(poly);
        for (size_t i = 0; i + 2 < c.tris.size(); i += 3)
            c.boxes.push_back(boundsOf({ c.tris[i], c.tris[i + 1], c.tris[i + 2] }));
        return c;
    }

    /// Sutherland-Hodgman on coloured vertices: positions AND colours are
    /// interpolated at every cut, so a clipped gradient stays a gradient.
    inline std::vector<sf::Vertex> clipConvexV(const std::vector<sf::Vertex>& subject,
        const std::vector<V2>& clip)
    {
        std::vector<sf::Vertex> out = subject;
        if (clip.size() < 3) return {};
        const float sgn = enemyarch::geom::signedArea(clip) >= 0.f ? 1.f : -1.f;
        for (size_t e = 0; e < clip.size() && !out.empty(); ++e) {
            const V2 A = clip[e], B = clip[(e + 1) % clip.size()];
            auto side = [&](V2 p) { return sgn * ((B.x - A.x) * (p.y - A.y) - (B.y - A.y) * (p.x - A.x)); };
            std::vector<sf::Vertex> in = std::move(out);
            out.clear();
            for (size_t i = 0; i < in.size(); ++i) {
                const sf::Vertex& P = in[i];
                const sf::Vertex& Q = in[(i + 1) % in.size()];
                const float sp = side(P.position), sq = side(Q.position);
                if (sp >= 0.f) out.push_back(P);
                if ((sp >= 0.f) != (sq >= 0.f)) {
                    const float t = sp / (sp - sq);
                    out.push_back(sf::Vertex{ P.position + (Q.position - P.position) * t,
                        lerp(P.color, Q.color, t) });
                }
            }
        }
        return out;
    }

    /**
     * @brief Append `src` to `out`, keeping only what lies inside `clip`.
     *
     * Both sides are triangle lists, and a triangle is convex, so every piece
     * is an exact convex intersection -- no special cases for concave rocks or
     * hulls. The clip triangles partition the region, so the pieces never
     * overlap and a translucent fill never double-blends along a seam.
     * Vertex colours are carried through, so gradients clip correctly.
     */
    inline void clipMeshInto(Mesh& out, const Mesh& src, const ClipRegion& clip) {
        for (size_t i = 0; i + 2 < src.v.size(); i += 3) {
            const std::vector<sf::Vertex> t{ src.v[i], src.v[i + 1], src.v[i + 2] };
            const Box tb = boundsOf({ t[0].position, t[1].position, t[2].position });
            for (size_t k = 0; k < clip.boxes.size(); ++k) {
                const Box& cb = clip.boxes[k];
                if (tb.maxX < cb.minX || tb.minX > cb.maxX || tb.maxY < cb.minY || tb.minY > cb.maxY) continue;
                const std::vector<V2> ct{ clip.tris[k * 3], clip.tris[k * 3 + 1], clip.tris[k * 3 + 2] };
                const std::vector<sf::Vertex> piece = clipConvexV(t, ct);
                for (size_t j = 1; j + 1 < piece.size(); ++j) {
                    out.v.push_back(piece[0]);
                    out.v.push_back(piece[j]);
                    out.v.push_back(piece[j + 1]);
                }
            }
        }
    }

    /// Transform every vertex: scale, then rotate, then translate.
    inline void transform(Mesh& m, float scale, float angleRad, V2 offset) {
        const float c = std::cos(angleRad), s = std::sin(angleRad);
        for (auto& v : m.v) v.position = rot(v.position * scale, c, s) + offset;
    }

} // namespace fieldgeom
