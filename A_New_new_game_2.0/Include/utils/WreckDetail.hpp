/**
 * @file WreckDetail.hpp
 * @brief Cold ship wrecks: damage tiers, breaches, soot, armour and dead
 *        hardware, ported from the "Cold Ship Wrecks" design lab.
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * A wreck used to be the live ship's silhouette with one flat fill and one
 * stroke. At gameplay distance that reads as "ship-shaped rock". What makes a
 * hull read as a DEAD SHIP is what the design lab layers on top:
 *
 *   BITES     torn edges -- vertices pulled toward the centre, so a wing or
 *             sponson is visibly missing
 *   BREACHES  holes punched through to space, with a torn lip
 *   SOOT      burnt patches; a plate that came off leaves its own outline
 *   ARMOUR    the ship's own plates: attached, missing or peeled half off
 *             and hanging past the silhouette
 *   HARDWARE  dead nozzles and a drooping turret -- the unit still reads as
 *             the unit it was
 *   PITTING   micrometeorite dust on anything that has been out here a while
 *
 * ============================================================================
 * DAMAGE TIERS
 * ============================================================================
 *   0  TURNED OFF   pristine, powered down, every plate on. RESERVED FOR
 *                   AMBUSHERS: a dormant enemy wears this; a plain wreck
 *                   never rolls it. So a clean dark hull is always a live
 *                   ship waiting -- the tell is fair, and it is the only one.
 *   1  LIGHT        one bite, one breach, plating mostly attached
 *   2  HEAVY        torn sections, up to three breaches, missing and peeled
 *                   plates
 *
 * There is no fourth tier. A "destroyed hulk" tier was tried and cut: on a
 * Raider-sized hull, deep bites plus five breaches left nothing that read as
 * the ship it used to be, and a wreck that cannot be identified is just a
 * rock with extra steps.
 *
 * ============================================================================
 * ART DIRECTION: FLAT, NOT THE LAB'S LIGHTING
 * ============================================================================
 * The HTML lab lit its wrecks: radial soot gradients, soft halos around
 * breaches, a starlit rim, cold blue glints on every torn edge. All of that
 * was cut. It is faked lighting, the same thing that got craters removed
 * from rocks (RenderSystem 1.6), and next to everything else in the field
 * it looked like it came from a different game.
 *
 * What is left follows ScrapDetail's rules:
 *   - every surface is ONE flat colour, no per-vertex gradients
 *   - every stroke is a shade of the object's own colour -- no foreign blue
 *   - soot is a flat translucent patch, not a bloom
 *   - the silhouette gets the same hard outline every rock gets (`shape`'s
 *     stroke, set in EntityFactory), nothing baked here
 *
 * ============================================================================
 * NOT PORTED FROM THE LAB
 * ============================================================================
 *  - FLOATING DEBRIS FIELD. Baked geometry rotates rigidly with the body,
 *    so an orbiting debris cloud would turn into a spinning halo. Needs its
 *    own tiny system if it is wanted.
 *  - DROP SHADOW, TILT, DRIFT. Box2D moves the body; nothing else in the
 *    game casts a shadow.
 *  - TRUE ALPHA HOLES. A breach is a near-black fill, not a cut; a real cut
 *    needs a render texture per wreck.
 *
 * The lab clipped everything to the hull with ctx.clip(). Here overlays are
 * intersected with the hull's own triangulation once, at spawn, so nothing
 * spills onto the starfield.
 *
 * ============================================================================
 * OUTPUT
 * ============================================================================
 * Everything lands in ONE triangle list with colours baked in, layered in
 * draw order. Strokes are thin quads rather than sf::Lines so they can sit
 * UNDER later layers -- a plate seam must disappear into a breach, not be
 * drawn across it. The result goes straight into RenderComponent::detailTris
 * and rides the existing detail path: world transform and hit flash included.
 *
 * All geometry is in PIXELS, local to the body. `Source::unit` is pixels per
 * authored hull unit; widths and hardware sizes are multiplied by it so a
 * scaled hull keeps its proportions.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EnemyArchetypes.hpp"   // enemyarch::geom
#include <SFML/Graphics.hpp>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace wreckdetail {

    inline constexpr float TAU = 6.2831853f;

    /// Tiers are 0 (turned off), 1 (light), 2 (heavy). See the file header.
    inline constexpr int MAX_TIER = 2;

    // ========================================================================
    // DATA
    // ========================================================================

    struct Plate {
        std::vector<sf::Vector2f> pts;
        float shade = 1.f;
        bool  accent = false;
    };

    /// Everything authored about the ship, already scaled into pixels.
    struct Source {
        std::vector<sf::Vector2f> hull;                 ///< Pristine silhouette
        std::vector<Plate> plates;                      ///< Already mirrored
        std::vector<sf::Vector2f> thrusters;
        std::vector<sf::Vector2f> turrets;
        std::vector<std::vector<sf::Vector2f>> scars;   ///< Polylines
        float scarWidth = 1.6f;                         ///< Authored units
        float unit = 1.f;                               ///< Pixels per authored unit
    };

    struct Hole { sf::Vector2f c; float r = 0.f; std::vector<sf::Vector2f> pts; };
    struct Blot { std::vector<sf::Vector2f> pts; float a = 0.f; };   ///< Flat soot patch
    struct Pit { sf::Vector2f c; float r = 0.f; float a = 0.f; };

    /// A rolled wreck: geometry only, no colour. Deterministic from the seed.
    struct Model {
        int  tier = 0;
        bool pristine = true;
        std::vector<sf::Vector2f> hull;                 ///< Bitten silhouette
        float hullR = 0.f;
        std::vector<Hole>  holes;
        std::vector<Blot>  scorch;
        std::vector<Plate> plates;                      ///< Still attached
        std::vector<Plate> peeled;                      ///< Final position, hanging off
        std::vector<Pit>   pits;
    };

    // ========================================================================
    // RNG -- mulberry32, same generator as the lab, so a seed means the same
    // wreck in both.
    // ========================================================================

    struct Rng {
        uint32_t a;
        explicit Rng(uint32_t seed) : a(seed) {}
        float operator()() {
            a += 0x6D2B79F5u;
            uint32_t t = a;
            t = (t ^ (t >> 15)) * (t | 1u);
            t ^= t + (t ^ (t >> 7)) * (t | 61u);
            t ^= (t >> 14);
            // float(0xFFFFFFFF / 2^32) rounds UP to 1.0f, and rng() * n == n
            // is an out-of-range index. Clamp below one.
            return std::min(static_cast<float>(t / 4294967296.0), 0.99999994f);
        }
    };

    // ========================================================================
    // GEOMETRY
    // ========================================================================

    namespace detail {

        inline sf::Vector2f centroidOf(const std::vector<sf::Vector2f>& p) {
            sf::Vector2f c{ 0.f, 0.f };
            if (p.empty()) return c;
            for (const auto& v : p) { c.x += v.x; c.y += v.y; }
            c.x /= static_cast<float>(p.size());
            c.y /= static_cast<float>(p.size());
            return c;
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

        inline float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }

        inline float crossv(sf::Vector2f a, sf::Vector2f b, sf::Vector2f c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        }

        /// Proper crossing of segments ab and cd (shared endpoints do not count).
        inline bool segsCross(sf::Vector2f a, sf::Vector2f b, sf::Vector2f c, sf::Vector2f d) {
            const float d1 = crossv(a, b, c), d2 = crossv(a, b, d);
            const float d3 = crossv(c, d, a), d4 = crossv(c, d, b);
            return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f))
                && std::fabs(d1) > 1e-5f && std::fabs(d2) > 1e-5f
                && std::fabs(d3) > 1e-5f && std::fabs(d4) > 1e-5f;
        }

        /// A bite can fold an edge across its neighbour on a concave hull.
        /// Ear clipping on a self-crossing outline degrades to a fan, which
        /// draws the wreck wrong -- so a crossed result is rejected upstream.
        inline bool isSimple(const std::vector<sf::Vector2f>& p) {
            const size_t n = p.size();
            if (n < 4) return n == 3;
            for (size_t i = 0; i < n; ++i) {
                const auto& a = p[i]; const auto& b = p[(i + 1) % n];
                for (size_t j = i + 2; j < n; ++j) {
                    if (i == 0 && j == n - 1) continue;          // adjacent via wrap
                    if (segsCross(a, b, p[j], p[(j + 1) % n])) return false;
                }
            }
            return true;
        }

        inline float segDist(sf::Vector2f p, sf::Vector2f a, sf::Vector2f b) {
            const sf::Vector2f ab{ b.x - a.x, b.y - a.y };
            const float l2 = ab.x * ab.x + ab.y * ab.y;
            const float t = (l2 > 1e-9f)
                ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.f, 1.f) : 0.f;
            return len({ p.x - (a.x + ab.x * t), p.y - (a.y + ab.y * t) });
        }

        /// Clear distance between two closed polygons; 0 if they touch,
        /// cross, or one sits inside the other.
        inline float polyGap(const std::vector<sf::Vector2f>& A, const std::vector<sf::Vector2f>& B) {
            if (A.empty() || B.empty()) return 1e9f;
            if (inside(A[0], B) || inside(B[0], A)) return 0.f;
            float best = 1e9f;
            for (size_t i = 0; i < A.size(); ++i) {
                const auto& a0 = A[i]; const auto& a1 = A[(i + 1) % A.size()];
                for (size_t j = 0; j < B.size(); ++j) {
                    const auto& b0 = B[j]; const auto& b1 = B[(j + 1) % B.size()];
                    if (segsCross(a0, a1, b0, b1)) return 0.f;
                    best = std::min({ best, segDist(a0, b0, b1), segDist(a1, b0, b1),
                                            segDist(b0, a0, a1), segDist(b1, a0, a1) });
                }
            }
            return best;
        }

        inline sf::Color scale(sf::Color c, float k, std::uint8_t a = 255) {
            const auto ch = [&](std::uint8_t v) {
                return static_cast<std::uint8_t>(std::clamp(v * k, 0.f, 255.f));
                };
            return sf::Color(ch(c.r), ch(c.g), ch(c.b), a);
        }

        inline std::uint8_t a8(float a) {
            return static_cast<std::uint8_t>(std::clamp(a * 255.f + 0.5f, 0.f, 255.f));
        }

        /// Clip a convex polygon by a positively-wound triangle.
        inline std::vector<sf::Vector2f> clipByTri(std::vector<sf::Vector2f> poly,
            const sf::Vector2f* t)
        {
            for (int e = 0; e < 3 && !poly.empty(); ++e) {
                const sf::Vector2f A = t[e], B = t[(e + 1) % 3];
                std::vector<sf::Vector2f> out;
                out.reserve(poly.size() + 2);
                for (size_t i = 0; i < poly.size(); ++i) {
                    const sf::Vector2f P = poly[i], Q = poly[(i + 1) % poly.size()];
                    const float dp = crossv(A, B, P), dq = crossv(A, B, Q);
                    const bool pin = dp >= 0.f, qin = dq >= 0.f;
                    if (pin) out.push_back(P);
                    if (pin != qin) {
                        const float s = dp / (dp - dq);
                        out.push_back({ P.x + (Q.x - P.x) * s, P.y + (Q.y - P.y) * s });
                    }
                }
                poly.swap(out);
            }
            return poly;
        }

        /// The hull, as the triangles every overlay gets clipped against.
        struct Clipper {
            std::vector<sf::Vector2f> hull;             ///< The outline itself
            std::vector<sf::Vector2f> tris;             ///< Flat, positive winding
            std::vector<std::array<float, 4>> boxes;    ///< minX minY maxX maxY

            explicit Clipper(const std::vector<sf::Vector2f>& h0) : hull(h0) {
                std::vector<sf::Vector2f>& h = hull;
                if (enemyarch::geom::signedArea(h) < 0.f) std::reverse(h.begin(), h.end());
                tris = enemyarch::geom::triangulate(h);
                for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                    // Normalise every triangle, including any the fallback fan
                    // might emit, so the half-plane test has one sign.
                    if (crossv(tris[i], tris[i + 1], tris[i + 2]) < 0.f)
                        std::swap(tris[i + 1], tris[i + 2]);
                    boxes.push_back({
                        std::min({ tris[i].x, tris[i + 1].x, tris[i + 2].x }),
                        std::min({ tris[i].y, tris[i + 1].y, tris[i + 2].y }),
                        std::max({ tris[i].x, tris[i + 1].x, tris[i + 2].x }),
                        std::max({ tris[i].y, tris[i + 1].y, tris[i + 2].y }) });
                }
            }

            /// Every hull-clipped piece of a convex polygon, as convex polygons.
            /// True when the polygon is wholly on the hull: every vertex
            /// inside and no hull edge crossing it. Most pieces are, and
            /// splitting them against every hull triangle anyway multiplies
            /// the vertex count for nothing.
            bool contains(const std::vector<sf::Vector2f>& poly) const {
                for (const auto& v : poly) if (!inside(v, hull)) return false;
                for (size_t i = 0; i < poly.size(); ++i) {
                    const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
                    for (size_t j = 0; j < hull.size(); ++j)
                        if (segsCross(a, b, hull[j], hull[(j + 1) % hull.size()])) return false;
                }
                return true;
            }

            template <class Fn>
            void clipConvex(const std::vector<sf::Vector2f>& poly, Fn&& emit) const {
                if (contains(poly)) { emit(poly); return; }
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& v : poly) {
                    x0 = std::min(x0, v.x); y0 = std::min(y0, v.y);
                    x1 = std::max(x1, v.x); y1 = std::max(y1, v.y);
                }
                for (size_t k = 0; k < boxes.size(); ++k) {
                    const auto& b = boxes[k];
                    if (x1 < b[0] || x0 > b[2] || y1 < b[1] || y0 > b[3]) continue;
                    auto piece = clipByTri(poly, &tris[k * 3]);
                    if (piece.size() >= 3) emit(piece);
                }
            }

            /// Parts of segment ab inside the hull, as merged [t0,t1] runs.
            std::vector<std::pair<float, float>> clipSegment(sf::Vector2f a, sf::Vector2f b) const {
                std::vector<std::pair<float, float>> runs;
                const sf::Vector2f d{ b.x - a.x, b.y - a.y };
                for (size_t k = 0; k < boxes.size(); ++k) {
                    const sf::Vector2f* t = &tris[k * 3];
                    float t0 = 0.f, t1 = 1.f;
                    bool ok = true;
                    for (int e = 0; e < 3 && ok; ++e) {
                        const sf::Vector2f A = t[e], B = t[(e + 1) % 3];
                        // f(t) = cross(A,B,a + d t) = fa + t * fd, keep f >= 0
                        const float fa = crossv(A, B, a);
                        const float fd = (B.x - A.x) * d.y - (B.y - A.y) * d.x;
                        if (std::fabs(fd) < 1e-9f) { if (fa < 0.f) ok = false; continue; }
                        const float r = -fa / fd;
                        if (fd > 0.f) t0 = std::max(t0, r); else t1 = std::min(t1, r);
                        if (t0 > t1) ok = false;
                    }
                    if (ok && t1 - t0 > 1e-4f) runs.push_back({ t0, t1 });
                }
                std::sort(runs.begin(), runs.end());
                std::vector<std::pair<float, float>> merged;
                for (const auto& r : runs) {
                    if (!merged.empty() && r.first <= merged.back().second + 1e-3f)
                        merged.back().second = std::max(merged.back().second, r.second);
                    else merged.push_back(r);
                }
                return merged;
            }
        };

        // ---- triangle-list writers -----------------------------------------

        inline void fan(std::vector<sf::Vertex>& out, const std::vector<sf::Vector2f>& p, sf::Color c) {
            for (size_t i = 1; i + 1 < p.size(); ++i) {
                out.push_back(sf::Vertex{ p[0], c });
                out.push_back(sf::Vertex{ p[i], c });
                out.push_back(sf::Vertex{ p[i + 1], c });
            }
        }

        inline void fill(std::vector<sf::Vertex>& out, std::vector<sf::Vector2f> p, sf::Color c) {
            if (p.size() < 3) return;
            if (enemyarch::geom::signedArea(p) < 0.f) std::reverse(p.begin(), p.end());
            for (const auto& v : enemyarch::geom::triangulate(p)) out.push_back(sf::Vertex{ v, c });
        }

        /// A stroke as a quad. `cap` extends it by half a width each way so
        /// joints in a loop close up; left off for translucent strokes, where
        /// the overlap would show as darker dots at every corner.
        inline void stroke(std::vector<sf::Vertex>& out, sf::Vector2f a, sf::Vector2f b,
            float w, sf::Color c, bool cap)
        {
            sf::Vector2f d{ b.x - a.x, b.y - a.y };
            const float l = len(d);
            if (l < 1e-4f) return;
            d.x /= l; d.y /= l;
            const float h = std::max(0.5f, w * 0.5f);
            const sf::Vector2f n{ -d.y * h, d.x * h };
            if (cap) { a.x -= d.x * h; a.y -= d.y * h; b.x += d.x * h; b.y += d.y * h; }
            const sf::Vector2f p0{ a.x + n.x, a.y + n.y }, p1{ b.x + n.x, b.y + n.y };
            const sf::Vector2f p2{ b.x - n.x, b.y - n.y }, p3{ a.x - n.x, a.y - n.y };
            out.push_back(sf::Vertex{ p0, c }); out.push_back(sf::Vertex{ p1, c }); out.push_back(sf::Vertex{ p2, c });
            out.push_back(sf::Vertex{ p0, c }); out.push_back(sf::Vertex{ p2, c }); out.push_back(sf::Vertex{ p3, c });
        }

        /// Stroke only the parts of ab that are inside the hull.
        inline void strokeClipped(std::vector<sf::Vertex>& out, const Clipper& clip,
            sf::Vector2f a, sf::Vector2f b, float w, sf::Color c, bool cap)
        {
            for (const auto& r : clip.clipSegment(a, b)) {
                const sf::Vector2f p{ a.x + (b.x - a.x) * r.first,  a.y + (b.y - a.y) * r.first };
                const sf::Vector2f q{ a.x + (b.x - a.x) * r.second, a.y + (b.y - a.y) * r.second };
                // Only cap the real ends: a cap at a clip point would poke
                // past the torn edge.
                stroke(out, p, q, w, c, cap && r.first < 1e-3f && r.second > 1.f - 1e-3f);
            }
        }

        inline void loop(std::vector<sf::Vertex>& out, const std::vector<sf::Vector2f>& p,
            float w, sf::Color c, bool cap)
        {
            for (size_t i = 0; i < p.size(); ++i) stroke(out, p[i], p[(i + 1) % p.size()], w, c, cap);
        }

        // ---- lab generators --------------------------------------------------

        inline std::vector<int> pickIndices(int n, int count, Rng& rng, int minGap) {
            std::vector<int> chosen;
            int guard = 0;
            while (static_cast<int>(chosen.size()) < count && guard++ < 300) {
                const int i = static_cast<int>(rng() * n);
                bool ok = true;
                for (int c : chosen) {
                    int d = std::abs(c - i);
                    d = std::min(d, n - d);
                    if (d < minGap) { ok = false; break; }
                }
                if (ok) chosen.push_back(i);
            }
            return chosen;
        }

        struct Bite { int index; int radius; float depth; };

        inline std::vector<sf::Vector2f> applyBites(const std::vector<sf::Vector2f>& pts,
            sf::Vector2f cen, const std::vector<Bite>& bites, Rng& rng, float jitter)
        {
            const int n = static_cast<int>(pts.size());
            std::vector<sf::Vector2f> out;
            out.reserve(pts.size() * 2);
            for (int i = 0; i < n; ++i) {
                const sf::Vector2f p = pts[static_cast<size_t>(i)];
                float pull = 0.f;
                for (const auto& b : bites) {
                    int d = std::abs(i - b.index);
                    d = std::min(d, n - d);
                    if (d <= b.radius) {
                        const float t = 1.f - d / (b.radius + 0.5f);
                        pull = std::max(pull, t * t * (3.f - 2.f * t) * b.depth);
                    }
                }
                const float k = 1.f - pull * 0.92f;
                const float jx = (rng() - 0.5f) * pull * 6.f * jitter;
                const float jy = (rng() - 0.5f) * pull * 6.f * jitter;
                out.push_back({ cen.x + (p.x - cen.x) * k + jx, cen.y + (p.y - cen.y) * k + jy });

                // A deep pull gets a torn midpoint, so the bite has a ragged
                // floor instead of one clean chord.
                if (pull > 0.28f) {
                    const sf::Vector2f q = pts[static_cast<size_t>((i + 1) % n)];
                    const float mx = (p.x + q.x) * 0.5f, my = (p.y + q.y) * 0.5f;
                    const float mk = 1.f - pull * (0.72f + rng() * 0.34f);
                    out.push_back({ cen.x + (mx - cen.x) * mk + (rng() - 0.5f) * 5.f * jitter,
                                    cen.y + (my - cen.y) * mk + (rng() - 0.5f) * 5.f * jitter });
                }
            }
            return out;
        }

        inline std::vector<sf::Vector2f> makeHole(sf::Vector2f c, float r, Rng& rng, int verts) {
            std::vector<sf::Vector2f> p;
            p.reserve(static_cast<size_t>(verts));
            for (int i = 0; i < verts; ++i) {
                const float a = (i / static_cast<float>(verts)) * TAU + (rng() - 0.5f) * 0.35f;
                const float rr = r * (0.55f + rng() * 0.8f);
                p.push_back({ c.x + std::cos(a) * rr, c.y + std::sin(a) * rr });
            }
            return p;
        }

    } // namespace detail

    // ========================================================================
    // COLOUR RAMP
    // ========================================================================

    /// Hull base fill for a tier. A powered-down pristine hull is fresh metal
    /// and reads slightly brighter; every tier of damage darkens it.
    inline sf::Color hullColor(sf::Color cold, int tier) {
        return detail::scale(cold, tier == 0 ? 0.68f : 0.55f - tier * 0.04f);
    }

    // ========================================================================
    // BUILD -- roll the wreck. Geometry only.
    // ========================================================================

    inline Model build(const Source& src, int tier, uint32_t seed) {
        using namespace detail;
        Model m;
        Rng rng(seed);
        m.tier = std::clamp(tier, 0, MAX_TIER);
        m.pristine = (m.tier == 0);
        m.hull = src.hull;
        if (src.hull.size() < 3) return m;

        const auto& base = src.hull;
        const sf::Vector2f cen = centroidOf(base);
        const int n = static_cast<int>(base.size());
        const float u = src.unit;
        const int dmg = m.tier;

        for (const auto& v : base) m.hullR = std::max(m.hullR, len({ v.x - cen.x, v.y - cen.y }));
        const float hullR = m.hullR;

        // ---- BITES --------------------------------------------------------
        if (!m.pristine) {
            std::vector<Bite> bites;
            const auto idx = pickIndices(n, dmg, rng, 2);   // light 1 bite, heavy 2
            for (size_t i = 0; i < idx.size(); ++i) {
                Bite b{ idx[i], 1, 0.f };
                if (dmg == 1) { b.radius = 1; b.depth = 0.22f + rng() * 0.16f; }
                else { b.radius = (i == 0) ? 2 : 1; b.depth = 0.44f + rng() * 0.22f; }
                bites.push_back(b);
            }

            // Canvas fills a self-crossing outline without complaint; ear
            // clipping does not. Retry calmer before giving up on the bite.
            const float baseArea = std::fabs(enemyarch::geom::signedArea(base));
            std::vector<sf::Vector2f> bitten;
            for (int attempt = 0; attempt < 3; ++attempt) {
                std::vector<Bite> b = bites;
                if (attempt == 2) for (auto& x : b) x.depth *= 0.6f;
                auto h = applyBites(base, cen, b, rng, attempt == 0 ? u : 0.f);
                if (isSimple(h) && std::fabs(enemyarch::geom::signedArea(h)) > baseArea * 0.25f) {
                    bitten = std::move(h);
                    break;
                }
            }
            if (!bitten.empty()) m.hull = std::move(bitten);
        }

        if (enemyarch::geom::signedArea(m.hull) < 0.f) std::reverse(m.hull.begin(), m.hull.end());

        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (const auto& v : m.hull) {
            bx0 = std::min(bx0, v.x); by0 = std::min(by0, v.y);
            bx1 = std::max(bx1, v.x); by1 = std::max(by1, v.y);
        }
        const auto randIn = [&]() {
            return sf::Vector2f{ bx0 + rng() * (bx1 - bx0), by0 + rng() * (by1 - by0) };
            };

        // ---- BREACHES ------------------------------------------------------
        // Two rules, both hard:
        //   FIT   every lip vertex and edge midpoint on the hull. The lab's
        //         clip hid a breach that overhung the edge; here it would draw
        //         a void out in space. Shrink until it fits.
        //   APART no breach touches another. Two voids that merge read as one
        //         blob with a seam through it, not as two hits. The gap is a
        //         few lip-strokes wide so the plating between them stays
        //         visible at gameplay zoom.
        // A breach that cannot satisfy both after all its tries is dropped:
        // a small hull simply gets fewer holes, which is the right outcome.
        if (!m.pristine) {
            const int holeCount = (dmg == 1) ? 1 : 3;
            const float gap = std::max(3.f * u, hullR * 0.05f);
            for (int i = 0; i < holeCount; ++i) {
                bool placed = false;
                for (int attempt = 0; attempt < 30 && !placed; ++attempt) {
                    const sf::Vector2f c = randIn();
                    if (!inside(c, m.hull)) continue;
                    float nearest = 1e9f;
                    for (const auto& v : m.hull) nearest = std::min(nearest, len({ v.x - c.x, v.y - c.y }));
                    if (nearest < hullR * 0.14f) continue;

                    float r = hullR * (0.07f + rng() * 0.10f) * (0.8f + dmg * 0.18f);
                    const int verts = 8 + static_cast<int>(rng() * 3);
                    for (int shrink = 0; shrink < 4 && !placed; ++shrink, r *= 0.75f) {
                        auto pts = makeHole(c, r, rng, verts);
                        bool fits = true;
                        for (size_t k = 0; k < pts.size() && fits; ++k) {
                            const auto& p0 = pts[k];
                            const auto& p1 = pts[(k + 1) % pts.size()];
                            fits = inside(p0, m.hull)
                                && inside({ (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f }, m.hull);
                        }
                        if (!fits) continue;
                        bool apart = true;
                        for (const auto& other : m.holes)
                            if (polyGap(pts, other.pts) < gap) { apart = false; break; }
                        // Shrinking cannot fix an overlap with a neighbour
                        // centred right here -- move on to a new spot.
                        if (!apart) break;
                        m.holes.push_back({ c, r, std::move(pts) });
                        placed = true;
                    }
                }
            }
        }

        // ---- SOOT ----------------------------------------------------------
        // Flat patches. Radius is ~0.6 of the lab's bloom radius: a gradient
        // only reads as dark in its inner half, so a flat patch of the full
        // radius would be twice the visual size.
        if (!m.pristine) {
            const int scorchCount = 2 + dmg * 2;
            for (int i = 0; i < scorchCount; ++i) {
                const Hole* h = (i < static_cast<int>(m.holes.size())) ? &m.holes[static_cast<size_t>(i)] : nullptr;
                sf::Vector2f c;
                if (h) c = { h->c.x + (rng() - 0.5f) * hullR * 0.3f, h->c.y + (rng() - 0.5f) * hullR * 0.3f };
                else   c = randIn();
                const float r = hullR * (0.14f + rng() * 0.22f) * (1.f + dmg * 0.14f) * 0.6f;
                m.scorch.push_back({ makeHole(c, r, rng, 7 + static_cast<int>(rng() * 3)),
                    (h ? 0.38f : 0.20f) + dmg * 0.05f });
            }
        }

        // ---- ARMOUR --------------------------------------------------------
        const float hullArea = std::fabs(enemyarch::geom::signedArea(m.hull));
        if (m.pristine) {
            m.plates = src.plates;
        }
        else {
            const float dropChance = (dmg == 1) ? 0.06f : 0.30f;
            for (const auto& pl : src.plates) {
                const sf::Vector2f pc = centroidOf(pl.pts);
                if (!inside(pc, m.hull)) continue;              // bitten away

                if (rng() < dropChance) {
                    // Gone. It leaves its own outline burnt into the hull,
                    // so the missing plate still reads as a plate.
                    m.scorch.push_back({ pl.pts, 0.40f });
                    continue;
                }
                // Only small plates peel. A full-length belt like the Barge's
                // dorsal spine hanging off the side reads as a second ship,
                // not as loose armour -- a plate that big stays or goes.
                const float plateArea = std::fabs(enemyarch::geom::signedArea(pl.pts));
                const bool peelable = plateArea < hullArea * 0.12f;
                if (peelable && dmg >= 2 && rng() < 0.16f) {
                    // Peeled: pushed outward from the keel and twisted about
                    // its own centre, so it hangs off the silhouette. The lab
                    // rotated about the hull origin, which on a long hull
                    // could fling a plate clean across the ship.
                    sf::Vector2f dir{ pc.x - cen.x, pc.y - cen.y };
                    const float dl = len(dir);
                    float ang = (dl > 1e-3f) ? std::atan2(dir.y, dir.x) : rng() * TAU;
                    ang += (rng() - 0.5f) * 1.2f;
                    const float dist = hullR * (0.10f + rng() * 0.20f);
                    const float rot = (rng() - 0.5f) * 1.6f;
                    const float cr = std::cos(rot), sr = std::sin(rot);
                    Plate p = pl;
                    for (auto& v : p.pts) {
                        const float x = v.x - pc.x, y = v.y - pc.y;
                        v = { pc.x + std::cos(ang) * dist + x * cr - y * sr,
                              pc.y + std::sin(ang) * dist + x * sr + y * cr };
                    }
                    m.peeled.push_back(std::move(p));
                }
                else {
                    m.plates.push_back(pl);
                }
            }
        }

        // ---- PITTING -------------------------------------------------------
        // The lab used a flat count on a canvas-filling ship. In game a Raider
        // is 30px and a Barge 80px, so count follows area or small hulls turn
        // to static. Radii are in PIXELS on purpose: sub-pixel pits vanish.
        if (!m.pristine) {
            const float areaK = std::clamp((hullR / 55.f) * (hullR / 55.f), 0.3f, 1.4f);
            const int pitCount = static_cast<int>((24 + dmg * 14) * areaK);
            for (int i = 0; i < pitCount; ++i) {
                for (int attempt = 0; attempt < 12; ++attempt) {
                    const sf::Vector2f p = randIn();
                    if (inside(p, m.hull)) {
                        m.pits.push_back({ p, 0.5f + rng() * 0.8f, 0.12f + rng() * 0.35f });
                        break;
                    }
                }
            }
        }

        return m;
    }

    // ========================================================================
    // BAKE -- colour it into one layered triangle list
    // ========================================================================

    /**
     * @param cold          The type's cold hull colour. Every shade derives from it.
     * @param outline       Silhouette stroke colour -- pass the one rocks use.
     * @param outlineWidth  Silhouette stroke width, px.
     * @return Triangles, colours and alpha baked, in draw order.
     */
    inline std::vector<sf::Vertex> bake(const Source& src, const Model& m, sf::Color cold,
        sf::Color outline, float outlineWidth)
    {
        using namespace detail;
        std::vector<sf::Vertex> out;
        if (m.hull.size() < 3) return out;

        const float u = src.unit;
        const int dmg = m.tier;
        const Clipper clip(m.hull);
        out.reserve(4096);

        // ---- 1. hull base --------------------------------------------------
        {
            const sf::Color c = hullColor(cold, dmg);
            for (const auto& v : clip.tris) out.push_back(sf::Vertex{ v, c });
        }

        // Every stroke is a shade of the hull's own colour, like scrap and
        // rocks. One helper so the whole wreck shares one palette.
        const sf::Color edgeLit = scale(cold, 1.55f);     // accent seams, lips, hardware
        const sf::Color edgeDim = scale(cold, 1.15f);     // ordinary seams
        const float thin = std::max(1.f, 0.9f * u);

        // Fill a (possibly concave) polygon, clipped to the hull.
        const auto fillClipped = [&](std::vector<sf::Vector2f> p, sf::Color c) {
            if (p.size() < 3) return;
            if (enemyarch::geom::signedArea(p) < 0.f) std::reverse(p.begin(), p.end());
            const auto tris = enemyarch::geom::triangulate(p);
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                std::vector<sf::Vector2f> t{ tris[i], tris[i + 1], tris[i + 2] };
                if (crossv(t[0], t[1], t[2]) < 0.f) std::swap(t[1], t[2]);
                clip.clipConvex(t, [&](const std::vector<sf::Vector2f>& piece) { fan(out, piece, c); });
            }
            };

        // ---- 2. armour plates, clipped to the (bitten) hull ---------------
        const float plateK = m.pristine ? 0.88f : 0.72f - dmg * 0.04f;
        for (const auto& pl : m.plates) {
            if (pl.pts.size() < 3) continue;
            fillClipped(pl.pts, scale(cold, pl.shade * plateK));
            for (size_t i = 0; i < pl.pts.size(); ++i)
                strokeClipped(out, clip, pl.pts[i], pl.pts[(i + 1) % pl.pts.size()], thin,
                    pl.accent ? edgeLit : edgeDim, false);
        }

        // ---- 3. soot: flat translucent patches -----------------------------
        for (const auto& s : m.scorch) fillClipped(s.pts, sf::Color(0, 0, 0, a8(s.a)));

        // ---- 4. micrometeorite pitting --------------------------------------
        for (const auto& pt : m.pits) {
            std::vector<sf::Vector2f> hex;
            for (int k = 0; k < 6; ++k) {
                const float a = k / 6.f * TAU;
                hex.push_back({ pt.c.x + std::cos(a) * pt.r, pt.c.y + std::sin(a) * pt.r });
            }
            fan(out, hex, sf::Color(0, 0, 0, a8(pt.a)));
        }

        // ---- 5. breaches: the void and its torn lip --------------------------
        for (const auto& h : m.holes) {
            fill(out, h.pts, sf::Color(2, 3, 5));
            loop(out, h.pts, thin, edgeLit, true);
        }

        // ---- 6. battle scars (damaged hulls only) --------------------------
        if (!m.pristine) {
            const float w = src.scarWidth * u;
            for (const auto& sc : src.scars)
                for (size_t k = 0; k + 1 < sc.size(); ++k)
                    strokeClipped(out, clip, sc[k], sc[k + 1], w, sf::Color(2, 3, 5, a8(0.82f)), true);
        }

        // ---- 7. peeled plates, hanging past the silhouette ----------------
        for (const auto& pl : m.peeled) {
            fill(out, pl.pts, scale(cold, 0.68f));
            loop(out, pl.pts, thin, edgeLit, true);
        }

        // ---- 8. dead thrusters -----------------------------------------------
        // Only where the hull still is: a nozzle floating past a bitten-off
        // engine block reads as a bug, not as damage.
        for (const auto& t : src.thrusters) {
            if (!inside(t, m.hull)) continue;
            const auto rect = [&](float hw, float hh) {
                return std::vector<sf::Vector2f>{
                    { t.x - hw * u, t.y - hh * u }, { t.x + hw * u, t.y - hh * u },
                    { t.x + hw * u, t.y + hh * u }, { t.x - hw * u, t.y + hh * u } };
                };
            const auto housing = rect(3.6f, 3.2f);
            fan(out, housing, scale(cold, 0.25f));
            loop(out, housing, thin, edgeLit, true);
            fan(out, rect(2.1f, 1.6f), sf::Color(2, 3, 5));                     // cold bore
        }

        // ---- 9. dead turrets, barrels drooping -------------------------------
        static const std::array<sf::Vector2f, 8> BARREL{ {
            { -3.f, 4.f }, { 3.f, 4.f }, { 3.f, -4.f }, { 1.f, -4.f },
            { 1.f, -12.f }, { -1.f, -12.f }, { -1.f, -4.f }, { -3.f, -4.f } } };
        for (const auto& tu : src.turrets) {
            if (!inside(tu, m.hull)) continue;
            std::vector<sf::Vector2f> ring;
            for (int k = 0; k < 14; ++k) {
                const float a = k / 14.f * TAU;
                ring.push_back({ tu.x + std::cos(a) * 5.2f * u, tu.y + std::sin(a) * 5.2f * u });
            }
            fan(out, ring, scale(cold, 0.25f));
            loop(out, ring, thin, edgeLit, true);

            // Slumped off its firing line; the tiny offset per mount keeps two
            // dead turrets from drooping in perfect unison.
            const float rot = 1.75f + (tu.x / std::max(0.01f, u)) * 0.02f;
            const float cr = std::cos(rot), sr = std::sin(rot);
            std::vector<sf::Vector2f> barrel;
            for (const auto& p : BARREL)
                barrel.push_back({ tu.x + (p.x * cr - p.y * sr) * u, tu.y + (p.x * sr + p.y * cr) * u });
            fill(out, barrel, scale(cold, 0.30f));
            loop(out, barrel, thin, edgeLit, true);
        }

        // ---- 10. hard outline ------------------------------------------------
        // The same colour and width every rock's `shape` stroke has, but baked
        // as capped quads: SFML's outline mitres sharp corners without limit,
        // and a bitten edge is all sharp corners -- it would throw spikes.
        loop(out, m.hull, outlineWidth, outline, true);
        return out;
    }

} // namespace wreckdetail