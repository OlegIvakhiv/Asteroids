/**
 * @file FieldObjectModels.hpp
 * @brief The "Cold Field Objects" designs, ported: asteroids, authored scrap
 *        clusters, and the Rakshari unstable core.
 *
 * ============================================================================
 * SOURCE OF TRUTH
 * ============================================================================
 * design lab page "scrap and asteroids.html". Each builder below is that
 * page's build*() function followed by its draw*() function, in the same
 * order, with the same RNG draws in the same sequence, so one seed gives the
 * same object in the lab and in the game. If a piece of art looks wrong in
 * game, change it in the lab first, then here -- never the other way round.
 *
 * Geometry is built in the DESIGN's own units, then scaled once to the rolled
 * pixel radius. Every line weight in the page is authored in those units, so
 * this is what keeps the weights proportional to the object instead of
 * drifting as sizes change. Mesh::minWidth is set to one screen pixel in
 * design units, so a 0.4 hairline on a small rock still exists on screen.
 *
 *   ASTEROID       hull, darker inner shell, facet chords, crevices, fracture
 *                  lines, all clipped to the hull, then the edge. No craters:
 *                  the design removed them on purpose.
 *                  Animated: ore hints flicker (drawn per frame by RenderSystem).
 *
 *   SCRAP CLUSTER  a hand-authored template per size (scrap_templates in
 *                  asteroids.lua), with per-object tint, age, rivet density,
 *                  strut brightness and a few percent of jitter -- recognisable
 *                  as the same designed object, never an identical stamp.
 *                  Animated: rivets shimmer, a rare glint catches a corner.
 *
 *   UNSTABLE CORE  armoured containment vessel: a hot core under six angled
 *                  plates, bracing struts, two open vents. Rakshari-only
 *                  alternative to the magmatic rock. The magmatic rock itself
 *                  is NOT built here and is untouched.
 *                  Animated: the whole vessel breathes orange.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/FieldGeom.hpp"
#include <sol/sol.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>

namespace fieldmodel {

    using fieldgeom::V2;
    using fieldgeom::Mesh;
    using fieldgeom::Rng;
    using fieldgeom::rgba;
    using fieldgeom::hex;
    using fieldgeom::TAU;
    using fieldgeom::PI;

    /// A per-frame decoration: an ore fleck or a rivet. Pixel units.
    struct Fx {
        V2    p;
        float r = 1.f;      ///< radius (ore) or square size (rivet)
        float phase = 0.f;  ///< the design's per-item phase term
    };

    /// Everything a field object needs, already scaled to pixels.
    struct Model {
        std::vector<V2>         outline;   ///< visual silhouette (shape points)
        std::vector<V2>         physics;   ///< convex, <= 8 points -- Box2D
        std::vector<sf::Vertex> tris;      ///< baked painter-ordered triangles
        std::vector<Fx>         fx;        ///< ores (rock) or rivets (scrap)
        std::vector<V2>         glintSpots;///< scrap: corners a glint may land on
        float scale = 1.f;                 ///< design unit -> pixel
        float glowRadius = 0.f;            ///< unstable core pulse radius
        float phase = 0.f;                 ///< per-object pulse phase
        sf::Color debris{ 60, 58, 56 };    ///< what fracture shards are cut from
        bool ok() const { return outline.size() >= 3 && physics.size() >= 3 && !tris.empty(); }
    };

    // ------------------------------------------------------------------
    // shared finish: scale into pixels, derive the physics hull
    // ------------------------------------------------------------------
    inline void finish(Model& md, Mesh& m, std::vector<V2> outlineDesign, float k) {
        fieldgeom::transform(m, k, 0.f, V2());
        md.tris = std::move(m.v);
        for (auto& p : outlineDesign) p *= k;
        md.outline = std::move(outlineDesign);
        md.physics = enemyarch::geom::decimateConvex(enemyarch::geom::convexHull(md.outline), 8);
        md.scale = k;
    }

    // ==================================================================
    // ASTEROID
    // ==================================================================

    struct RockParams {
        float designR = 46.f;   ///< the lab's cfg.r for this size class
        int   verts = 12;
        int   crevices = 5;
        int   facets = 9;
        int   fractures = 6;
        float oreChance = 0.45f;
    };

    inline Model buildAsteroid(float pixelRadius, const RockParams& P, uint32_t seed) {
        Model md;
        Rng rng(seed);
        const float R = std::max(1.f, P.designR);
        const int   n = std::clamp(P.verts, 5, 32);
        const float k = pixelRadius / R;

        std::vector<V2> hull;
        for (int i = 0; i < n; ++i) {
            const float a = (i / static_cast<float>(n)) * TAU + (rng() - 0.5f) * 0.22f;
            const float r = R * (0.72f + rng() * 0.42f);
            hull.push_back({ std::cos(a) * r, std::sin(a) * r });
        }

        std::vector<V2> shell;
        for (int i = 0; i < n; ++i) {
            const V2 p = hull[static_cast<size_t>(i)];
            const float kk = 0.55f + rng() * 0.12f;
            const float jitter = (rng() - 0.5f) * R * 0.06f;
            shell.push_back({ p.x * kk + jitter, p.y * kk + jitter });
        }

        struct Chord { V2 a, b; float alpha; };
        std::vector<Chord> facets;
        for (int i = 0; i < P.facets; ++i) {
            const int a = static_cast<int>(std::floor(rng() * n));
            const int b = (a + 3 + static_cast<int>(std::floor(rng() * std::max(1, n - 5)))) % n;
            if (a == b) continue;
            facets.push_back({ hull[static_cast<size_t>(a)], hull[static_cast<size_t>(b)], 0.10f + rng() * 0.18f });
        }

        struct Crevice { std::vector<V2> pts; float width; };
        std::vector<Crevice> crevices;
        for (int i = 0; i < P.crevices; ++i) {
            const V2 start = hull[static_cast<size_t>(std::floor(rng() * n)) % hull.size()];
            const float dir = std::atan2(start.y, start.x) + PI + (rng() - 0.5f) * 0.9f;
            const float len = R * (0.4f + rng() * 0.45f);
            const int segs = 4 + static_cast<int>(std::floor(rng() * 3));
            Crevice c;
            c.pts.push_back(start);
            float px = start.x, py = start.y, a = dir;
            for (int s = 0; s < segs; ++s) {
                a += (rng() - 0.5f) * 0.6f;
                const float step = len / segs;
                px += std::cos(a) * step;
                py += std::sin(a) * step;
                c.pts.push_back({ px, py });
            }
            c.width = R * (0.015f + rng() * 0.025f);
            crevices.push_back(std::move(c));
        }

        struct Frac { V2 from, to; };
        std::vector<Frac> fractures;
        for (int i = 0; i < P.fractures; ++i) {
            const V2 impact{ (rng() - 0.5f) * R * 0.8f, (rng() - 0.5f) * R * 0.8f };
            const float a = rng() * TAU;
            const float len = R * (0.25f + rng() * 0.45f);
            fractures.push_back({ impact, { impact.x + std::cos(a) * len, impact.y + std::sin(a) * len } });
        }

        if (rng() < P.oreChance) {
            const int cnt = 3 + static_cast<int>(std::floor(rng() * 5));
            for (int i = 0; i < cnt; ++i) {
                const float a = rng() * TAU;
                const float d = rng() * R * 0.65f;
                Fx o;
                o.p = { std::cos(a) * d, std::sin(a) * d };
                o.r = 0.6f + rng() * 1.2f;
                o.phase = o.p.x * 0.05f;          // the design's `o.x * 0.05`, design units
                o.p *= k;
                o.r = std::max(0.7f, o.r * k);
                md.fx.push_back(o);
            }
        }

        // ---------------- draw ----------------
        Mesh m;
        m.minWidth = 1.f / k;

        fieldgeom::fillPoly(m, hull, rgba(36, 34, 33));
        fieldgeom::fillPoly(m, shell, rgba(0, 0, 0, 0.30f));

        Mesh in;
        in.minWidth = m.minWidth;
        for (const auto& f : facets) {
            fieldgeom::strokeLine(in, f.a, f.b, 1.1f, rgba(0, 0, 0, f.alpha));
            fieldgeom::strokeLine(in, { f.a.x, f.a.y - 0.8f }, { f.b.x, f.b.y - 0.8f },
                0.5f, rgba(180, 175, 168, f.alpha * 0.45f));
        }
        for (const auto& c : crevices) {
            fieldgeom::strokePath(in, c.pts, false, c.width, rgba(0, 0, 0, 0.88f), true);
            std::vector<V2> lit = c.pts;
            for (auto& p : lit) p.y -= 0.6f;
            fieldgeom::strokePath(in, lit, false, c.width * 0.35f, rgba(110, 105, 100, 0.35f), true);
        }
        for (const auto& f : fractures) {
            fieldgeom::strokeLine(in, f.from, f.to, 0.9f, rgba(0, 0, 0, 0.7f));
            fieldgeom::strokeLine(in, { f.from.x, f.from.y - 0.5f }, { f.to.x, f.to.y - 0.5f },
                0.4f, rgba(150, 145, 138, 0.22f));
        }
        fieldgeom::clipMeshInto(m, in, fieldgeom::makeClip(hull));

        fieldgeom::strokeLoop(m, hull, 1.4f, rgba(150, 145, 140, 0.55f));

        md.debris = rgba(36, 34, 33);
        finish(md, m, hull, k);
        return md;
    }

    // ==================================================================
    // SCRAP CLUSTER -- authored templates
    // ==================================================================

    struct TplChunk {
        std::vector<V2> body;
        sf::Color fill, stroke;
    };
    struct Template {
        std::vector<TplChunk> chunks;
        std::vector<std::pair<V2, V2>> struts;
        std::vector<std::vector<V2>> gaps;
        bool valid() const { return !chunks.empty(); }
    };

    /// "#rrggbb" or { r=, g=, b= } -> colour. Anything else -> `def`.
    inline sf::Color readColor(const sol::object& o, sf::Color def) {
        if (o.is<std::string>()) {
            std::string s = o.as<std::string>();
            if (!s.empty() && s[0] == '#') s.erase(0, 1);
            if (s.size() == 6) {
                try { return hex(static_cast<uint32_t>(std::stoul(s, nullptr, 16))); }
                catch (...) {}
            }
            return def;
        }
        if (o.is<sol::table>()) {
            sol::table t = o.as<sol::table>();
            return sf::Color(
                static_cast<std::uint8_t>(std::clamp(t["r"].get_or(static_cast<int>(def.r)), 0, 255)),
                static_cast<std::uint8_t>(std::clamp(t["g"].get_or(static_cast<int>(def.g)), 0, 255)),
                static_cast<std::uint8_t>(std::clamp(t["b"].get_or(static_cast<int>(def.b)), 0, 255)));
        }
        return def;
    }

    inline std::vector<V2> readPts(const sol::table& t) {
        std::vector<V2> out;
        for (size_t i = 1; i <= t.size(); ++i) {
            sol::optional<sol::table> p = t[i];
            if (p) out.push_back({ (*p)[1].get_or(0.f), (*p)[2].get_or(0.f) });
        }
        return out;
    }

    /// Read one entry of `scrap_templates` from asteroids.lua. Read at spawn,
    /// not cached, so an F5 reload picks up template edits immediately.
    inline Template readTemplate(sol::state_view lua, const std::string& key) {
        Template T;
        sol::optional<sol::table> all = lua["scrap_templates"];
        if (!all) return T;
        sol::optional<sol::table> t = (*all)[key];
        if (!t) {
            std::cerr << "[FieldModel] scrap_template \"" << key << "\" not in scrap_templates.\n";
            return T;
        }
        sol::optional<sol::table> chunks = (*t)["chunks"];
        if (chunks) {
            for (size_t i = 1; i <= chunks->size(); ++i) {
                sol::optional<sol::table> c = (*chunks)[i];
                if (!c) continue;
                sol::optional<sol::table> body = (*c)["body"];
                if (!body) continue;
                TplChunk ch;
                ch.body = readPts(*body);
                ch.fill = readColor((*c)["fill"], hex(0x38271e));
                ch.stroke = readColor((*c)["stroke"], hex(0x914e2d));
                if (ch.body.size() >= 3) T.chunks.push_back(std::move(ch));
            }
        }
        sol::optional<sol::table> struts = (*t)["struts"];
        if (struts) {
            for (size_t i = 1; i <= struts->size(); ++i) {
                sol::optional<sol::table> s = (*struts)[i];
                if (!s) continue;
                const auto pts = readPts(*s);
                if (pts.size() >= 2) T.struts.push_back({ pts[0], pts[1] });
            }
        }
        sol::optional<sol::table> gaps = (*t)["gaps"];
        if (gaps) {
            for (size_t i = 1; i <= gaps->size(); ++i) {
                sol::optional<sol::table> g = (*gaps)[i];
                if (!g) continue;
                auto pts = readPts(*g);
                if (pts.size() >= 3) T.gaps.push_back(std::move(pts));
            }
        }
        return T;
    }

    /// The lab's tintColor(): warm/cool shift, then darken with age.
    inline sf::Color tintColor(sf::Color c, float tint, float age) {
        const float darken = 1.f - age * 0.22f;
        return rgba((c.r + tint * 14.f) * darken, (c.g + tint * 4.f) * darken,
            (c.b - tint * 10.f) * darken);
    }

    /**
     * @param shimmerSeed  The lab's Math.random()*1000 -- deliberately NOT from
     *                     the seeded RNG there, so it is passed in here too.
     */
    inline Model buildScrap(const Template& T, float pixelRadius, uint32_t seed, float shimmerSeed) {
        Model md;
        if (!T.valid()) return md;
        Rng rng(seed);

        const float tint = (rng() - 0.5f) * 0.9f;
        const float age = 0.15f + rng() * 0.55f;
        const float rivetDensity = 0.60f + rng() * 0.40f;
        const float strutBright = 0.35f + rng() * 0.55f;
        std::vector<float> chunkAges;
        for (size_t i = 0; i < T.chunks.size(); ++i) chunkAges.push_back(0.25f + rng() * 0.5f);

        const float pileRot = (rng() - 0.5f) * 0.30f;
        const float cosR = std::cos(pileRot), sinR = std::sin(pileRot);

        std::vector<V2> tplPts;
        for (const auto& c : T.chunks) tplPts.insert(tplPts.end(), c.body.begin(), c.body.end());
        const float jitter = fieldgeom::radiusOf(tplPts) * 0.025f;

        struct Chunk { std::vector<V2> body; sf::Color fill, stroke; bool rivets; };
        std::vector<Chunk> chunks;
        for (const auto& c : T.chunks) {
            const V2 cen = fieldgeom::centroid(c.body);
            const float chunkRot = (rng() - 0.5f) * 0.12f;
            const float cC = std::cos(chunkRot), cS = std::sin(chunkRot);
            Chunk out;
            for (const auto& p : c.body) {
                const float jx = (rng() - 0.5f) * jitter;
                const float jy = (rng() - 0.5f) * jitter;
                const V2 d{ p.x + jx - cen.x, p.y + jy - cen.y };
                const V2 r{ d.x * cC - d.y * cS + cen.x, d.x * cS + d.y * cC + cen.y };
                out.body.push_back({ r.x * cosR - r.y * sinR, r.x * sinR + r.y * cosR });
            }
            out.fill = c.fill;
            out.stroke = c.stroke;
            out.rivets = rng() < rivetDensity;
            chunks.push_back(std::move(out));
        }

        std::vector<std::pair<V2, V2>> struts;
        for (const auto& s : T.struts) {
            const float j0x = (rng() - 0.5f) * jitter, j0y = (rng() - 0.5f) * jitter;
            const float j1x = (rng() - 0.5f) * jitter, j1y = (rng() - 0.5f) * jitter;
            const V2 p0{ s.first.x + j0x, s.first.y + j0y };
            const V2 p1{ s.second.x + j1x, s.second.y + j1y };
            struts.push_back({ fieldgeom::rot(p0, cosR, sinR), fieldgeom::rot(p1, cosR, sinR) });
        }

        std::vector<std::vector<V2>> gaps;
        for (const auto& g : T.gaps) {
            std::vector<V2> pts;
            for (const auto& p : g) {
                const float jx = (rng() - 0.5f) * jitter * 0.6f;
                const float jy = (rng() - 0.5f) * jitter * 0.6f;
                pts.push_back(fieldgeom::rot({ p.x + jx, p.y + jy }, cosR, sinR));
            }
            gaps.push_back(std::move(pts));
        }

        std::vector<V2> all;
        for (const auto& c : chunks) all.insert(all.end(), c.body.begin(), c.body.end());
        const float rad = std::max(1.f, fieldgeom::radiusOf(all));
        const float k = pixelRadius / rad;

        // ---------------- draw ----------------
        Mesh m;
        m.minWidth = 1.f / k;

        // Drop shadow: every chunk, offset, in the object's own frame.
        for (const auto& c : chunks) {
            std::vector<V2> s = c.body;
            for (auto& p : s) p += V2(3.f, 4.f);
            fieldgeom::fillPoly(m, s, rgba(0, 0, 0, 0.5f));
        }

        for (size_t ci = 0; ci < chunks.size(); ++ci) {
            const auto& c = chunks[ci];
            const float chunkAge = chunkAges[ci] * 0.55f + age * 0.45f;
            fieldgeom::fillPoly(m, c.body, tintColor(c.fill, tint, chunkAge));
            fieldgeom::strokeLoop(m, c.body, 1.5f, tintColor(c.stroke, tint, chunkAge * 0.4f));

            for (size_t i = 0; i < c.body.size(); ++i) {
                md.glintSpots.push_back(c.body[i] * k);
                if (!c.rivets) continue;
                Fx r;
                r.p = c.body[i] * k;
                r.r = std::max(1.f, 1.6f * k);
                r.phase = i * 1.7f + shimmerSeed * 0.11f + c.body[0].x * 0.05f;
                md.fx.push_back(r);
            }
        }

        for (const auto& s : struts) {
            fieldgeom::strokeLine(m, s.first, s.second, 2.f, rgba(34, 21, 16, 0.75f + strutBright * 0.25f));
            fieldgeom::strokeLine(m, s.first, s.second, 1.f, rgba(145, 78, 45, 0.35f + strutBright * 0.45f));
        }

        for (const auto& g : gaps) {
            fieldgeom::fillPoly(m, g, hex(0x050302));
            fieldgeom::strokeLoop(m, g, 1.f, hex(0x1a0e08));
        }

        // Shards come off a plate: the fracture path darkens this by 0.72,
        // which lands them on the lab's debris colour (#38271e).
        md.debris = tintColor(hex(0x543b2e), tint, age);
        md.glowRadius = 3.2f * k;   // glint radius
        finish(md, m, enemyarch::geom::convexHull(all), k);
        return md;
    }

    // ==================================================================
    // UNSTABLE CORE
    // ==================================================================

    inline Model buildUnstableCore(float pixelRadius, uint32_t seed) {
        Model md;
        Rng rng(seed);

        const float R = 32.f;
        const int   N = 6;
        const float sectorSpan = TAU / N;
        const float ANGULAR_FILL = 0.72f;

        const int coreVerts = 10 + static_cast<int>(std::floor(rng() * 3));
        const float coreR = R * 0.55f;
        std::vector<V2> core;
        for (int i = 0; i < coreVerts; ++i) {
            const float a = (i / static_cast<float>(coreVerts)) * TAU + (rng() - 0.5f) * 0.14f;
            const float rr = coreR * (0.85f + rng() * 0.22f);
            core.push_back({ std::cos(a) * rr, std::sin(a) * rr });
        }

        struct Plate { V2 q[4]; sf::Color fill, stroke; V2 cen; };
        std::vector<Plate> plates;
        for (int i = 0; i < N; ++i) {
            const float sectorMid = (i / static_cast<float>(N)) * TAU + (rng() - 0.5f) * 0.10f;
            const float innerR = coreR * (0.30f + rng() * 0.20f);
            const float outerR = coreR * (1.05f + rng() * 0.35f);
            const float plateSpan = sectorSpan * ANGULAR_FILL * (0.85f + rng() * 0.15f);
            const float a0 = sectorMid - plateSpan * 0.5f;
            const float a1 = sectorMid + plateSpan * 0.5f;

            const float jInA = (rng() - 0.5f) * 0.10f;
            const float jInB = (rng() - 0.5f) * 0.10f;
            const float jOutA = (rng() - 0.5f) * 0.16f;
            const float jOutB = (rng() - 0.5f) * 0.16f;
            const float jInR = (rng() - 0.5f) * coreR * 0.10f;
            const float jOutR = (rng() - 0.5f) * coreR * 0.16f;

            const float ri = innerR + jInR, ro = outerR + jOutR;
            Plate p;
            p.q[0] = fieldgeom::pol(ri, a0 + jInA);
            p.q[1] = fieldgeom::pol(ri, a1 + jInB);
            p.q[2] = fieldgeom::pol(ro, a1 + jOutB);
            p.q[3] = fieldgeom::pol(ro, a0 + jOutA);
            p.fill = (i % 2 == 0) ? hex(0x551d0c) : hex(0x2e0e06);
            p.stroke = (i % 2 == 0) ? hex(0xff4400) : hex(0xffaa00);
            p.cen = (p.q[0] + p.q[1] + p.q[2] + p.q[3]) * 0.25f;
            plates.push_back(p);
        }

        std::vector<std::pair<V2, V2>> struts;
        for (int i = 0; i < 3; ++i) {
            const V2 a = plates[static_cast<size_t>(i % N)].cen;
            const V2 b = plates[static_cast<size_t>((i + 1) % N)].cen;
            const V2 pa{ a.x + (rng() - 0.5f) * R * 0.05f, a.y + (rng() - 0.5f) * R * 0.05f };
            const V2 pb{ b.x + (rng() - 0.5f) * R * 0.05f, b.y + (rng() - 0.5f) * R * 0.05f };
            struts.push_back({ pa, pb });
        }

        std::vector<std::vector<V2>> gaps;
        for (int i = 0; i < 2; ++i) {
            const float boundaryA = (i / 2.f) * TAU + (rng() - 0.5f) * 0.3f;
            const float d = coreR * (0.7f + rng() * 0.3f);
            const V2 c{ std::cos(boundaryA) * d, std::sin(boundaryA) * d };
            const float gR = R * (0.05f + rng() * 0.08f);
            const int verts = 4 + static_cast<int>(std::floor(rng() * 2));
            std::vector<V2> pts;
            for (int v = 0; v < verts; ++v) {
                const float aa = (v / static_cast<float>(verts)) * TAU + (rng() - 0.5f) * 0.5f;
                const float rr = gR * (0.7f + rng() * 0.5f);
                pts.push_back({ c.x + std::cos(aa) * rr, c.y + std::sin(aa) * rr });
            }
            gaps.push_back(std::move(pts));
        }
        rng(); rng();                        // tilt, vrot -- physics owns spin here
        md.phase = rng() * TAU;              // pulsePhase

        std::vector<V2> all = core;
        for (const auto& p : plates) all.insert(all.end(), p.q, p.q + 4);
        const float rad = std::max(1.f, fieldgeom::radiusOf(all));
        const float k = pixelRadius / rad;

        // ---------------- draw ----------------
        Mesh m;
        m.minWidth = 1.f / k;

        // Shadow
        {
            std::vector<V2> s = core;
            for (auto& p : s) p += V2(4.f, 5.f);
            fieldgeom::fillPoly(m, s, rgba(0, 0, 0, 0.55f));
            for (const auto& p : plates) {
                std::vector<V2> q(p.q, p.q + 4);
                for (auto& v : q) v += V2(4.f, 5.f);
                fieldgeom::fillPoly(m, q, rgba(0, 0, 0, 0.55f));
            }
        }

        // Core, darkened toward the middle (radial 2 -> rad, black .55 -> 0).
        // A fan from the centre so the gradient's centre is actually a vertex.
        fieldgeom::fillPoly(m, core, hex(0x3a1208));
        {
            const std::vector<fieldgeom::Stop> st{ { 0.f, rgba(0, 0, 0, 0.55f) }, { 1.f, rgba(0, 0, 0, 0.f) } };
            auto at = [&](V2 p) { return fieldgeom::sample(st, (fieldgeom::len(p) - 2.f) / (rad - 2.f)); };
            for (size_t i = 0; i < core.size(); ++i) {
                const V2 a = core[i], b = core[(i + 1) % core.size()];
                m.tri(V2(), a, b, at(V2()), at(a), at(b));
            }
        }
        fieldgeom::strokeLoop(m, core, 1.2f, hex(0xff4400));

        for (const auto& p : plates) {
            const std::vector<V2> q(p.q, p.q + 4);
            fieldgeom::fillPoly(m, q, p.fill);
            const fieldgeom::Box bb = fieldgeom::boundsOf(q);
            const V2 g0{ bb.minX, bb.minY }, gd{ bb.w(), bb.h() };
            const float gl2 = std::max(1e-4f, fieldgeom::dot(gd, gd));
            const std::vector<fieldgeom::Stop> st{
                { 0.0f, rgba(255, 140, 70, 0.22f) },
                { 0.5f, rgba(255, 60, 20, 0.06f) },
                { 1.0f, rgba(0, 0, 0, 0.34f) } };
            fieldgeom::fillQuadGradient(m, p.q,
                [&](V2 v) { return fieldgeom::sample(st, fieldgeom::dot(v - g0, gd) / gl2); });
            fieldgeom::strokeLoop(m, q, 1.5f, p.stroke);
            for (const auto& v : q) fieldgeom::rect(m, v.x - 0.7f, v.y - 0.7f, 1.7f, 1.7f, hex(0xffaa00));
        }

        for (const auto& s : struts) {
            fieldgeom::strokeLine(m, s.first, s.second, 1.6f, hex(0xffaa00));
            fieldgeom::strokeLine(m, s.first, s.second, 0.8f, hex(0xff3300));
        }
        for (const auto& g : gaps) {
            fieldgeom::fillPoly(m, g, hex(0xff2200));
            fieldgeom::strokeLoop(m, g, 1.f, hex(0xffaa00));
        }

        md.debris = hex(0x551d0c);
        md.glowRadius = rad * 1.2f * k;
        finish(md, m, enemyarch::geom::convexHull(all), k);
        return md;
    }

} // namespace fieldmodel
