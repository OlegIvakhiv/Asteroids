/**
 * @file TermDraw.hpp
 * @brief Batched flat-shape drawing for the terminal screens, plus hull meshes.
 *
 * The terminal's live panels (the tactical feed, the doctrine reels, the codex
 * specimen) draw hundreds of small flat shapes a frame: rock outlines, bullet
 * streaks, sparks, hull fills, dashed telegraph lanes. One sf::Shape per item
 * is one draw call per item. Batch collects them as plain triangles and the
 * whole lot goes out in ONE draw -- the same trick the starfield and particle
 * systems already use.
 *
 * Everything is flat-coloured, hard-edged geometry: the design contract has no
 * gradients and no bloom, so there is nothing here that would need a shader.
 *
 * HULLS: two sources, one look.
 *   - the PLAYER's ship comes from ship::ShipDesign::renderOutline() -- the
 *     model the refit bay edits -- with the livery pass on top, exactly as
 *     RenderSystem draws it in flight;
 *   - enemies come from enemyarch::ArchetypeDef (visual tris + plates + the
 *     authored outline strip), coloured the way RenderSystem colours a unit
 *     in COMBAT.
 * So the codex specimen, the feed and the doctrine reels show the real ships,
 * and a hull edit in Lua or the refit bay shows up in the menu on the next F5.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <vector>
#include <cmath>
#include <algorithm>
#include "core/EnemyArchetypes.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/ShipLivery.hpp"

namespace tdraw {

    inline constexpr float PI = 3.14159265f;
    inline constexpr float TAU = 6.2831853f;

    inline sf::Color alpha(sf::Color c, float a01) {
        c.a = static_cast<std::uint8_t>(std::clamp(a01, 0.f, 1.f) * static_cast<float>(c.a));
        return c;
    }
    inline sf::Color mix(sf::Color a, sf::Color b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return sf::Color(
            static_cast<std::uint8_t>(a.r + (b.r - a.r) * t),
            static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
            static_cast<std::uint8_t>(a.b + (b.b - a.b) * t),
            static_cast<std::uint8_t>(a.a + (b.a - a.a) * t));
    }

    /// Transform for a hull at `pos`, heading `headingRad` (0 = +X, standard
    /// maths), drawn `scale` times its authored size. Hull models face -Y, so
    /// the rotation is heading + 90 degrees -- the same mapping the game uses
    /// between TransformComponent::rotation and the direction of travel.
    inline sf::Transform hullXf(sf::Vector2f pos, float headingRad, float scale) {
        sf::Transform xf;
        xf.translate(pos);
        xf.rotate(sf::degrees(headingRad * 180.f / PI + 90.f));
        xf.scale({ scale, scale });
        return xf;
    }

    // ========================================================================
    // BATCH
    // ========================================================================

    class Batch {
    public:
        void clear() { m_v.clear(); }
        bool empty() const { return m_v.empty(); }

        void tri(sf::Vector2f a, sf::Vector2f b, sf::Vector2f c, sf::Color col) {
            m_v.push_back({ a, col }); m_v.push_back({ b, col }); m_v.push_back({ c, col });
        }

        void rect(float x, float y, float w, float h, sf::Color c) {
            tri({ x, y }, { x + w, y }, { x + w, y + h }, c);
            tri({ x, y }, { x + w, y + h }, { x, y + h }, c);
        }

        /// A segment as a quad `w` px wide. Lines drawn as real geometry keep
        /// their width under the letterbox scale; Lines primitives do not.
        void line(sf::Vector2f a, sf::Vector2f b, float w, sf::Color c) {
            sf::Vector2f d = b - a;
            const float l = std::sqrt(d.x * d.x + d.y * d.y);
            if (l < 0.0001f) return;
            const sf::Vector2f n{ -d.y / l * w * 0.5f, d.x / l * w * 0.5f };
            tri(a + n, b + n, b - n, c);
            tri(a + n, b - n, a - n, c);
        }

        /// Marching dashes along a->b. `phase` in px slides them toward b.
        void dashed(sf::Vector2f a, sf::Vector2f b, float w, sf::Color c,
            float dash = 10.f, float gap = 8.f, float phase = 0.f) {
            sf::Vector2f d = b - a;
            const float L = std::sqrt(d.x * d.x + d.y * d.y);
            if (L < 1.f) return;
            d /= L;
            const float period = dash + gap;
            float s = std::fmod(phase, period);
            if (s > 0.f) s -= period;
            for (; s < L; s += period) {
                const float s0 = std::max(0.f, s), s1 = std::min(L, s + dash);
                if (s1 > s0) line(a + d * s0, a + d * s1, w, c);
            }
        }

        /// Convex polygon as a fan. Concave input needs polyTris().
        void fan(const std::vector<sf::Vector2f>& p, sf::Color c) {
            for (std::size_t i = 1; i + 1 < p.size(); ++i) tri(p[0], p[i], p[i + 1], c);
        }

        /// Closed outline of `p`, every edge a quad.
        void loop(const std::vector<sf::Vector2f>& p, float w, sf::Color c) {
            for (std::size_t i = 0; i < p.size(); ++i) line(p[i], p[(i + 1) % p.size()], w, c);
        }

        void ring(sf::Vector2f o, float r, float w, sf::Color c, int segs = 40) {
            if (r <= 0.5f) return;
            const float r0 = std::max(0.f, r - w * 0.5f), r1 = r + w * 0.5f;
            for (int i = 0; i < segs; ++i) {
                const float a0 = TAU * i / segs, a1 = TAU * (i + 1) / segs;
                const sf::Vector2f u0{ std::cos(a0), std::sin(a0) }, u1{ std::cos(a1), std::sin(a1) };
                tri(o + u0 * r0, o + u0 * r1, o + u1 * r1, c);
                tri(o + u0 * r0, o + u1 * r1, o + u1 * r0, c);
            }
        }

        /// Pie slice, for the stepped phosphor wedges. Flat colour per slice.
        void wedge(sf::Vector2f o, float r, float a0, float a1, sf::Color c, int segs = 6) {
            for (int i = 0; i < segs; ++i) {
                const float u0 = a0 + (a1 - a0) * i / segs, u1 = a0 + (a1 - a0) * (i + 1) / segs;
                tri(o, o + sf::Vector2f(std::cos(u0), std::sin(u0)) * r,
                    o + sf::Vector2f(std::cos(u1), std::sin(u1)) * r, c);
            }
        }

        /// Pre-triangulated local mesh through a transform.
        void tris(const std::vector<sf::Vector2f>& t, const sf::Transform& xf, sf::Color c) {
            for (const auto& p : t) m_v.push_back({ xf.transformPoint(p), c });
        }

        /// A TriangleStrip (outlineStrip's output) as plain triangles.
        void strip(const std::vector<sf::Vector2f>& s, const sf::Transform& xf, sf::Color c) {
            for (std::size_t i = 0; i + 2 < s.size(); ++i)
                tri(xf.transformPoint(s[i]), xf.transformPoint(s[i + 1]), xf.transformPoint(s[i + 2]), c);
        }

        /// Raw vertices already in world space (livery pass output).
        void vertices(const std::vector<sf::Vertex>& v, const sf::Transform& xf, float a01 = 1.f) {
            for (const auto& s : v) m_v.push_back({ xf.transformPoint(s.position), alpha(s.color, a01) });
        }

        void draw(sf::RenderTarget& t) const {
            if (!m_v.empty()) t.draw(m_v.data(), m_v.size(), sf::PrimitiveType::Triangles);
        }
        void drawWith(sf::RenderTarget& t, const sf::RenderStates& s) const {
            if (!m_v.empty()) t.draw(m_v.data(), m_v.size(), sf::PrimitiveType::Triangles, s);
        }

    private:
        std::vector<sf::Vertex> m_v;
    };

    // ========================================================================
    // HULL MESHES
    // ========================================================================

    struct HullMesh {
        std::vector<sf::Vector2f> outline;   ///< CCW, local, forward = -Y
        std::vector<sf::Vector2f> tris;
        std::vector<sf::Vector2f> edge;      ///< outlineStrip at `edgeWidth`
        float edgeWidth = 0.f;
        float radius = 30.f;
        bool  valid() const { return outline.size() >= 3 && !tris.empty(); }
    };

    inline HullMesh meshFromOutline(std::vector<sf::Vector2f> pts, float edgeWidth = 2.2f) {
        HullMesh m;
        if (pts.size() < 3) return m;
        if (enemyarch::geom::signedArea(pts) < 0.f) std::reverse(pts.begin(), pts.end());
        m.outline = pts;
        m.tris = enemyarch::geom::triangulate(pts);
        m.edgeWidth = edgeWidth;
        m.edge = enemyarch::geom::outlineStrip(pts, edgeWidth);
        float r = 0.f;
        for (const auto& p : pts) r = std::max(r, std::sqrt(p.x * p.x + p.y * p.y));
        m.radius = std::max(1.f, r);
        return m;
    }

    inline HullMesh meshFromDesign(const ship::ShipDesign& d, float edgeWidth = 2.2f) {
        return meshFromOutline(d.renderOutline(), edgeWidth);
    }

    /// A design hull, with the owner's livery (plates, figures, cockpit) when
    /// `lv` is given -- the same ship::liveryPass call flight and the refit bay make.
    inline void drawHull(Batch& b, const HullMesh& m, const sf::Transform& xf,
        sf::Color fill, sf::Color edge, const ship::Livery* lv = nullptr,
        std::vector<sf::Vertex>* scratch = nullptr) {
        if (!m.valid()) return;
        b.tris(m.tris, xf, fill);
        if (lv && scratch) {
            scratch->clear();
            ship::liveryPass(*lv, { fill, edge }, false, *scratch);
            b.vertices(*scratch, xf);
            scratch->clear();
            ship::liveryPass(*lv, { fill, edge }, true, *scratch);
            b.vertices(*scratch, xf);
        }
        b.strip(m.edge, xf, edge);
    }

    /// An enemy archetype as RenderSystem draws it in COMBAT, recoloured by
    /// `fill`. Plates follow the live fill, like they do in flight.
    inline void drawArchetype(Batch& b, const enemyarch::ArchetypeDef& a, const sf::Transform& xf,
        sf::Color fill, sf::Color edge, float edgeWidth, std::vector<sf::Vector2f>* scratch = nullptr) {
        if (!a.visualTris.empty()) b.tris(a.visualTris, xf, fill);
        for (const auto& pl : a.plates) {
            sf::Color pc(
                static_cast<std::uint8_t>(std::clamp(fill.r * pl.shade, 0.f, 255.f)),
                static_cast<std::uint8_t>(std::clamp(fill.g * pl.shade, 0.f, 255.f)),
                static_cast<std::uint8_t>(std::clamp(fill.b * pl.shade, 0.f, 255.f)), fill.a);
            if (pl.tintMix > 0.f) pc = mix(pc, sf::Color(pl.tint.r, pl.tint.g, pl.tint.b, fill.a), pl.tintMix);
            b.tris(pl.tris, xf, pc);
        }
        if (a.visual.size() >= 3) {
            std::vector<sf::Vector2f> local;
            std::vector<sf::Vector2f>& s = scratch ? *scratch : local;
            s = enemyarch::geom::outlineStrip(a.visual, edgeWidth);
            b.strip(s, xf, edge);
        }
        if (!a.lampTris.empty()) b.tris(a.lampTris, xf, a.lampMelee);
    }

    /// RenderSystem's COMBAT colours for a unit: fill = archetype colour,
    /// outline lifted toward white.
    inline sf::Color enemyFill(const enemyarch::ArchetypeDef& a) { return sf::Color(a.color.r, a.color.g, a.color.b); }
    inline sf::Color enemyEdge(const enemyarch::ArchetypeDef& a) {
        return sf::Color(static_cast<std::uint8_t>(std::min(255, a.color.r + 60)),
            static_cast<std::uint8_t>(std::min(255, a.color.g + 50)),
            static_cast<std::uint8_t>(std::min(255, a.color.b + 50)), 200);
    }

    /// Radius of an archetype silhouette, for framing a specimen.
    inline float archetypeRadius(const enemyarch::ArchetypeDef& a) {
        float r = 0.f;
        for (const auto& p : a.visual) r = std::max(r, std::sqrt(p.x * p.x + p.y * p.y));
        return r > 0.f ? r : a.radius;
    }

    // ========================================================================
    // FIELD OBJECTS (menu stand-ins for the in-flight models)
    // ========================================================================

    /// Lumpy rock outline. `shape` holds per-vertex radius multipliers.
    inline std::vector<sf::Vector2f> rockPoints(sf::Vector2f o, float r, float rot, const float* shape, int n) {
        std::vector<sf::Vector2f> p;
        p.reserve(n);
        for (int i = 0; i < n; ++i) {
            const float a = rot + TAU * i / n;
            p.push_back(o + sf::Vector2f(std::cos(a), std::sin(a)) * (r * shape[i]));
        }
        return p;
    }

} // namespace tdraw
