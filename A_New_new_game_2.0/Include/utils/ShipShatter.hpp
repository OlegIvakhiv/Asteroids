/**
 * @file ShipShatter.hpp
 * @brief Cut a ship's hull into wreck pieces. Pure geometry, no rendering.
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * A ship used to die by vanishing behind a burst of particles. Particles say
 * "something exploded"; they do not say "THAT ship came apart". What sells it
 * is seeing the silhouette the player has been reading all fight break into
 * recognisable chunks -- a nose here, a wing there -- tumble away, and fade.
 *
 * The pieces are decorative. No Box2D body, no collision, no ECS slot: the
 * same contract as DebrisSystem's rock shards, for the same reason. A kill
 * must not hand the player a dozen new things to fly into.
 *
 * ============================================================================
 * HOW THE CUT WORKS
 * ============================================================================
 * The design lab sliced the hull POLYGON with straight lines. That is fine on
 * a canvas, which fills a self-touching outline without complaint, but the
 * Rakshari hulls are concave: a straight cut through the Barge's exhaust
 * teeth leaves a "polygon" that is really two teeth joined by a seam along
 * the cut line, and ear clipping folds that into garbage.
 *
 * So the cut runs on the hull's TRIANGULATION instead (ArchetypeDef::
 * visualTris, already built at load). Every triangle is clipped against each
 * cut line; a convex piece clipped by a half-plane stays convex, so nothing
 * here ever needs triangulating again. Each fragment remembers which side of
 * every cut it fell on -- that bit pattern is its CELL -- and the fragments
 * of one cell are grouped into a piece. Fragments that do not touch (the two
 * outer teeth of the Barge, cut off by the same line) become separate
 * pieces, so nothing flies around welded to a part it is not attached to.
 *
 * ============================================================================
 * EDGE TAGS
 * ============================================================================
 * Every fragment edge carries one of three tags through the clipping:
 *
 *   INNER  a diagonal of the triangulation -- never drawn
 *   SKIN   part of the original silhouette -- drawn as the hull outline
 *   SCAR   made by a cut -- the fresh fracture face, drawn hot and cooling
 *
 * That is what lets a piece draw ONLY its real edges. Stroking every triangle
 * would put the triangulation on screen as a wireframe.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EnemyArchetypes.hpp"   // enemyarch::geom
#include <SFML/System/Vector2.hpp>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <algorithm>

namespace shatter {

    /// One wreck piece, in the SOURCE frame's orientation but recentred on
    /// its own centroid, so it can spin about its own middle.
    struct Piece {
        sf::Vector2f centre;                  ///< Centroid in the source frame
        float area = 0.f;                     ///< px^2
        float radius = 0.f;                   ///< Furthest vertex from centre
        std::vector<sf::Vector2f> tris;       ///< 3 per triangle, centred
        std::vector<sf::Vector2f> skin;       ///< Segment pairs: original outline
        std::vector<sf::Vector2f> scar;       ///< Segment pairs: fracture faces
    };

    namespace detail {

        enum : uint8_t { INNER = 0, SKIN = 1, SCAR = 2 };

        struct Frag {
            std::vector<sf::Vector2f> v;
            std::vector<uint8_t> e;           ///< e[i] tags edge v[i] -> v[i+1]
            uint32_t cell = 0;                ///< Bit k set = front of cut k
        };

        inline float frand() { return (rand() % 10000) / 10000.f; }

        inline bool same(sf::Vector2f a, sf::Vector2f b, float eps) {
            return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
        }

        inline float area(const std::vector<sf::Vector2f>& p) {
            return std::fabs(enemyarch::geom::signedArea(p));
        }

        /// Drop near-duplicate neighbours. When a pair collapses the EARLIER
        /// vertex goes: the edge into it lies on the same line as the edge
        /// out of the survivor, so every remaining tag still describes the
        /// edge it sits on.
        inline void dedupe(Frag& f) {
            for (size_t guard = 0; f.v.size() >= 2 && guard < 64; ++guard) {
                bool removed = false;
                for (size_t i = 0; i < f.v.size(); ++i) {
                    const size_t j = (i + 1) % f.v.size();
                    if (i != j && same(f.v[i], f.v[j], 1e-3f)) {
                        f.v.erase(f.v.begin() + static_cast<long>(i));
                        f.e.erase(f.e.begin() + static_cast<long>(i));
                        removed = true;
                        break;
                    }
                }
                if (!removed) break;
            }
        }

        /// Keep the part of convex `f` where dot(x - p, n) >= 0.
        inline Frag clip(const Frag& f, sf::Vector2f p, sf::Vector2f n) {
            Frag out;
            out.cell = f.cell;
            const size_t m = f.v.size();
            out.v.reserve(m + 2);
            out.e.reserve(m + 2);
            for (size_t i = 0; i < m; ++i) {
                const sf::Vector2f a = f.v[i], b = f.v[(i + 1) % m];
                const float da = (a.x - p.x) * n.x + (a.y - p.y) * n.y;
                const float db = (b.x - p.x) * n.x + (b.y - p.y) * n.y;
                const bool ain = da >= 0.f, bin = db >= 0.f;
                if (ain) {
                    out.v.push_back(a); out.e.push_back(f.e[i]);       // a -> ... along edge i
                    if (!bin) {
                        const float t = da / (da - db);
                        out.v.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
                        out.e.push_back(SCAR);                          // exit -> entry: the cut
                    }
                }
                else if (bin) {
                    const float t = da / (da - db);
                    out.v.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
                    out.e.push_back(f.e[i]);                            // entry -> b along edge i
                }
            }
            dedupe(out);
            return out;
        }

        /// Is a->b one of the outline's own edges (either direction)?
        inline bool onOutline(sf::Vector2f a, sf::Vector2f b, const std::vector<sf::Vector2f>& ol) {
            const size_t n = ol.size();
            for (size_t i = 0; i < n; ++i) {
                const sf::Vector2f& p = ol[i];
                const sf::Vector2f& q = ol[(i + 1) % n];
                if ((same(a, p, 1e-3f) && same(b, q, 1e-3f)) ||
                    (same(a, q, 1e-3f) && same(b, p, 1e-3f))) return true;
            }
            return false;
        }

        struct UnionFind {
            std::vector<int> parent;
            explicit UnionFind(size_t n) : parent(n) { for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i); }
            int find(int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; }
            void join(int a, int b) { parent[find(a)] = find(b); }
        };

        /// Two fragments touch if any vertex of one lies on any edge of the
        /// other. Vertex-equality is not enough: the clip of a shared
        /// diagonal can put a vertex mid-way along the neighbour's edge.
        inline bool touching(const Frag& A, const Frag& B) {
            constexpr float EPS = 0.05f;
            const auto onEdge = [](sf::Vector2f q, sf::Vector2f a, sf::Vector2f b) {
                const sf::Vector2f ab{ b.x - a.x, b.y - a.y };
                const float l2 = ab.x * ab.x + ab.y * ab.y;
                if (l2 < 1e-9f) return same(q, a, EPS);
                const float t = std::clamp(((q.x - a.x) * ab.x + (q.y - a.y) * ab.y) / l2, 0.f, 1.f);
                const float dx = q.x - (a.x + ab.x * t), dy = q.y - (a.y + ab.y * t);
                return dx * dx + dy * dy <= EPS * EPS;
                };
            for (const auto& q : A.v)
                for (size_t j = 0; j < B.v.size(); ++j)
                    if (onEdge(q, B.v[j], B.v[(j + 1) % B.v.size()])) return true;
            for (const auto& q : B.v)
                for (size_t j = 0; j < A.v.size(); ++j)
                    if (onEdge(q, A.v[j], A.v[(j + 1) % A.v.size()])) return true;
            return false;
        }

        inline Piece buildPiece(const std::vector<const Frag*>& frags) {
            Piece pc;
            float aSum = 0.f, cx = 0.f, cy = 0.f;
            for (const Frag* f : frags) {
                // Area-weighted: a sliver hanging off a big chunk must not
                // drag the spin centre toward it.
                const float a = area(f->v);
                float fx = 0.f, fy = 0.f;
                for (const auto& v : f->v) { fx += v.x; fy += v.y; }
                fx /= static_cast<float>(f->v.size());
                fy /= static_cast<float>(f->v.size());
                cx += fx * a; cy += fy * a; aSum += a;
            }
            pc.area = aSum;
            pc.centre = (aSum > 1e-6f) ? sf::Vector2f(cx / aSum, cy / aSum) : frags.front()->v.front();

            const sf::Vector2f c = pc.centre;
            for (const Frag* f : frags) {
                for (size_t i = 1; i + 1 < f->v.size(); ++i) {
                    pc.tris.push_back(f->v[0] - c);
                    pc.tris.push_back(f->v[i] - c);
                    pc.tris.push_back(f->v[i + 1] - c);
                }
                for (size_t i = 0; i < f->v.size(); ++i) {
                    if (f->e[i] == INNER) continue;
                    auto& dst = (f->e[i] == SKIN) ? pc.skin : pc.scar;
                    dst.push_back(f->v[i] - c);
                    dst.push_back(f->v[(i + 1) % f->v.size()] - c);
                }
                for (const auto& v : f->v) {
                    const sf::Vector2f d = v - c;
                    pc.radius = std::max(pc.radius, std::sqrt(d.x * d.x + d.y * d.y));
                }
            }
            return pc;
        }

    } // namespace detail

    /**
     * @brief Cut a hull into pieces with `cuts` random straight lines.
     *
     * @param outline   The silhouette (ArchetypeDef::visual) -- used only to
     *                  tell outline edges from triangulation diagonals.
     * @param tris      Its triangulation, 3 points per triangle.
     * @param cuts      Number of cut lines. Pieces grow roughly as
     *                  (cuts^2 + cuts + 2) / 2: 2 -> 4, 3 -> 7, 4 -> 11.
     * @param minArea   Pieces smaller than this (px^2) are dropped; a
     *                  two-pixel sliver reads as noise, not as wreckage.
     *
     * The cut ANGLES are spread evenly around the half-turn and then jittered,
     * rather than rolled freely. Free rolls regularly produce two near-
     * parallel cuts, which slice the ship into planks -- that reads as a
     * sawmill, not an explosion.
     */
    inline std::vector<Piece> slice(const std::vector<sf::Vector2f>& outline,
        const std::vector<sf::Vector2f>& tris, int cuts, float minArea = 6.f)
    {
        using namespace detail;
        std::vector<Piece> out;
        if (tris.size() < 3 || outline.size() < 3) return out;
        cuts = std::clamp(cuts, 0, 8);

        // ---- Seed fragments: the triangles, with their edges tagged ----
        std::vector<Frag> frags;
        frags.reserve(tris.size() / 3);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            Frag f;
            f.v = { tris[i], tris[i + 1], tris[i + 2] };
            for (int k = 0; k < 3; ++k)
                f.e.push_back(onOutline(f.v[k], f.v[(k + 1) % 3], outline) ? SKIN : INNER);
            if (area(f.v) > 1e-4f) frags.push_back(std::move(f));
        }

        // ---- Size of the hull, to aim the cuts at its body ----
        // Half-extents of the bounding box, not one radius: the Barge is
        // twice as long as it is wide, and cuts aimed inside a circle all
        // land mid-ship, leaving an untouched nose and tail.
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& v : outline) {
            x0 = std::min(x0, v.x); y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
        }
        const sf::Vector2f mid{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f };
        const sf::Vector2f half{ std::max(1.f, (x1 - x0) * 0.5f), std::max(1.f, (y1 - y0) * 0.5f) };

        // ---- Cut ----
        const float PI = 3.14159265f;
        const float base = frand() * PI;
        for (int k = 0; k < cuts; ++k) {
            const float step = PI / static_cast<float>(std::max(1, cuts));
            const float ang = base + k * step + (frand() - 0.5f) * step * 0.7f;
            const sf::Vector2f n{ std::cos(ang), std::sin(ang) };

            // Through the middle half of the hull's ellipse. Further out, a
            // cut tends to shave a sliver off one edge and miss everything.
            const float pa = frand() * 2.f * PI;
            const float pr = 0.5f * std::sqrt(frand());
            const sf::Vector2f p{ mid.x + std::cos(pa) * pr * half.x,
                                  mid.y + std::sin(pa) * pr * half.y };

            std::vector<Frag> next;
            next.reserve(frags.size() * 2);
            for (const Frag& f : frags) {
                Frag front = clip(f, p, n);
                Frag back = clip(f, p, { -n.x, -n.y });
                front.cell |= (1u << k);
                if (front.v.size() >= 3 && area(front.v) > 1e-3f) next.push_back(std::move(front));
                if (back.v.size() >= 3 && area(back.v) > 1e-3f)  next.push_back(std::move(back));
            }
            frags.swap(next);
        }

        // ---- Group: same cell AND physically touching ----
        UnionFind uf(frags.size());
        for (size_t a = 0; a < frags.size(); ++a)
            for (size_t b = a + 1; b < frags.size(); ++b)
                if (frags[a].cell == frags[b].cell && touching(frags[a], frags[b]))
                    uf.join(static_cast<int>(a), static_cast<int>(b));

        std::vector<std::vector<const Frag*>> groups;
        std::vector<int> slot(frags.size(), -1);
        for (size_t i = 0; i < frags.size(); ++i) {
            const int r = uf.find(static_cast<int>(i));
            if (slot[static_cast<size_t>(r)] < 0) {
                slot[static_cast<size_t>(r)] = static_cast<int>(groups.size());
                groups.emplace_back();
            }
            groups[static_cast<size_t>(slot[static_cast<size_t>(r)])].push_back(&frags[i]);
        }

        for (const auto& g : groups) {
            Piece pc = buildPiece(g);
            if (pc.area >= minArea && !pc.tris.empty()) out.push_back(std::move(pc));
        }
        return out;
    }

    /**
     * @brief A whole polygon as one piece -- an armour plate tearing loose.
     * @param points  Outline (skin)
     * @param tris    Its triangulation (ArchetypeDef::Plate::tris)
     */
    inline Piece whole(const std::vector<sf::Vector2f>& points, const std::vector<sf::Vector2f>& tris) {
        Piece pc;
        if (points.size() < 3 || tris.size() < 3) return pc;
        pc.area = detail::area(points);
        sf::Vector2f c{ 0.f, 0.f };
        for (const auto& v : points) c += v;
        c /= static_cast<float>(points.size());
        pc.centre = c;
        for (const auto& v : tris) pc.tris.push_back(v - c);
        for (size_t i = 0; i < points.size(); ++i) {
            pc.skin.push_back(points[i] - c);
            pc.skin.push_back(points[(i + 1) % points.size()] - c);
            const sf::Vector2f d = points[i] - c;
            pc.radius = std::max(pc.radius, std::sqrt(d.x * d.x + d.y * d.y));
        }
        return pc;
    }

} // namespace shatter
