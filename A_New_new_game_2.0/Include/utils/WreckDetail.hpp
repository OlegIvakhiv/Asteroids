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
 * hull read as a DEAD SHIP is everything the design lab layers on top:
 *
 *   BITES     torn edges -- vertices pulled toward the centre, so a wing or
 *             sponson is visibly missing
 *   BREACHES  holes punched through to space, with a torn lip that catches
 *             starlight and a soot halo bleeding onto the plating
 *   SOOT      cold scorch blooms -- no embers, nothing glowing
 *   ARMOUR    the ship's own plates: attached, missing (soot where they were)
 *             or peeled half off and hanging past the silhouette
 *   HARDWARE  dead nozzles and a drooping turret -- the unit still reads as
 *             the unit it was
 *   PITTING   micrometeorite dust on anything that has been out here a while
 *
 * ============================================================================
 * DAMAGE TIERS
 * ============================================================================
 *   0  TURNED OFF   pristine, powered down. Indistinguishable from a live ship
 *                   running dark -- which is exactly what an ambusher needs.
 *   1  LIGHT        one bite, one breach, plating mostly attached
 *   2  HEAVY        torn sections, missing and peeled plates
 *   3  DESTROYED    deep bites, most plating gone, five breaches
 *
 * ============================================================================
 * WHAT IS DIFFERENT FROM THE HTML LAB, AND WHY
 * ============================================================================
 *  - NO CANVAS CLIP. The lab clipped soot, halos and plates to the hull with
 *    ctx.clip(). Here every overlay triangle is intersected with the hull's
 *    own triangulation (Sutherland-Hodgman, triangle vs triangle), so nothing
 *    bleeds past the silhouette onto the starfield. Done once at spawn.
 *
 *  - GRADIENTS ARE RINGS. createRadialGradient becomes concentric rings, one
 *    per gradient stop, with the colour at every clipped vertex evaluated
 *    from the same stops. Piecewise-linear along the radius, exactly like the
 *    canvas stops it replaces.
 *
 *  - THE STARLIGHT RIM IS NOT HERE. It depends on which way the hull faces
 *    the light, and wrecks spin, so baking it would spin the sun with them.
 *    RenderSystem draws it per frame in world space (drawColdRim).
 *
 *  - NO FLOATING DEBRIS FIELD. Baked geometry rotates rigidly with the body,
 *    so an orbiting debris cloud would turn into a spinning halo. Needs its
 *    own tiny system if it is wanted.
 *
 *  - NO DROP SHADOW, NO TILT, NO DRIFT. Box2D moves the body; nothing else in
 *    the game casts a shadow.
 *
 * ============================================================================
 * OUTPUT
 * ============================================================================
 * Everything lands in ONE triangle list with colours (and alpha) baked in,
 * layered in draw order. Strokes are thin quads rather than sf::Lines so they
 * can sit UNDER later layers -- a plate seam must disappear into a breach,
 * not be drawn across it. The result goes straight into
 * RenderComponent::detailTris and rides the existing detail path: world
 * transform and hit flash included.
 *
 * All geometry is in PIXELS, local to the body. `Source::unit` is pixels per
 * authored hull unit; widths and hardware sizes from the lab are multiplied
 * by it so a scaled hull keeps its proportions.
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
    struct Blot { sf::Vector2f c; float r = 0.f; float a = 0.f; };
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
            /// inside and no hull edge crossing it. Most soot and halo cells
            /// are, and splitting them against every hull triangle anyway
            /// multiplied a Barge's vertex count by five for nothing.
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
        /// joints in a loop close up; left off for translucent glints, where
        /// the overlap would show as bright dots at every corner.
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

        // ---- radial gradients ----------------------------------------------

        struct Stop { float t; float a; sf::Color c; };

        /// Colour of a radial gradient at distance `dist`. Stops are over
        /// [r0, r1]; inside r0 the first stop holds, past r1 the last.
        template <size_t N>
        inline sf::Color gradAt(const std::array<Stop, N>& s, float r0, float r1, float dist) {
            const float t = std::clamp((dist - r0) / std::max(1e-4f, r1 - r0), 0.f, 1.f);
            size_t i = 0;
            while (i + 1 < N && t > s[i + 1].t) ++i;
            if (i + 1 >= N) return sf::Color(s[N - 1].c.r, s[N - 1].c.g, s[N - 1].c.b, a8(s[N - 1].a));
            const float u = std::clamp((t - s[i].t) / std::max(1e-4f, s[i + 1].t - s[i].t), 0.f, 1.f);
            const auto L = [&](std::uint8_t x, std::uint8_t y) {
                return static_cast<std::uint8_t>(x + (static_cast<float>(y) - x) * u);
                };
            return sf::Color(L(s[i].c.r, s[i + 1].c.r), L(s[i].c.g, s[i + 1].c.g),
                L(s[i].c.b, s[i + 1].c.b), a8(s[i].a + (s[i + 1].a - s[i].a) * u));
        }

        /// A radial gradient disc, clipped to the hull. One ring per stop, so
        /// the interpolation between rings IS the gradient.
        template <size_t N>
        inline void radial(std::vector<sf::Vertex>& out, const Clipper& clip,
            sf::Vector2f c, float r0, float r1, const std::array<Stop, N>& stops, int seg = 14)
        {
            std::vector<float> radii;
            radii.push_back(0.f);
            for (const auto& s : stops) {
                const float r = r0 + s.t * (r1 - r0);
                if (r > radii.back() + 0.05f) radii.push_back(r);
            }
            const auto ring = [&](size_t ri, int k) {
                const float a = (k % seg) / static_cast<float>(seg) * TAU;
                return sf::Vector2f{ c.x + std::cos(a) * radii[ri], c.y + std::sin(a) * radii[ri] };
                };
            const auto emitPiece = [&](const std::vector<sf::Vector2f>& piece) {
                std::uint8_t maxA = 0;
                std::vector<sf::Color> cols;
                cols.reserve(piece.size());
                for (const auto& v : piece) {
                    cols.push_back(gradAt(stops, r0, r1, len({ v.x - c.x, v.y - c.y })));
                    maxA = std::max(maxA, cols.back().a);
                }
                if (maxA < 2) return;                       // invisible, skip it
                for (size_t i = 1; i + 1 < piece.size(); ++i) {
                    out.push_back(sf::Vertex{ piece[0], cols[0] });
                    out.push_back(sf::Vertex{ piece[i], cols[i] });
                    out.push_back(sf::Vertex{ piece[i + 1], cols[i + 1] });
                }
                };
            for (size_t ri = 1; ri < radii.size(); ++ri) {
                for (int k = 0; k < seg; ++k) {
                    std::vector<sf::Vector2f> cell;
                    if (ri == 1) cell = { c, ring(1, k), ring(1, k + 1) };
                    else cell = { ring(ri - 1, k), ring(ri, k), ring(ri, k + 1), ring(ri - 1, k + 1) };
                    if (enemyarch::geom::signedArea(cell) < 0.f) std::reverse(cell.begin(), cell.end());
                    clip.clipConvex(cell, emitPiece);
                }
            }
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
        m.tier = std::clamp(tier, 0, 3);
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
            const int count = dmg;
            const int gap = (dmg == 3) ? 3 : 2;
            std::vector<Bite> bites;
            const auto idx = pickIndices(n, count, rng, gap);
            for (size_t i = 0; i < idx.size(); ++i) {
                Bite b{ idx[i], 1, 0.f };
                if (dmg == 1) { b.radius = 1; b.depth = 0.22f + rng() * 0.16f; }
                else if (dmg == 2) { b.radius = (i == 0) ? 2 : 1; b.depth = 0.44f + rng() * 0.22f; }
                else {
                    b.radius = (i == 0) ? 3 : 2;
                    b.depth = (i == 0) ? 0.88f + rng() * 0.10f : 0.55f + rng() * 0.30f;
                }
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
        if (!m.pristine) {
            const int holeCount = dmg == 1 ? 1 : dmg == 2 ? 3 : 5;
            for (int i = 0; i < holeCount; ++i) {
                sf::Vector2f c;
                bool ok = false;
                for (int attempt = 0; attempt < 40 && !ok; ++attempt) {
                    c = randIn();
                    ok = inside(c, m.hull);
                    if (ok) {
                        float nearest = 1e9f;
                        for (const auto& v : m.hull) nearest = std::min(nearest, len({ v.x - c.x, v.y - c.y }));
                        if (nearest < hullR * 0.14f) ok = false;
                    }
                }
                if (!ok) continue;

                const float rBase = hullR * (0.07f + rng() * 0.10f);
                float r = (dmg == 3 && i == 0) ? rBase * 1.9f : rBase * (0.8f + dmg * 0.18f);
                const int verts = 8 + static_cast<int>(rng() * 3);

                // The lab's clip hid a breach that overhung the edge. Here
                // it would draw a void out in space, so it has to fit: shrink
                // until every lip vertex is on the hull.
                for (int shrink = 0; shrink < 4; ++shrink) {
                    auto pts = makeHole(c, r, rng, verts);
                    bool fits = true;
                    for (size_t k = 0; k < pts.size() && fits; ++k) {
                        const auto& a = pts[k];
                        const auto& b = pts[(k + 1) % pts.size()];
                        fits = inside(a, m.hull) && inside({ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f }, m.hull);
                    }
                    if (fits) { m.holes.push_back({ c, r, std::move(pts) }); break; }
                    r *= 0.75f;
                }
            }
        }

        // ---- SOOT ----------------------------------------------------------
        if (!m.pristine) {
            const int scorchCount = 2 + dmg * 2;
            for (int i = 0; i < scorchCount; ++i) {
                const Hole* h = (i < static_cast<int>(m.holes.size())) ? &m.holes[static_cast<size_t>(i)] : nullptr;
                sf::Vector2f c;
                if (h) c = { h->c.x + (rng() - 0.5f) * hullR * 0.6f, h->c.y + (rng() - 0.5f) * hullR * 0.6f };
                else   c = randIn();
                m.scorch.push_back({ c,
                    hullR * (0.14f + rng() * 0.22f) * (1.f + dmg * 0.14f),
                    (h ? 0.55f : 0.25f) + dmg * 0.08f });
            }
        }

        // ---- ARMOUR --------------------------------------------------------
        const float hullArea = std::fabs(enemyarch::geom::signedArea(m.hull));
        if (m.pristine) {
            m.plates = src.plates;
        }
        else {
            const float dropChance = dmg == 1 ? 0.06f : dmg == 2 ? 0.30f : 0.62f;
            for (const auto& pl : src.plates) {
                const sf::Vector2f pc = centroidOf(pl.pts);
                if (!inside(pc, m.hull)) continue;              // bitten away

                if (rng() < dropChance) {                       // gone: soot where it sat
                    m.scorch.push_back({ pc, hullR * 0.28f, 0.45f });
                    continue;
                }
                // Only small plates peel. A full-length belt like the Barge's
                // dorsal spine hanging off the side reads as a second ship,
                // not as loose armour -- a plate that big stays or goes.
                const float plateArea = std::fabs(enemyarch::geom::signedArea(pl.pts));
                const bool peelable = plateArea < hullArea * 0.12f;
                if (peelable && dmg >= 2 && rng() < (dmg == 3 ? 0.38f : 0.16f)) {
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
     * @param cold  The type's cold hull colour. Every shade derives from it.
     * @return Triangles, colours and alpha baked, in draw order.
     */
    inline std::vector<sf::Vertex> bake(const Source& src, const Model& m, sf::Color cold) {
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

        // ---- 2. armour plates, clipped to the (bitten) hull ---------------
        const float plateK = m.pristine ? 0.88f : 0.72f - dmg * 0.04f;
        for (const auto& pl : m.plates) {
            if (pl.pts.size() < 3) continue;
            const sf::Color c = scale(cold, pl.shade * plateK);
            std::vector<sf::Vector2f> p = pl.pts;
            if (enemyarch::geom::signedArea(p) < 0.f) std::reverse(p.begin(), p.end());
            const auto ptris = enemyarch::geom::triangulate(p);
            for (size_t i = 0; i + 2 < ptris.size(); i += 3) {
                std::vector<sf::Vector2f> t{ ptris[i], ptris[i + 1], ptris[i + 2] };
                if (crossv(t[0], t[1], t[2]) < 0.f) std::swap(t[1], t[2]);
                clip.clipConvex(t, [&](const std::vector<sf::Vector2f>& piece) { fan(out, piece, c); });
            }
            const sf::Color edge = pl.accent ? sf::Color(150, 170, 200, a8(0.26f))
                : sf::Color(90, 95, 105, a8(0.16f));
            for (size_t i = 0; i < p.size(); ++i)
                strokeClipped(out, clip, p[i], p[(i + 1) % p.size()], std::max(1.f, 0.9f * u), edge, false);
        }

        // ---- 3. micrometeorite pitting --------------------------------------
        for (const auto& pt : m.pits) {
            std::vector<sf::Vector2f> hex;
            for (int k = 0; k < 6; ++k) {
                const float a = k / 6.f * TAU;
                hex.push_back({ pt.c.x + std::cos(a) * pt.r, pt.c.y + std::sin(a) * pt.r });
            }
            fan(out, hex, sf::Color(0, 0, 0, a8(pt.a)));
        }

        // ---- 4. cold scorch blooms ----------------------------------------
        for (const auto& s : m.scorch) {
            const std::array<Stop, 4> stops{ {
                { 0.00f, s.a,         sf::Color(2, 2, 3) },
                { 0.45f, s.a * 0.55f, sf::Color(5, 5, 8) },
                { 0.78f, s.a * 0.20f, sf::Color(8, 9, 13) },
                { 1.00f, 0.f,         sf::Color(8, 9, 13) } } };
            radial(out, clip, s.c, 0.f, s.r, stops);
        }

        // ---- 5. breaches -----------------------------------------------------
        for (const auto& h : m.holes) {
            // soft dark halo bleeding onto the surrounding plating
            const std::array<Stop, 4> halo{ {
                { 0.00f, 0.94f, sf::Color(0, 0, 0) },
                { 0.42f, 0.58f, sf::Color(6, 7, 10) },
                { 0.78f, 0.20f, sf::Color(10, 12, 17) },
                { 1.00f, 0.00f, sf::Color(10, 12, 17) } } };
            radial(out, clip, h.c, h.r * 0.4f, h.r * 2.6f, halo);

            // interior haze: depth, where the torn lip is wider than the void
            const std::array<Stop, 3> haze{ {
                { 0.00f, 0.55f, sf::Color(28, 32, 40) },
                { 0.70f, 0.20f, sf::Color(12, 14, 18) },
                { 1.00f, 0.00f, sf::Color(0, 0, 0) } } };
            radial(out, clip, h.c, 0.f, h.r * 0.9f, haze, 12);

            // the void itself
            fill(out, h.pts, sf::Color(2, 3, 5));

            // dark inner shadow, then the cold glint on the torn lip on top
            loop(out, h.pts, 2.6f * u, sf::Color(0, 0, 0, a8(0.55f)), true);
            loop(out, h.pts, std::max(1.f, 1.0f * u), sf::Color(160, 182, 215, a8(0.34f)), false);
        }

        // ---- 6. battle scars (damaged hulls only) --------------------------
        if (!m.pristine) {
            const float w = src.scarWidth * u;
            for (const auto& sc : src.scars) {
                for (size_t k = 0; k + 1 < sc.size(); ++k)
                    strokeClipped(out, clip, sc[k], sc[k + 1], w, sf::Color(2, 3, 5, a8(0.82f)), true);
                for (size_t k = 0; k + 1 < sc.size(); ++k)
                    strokeClipped(out, clip, sc[k], sc[k + 1], std::max(0.8f, w * 0.4f),
                        sf::Color(140, 158, 185, a8(0.16f)), false);
            }
        }

        // ---- 7. peeled plates, hanging past the silhouette ----------------
        for (const auto& pl : m.peeled) {
            fill(out, pl.pts, scale(cold, 0.68f));
            loop(out, pl.pts, std::max(1.f, 0.9f * u), sf::Color(140, 158, 185, a8(0.32f)), false);
        }

        // ---- 8. hull outline: dark, cold -------------------------------------
        // The starlit rim goes on top of this at draw time (RenderSystem).
        loop(out, m.hull, 2.4f * u, sf::Color(4, 5, 8, a8(0.92f)), true);

        // ---- 9. dead thrusters -----------------------------------------------
        // Only where the hull still is: a nozzle floating past a bitten-off
        // engine block reads as a bug, not as damage.
        for (const auto& t : src.thrusters) {
            if (!inside(t, m.hull)) continue;
            const auto rect = [&](float hw, float hh, float ox = 0.f) {
                return std::vector<sf::Vector2f>{
                    { t.x + (ox - hw) * u, t.y - hh * u }, { t.x + (ox + hw) * u, t.y - hh * u },
                    { t.x + (ox + hw) * u, t.y + hh * u }, { t.x + (ox - hw) * u, t.y + hh * u } };
                };
            const auto housing = rect(3.6f, 3.2f);
            fan(out, housing, sf::Color(13, 14, 18));
            loop(out, housing, std::max(0.8f, 0.8f * u), sf::Color(120, 132, 150, a8(0.45f)), false);
            fan(out, rect(2.1f, 1.6f), sf::Color(2, 3, 5));                         // cold bore
            fan(out, rect(0.55f, 1.5f, -1.45f), sf::Color(150, 172, 205, a8(0.10f))); // starlight on the lip
        }

        // ---- 10. dead turrets, barrels drooping ------------------------------
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
            fan(out, ring, sf::Color(14, 16, 20));
            loop(out, ring, std::max(1.f, u), sf::Color(120, 132, 150, a8(0.42f)), false);

            // Slumped off its firing line; the tiny offset per mount keeps two
            // dead turrets from drooping in perfect unison.
            const float rot = 1.75f + (tu.x / std::max(0.01f, u)) * 0.02f;
            const float cr = std::cos(rot), sr = std::sin(rot);
            std::vector<sf::Vector2f> barrel;
            for (const auto& p : BARREL)
                barrel.push_back({ tu.x + (p.x * cr - p.y * sr) * u, tu.y + (p.x * sr + p.y * cr) * u });
            fill(out, barrel, sf::Color(18, 20, 23));
            loop(out, barrel, std::max(0.9f, 0.9f * u), sf::Color(140, 152, 172, a8(0.40f)), false);
            for (int k = 0; k < 3; ++k)                                             // cold glint on the breech
                stroke(out, barrel[static_cast<size_t>(k)], barrel[static_cast<size_t>(k + 1)],
                    std::max(0.6f, 0.5f * u), sf::Color(170, 190, 220, a8(0.28f)), false);
        }

        return out;
    }

} // namespace wreckdetail