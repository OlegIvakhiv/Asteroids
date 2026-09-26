/**
 * @file CitadelModel.hpp
 * @brief The Rakshari Scrap Citadel, ported from the design lab page
 *        "rust citadel.html" (Redesign Study v2).
 *
 * ============================================================================
 * WHAT IS PORTED
 * ============================================================================
 * Everything the page draws, in the page's order, from the page's own seed
 * (0xC17ADE1) -- so the core outline, the seven craters, the five fissures and
 * the eight armour-ring segments are the exact shapes that were signed off:
 *
 *   spikes    ten perimeter blades, hot ones rimmed red, collar clamps
 *   masts     three tapered sensor towers: bracing, platform, dish, beacon
 *   chains    seven trophy chains -- interlocking links in two passes, rusty
 *             grey-brown, swaying and wobbling -- each ending in a trophy:
 *             chained asteroid, fighter, interceptor or barge wreck
 *   core      hollowed rock, gradient fill, craters and glowing fissures
 *             clipped to the rock
 *   maw       the furnace: pulsing heat bloom, hot inner throat, grate bars
 *   ring      eight bolted armour segments with hazard strips
 *   wear      dashed weld seams and rust streaks on the ring
 *   plates    six asymmetric hero slabs: shadow, hazard stripes, rivet grid,
 *             rim light, bolts
 *   hangar    launch bay with its breathing light and running lights
 *   turrets   twin batteries and point-defence mounts, barrels tracking
 *   lights    blinking hull lights
 *
 * The page's presentation-only pieces (backdrop, tactical grid, labels) are
 * not part of the model. Its two display toggles are: `glow` (the page's
 * "Furnace Glow" button) and `trophies` ("Trophy Chains"), both per prop in
 * zones.lua.
 *
 * ============================================================================
 * HOW IT IS DRAWN
 * ============================================================================
 * Static parts (spikes, masts, the rock, the ring, the plates -- about 80% of
 * the vertices) are baked ONCE by build(). frame() appends them, in order,
 * around the parts that move, so one call yields one painter-ordered triangle
 * list and the whole citadel is a single draw.
 *
 * Units are the page's canvas units, origin at the citadel's centre. The
 * caller scales it (ZoneSystem draws it through a render texture so the
 * landmark alpha applies to the finished image, not to every layer).
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/FieldGeom.hpp"
#include <vector>
#include <array>
#include <cmath>

namespace citadel {

    using fieldgeom::V2;
    using fieldgeom::Mesh;
    using fieldgeom::Stop;
    using fieldgeom::rgba;
    using fieldgeom::hex;
    using fieldgeom::TAU;
    using fieldgeom::PI;
    namespace fg = fieldgeom;

    // ---- palette (the page's C table) ----
    namespace C {
        inline const sf::Color rockDark = hex(0x0b0707), rockMid = hex(0x150e0e), rockEdge = hex(0x331a18);
        inline const sf::Color plateMid = hex(0x7f1d1d), plateRim = hex(0xef4444);
        inline const sf::Color scrapDark = hex(0x141416), scrapMid = hex(0x26262b);
        inline const sf::Color scrapLit = hex(0x3d3d45), scrapHi = hex(0x5a5a64);
        inline const sf::Color chain = hex(0x7a6a5c), chainDark = hex(0x241814);
        inline const sf::Color glow = hex(0xef4444), glowCore = hex(0xffd08a);
    }

    // ------------------------------------------------------------------
    // ctx.save / translate / rotate / scale, as a value
    // ------------------------------------------------------------------
    struct Xf {
        float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;   // x' = a x + c y + tx
        V2 operator()(V2 p) const { return { a * p.x + c * p.y + tx, b * p.x + d * p.y + ty }; }
        Xf translate(float x, float y) const { Xf r = *this; r.tx += a * x + c * y; r.ty += b * x + d * y; return r; }
        Xf rotate(float ang) const {
            const float cs = std::cos(ang), sn = std::sin(ang);
            Xf r = *this;
            r.a = a * cs + c * sn; r.b = b * cs + d * sn;
            r.c = -a * sn + c * cs; r.d = -b * sn + d * cs;
            return r;
        }
        Xf scale(float s) const { Xf r = *this; r.a *= s; r.b *= s; r.c *= s; r.d *= s; return r; }
    };
    inline void appendXf(Mesh& out, const Mesh& local, const Xf& x) {
        // No reserve() here: reserving exactly on every call defeats the
        // vector's geometric growth and turns ~400 small appends per frame
        // into ~400 reallocations. The caller reuses one mesh across frames.
        for (const auto& v : local.v) out.v.push_back(sf::Vertex{ x(v.position), v.color });
    }

    // ------------------------------------------------------------------
    // the page's little helpers
    // ------------------------------------------------------------------
    inline std::vector<V2> circlePts(V2 c, float r, int seg = 16) {
        auto p = fg::arcPts(c, r, r, 0.f, TAU, false, seg);
        p.pop_back();
        return p;
    }
    inline std::vector<V2> rectPts(float x, float y, float w, float h) {
        return { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    }
    inline void strokeRect(Mesh& m, float x, float y, float w, float h, float lw, sf::Color c) {
        fg::strokeLoop(m, rectPts(x, y, w, h), lw, c);
    }
    /// Fan from a centre point with per-vertex colour -- a gradient fill for a
    /// shape that is star-shaped about `c`.
    template <class F>
    inline void fanFill(Mesh& m, V2 c, const std::vector<V2>& pts, F colorAt) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const V2 a = pts[i], b = pts[(i + 1) % pts.size()];
            m.tri(c, a, b, colorAt(c), colorAt(a), colorAt(b));
        }
    }
    /// A radial glow disc: createRadialGradient(c,r0,c,r1) filled to r1.
    inline void glowDisc(Mesh& m, V2 c, float r0, float r1, const std::vector<Stop>& st, int seg = 20) {
        fg::radialGradient(m, c, r0, r1, r1, st, seg);
    }

    inline void boltsAlong(Mesh& m, const std::vector<V2>& pts, float spacing, float size, sf::Color col) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const V2 a = pts[i], b = pts[(i + 1) % pts.size()];
            const float L = fg::len(b - a);
            const int n = std::max(1, static_cast<int>(std::lround(L / spacing)));
            for (int k = 0; k < n; ++k) {
                const float t = (k + 0.5f) / n;
                fg::disc(m, a + (b - a) * t, size, col, 8);
            }
        }
    }

    inline void rivetGrid(Mesh& m, const std::vector<V2>& pts, float spacing, float size, sf::Color col) {
        const fg::Box bb = fg::boundsOf(pts);
        Mesh sq;
        for (float x = bb.minX; x < bb.maxX; x += spacing) {
            for (float y = bb.minY; y < bb.maxY; y += spacing) {
                const float ox = (static_cast<int>(y / spacing) % 2 != 0) ? spacing * 0.5f : 0.f;
                fg::rect(sq, x + ox, y, size, size, col);
            }
        }
        fg::clipMeshInto(m, sq, fg::makeClip(pts));
    }

    /// Paint `pts` in c1, then diagonal c2 bars of width w, clipped to `pts`.
    /// `alpha` is the page's globalAlpha around the call.
    inline void hazardStripes(Mesh& m, const std::vector<V2>& pts, sf::Color c1, sf::Color c2,
        float w, float alpha)
    {
        c1 = fg::withAlpha(c1, alpha);
        c2 = fg::withAlpha(c2, alpha);
        fg::fillPoly(m, pts, c1);
        const fg::Box bb = fg::boundsOf(pts);
        const float span = (bb.w() + bb.h()) * 1.5f;
        Mesh bars;
        for (float i = -span; i < span; i += w * 2.1f)
            fg::strokeLine(bars, { bb.minX + i, bb.minY - 4.f },
                { bb.minX + i + bb.h() + 8.f, bb.maxY + 4.f }, w, c2);
        fg::clipMeshInto(m, bars, fg::makeClip(pts));
    }

    /// Tapering rust run: a trapezoid, gradient 0 / .55 / 1 down its length.
    inline void rustStreak(Mesh& m, float x, float y, float len, float wid, float alpha) {
        const sf::Color c0 = rgba(140, 60, 20, alpha), c1 = rgba(100, 40, 14, alpha * 0.45f),
            c2 = rgba(60, 24, 8, 0.f);
        const V2 tl{ x - wid * 0.5f, y }, tr{ x + wid * 0.5f, y };
        const V2 bl{ x - wid * 0.15f, y + len }, br{ x + wid * 0.15f, y + len };
        const V2 ml = tl + (bl - tl) * 0.55f, mr = tr + (br - tr) * 0.55f;
        m.tri(tl, tr, mr, c0, c0, c1); m.tri(tl, mr, ml, c0, c1, c1);
        m.tri(ml, mr, br, c1, c1, c2); m.tri(ml, br, bl, c1, c2, c2);
    }

    // ==================================================================
    // THE MODEL
    // ==================================================================
    class Model {
    public:
        Model() { build(); }

        /// Largest distance from the centre anything reaches, animation
        /// included, in page units. Sizes the render texture.
        float extent() const { return m_extent; }

        /**
         * @brief Append one frame of the citadel, painter-ordered, to `out`.
         * @param t        seconds -- drives every sway, pulse and blink
         * @param glowOn   the page's "Furnace Glow"
         * @param trophies the page's "Trophy Chains"
         */
        void frame(float t, bool glowOn, bool trophies, Mesh& out) const {
            out.append(m_spikesMasts);
            beacons(out, t);
            if (trophies) {
                for (size_t i = 0; i < CHAINS.size(); ++i) {
                    const auto& ch = CHAINS[i];
                    const float seed = static_cast<float>(i + 1);
                    drawChain(out, t, ch.x1, ch.y1, ch.x2, ch.y2, ch.sag, seed);
                    drawTrophy(out, t, glowOn, ch.kind, ch.x2, ch.y2 + 14.f, 0.4f, static_cast<int>(i + 1));
                }
            }
            out.append(m_coreBase);
            for (const auto& f : m_fissureGlow) {
                const float a = 0.22f + 0.12f * std::sin(t * 2.f + f.phase);
                for (const auto& v : f.mesh.v) {
                    sf::Vertex q = v;
                    q.color.a = fg::u8(v.color.a * a);
                    out.v.push_back(q);
                }
            }
            out.append(m_coreEdge);
            maw(out, t, glowOn);
            out.append(m_armour);
            hangar(out, t, glowOn);
            for (const auto& tr : TURRETS) turret(out, t, glowOn, tr);
            lights(out, t);
        }

    private:
        // ---------------- data from the page ----------------
        struct Seg { std::vector<V2> pts; bool hazard; float shade, streakSeed; };
        struct Hero { std::vector<V2> pts; bool hazard; float shade; };
        struct Spike { float a, base, len, w; bool hot; };
        struct Turret { float x, y, s, rot; int barrels; bool big; };
        struct Mast { float a, r, h, phase; };
        enum class Trophy { Rock, Fighter, Interceptor, Barge };
        struct Chain { float x1, y1, x2, y2, sag; Trophy kind; };
        struct Glow { Mesh mesh; float phase; };

        inline static const std::vector<Hero> HERO = {
            { { { -152, -62 }, { -118, -140 }, { -52, -158 }, { -30, -104 }, { -86, -54 } }, true,  1.05f },
            { { { 16, -158 }, { 78, -140 }, { 124, -96 }, { 72, -66 }, { 16, -92 } },        false, 0.85f },
            { { { 132, -50 }, { 166, -6 }, { 152, 58 }, { 104, 48 }, { 98, -10 } },          false, 1.15f },
            { { { 112, 74 }, { 78, 126 }, { 22, 152 }, { 26, 100 }, { 74, 62 } },            true,  0.9f },
            { { { -88, 108 }, { -128, 52 }, { -152, -14 }, { -104, -12 }, { -84, 56 } },     false, 1.1f },
            { { { -40, 156 }, { -96, 132 }, { -72, 84 }, { -14, 96 } },                      false, 0.8f },
        };
        inline static const std::vector<Spike> SPIKES = {
            { -2.25f, 128, 120, 24, false }, { -1.75f, 142, 158, 30, true },
            { -1.30f, 150, 108, 22, false }, { -0.72f, 158, 176, 34, true },
            { -0.18f, 152, 128, 26, false }, { 0.42f, 156, 150, 30, true },
            { 0.96f, 146, 118, 24, false },  { 1.52f, 150, 162, 32, true },
            { 2.05f, 138, 126, 26, false },  { 2.62f, 130, 148, 28, true },
        };
        inline static const std::vector<Turret> TURRETS = {
            { -66, -104, 1.35f, -0.62f, 2, true }, { 78, -46, 1.20f, 0.34f, 2, true },
            { -104, 42, 0.90f, 1.10f, 2, false },  { 58, 98, 0.85f, 2.30f, 2, false },
            { 0, 128, 0.70f, 3.05f, 2, false },    { -36, -34, 0.62f, -1.30f, 1, false },
        };
        inline static const std::vector<Mast> MASTS = {
            { -0.42f, 50, 160, 0.0f }, { -1.55f, 45, 150, 1.7f }, { 1.65f, 50, 155, 3.1f },
        };
        inline static const std::vector<Chain> CHAINS = {
            { 70, -25, 250, -110, 42, Trophy::Interceptor },
            { 55, 55, 220, 175, 42, Trophy::Fighter },
            { -10, 75, -55, 285, 46, Trophy::Barge },
            { -70, 60, -200, 200, 46, Trophy::Rock },
            { -80, -20, -280, 60, 46, Trophy::Fighter },
            { -40, -75, -20, -285, 38, Trophy::Rock },
            { 55, -75, 175, -215, 38, Trophy::Interceptor },
        };
        inline static const std::vector<V2> LIGHTS = {
            { -120, -70 }, { 110, -80 }, { 148, 20 }, { -40, 140 }, { 60, 120 }, { -96, 60 }, { 10, -150 },
        };

        std::vector<V2> m_core;
        std::vector<Seg> m_ring;
        Mesh m_spikesMasts, m_coreBase, m_coreEdge, m_armour;
        std::vector<Glow> m_fissureGlow;
        Mesh m_linkAligned, m_linkPerp;
        float m_extent = 360.f;

        // ---------------- build: everything that never moves ----------------
        void build() {
            // CORE and RING come from the page's seeded RNG, in its order.
            fg::Rng RNG(0xC17ADE1u);
            const int n = 15;
            for (int i = 0; i < n; ++i) {
                const float a = (i / static_cast<float>(n)) * TAU - PI / 2.f;
                const float r = 132.f * (0.86f + RNG() * 0.26f);
                m_core.push_back(fg::pol(r, a));
            }
            std::vector<std::vector<V2>> craters;
            for (int i = 0; i < 7; ++i) {
                const float a = RNG() * TAU, d = RNG() * 85.f, r = 14.f + RNG() * 26.f;
                const V2 cc{ std::cos(a) * d, std::sin(a) * d };
                std::vector<V2> cv;
                const int vn = 8 + static_cast<int>(RNG() * 3.f);
                for (int k = 0; k < vn; ++k) {
                    const float aa = (k / static_cast<float>(vn)) * TAU;
                    const float rr = r * (0.72f + RNG() * 0.5f);
                    cv.push_back(cc + V2(std::cos(aa) * rr, std::sin(aa) * rr));
                }
                craters.push_back(std::move(cv));
            }
            std::vector<std::vector<V2>> fissures;
            for (int i = 0; i < 5; ++i) {
                const float a0 = RNG() * TAU;
                const V2 p0 = fg::pol(30.f + RNG() * 60.f, a0);
                std::vector<V2> seg;
                float x = p0.x, y = p0.y, a = a0 + (RNG() - 0.5f) * 0.6f;
                seg.push_back({ x, y });
                for (int k = 0; k < 5; ++k) {
                    a += (RNG() - 0.5f) * 0.7f;
                    x += std::cos(a) * (12.f + RNG() * 16.f);
                    y += std::sin(a) * (12.f + RNG() * 16.f);
                    seg.push_back({ x, y });
                }
                fissures.push_back(std::move(seg));
            }
            for (int i = 0; i < 8; ++i) {
                const float a = (i / 8.f) * TAU - PI / 2.f + 0.09f;
                const float half = (PI / 8.f) * 0.76f;
                const float rIn = 68.f + (RNG() - 0.5f) * 8.f;
                const float rOut = 146.f + (RNG() - 0.5f) * 20.f;
                Seg s;
                s.pts = { fg::pol(rIn, a - half), fg::pol(rIn, a + half),
                          fg::pol(rOut, a + half * 1.05f), fg::pol(rOut, a - half * 1.05f) };
                s.hazard = (i == 1 || i == 4 || i == 6);
                s.shade = 0.75f + RNG() * 0.5f;
                s.streakSeed = RNG();
                m_ring.push_back(std::move(s));
            }

            // ---- spikes ----
            for (size_t i = 0; i < SPIKES.size(); ++i) {
                const auto& s = SPIKES[i];
                const float v = 0.7f + ((i * 37) % 11) / 11.f * 0.6f;
                spike(m_spikesMasts, s.base - 30.f * v, s.a, s.len + 30.f * v, s.w, s.hot);
            }
            // ---- masts (the beacons blink, so they are drawn per frame) ----
            for (const auto& mst : MASTS) mastStatic(m_spikesMasts, mst);

            // ---- core ----
            {
                Mesh& m = m_coreBase;
                std::vector<V2> sh = m_core;
                for (auto& p : sh) p += V2(7.f, 9.f);
                fg::fillPoly(m, sh, rgba(0, 0, 0, 0.62f));

                const fg::Box bb = fg::boundsOf(m_core);
                const V2 g0{ bb.minX, bb.minY }, gd{ bb.maxX * 0.5f - bb.minX, bb.maxY - bb.minY };
                const float gl = fg::dot(gd, gd);
                const std::vector<Stop> st{ { 0.f, C::rockMid }, { 0.5f, C::rockDark }, { 1.f, hex(0x060303) } };
                fanFill(m, V2(), m_core, [&](V2 p) { return fg::sample(st, fg::dot(p - g0, gd) / gl); });

                const fg::ClipRegion clip = fg::makeClip(m_core);
                Mesh in;
                for (const auto& cv : craters) {
                    fg::fillPoly(in, cv, rgba(2, 1, 1, 0.9f));
                    fg::strokeLoop(in, cv, 1.1f, rgba(90, 44, 40, 0.30f));
                }
                for (const auto& f : fissures)
                    fg::strokePath(in, f, false, 3.4f, rgba(2, 0, 0, 0.95f), true, 2.f);
                fg::clipMeshInto(m, in, clip);

                // Glow lines are clipped once, at full alpha; frame() only
                // rescales their alpha.
                for (const auto& f : fissures) {
                    Mesh g, gc;
                    fg::strokePath(g, f, false, 1.2f, rgba(180, 40, 20, 1.f), true, 2.f);
                    fg::clipMeshInto(gc, g, clip);
                    m_fissureGlow.push_back({ std::move(gc), f[0].x });
                }

                fg::strokeLoop(m_coreEdge, m_core, 2.4f, C::rockEdge);
                fg::strokeLoop(m_coreEdge, m_core, 1.0f, rgba(180, 80, 60, 0.14f));
            }

            // ---- ring, wear, hero plates ----
            {
                Mesh& m = m_armour;
                for (const auto& seg : m_ring) {
                    const V2 cen = fg::centroid(seg.pts);
                    const float rimAlpha = std::max(0.10f, 1.f - fg::len(cen) / 220.f);
                    fg::fillPoly(m, seg.pts, rgba(std::round(58 * seg.shade), std::round(14 * seg.shade), std::round(14 * seg.shade)));
                    fg::strokeLoop(m, seg.pts, 2.2f, rgba(255, 90, 40, rimAlpha * 0.55f));
                    fg::strokeLoop(m, seg.pts, 1.0f, rgba(10, 3, 3, 0.9f));
                    if (seg.hazard) {
                        const fg::Box bb = fg::boundsOf(seg.pts);
                        const std::vector<V2> strip{ { bb.minX + 8, bb.maxY - 26 }, { bb.maxX - 8, bb.maxY - 26 },
                                                     { bb.maxX - 8, bb.maxY - 10 }, { bb.minX + 8, bb.maxY - 10 } };
                        hazardStripes(m, strip, rgba(60, 14, 10, 0.9f), rgba(230, 190, 60, 0.75f), 6.f, 0.42f);
                    }
                    boltsAlong(m, seg.pts, 26.f, 2.0f, rgba(150, 150, 160, 0.55f));
                    boltsAlong(m, seg.pts, 26.f, 1.1f, rgba(230, 230, 240, 0.35f));
                }
                for (const auto& seg : m_ring) {
                    for (size_t i = 0; i < seg.pts.size(); ++i)
                        fg::dashLine(m, seg.pts[i], seg.pts[(i + 1) % seg.pts.size()], 2.f,
                            rgba(20, 8, 6, 0.85f), 2.5f, 1.8f);
                    V2 low = seg.pts[0];
                    for (const auto& p : seg.pts) if (p.y > low.y) low = p;
                    const fg::Box nb = fg::boundsOf(seg.pts);
                    rustStreak(m, low.x, low.y - 2.f, nb.h() * 0.55f + 20.f,
                        4.f + seg.streakSeed * 5.f, 0.30f + seg.streakSeed * 0.25f);
                }
                for (const auto& pl : HERO) {
                    const float dist = fg::len(fg::centroid(pl.pts));
                    std::vector<V2> sh = pl.pts;
                    for (auto& p : sh) p += V2(5.f, 6.f);
                    fg::fillPoly(m, sh, rgba(0, 0, 0, 0.55f));
                    fg::fillPoly(m, pl.pts, rgba(std::round(72 * pl.shade), std::round(17 * pl.shade), std::round(16 * pl.shade)));
                    if (pl.hazard)
                        hazardStripes(m, pl.pts, rgba(40, 10, 8, 0.85f), rgba(235, 190, 60, 0.65f), 7.f, 0.5f);
                    rivetGrid(m, pl.pts, 15.f, 1.5f, rgba(120, 110, 110, 0.35f));
                    fg::strokeLoop(m, pl.pts, 2.0f, rgba(255, 90, 45, std::max(0.12f, 1.f - dist / 260.f) * 0.6f));
                    fg::strokeLoop(m, pl.pts, 1.2f, rgba(6, 2, 2, 0.95f));
                    boltsAlong(m, pl.pts, 34.f, 2.4f, rgba(160, 160, 170, 0.5f));
                }
            }

            // ---- chain link templates, in link-local space ----
            linkTemplate(m_linkAligned, 4.2f, 1.9f);
            linkTemplate(m_linkPerp, 2.4f, 1.0f);

            // ---- extent: measure a real frame, both toggles on ----
            Mesh probe;
            frame(0.f, true, true, probe);
            float e = 0.f;
            for (const auto& v : probe.v) e = std::max(e, std::max(std::fabs(v.position.x), std::fabs(v.position.y)));
            m_extent = e + 24.f;   // headroom for sway and bob
        }

        static void spike(Mesh& m, float base, float angle, float len, float width, bool hot) {
            const V2 b = fg::pol(base, angle);
            const V2 p{ -std::sin(angle), std::cos(angle) };
            const V2 tip = b + fg::pol(len, angle);
            const std::vector<V2> tri{ b + p * (width * 0.5f), tip, b - p * (width * 0.5f) };
            fg::fillPoly(m, tri, C::scrapDark);
            fg::strokeLoop(m, tri, 1.6f, hot ? C::plateRim : C::plateMid);
            fg::fillPoly(m, { b + p * (width * 0.5f), tip, b + p * (width * 0.06f) },
                hot ? rgba(200, 40, 40, 0.55f) : rgba(120, 20, 20, 0.4f));
            Mesh col;
            fg::rect(col, -4.f, -width * 0.62f, 9.f, width * 1.24f, C::scrapMid);
            strokeRect(col, -4.f, -width * 0.62f, 9.f, width * 1.24f, 1.2f, C::plateMid);
            appendXf(m, col, Xf().translate(b.x, b.y).rotate(angle));
        }

        static Xf mastXf(const Mast& mst) {
            const V2 b = fg::pol(mst.r, mst.a);
            return Xf().translate(b.x, b.y).rotate(mst.a + PI / 2.f);
        }

        static void mastStatic(Mesh& out, const Mast& mst) {
            Mesh m;
            const float baseW = 5.f, topW = 2.2f, h = mst.h;
            const std::vector<V2> shaft{ { -baseW, 0 }, { baseW, 0 }, { topW, -h }, { -topW, -h } };
            fg::fillPoly(m, shaft, hex(0x242428));
            fg::strokeLoop(m, shaft, 1.2f, C::plateMid);
            fg::strokeLine(m, { 0, 0 }, { 0, -h }, 0.7f, rgba(80, 80, 90, 0.5f));
            for (float y = -10.f; y > -h + 12.f; y -= 14.f) {
                const float w1 = baseW + (topW - baseW) * (-y / h);
                const float w2 = baseW + (topW - baseW) * (-(y - 14.f) / h);
                fg::strokeLine(m, { -w1, y }, { w2, y - 14.f }, 1.f, rgba(120, 120, 130, 0.55f));
                fg::strokeLine(m, { w1, y }, { -w2, y - 14.f }, 1.f, rgba(120, 120, 130, 0.55f));
            }
            fg::rect(m, -baseW - 1.5f, -3.f, (baseW + 1.5f) * 2.f, 6.f, hex(0x1a1a1e));
            strokeRect(m, -baseW - 1.5f, -3.f, (baseW + 1.5f) * 2.f, 6.f, 1.f, C::plateMid);
            fg::rect(m, -topW - 2.f, -h - 2.f, (topW + 2.f) * 2.f, 4.f, C::scrapLit);
            strokeRect(m, -topW - 2.f, -h - 2.f, (topW + 2.f) * 2.f, 4.f, 1.f, C::plateMid);

            const float dishY = -h - 6.f;
            const auto dish = fg::arcPts({ 0, dishY }, 7.f, 7.f, PI * 0.15f, PI * 0.85f, true, 20);
            fg::fillPoly(m, dish, rgba(38, 38, 48, 0.85f));
            fg::strokePath(m, dish, false, 1.8f, C::scrapHi);
            fg::strokePath(m, fg::arcPts({ 0, dishY }, 3.4f, 3.4f, PI * 0.2f, PI * 0.8f, true, 14),
                false, 1.f, rgba(160, 160, 175, 0.6f));
            fg::strokeLine(m, { 0, dishY }, { 0, dishY + 4.f }, 0.9f, rgba(180, 180, 190, 0.6f));
            appendXf(out, m, mastXf(mst));
        }

        static void beacons(Mesh& out, float t) {
            for (const auto& mst : MASTS) {
                Mesh m;
                const float beaconY = -mst.h - 13.f;
                const float blink = std::fabs(std::sin(t * 2.2f + mst.phase));
                if (blink > 0.55f)
                    glowDisc(m, { 0, beaconY }, 0.f, 14.f, { { 0.f, rgba(255, 70, 60, blink) }, { 1.f, rgba(255, 40, 20, 0.f) } });
                fg::disc(m, { 0, beaconY }, 2.2f, blink > 0.55f ? hex(0xff5a4a) : hex(0x4a1410));
                appendXf(out, m, mastXf(mst));
            }
        }

        // ---------------- chains ----------------
        static void linkTemplate(Mesh& m, float a, float b) {
            auto ell = [](V2 c, float rx, float ry) {
                // 10 segments: a link is ~6px on screen; more is invisible
                // and the seven chains are most of the citadel's vertices.
                auto p = fg::arcPts(c, rx, ry, 0.f, TAU, false, 10);
                p.pop_back();
                return p;
                };
            fg::strokeLoop(m, ell({ 1.f, 1.f }, a, b), 4.0f, rgba(0, 0, 0, 0.9f));        // shadow
            fg::strokeLoop(m, ell({ 0, 0 }, a, b), 3.4f, C::chainDark);                    // body
            fg::strokeLoop(m, ell({ 0, 0 }, a, b), 2.0f, C::chain);                        // mid tone
            fg::strokeLoop(m, ell({ 0, 0 }, a - 0.4f, b - 0.4f), 0.9f, rgba(200, 170, 140, 0.7f));
            fg::strokePath(m, fg::arcPts({ -0.2f, -0.3f }, a * 0.82f, b * 0.82f, PI * 1.15f, PI * 1.72f, false, 6),
                false, 0.6f, rgba(240, 210, 180, 0.9f));                                   // specular
            fg::strokeLoop(m, ell({ 0, 0 }, a * 0.55f, b * 0.5f), 0.6f, rgba(0, 0, 0, 0.55f)); // the hole
        }

        void drawChain(Mesh& out, float t, float x1, float y1, float x2, float y2, float sag, float seed) const {
            const float cx = (x1 + x2) * 0.5f;
            const float cy = (y1 + y2) * 0.5f + sag;
            const float globalSway = std::sin(t * 0.7f + seed * 1.7f) * 0.075f
                + std::sin(t * 1.35f + seed * 0.4f) * 0.04f;
            const float sagPulse = 1.f + std::sin(t * 0.55f + seed) * 0.045f;

            constexpr int N = 140;
            std::array<V2, N + 1> P;
            for (int i = 0; i <= N; ++i) {
                const float u = i / static_cast<float>(N), mu = 1.f - u;
                P[i] = { mu * mu * x1 + 2 * mu * u * cx + u * u * x2,
                         mu * mu * y1 + 2 * mu * u * (cy * sagPulse) + u * u * y2 };
            }
            std::array<float, N + 1> cum;
            cum[0] = 0.f;
            for (int i = 1; i <= N; ++i) cum[i] = cum[i - 1] + fg::len(P[i] - P[i - 1]);
            const float total = cum[N];

            auto posAt = [&](float d, V2& pos, float& ang) {
                if (d <= 0.f) { pos = P[0]; ang = std::atan2(P[1].y - P[0].y, P[1].x - P[0].x); return; }
                if (d >= total) { pos = P[N]; ang = std::atan2(P[N].y - P[N - 1].y, P[N].x - P[N - 1].x); return; }
                int lo = 0, hi = N;
                while (lo < hi - 1) { const int mid = (lo + hi) >> 1; if (cum[mid] < d) lo = mid; else hi = mid; }
                const float f = cum[hi] > cum[lo] ? (d - cum[lo]) / (cum[hi] - cum[lo]) : 0.f;
                pos = P[lo] + (P[hi] - P[lo]) * f;
                ang = std::atan2(P[hi].y - P[lo].y, P[hi].x - P[lo].x);
                };

            const int numLinks = std::max(10, static_cast<int>(std::lround(total / 4.2f)));
            const float step = total / numLinks;
            auto wob = [&](int i) {
                return globalSway + std::sin(t * 1.9f + i * 0.7f + seed * 2.5f) * 0.09f
                    + std::sin(t * 3.1f + i * 1.3f + seed) * 0.04f;
                };

            // Pass 1: perpendicular links, behind. Pass 2: aligned, in front.
            for (int pass = 0; pass < 2; ++pass) {
                const Mesh& tpl = pass == 0 ? m_linkPerp : m_linkAligned;
                for (int i = pass == 0 ? 1 : 0; i < numLinks; i += 2) {
                    V2 pos; float ang;
                    posAt((i + 0.5f) * step, pos, ang);
                    if (pass == 0) ang += PI / 2.f;
                    appendXf(out, tpl, Xf().translate(pos.x, pos.y).rotate(ang + wob(i)));
                }
            }

            Mesh m;
            fg::disc(m, { x1, y1 }, 6.f, hex(0x1a1a1e), 16);                  // anchor plate
            fg::strokeLoop(m, circlePts({ x1, y1 }, 6.f), 1.8f, C::scrapHi);
            fg::disc(m, { x1, y1 }, 3.f, hex(0x0a0505), 12);
            fg::disc(m, { x2, y2 }, 4.5f, C::chainDark, 14);                  // end shackle
            fg::strokeLoop(m, circlePts({ x2, y2 }, 4.5f), 1.6f, C::chain);
            fg::disc(m, { x2, y2 }, 2.f, hex(0x0a0505), 10);
            out.append(m);
        }

        // ---------------- trophies ----------------
        static void drawTrophy(Mesh& out, float t, bool glowOn, Trophy kind, float x, float y, float ang, int seed) {
            const float swing = std::sin(t * 0.55f + seed * 1.3f) * 0.16f;
            const float bob = std::cos(t * 0.5f + seed * 1.7f) * 2.f;
            Mesh m;
            switch (kind) {
            case Trophy::Rock:        rockTrophy(m, t, glowOn, seed); break;
            case Trophy::Fighter:     fighterWreck(m, t, glowOn, seed); break;
            case Trophy::Interceptor: interceptorWreck(m, t, glowOn, seed); break;
            default:                  bargeWreck(m, t, glowOn, seed); break;
            }
            appendXf(out, m, Xf().translate(x, y + bob).rotate(ang + swing));
        }

        static void poly(Mesh& m, std::vector<V2> pts, sf::Color fill, sf::Color line, float lw) {
            fg::fillPoly(m, pts, fill);
            fg::strokeLoop(m, pts, lw, line);
        }

        static void rockTrophy(Mesh& m, float t, bool glowOn, int seed) {
            fg::Rng R(static_cast<uint32_t>(seed * 1000 + 42));
            std::vector<V2> pts;
            for (int i = 0; i < 9; ++i) {
                const float a = (i / 9.f) * TAU;
                const float r = 9.f + R() * 9.f;
                pts.push_back({ std::cos(a) * r, std::sin(a) * r });
            }
            std::vector<V2> sh = pts;
            for (auto& p : sh) p += V2(2.5f, 3.5f);
            fg::fillPoly(m, sh, rgba(0, 0, 0, 0.55f));
            poly(m, pts, hex(0x1a1010), rgba(120, 50, 40, 0.55f), 1.3f);
            fg::disc(m, { -2.5f, 1.5f }, 3.2f, rgba(0, 0, 0, 0.6f));
            fg::disc(m, { 3.5f, -2.5f }, 2.2f, rgba(0, 0, 0, 0.6f));
            fg::disc(m, { 1.f, 4.f }, 1.8f, rgba(0, 0, 0, 0.6f));
            if (glowOn) {
                const float pulse = 0.35f + 0.30f * std::sin(t * 2.2f + seed * 3.f);
                fg::strokePath(m, { { -4, -4 }, { -1, -1 }, { -3, 3 } }, false, 1.3f, rgba(255, 90, 40, pulse));
                glowDisc(m, { 0, 0 }, 1.f, 14.f, { { 0.f, rgba(255, 90, 40, pulse * 0.35f) }, { 1.f, rgba(180, 30, 10, 0.f) } });
            }
        }

        static void fighterWreck(Mesh& m, float t, bool glowOn, int seed) {
            poly(m, { { 0, -15 }, { 5, -7 }, { 7, 5 }, { 4, 13 }, { -4, 13 }, { -7, 5 }, { -5, -7 } },
                hex(0x1a0d0c), rgba(200, 60, 40, 0.55f), 1.3f);
            const sf::Color pl = rgba(90, 40, 30, 0.5f);
            fg::strokeLine(m, { -4, -3 }, { 4, -3 }, 0.6f, pl);
            fg::strokeLine(m, { -5, 5 }, { 5, 5 }, 0.6f, pl);
            fg::strokeLine(m, { -3, -9 }, { 3, -9 }, 0.6f, pl);
            poly(m, { { -2.5f, -10 }, { 2.5f, -10 }, { 2, -5 }, { -2, -5 } }, hex(0x080404), rgba(120, 80, 70, 0.5f), 0.7f);
            poly(m, { { -5, -5 }, { -13, -9 }, { -14, -3 }, { -6, -1 } }, hex(0x150a0a), rgba(180, 50, 30, 0.45f), 0.9f);
            poly(m, { { 5, -4 }, { 10, -2 }, { 11, 3 }, { 6, 2 } }, hex(0x150a0a), rgba(180, 50, 30, 0.45f), 0.9f);
            poly(m, { { -3, 13 }, { 3, 13 }, { 4, 17 }, { -4, 17 } }, C::scrapDark, rgba(120, 120, 130, 0.55f), 0.8f);
            if (glowOn) {
                const float flicker = 0.3f + 0.5f * std::fabs(std::sin(t * 6.f + seed * 4.f));
                glowDisc(m, { 0, 18 }, 0.f, 9.f, { { 0.f, rgba(255, 110, 50, flicker * 0.55f) }, { 1.f, rgba(150, 30, 10, 0.f) } }, 14);
            }
            const float blink = std::fabs(std::sin(t * 2.5f + seed * 5.f));
            if (blink > 0.6f) {
                glowDisc(m, { 0, -12 }, 0.f, 6.f, { { 0.f, rgba(255, 80, 60, (blink - 0.6f) * 2.5f) }, { 1.f, rgba(255, 40, 20, 0.f) } }, 12);
                fg::disc(m, { 0, -12 }, 1.3f, hex(0xff6a4a), 8);
            }
        }

        static void interceptorWreck(Mesh& m, float t, bool glowOn, int seed) {
            poly(m, { { 0, -19 }, { 4, -10 }, { 7, -2 }, { 10, 4 }, { 6, 12 }, { 2, 14 },
                      { -2, 14 }, { -6, 12 }, { -10, 4 }, { -7, -2 }, { -4, -10 } },
                hex(0x180c0b), rgba(210, 70, 50, 0.55f), 1.3f);
            poly(m, { { 0, -19 }, { 2.5f, -12 }, { -2.5f, -12 } }, hex(0x0e0606), rgba(120, 50, 40, 0.4f), 0.7f);
            poly(m, { { -2, -10 }, { 2, -10 }, { 1.8f, -5 }, { -1.8f, -5 } }, hex(0x080404), rgba(130, 90, 70, 0.5f), 0.7f);
            poly(m, { { -10, 4 }, { -16, 0 }, { -18, 6 }, { -12, 8 } }, hex(0x150a0a), rgba(180, 50, 30, 0.45f), 0.9f);
            poly(m, { { 10, 4 }, { 16, 0 }, { 18, 6 }, { 12, 8 } }, hex(0x150a0a), rgba(180, 50, 30, 0.45f), 0.9f);
            for (float ex : { -3.5f, 3.5f })
                poly(m, { { ex - 1.8f, 14 }, { ex + 1.8f, 14 }, { ex + 2.2f, 18 }, { ex - 2.2f, 18 } },
                    C::scrapDark, rgba(120, 120, 130, 0.55f), 0.7f);
            if (glowOn) {
                const float g1 = 0.4f + 0.3f * std::sin(t * 4.f + seed);
                const float g2 = std::fabs(std::sin(t * 9.f + seed * 3.f)) > 0.5f
                    ? 0.5f + 0.4f * std::fabs(std::sin(t * 12.f + seed)) : 0.08f;
                const float gs[2] = { g1, g2 };
                const float xs[2] = { -3.5f, 3.5f };
                for (int k = 0; k < 2; ++k)
                    glowDisc(m, { xs[k], 19 }, 0.f, 7.f, { { 0.f, rgba(255, 120, 60, gs[k] * 0.7f) }, { 1.f, rgba(180, 30, 10, 0.f) } }, 12);
            }
            const V2 lp[3] = { { -8, 3 }, { 8, 3 }, { 0, -14 } };
            for (int i = 0; i < 3; ++i) {
                const float blink = std::fabs(std::sin(t * 2.f + i * 1.7f + seed));
                if (blink > 0.65f) fg::disc(m, lp[i], 1.2f, rgba(255, 80, 60, blink), 8);
            }
        }

        static void bargeWreck(Mesh& m, float t, bool glowOn, int seed) {
            poly(m, { { 0, -24 }, { 9, -18 }, { 13, -6 }, { 14, 8 }, { 8, 20 },
                      { -8, 20 }, { -14, 8 }, { -13, -6 }, { -9, -18 } },
                hex(0x1c0f0e), rgba(210, 70, 50, 0.5f), 1.3f);
            const std::vector<std::vector<V2>> crates{
                { { -6, -22 }, { 0, -22 }, { 0, -14 }, { -6, -14 } },
                { { 1, -22 }, { 7, -22 }, { 7, -15 }, { 1, -15 } },
                { { -3, -14 }, { 3, -14 }, { 3, -7 }, { -3, -7 } } };
            for (size_t i = 0; i < crates.size(); ++i) {
                const float sh = 0.5f + std::fmod(i * 0.17f, 0.5f);
                poly(m, crates[i], rgba(std::round(60 * sh + 30), std::round(25 * sh + 15), std::round(20 * sh + 10)),
                    rgba(200, 80, 50, 0.5f), 0.9f);
            }
            poly(m, { { -5, -6 }, { 5, -6 }, { 4, -1 }, { -4, -1 } }, hex(0x080404), rgba(140, 100, 80, 0.55f), 0.8f);
            poly(m, { { -7, 20 }, { 7, 20 }, { 8, 26 }, { -8, 26 } }, C::scrapDark, rgba(130, 130, 140, 0.6f), 0.9f);
            if (glowOn) {
                for (float ex : { -3.5f, 3.5f }) {
                    const float flicker = 0.35f + 0.35f * std::fabs(std::sin(t * 3.f + seed + ex));
                    glowDisc(m, { ex, 28 }, 0.f, 11.f, { { 0.f, rgba(255, 140, 60, flicker * 0.7f) }, { 1.f, rgba(150, 30, 10, 0.f) } }, 14);
                }
            }
            rustStreak(m, -4, 14, 12, 2.5f, 0.4f);
            rustStreak(m, 5, -2, 10, 2, 0.35f);
        }

        // ---------------- the furnace ----------------
        static void maw(Mesh& m, float t, bool glowOn) {
            const float pulse = 0.55f + 0.45f * std::sin(t * 2.4f);
            const float pulse2 = 0.5f + 0.5f * std::sin(t * 6.1f + 1.3f);
            if (glowOn)
                glowDisc(m, { 0, 0 }, 6.f, 120.f, {
                    { 0.f,   rgba(255, 110, 40, 0.40f + 0.16f * pulse) },
                    { 0.35f, rgba(210, 50, 20, 0.20f + 0.08f * pulse) },
                    { 1.f,   rgba(120, 10, 0, 0.f) } }, 32);
            const std::vector<V2> outer{ { -48, -22 }, { -14, -46 }, { 30, -36 }, { 50, 4 }, { 18, 42 }, { -26, 36 }, { -50, 12 } };
            fg::fillPoly(m, outer, hex(0x2a0705));
            fg::strokeLoop(m, outer, 2.4f, glowOn ? C::glow : hex(0x5a1414));
            if (!glowOn) return;

            const std::vector<V2> inner{ { -28, -12 }, { -6, -28 }, { 20, -20 }, { 30, 4 }, { 10, 26 }, { -16, 20 } };
            Mesh heat;
            glowDisc(heat, { 0, 0 }, 2.f, 40.f, {
                { 0.f, rgba(255, 220, 150, 0.85f + 0.15f * pulse2) },
                { 0.4f, rgba(255, 120, 40, 0.70f + 0.20f * pulse) },
                { 1.f, rgba(180, 30, 10, 0.35f) } }, 24);
            fg::clipMeshInto(m, heat, fg::makeClip(inner));
            fg::strokeLoop(m, inner, 1.6f, C::glowCore);

            Mesh bars;
            for (float x = -50.f; x < 55.f; x += 14.f)
                fg::strokeLine(bars, { x, -50 }, { x + 12, 50 }, 3.f, rgba(20, 4, 2, 0.85f));
            fg::clipMeshInto(m, bars, fg::makeClip(outer));
        }

        static void hangar(Mesh& out, float t, bool glowOn) {
            Mesh m;
            const std::vector<V2> shell{ { -30, -22 }, { 30, -20 }, { 34, 22 }, { -28, 24 } };
            poly(m, shell, hex(0x150708), C::plateMid, 2.f);
            const V2 inner[4] = { { -22, -15 }, { 22, -14 }, { 25, 15 }, { -21, 17 } };
            if (glowOn) {
                const std::vector<Stop> st{ { 0.f, rgba(255, 140, 60, 0.30f + 0.12f * std::sin(t * 1.8f)) },
                                            { 1.f, rgba(120, 20, 5, 0.35f) } };
                fg::fillQuadGradient(m, inner, [&](V2 p) { return fg::sample(st, (p.y + 15.f) / 32.f); }, 2);
            }
            else {
                fg::fillPoly(m, { inner[0], inner[1], inner[2], inner[3] }, hex(0x0a0405));
            }
            fg::strokeLoop(m, { inner[0], inner[1], inner[2], inner[3] }, 1.2f, rgba(255, 110, 50, 0.4f));
            for (int i = 0; i < 5; ++i) {
                const float lx = -18.f + i * 9.f;
                const float blink = 0.35f + 0.65f * std::fabs(std::sin(t * 2.6f - i * 0.6f));
                fg::disc(m, { lx, 14 }, 1.5f, glowOn ? rgba(255, 200, 120, blink) : rgba(80, 70, 60, 0.5f), 8);
            }
            boltsAlong(m, shell, 12.f, 1.6f, rgba(170, 170, 180, 0.5f));
            appendXf(out, m, Xf().translate(112, 24).rotate(0.28f));
        }

        static void turret(Mesh& out, float t, bool glowOn, const Turret& tr) {
            Mesh m;
            fg::disc(m, { 0, 0 }, 15.f, C::scrapMid, 20);
            fg::strokeLoop(m, circlePts({ 0, 0 }, 15.f, 20), 2.f, C::plateMid);
            for (int i = 0; i < 8; ++i)
                fg::disc(m, fg::pol(11.5f, (i / 8.f) * TAU), 1.4f, rgba(180, 180, 190, 0.55f), 8);
            const Xf base = Xf().translate(tr.x, tr.y).rotate(tr.rot).scale(tr.s);
            appendXf(out, m, base);

            Mesh g;
            const float bw = tr.big ? 4.6f : 3.2f, bl = tr.big ? 34.f : 24.f;
            const float offs2[2] = { -4.6f, 4.6f };
            const int nb = tr.barrels == 2 ? 2 : 1;
            for (int k = 0; k < nb; ++k) {
                const float o = nb == 2 ? offs2[k] : 0.f;
                fg::rect(g, o - bw / 2.f, -bl, bw, bl, C::scrapDark);
                strokeRect(g, o - bw / 2.f, -bl, bw, bl, 1.f, C::plateMid);
                fg::rect(g, o - bw / 2.f - 1.2f, -bl - 1.f, bw + 2.4f, 4.f, C::scrapLit);
                if (tr.big && glowOn)
                    glowDisc(g, { o, -bl - 2.f }, 0.f, 9.f, {
                        { 0.f, rgba(255, 150, 60, 0.35f + 0.2f * std::sin(t * 4.f + o)) },
                        { 1.f, rgba(255, 60, 10, 0.f) } }, 12);
            }
            const std::vector<V2> body{ { -9, 6 }, { 9, 6 }, { 7, -10 }, { -7, -10 } };
            poly(g, body, C::plateMid, C::plateRim, 1.3f);
            boltsAlong(g, body, 7.f, 1.1f, rgba(200, 200, 210, 0.5f));
            appendXf(out, g, base.rotate(std::sin(t * 0.55f + tr.x * 0.03f) * 0.35f));
        }

        static void lights(Mesh& m, float t) {
            for (size_t i = 0; i < LIGHTS.size(); ++i) {
                const V2 p = LIGHTS[i];
                const float b = std::fabs(std::sin(t * 1.6f + i * 1.9f));
                if (b > 0.6f)
                    glowDisc(m, p, 0.f, 14.f, { { 0.f, rgba(255, 80, 60, (b - 0.6f) * 2.f) }, { 1.f, rgba(255, 40, 20, 0.f) } }, 14);
                fg::disc(m, p, 1.8f, b > 0.6f ? hex(0xff6a4a) : hex(0x401410), 8);
            }
        }
    };

} // namespace citadel
