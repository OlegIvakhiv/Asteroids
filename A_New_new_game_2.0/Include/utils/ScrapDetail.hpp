/**
 * @file ScrapDetail.hpp
 * @brief Procedural interior detail for field objects: clusters and rocks.
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * Every asteroid in the game was ONE polygon with one fill and one stroke.
 * That is the ceiling on how much a silhouette can say: however much the
 * outline is warped, a single flat shape reads as stone. Real salvage is
 * LAYERED -- separate chunks overlapping at bad angles, each with its own
 * edge, welded together with exposed struts, with holes punched through where
 * nothing bridged the gap.
 *
 * So an object is built as a small scene instead of a shape:
 *
 *   CLUSTER   3-6 overlapping chunk polygons, each its own shade and stroke,
 *             struts welded between their centres, dark gaps punched on top.
 *             The union is the silhouette; nobody draws a "body".
 *
 *   ROCK      the body polygon, plus interior facets fanned from an off-centre
 *             point (alternating shaded / bare, each stroked) and craters as
 *             dark pits with a lit rim.
 *
 * ============================================================================
 * EVERYTHING IS ROLLED PER OBJECT
 * ============================================================================
 * Nothing here is authored. Chunk count, placement, shade, strut routing, gap
 * positions, crater count and facet split are all rolled at spawn, so two
 * piles of the same type never come out the same. That is the entire point: a
 * hand-authored cluster is one recognisable object repeated across the screen,
 * which is worse than a plain polygon.
 *
 * ============================================================================
 * OUTPUT FORMAT
 * ============================================================================
 * Colours are BAKED into the vertices at build time rather than resolved at
 * draw. Every shade is derived from the object's own base colour, so the
 * rust/steel and dark-when-big ramps survive, and a fragment that inherits its
 * parent's colour is simply rebuilt with that colour instead.
 *
 * `outline` is the convex hull of everything: it feeds the physics shape and
 * nothing else. The visible edge is the chunks' own strokes.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EnemyArchetypes.hpp"   // enemyarch::geom
#include <SFML/Graphics.hpp>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace scrapdetail {

    struct Detail {
        std::vector<sf::Vector2f> outline;  ///< Convex hull -- physics only
        std::vector<sf::Vertex>   tris;     ///< Filled geometry, colours baked
        std::vector<sf::Vertex>   lines;    ///< Strokes, colours baked
    };

    // ------------------------------------------------------------------
    // helpers
    // ------------------------------------------------------------------

    inline float frand() { return (rand() % 1000) / 1000.f; }
    inline float frand(float a, float b) { return a + (b - a) * frand(); }
    inline int   irand(int a, int b) { return a + rand() % std::max(1, (b - a + 1)); }

    /// Scale a colour, keeping alpha. Every shade in here comes from the
    /// object's own base colour so the palette ramps stay intact.
    inline sf::Color shade(sf::Color c, float k, std::uint8_t a = 255) {
        const auto ch = [&](std::uint8_t v) {
            return static_cast<std::uint8_t>(std::clamp(v * k, 0.f, 255.f));
            };
        return sf::Color(ch(c.r), ch(c.g), ch(c.b), a);
    }

    inline void addPoly(std::vector<sf::Vertex>& out,
        const std::vector<sf::Vector2f>& pts, sf::Color col)
    {
        std::vector<sf::Vector2f> p = pts;
        if (enemyarch::geom::signedArea(p) < 0.f) std::reverse(p.begin(), p.end());
        for (const auto& v : enemyarch::geom::triangulate(p))
            out.push_back(sf::Vertex{ v, col });
    }

    inline void addLoop(std::vector<sf::Vertex>& out,
        const std::vector<sf::Vector2f>& pts, sf::Color col)
    {
        for (size_t i = 0; i < pts.size(); ++i) {
            out.push_back(sf::Vertex{ pts[i], col });
            out.push_back(sf::Vertex{ pts[(i + 1) % pts.size()], col });
        }
    }

    inline void addSeg(std::vector<sf::Vertex>& out,
        sf::Vector2f a, sf::Vector2f b, sf::Color col)
    {
        out.push_back(sf::Vertex{ a, col });
        out.push_back(sf::Vertex{ b, col });
    }

    /// An irregular closed blob: a jittered circle, used for chunks and gaps.
    inline std::vector<sf::Vector2f> blob(sf::Vector2f c, float r, int n, float rough) {
        std::vector<sf::Vector2f> p;
        p.reserve(static_cast<size_t>(n));
        const float phase = frand(0.f, 6.28318f);
        for (int i = 0; i < n; ++i) {
            const float a = phase + (i / static_cast<float>(n)) * 6.28318f
                + frand(-0.5f, 0.5f) * (1.4f / n);
            const float d = r * (1.f - rough * 0.5f + frand() * rough);
            p.push_back({ c.x + std::cos(a) * d, c.y + std::sin(a) * d });
        }
        return p;
    }

    inline bool inside(sf::Vector2f q, const std::vector<sf::Vector2f>& poly) {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const auto& a = poly[i]; const auto& b = poly[j];
            if (((a.y > q.y) != (b.y > q.y)) &&
                (q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y + 1e-6f) + a.x))
                in = !in;
        }
        return in;
    }

    // ------------------------------------------------------------------
    // CLUSTER -- a pile of separate pieces welded into one object
    // ------------------------------------------------------------------

    /**
     * @param radius   Overall size; the union is normalised to roughly this.
     * @param base     The object's colour. Every shade is derived from it.
     * @param nChunks  How many pieces. More reads as bigger, so it is passed
     *                 by size class rather than rolled across the full range.
     */
    inline Detail buildCluster(float radius, sf::Color base, int nChunks,
        float spread = 0.52f, int gapCount = 2, int strutCount = 3)
    {
        Detail d;
        nChunks = std::clamp(nChunks + irand(-1, 1), 2, 7);

        std::vector<sf::Vector2f> centres;
        std::vector<std::vector<sf::Vector2f>> chunks;
        std::vector<sf::Color> fills;

        for (int i = 0; i < nChunks; ++i) {
            // Deliberately uneven angular step: evenly placed chunks read as a
            // flower, not as a pile. The first one sits near the middle so the
            // union always has a solid core.
            const float a = (i / static_cast<float>(nChunks)) * 6.28318f
                + frand(-0.6f, 0.6f) * (6.28318f / nChunks);
            const float dist = (i == 0) ? radius * frand(0.f, 0.12f)
                : radius * spread * frand(0.45f, 1.05f);
            const sf::Vector2f c{ std::cos(a) * dist, std::sin(a) * dist };
            const float rr = radius * frand(0.34f, 0.58f);

            // Each piece came off something different, so each gets its own
            // value. That spread is what makes the pile read as assembled
            // rather than as one object with a busy outline.
            fills.push_back(shade(base, frand(0.62f, 1.34f)));
            centres.push_back(c);
            chunks.push_back(blob(c, rr, irand(5, 7), frand(0.24f, 0.44f)));
        }

        // Normalise so the rolled size still means what it says -- the size
        // bands are what tell small salvage from medium.
        float maxR = 0.001f;
        for (const auto& ch : chunks)
            for (const auto& v : ch) maxR = std::max(maxR, std::sqrt(v.x * v.x + v.y * v.y));
        const float k = radius / maxR;
        for (auto& ch : chunks) for (auto& v : ch) { v.x *= k; v.y *= k; }
        for (auto& c : centres) { c.x *= k; c.y *= k; }

        // ---- Struts first, so the plates sit on top of the frame ----
        const sf::Color strutCol = shade(base, 2.05f);
        for (int i = 0; i < strutCount && centres.size() >= 2; ++i) {
            const int a = irand(0, static_cast<int>(centres.size()) - 1);
            int b = irand(0, static_cast<int>(centres.size()) - 1);
            if (b == a) b = (b + 1) % static_cast<int>(centres.size());
            addSeg(d.lines, centres[static_cast<size_t>(a)],
                centres[static_cast<size_t>(b)], strutCol);
        }

        // ---- Plates ----
        for (size_t i = 0; i < chunks.size(); ++i) {
            addPoly(d.tris, chunks[i], fills[i]);
            addLoop(d.lines, chunks[i], shade(base, 2.3f));
            for (const auto& v : chunks[i]) d.outline.push_back(v);
        }

        // ---- Gaps: holes punched where nothing bridged ----
        const sf::Color gapFill(6, 4, 3);
        const sf::Color gapEdge = shade(base, 1.5f);
        for (int i = 0; i < gapCount; ++i) {
            const float a = frand(0.f, 6.28318f);
            const float dist = radius * frand(0.05f, 0.45f);
            const sf::Vector2f c{ std::cos(a) * dist, std::sin(a) * dist };
            auto g = blob(c, radius * frand(0.09f, 0.18f), irand(4, 5), 0.34f);
            addPoly(d.tris, g, gapFill);
            addLoop(d.lines, g, gapEdge);
        }

        d.outline = enemyarch::geom::decimateConvex(
            enemyarch::geom::convexHull(d.outline), 8);
        return d;
    }

    // ------------------------------------------------------------------
    // ROCK -- facets and craters cut into an existing silhouette
    // ------------------------------------------------------------------

    /**
     * @param body  The already-generated outline. Detail is cut INTO it, so
     *              the silhouette the physics hull came from is unchanged.
     */
    inline Detail buildRock(const std::vector<sf::Vector2f>& body, sf::Color base,
        int nCraters, bool facets = true)
    {
        Detail d;
        if (body.size() < 3) return d;

        float radius = 0.f;
        for (const auto& v : body) radius = std::max(radius, std::sqrt(v.x * v.x + v.y * v.y));

        addPoly(d.tris, body, base);

        // ---- Facets, fanned from an OFF-CENTRE hub ----
        // Off-centre matters: a fan from the centroid is radially symmetric
        // and reads as a pie chart. Shifting the hub makes the faces different
        // sizes, which reads as a solid form catching light unevenly.
        if (facets && body.size() >= 5) {
            const float ha = frand(0.f, 6.28318f);
            const sf::Vector2f hub{ std::cos(ha) * radius * frand(0.10f, 0.32f),
                                    std::sin(ha) * radius * frand(0.10f, 0.32f) };
            const int step = irand(2, 3);
            const sf::Color faceFill = shade(base, 0.62f);
            const sf::Color faceEdge = shade(base, 1.70f);
            int idx = 0;
            for (size_t i = 0; i < body.size(); i += static_cast<size_t>(step), ++idx) {
                const sf::Vector2f& a = body[i];
                const sf::Vector2f& b = body[(i + static_cast<size_t>(step)) % body.size()];
                // Alternate shaded / bare, so the interior reads as planes at
                // different angles rather than as a uniformly darker middle.
                if (idx % 2 == 0) {
                    d.tris.push_back(sf::Vertex{ hub, faceFill });
                    d.tris.push_back(sf::Vertex{ a,   faceFill });
                    d.tris.push_back(sf::Vertex{ b,   faceFill });
                }
                addSeg(d.lines, hub, a, faceEdge);
            }
        }

        // ---- Craters ----
        const sf::Color pitFill = shade(base, 0.45f);
        const sf::Color pitRim = shade(base, 1.95f);
        for (int i = 0; i < nCraters; ++i) {
            sf::Vector2f c{ 0.f, 0.f };
            const float cr = radius * frand(0.12f, 0.30f);

            // A crater hanging off the edge looks like a bite, not a pit, so
            // placements the silhouette does not contain are rejected.
            bool ok = false;
            for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
                const float a = frand(0.f, 6.28318f);
                const float dist = radius * frand(0.f, 0.52f);
                c = { std::cos(a) * dist, std::sin(a) * dist };
                ok = inside(c, body);
            }
            if (!ok) continue;

            const int seg = 9;
            std::vector<sf::Vector2f> ring;
            ring.reserve(static_cast<size_t>(seg));
            const float ph = frand(0.f, 6.28318f);
            for (int s = 0; s < seg; ++s) {
                const float a = ph + (s / static_cast<float>(seg)) * 6.28318f;
                const float rr = cr * frand(0.84f, 1.16f);
                ring.push_back({ c.x + std::cos(a) * rr, c.y + std::sin(a) * rr });
            }
            addPoly(d.tris, ring, pitFill);
            addLoop(d.lines, ring, pitRim);
        }

        d.outline = body;
        return d;
    }

} // namespace scrapdetail