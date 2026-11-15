/**
 * @file PoiModels.hpp
 * @brief The objects points of interest are built from: structures, loot
 *        caches and scrap-rich ore. Procedural, flat, in the field's style.
 *
 * ============================================================================
 * HOW THEY ARE USED
 * ============================================================================
 * Every one is an asteroids.lua type with `detail = "poi"` and
 * `poi_model = "<name>"`. EntityFactory::createAsteroid builds the model
 * here, at the rolled size, and puts it on the same baked-detail path the
 * field objects use (world transform and hit flash for free). The collider is
 * the convex hull of the model's FOOTPRINT, never of thin things like masts
 * and cables -- you hit the building, not the antenna's air.
 *
 * Lights (`Model::lights`) are drawn live by RenderSystem::drawFieldFx with
 * FieldStyle::Poi: a soft halo and a core, steady or blinking per light.
 *
 *   ore_rich   a rock run through with amber scrap veins (the mine's loot)
 *   cache      a strapped stack of crates with a lamp (camps, outposts)
 *   module     a pressurised hab module (outposts)
 *   antenna    a mast, a dish, a blinking tip
 *   crane      a salvage crane: base, long arm, cable, hook
 *   rig        a mining rig: braced frame, drill arm, bit
 *   beacon     a distress beacon: a small hull and a big red pulse
 *   dock       a docking ring with three bays
 *   lair       the Bloodseeker's lair: bone spines round a dark core
 *   tent       a scavenger shelter: patchwork tarp on a frame
 *   hulk       a dead capital ship, broken open (the derelict)
 *
 * Two palettes: `rust` (Rakshari) and `steel` (everything else), picked by
 * the type's `poi_palette`.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/FieldGeom.hpp"
#include "core/EnemyArchetypes.hpp"   // geom::convexHull / decimateConvex
#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace poimodel {

    using fieldgeom::V2;
    using fieldgeom::Mesh;
    using fieldgeom::Rng;
    using fieldgeom::rgba;
    using fieldgeom::TAU;

    struct Light {
        V2 p;
        float r = 3.f;
        float phase = 0.f;
        sf::Color c{ 255, 170, 60 };
        bool blink = false;
    };

    struct Model {
        std::vector<V2> outline;      ///< visual silhouette
        std::vector<V2> physics;      ///< convex, <= 8 points
        std::vector<sf::Vertex> tris;
        std::vector<Light> lights;
        sf::Color debris{ 60, 50, 44 };
        bool ok() const { return outline.size() >= 3 && physics.size() >= 3 && !tris.empty(); }
    };

    struct Pal { sf::Color dark, hull, plate, edge, lamp, lampHot; };
    inline Pal palette(const std::string& name) {
        if (name == "steel")
            return { rgba(26, 30, 38), rgba(58, 66, 80), rgba(84, 94, 110), rgba(150, 162, 180),
                     rgba(90, 220, 255), rgba(200, 245, 255) };
        return { rgba(30, 20, 16), rgba(72, 46, 34), rgba(100, 64, 44), rgba(160, 104, 66),
                 rgba(255, 170, 60), rgba(255, 220, 150) };
    }

    // ---- small helpers ----
    inline std::vector<V2> box(float cx, float cy, float w, float h, float a = 0.f) {
        const float c = std::cos(a), s = std::sin(a);
        std::vector<V2> p = { { -w, -h }, { w, -h }, { w, h }, { -w, h } };
        for (auto& q : p) q = V2(cx + q.x * c - q.y * s, cy + q.x * s + q.y * c);
        return p;
    }
    inline std::vector<V2> ngon(float cx, float cy, float r, int n, float a0 = 0.f, float sx = 1.f, float sy = 1.f) {
        std::vector<V2> p;
        for (int i = 0; i < n; ++i) { const float a = a0 + i * TAU / n; p.push_back({ cx + std::cos(a) * r * sx, cy + std::sin(a) * r * sy }); }
        return p;
    }
    inline void shape(Mesh& m, const std::vector<V2>& p, sf::Color fill, sf::Color line, float w) {
        fieldgeom::fillPoly(m, p, fill);
        if (w > 0.f) fieldgeom::strokeLoop(m, p, w, line);
    }
    inline void finish(Model& md, Mesh& m, const std::vector<V2>& footprint, const std::vector<V2>& outline) {
        md.tris = std::move(m.v);
        md.outline = outline;
        md.physics = enemyarch::geom::decimateConvex(enemyarch::geom::convexHull(footprint), 8);
    }

    // ========================================================================
    // THE MODELS (R = the rolled pixel radius)
    // ========================================================================

    inline Model oreRich(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        std::vector<V2> rock;
        const int n = 11;
        for (int i = 0; i < n; ++i) {
            const float a = i * TAU / n + (rng() - 0.5f) * 0.3f;
            rock.push_back(fieldgeom::pol(R * (0.78f + rng() * 0.24f), a));
        }
        shape(m, rock, rgba(54, 48, 46), rgba(96, 86, 80), 1.6f);
        // facets
        for (int f = 0; f < 6; ++f) {
            const int i = static_cast<int>(rng() * n) % n;
            const V2 a = rock[i], b = rock[(i + 1) % n];
            const V2 c = fieldgeom::pol(R * (0.2f + rng() * 0.3f), rng() * TAU);
            m.tri(a, b, c, rgba(70, 62, 58, 0.8f));
        }
        // amber veins
        for (int v = 0; v < 4; ++v) {
            V2 p = fieldgeom::pol(R * 0.15f, rng() * TAU);
            const float dir = rng() * TAU;
            for (int s = 0; s < 4; ++s) {
                const V2 q = p + fieldgeom::pol(R * (0.14f + rng() * 0.12f), dir + (rng() - 0.5f) * 1.1f);
                if (fieldgeom::len(q) > R * 0.8f) break;
                fieldgeom::strokeLine(m, p, q, std::max(1.4f, R * 0.045f), rgba(255, 160, 40, 0.85f));
                p = q;
            }
        }
        for (int k = 0; k < 7; ++k) {
            const V2 q = fieldgeom::pol(R * (0.15f + rng() * 0.55f), rng() * TAU);
            const float r = std::max(1.5f, R * (0.05f + rng() * 0.05f));
            fieldgeom::disc(m, q, r, rgba(255, 190, 70), 7);
            md.lights.push_back({ q, r * 0.9f, rng() * TAU, rgba(255, 180, 60), true });
        }
        md.debris = rgba(70, 60, 50);
        finish(md, m, rock, rock);
        return md;
    }

    inline Model cache(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        std::vector<V2> all;
        const int crates = 3 + static_cast<int>(rng() * 2.f);
        for (int c = 0; c < crates; ++c) {
            const float w = R * (0.32f + rng() * 0.12f), h = R * (0.24f + rng() * 0.1f);
            const float cx = (c - (crates - 1) * 0.5f) * R * 0.42f + (rng() - 0.5f) * R * 0.15f;
            const float cy = (c % 2 ? -1.f : 1.f) * R * 0.18f + (rng() - 0.5f) * R * 0.1f;
            const auto b = box(cx, cy, w, h, (rng() - 0.5f) * 0.35f);
            shape(m, b, c % 2 ? P.plate : P.hull, P.edge, 1.4f);
            // strap and corner caps
            fieldgeom::strokeLine(m, (b[0] + b[1]) * 0.5f, (b[3] + b[2]) * 0.5f, 2.f, P.dark);
            for (const auto& q : b) all.push_back(q);
        }
        // amber lamp on top
        const V2 lamp(0.f, -R * 0.35f);
        fieldgeom::disc(m, lamp, R * 0.08f, P.lampHot, 8);
        md.lights.push_back({ lamp, R * 0.1f, rng() * TAU, P.lamp, false });
        md.debris = P.hull;
        const auto hull = enemyarch::geom::convexHull(all);
        finish(md, m, hull, hull);
        return md;
    }

    inline Model module(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto body = ngon(0, 0, R, 8, TAU / 16.f, 1.f, 0.72f);
        shape(m, body, P.hull, P.edge, 2.f);
        shape(m, ngon(0, 0, R * 0.62f, 8, TAU / 16.f, 1.f, 0.62f), P.plate, P.dark, 1.2f);
        // window strip
        for (int k = -3; k <= 3; ++k) {
            const V2 w(k * R * 0.16f, 0.f);
            fieldgeom::rect(m, w.x - R * 0.05f, -R * 0.06f, R * 0.1f, R * 0.12f, P.lamp);
            if (k % 2 == 0) md.lights.push_back({ w, R * 0.06f, rng() * TAU, P.lamp, false });
        }
        // docking stubs
        fieldgeom::rect(m, -R * 1.12f, -R * 0.12f, R * 0.18f, R * 0.24f, P.plate);
        fieldgeom::rect(m, R * 0.94f, -R * 0.12f, R * 0.18f, R * 0.24f, P.plate);
        md.lights.push_back({ V2(0, -R * 0.68f), R * 0.06f, rng() * TAU, rgba(255, 70, 60), true });
        md.debris = P.hull;
        finish(md, m, body, body);
        return md;
    }

    inline Model antenna(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto base = box(0, R * 0.55f, R * 0.34f, R * 0.26f);
        shape(m, base, P.hull, P.edge, 1.6f);
        fieldgeom::strokeLine(m, V2(0, R * 0.3f), V2(0, -R * 0.95f), std::max(2.f, R * 0.06f), P.edge);
        for (int k = 0; k < 3; ++k) {
            const float y = -R * (0.1f + k * 0.28f);
            fieldgeom::strokeLine(m, V2(-R * 0.16f, y), V2(R * 0.16f, y), 1.6f, P.edge);
        }
        const auto dish = fieldgeom::arcPts(V2(R * 0.18f, -R * 0.45f), R * 0.32f, R * 0.32f, -1.0f, 1.0f, false, 8);
        fieldgeom::strokePath(m, dish, false, 2.2f, P.plate);
        md.lights.push_back({ V2(0, -R * 0.98f), R * 0.08f, rng() * TAU, rgba(255, 60, 50), true });
        md.debris = P.hull;
        std::vector<V2> outline = base; outline.push_back(V2(0, -R));
        finish(md, m, base, outline);
        return md;
    }

    inline Model crane(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto base = box(-R * 0.45f, R * 0.35f, R * 0.42f, R * 0.32f);
        shape(m, base, P.hull, P.edge, 1.8f);
        // the arm: a lattice boom
        const V2 a0(-R * 0.45f, R * 0.1f), a1(R * 0.95f, -R * 0.55f);
        const V2 n = fieldgeom::perp(fieldgeom::norm(a1 - a0)) * (R * 0.07f);
        fieldgeom::strokeLine(m, a0 + n, a1 + n, 2.4f, P.edge);
        fieldgeom::strokeLine(m, a0 - n, a1 - n, 2.4f, P.edge);
        for (int k = 0; k <= 6; ++k) {
            const float t0 = k / 6.f, t1 = std::min(1.f, (k + 1) / 6.f);
            fieldgeom::strokeLine(m, a0 + (a1 - a0) * t0 + n, a0 + (a1 - a0) * t1 - n, 1.4f, P.plate);
        }
        // cable and hook
        const V2 hook(R * 0.95f, R * 0.25f);
        fieldgeom::strokeLine(m, a1, hook, 1.2f, rgba(170, 170, 170, 0.8f));
        shape(m, box(hook.x, hook.y, R * 0.1f, R * 0.06f), P.plate, P.edge, 1.2f);
        md.lights.push_back({ a1, R * 0.07f, rng() * TAU, P.lamp, true });
        md.lights.push_back({ base[0] + V2(R * 0.12f, R * 0.1f), R * 0.06f, rng() * TAU, P.lamp, false });
        md.debris = P.hull;
        std::vector<V2> outline = base; outline.push_back(a1); outline.push_back(hook);
        finish(md, m, base, outline);
        return md;
    }

    inline Model rig(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto frame = box(0, 0, R * 0.62f, R * 0.62f, 0.f);
        shape(m, frame, P.dark, P.edge, 2.4f);
        fieldgeom::strokeLine(m, frame[0], frame[2], 1.8f, P.plate);
        fieldgeom::strokeLine(m, frame[1], frame[3], 1.8f, P.plate);
        shape(m, box(0, 0, R * 0.26f, R * 0.26f), P.hull, P.edge, 1.6f);
        // the drill: arm out of the frame, a bit on the end
        const V2 d0(R * 0.26f, 0.f), d1(R * 1.0f, R * 0.12f);
        fieldgeom::strokeLine(m, d0, d1, std::max(3.f, R * 0.1f), P.plate);
        std::vector<V2> bit = { d1 + V2(0, -R * 0.12f), d1 + V2(R * 0.22f, 0), d1 + V2(0, R * 0.12f) };
        shape(m, bit, rgba(200, 190, 170), P.edge, 1.f);
        for (int c = 0; c < 4; ++c)
            md.lights.push_back({ frame[c], R * 0.06f, c * 1.4f, P.lamp, c % 2 == 0 });
        md.debris = P.hull;
        std::vector<V2> outline = frame; outline.push_back(d1 + V2(R * 0.22f, 0));
        finish(md, m, frame, outline);
        return md;
    }

    inline Model beacon(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto body = ngon(0, 0, R * 0.6f, 6, 0.f);
        shape(m, body, P.hull, P.edge, 1.8f);
        for (int k = 0; k < 3; ++k) {
            const float a = k * TAU / 3.f + 0.5f;
            fieldgeom::strokeLine(m, fieldgeom::pol(R * 0.6f, a), fieldgeom::pol(R, a), 1.8f, P.edge);
        }
        fieldgeom::disc(m, V2(0, 0), R * 0.22f, rgba(255, 90, 70), 10);
        md.lights.push_back({ V2(0, 0), R * 0.3f, 0.f, rgba(255, 50, 40), true });
        md.debris = P.hull;
        finish(md, m, body, body);
        return md;
    }

    inline Model dock(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto outer = ngon(0, 0, R, 16);
        const auto inner = ngon(0, 0, R * 0.62f, 16);
        // the ring, as quads between the two loops
        for (int i = 0; i < 16; ++i) {
            const int j = (i + 1) % 16;
            m.quad(outer[i], outer[j], inner[j], inner[i], (i % 4 == 0) ? P.plate : P.hull);
        }
        fieldgeom::strokeLoop(m, outer, 2.2f, P.edge);
        fieldgeom::strokeLoop(m, inner, 1.6f, P.edge);
        shape(m, ngon(0, 0, R * 0.3f, 8), P.dark, P.edge, 1.4f);
        for (int b = 0; b < 3; ++b) {
            const float a = b * TAU / 3.f;
            const V2 c = fieldgeom::pol(R * 0.81f, a);
            shape(m, box(c.x, c.y, R * 0.14f, R * 0.09f, a), P.dark, P.lamp, 1.2f);
            md.lights.push_back({ fieldgeom::pol(R * 1.02f, a), R * 0.05f, b * 2.f, P.lamp, true });
        }
        md.debris = P.hull;
        finish(md, m, outer, outer);
        return md;
    }

    inline Model lair(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        const auto core = ngon(0, 0, R * 0.45f, 9, rng() * TAU);
        std::vector<V2> outline;
        const int spines = 9;
        for (int s = 0; s < spines; ++s) {
            const float a = s * TAU / spines + (rng() - 0.5f) * 0.3f;
            const float len = R * (0.8f + rng() * 0.25f);
            const V2 tip = fieldgeom::pol(len, a);
            std::vector<V2> sp = { fieldgeom::pol(R * 0.4f, a - 0.22f), tip, fieldgeom::pol(R * 0.4f, a + 0.22f) };
            shape(m, sp, rgba(196, 184, 160), rgba(120, 108, 92), 1.2f);
            outline.push_back(tip);
        }
        shape(m, core, rgba(26, 10, 12), rgba(120, 30, 30), 1.8f);
        md.lights.push_back({ V2(-R * 0.12f, -R * 0.05f), R * 0.07f, 0.f, rgba(255, 40, 40), true });
        md.lights.push_back({ V2(R * 0.12f, -R * 0.05f), R * 0.07f, 0.3f, rgba(255, 40, 40), true });
        md.debris = rgba(120, 108, 92);
        finish(md, m, ngon(0, 0, R * 0.7f, 8), outline);
        return md;
    }

    inline Model tent(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        std::vector<V2> tarp;
        const int n = 7;
        for (int i = 0; i < n; ++i) tarp.push_back(fieldgeom::pol(R * (0.75f + rng() * 0.25f), i * TAU / n + (rng() - 0.5f) * 0.25f));
        shape(m, tarp, P.hull, P.edge, 1.6f);
        // patches
        for (int p = 0; p < 4; ++p) {
            const V2 c = fieldgeom::pol(R * (0.2f + rng() * 0.35f), rng() * TAU);
            shape(m, box(c.x, c.y, R * (0.12f + rng() * 0.1f), R * (0.1f + rng() * 0.08f), rng() * 3.f),
                p % 2 ? P.plate : rgba(90, 84, 60), P.dark, 1.f);
        }
        // the frame poles
        for (int i = 0; i < n; i += 2) fieldgeom::strokeLine(m, V2(0, 0), tarp[i], 1.4f, P.dark);
        md.lights.push_back({ V2(0, 0), R * 0.09f, rng() * TAU, P.lamp, false });
        md.debris = P.hull;
        finish(md, m, tarp, tarp);
        return md;
    }

    inline Model hulk(float R, uint32_t seed, const Pal& P) {
        Model md; Mesh m; Rng rng(seed);
        // a long hull, bow left, broken stern right
        std::vector<V2> hull = { { -R, 0 }, { -R * 0.6f, -R * 0.3f }, { R * 0.3f, -R * 0.36f }, { R * 0.62f, -R * 0.24f },
                                 { R * 0.78f, -R * 0.36f }, { R * 0.95f, -R * 0.05f }, { R * 0.8f, R * 0.12f }, { R * 0.9f, R * 0.3f },
                                 { R * 0.3f, R * 0.36f }, { -R * 0.6f, R * 0.3f } };
        shape(m, hull, P.hull, P.edge, 2.2f);
        // plating and dark breaches
        for (int k = 0; k < 6; ++k) {
            const float x = -R * 0.55f + k * R * 0.22f;
            shape(m, box(x, 0, R * 0.08f, R * (0.18f + rng() * 0.06f)), P.plate, P.dark, 1.f);
        }
        for (int b = 0; b < 4; ++b) {
            const V2 c(-R * 0.4f + rng() * R * 1.1f, (rng() - 0.5f) * R * 0.4f);
            shape(m, ngon(c.x, c.y, R * (0.06f + rng() * 0.06f), 6, rng() * 2.f, 1.2f, 0.8f), P.dark, rgba(255, 120, 40, 0.5f), 1.f);
        }
        // the bridge, and its last light
        shape(m, box(-R * 0.45f, -R * 0.12f, R * 0.12f, R * 0.08f), P.plate, P.edge, 1.2f);
        md.lights.push_back({ V2(-R * 0.45f, -R * 0.12f), R * 0.03f, 0.f, rgba(255, 70, 60), true });
        md.debris = P.hull;
        finish(md, m, hull, hull);
        return md;
    }

    inline Model build(const std::string& name, float R, uint32_t seed, const std::string& pal) {
        const Pal P = palette(pal);
        if (name == "ore_rich") return oreRich(R, seed, P);
        if (name == "cache")    return cache(R, seed, P);
        if (name == "module")   return module(R, seed, P);
        if (name == "antenna")  return antenna(R, seed, P);
        if (name == "crane")    return crane(R, seed, P);
        if (name == "rig")      return rig(R, seed, P);
        if (name == "beacon")   return beacon(R, seed, P);
        if (name == "dock")     return dock(R, seed, P);
        if (name == "lair")     return lair(R, seed, P);
        if (name == "tent")     return tent(R, seed, P);
        if (name == "hulk")     return hulk(R, seed, P);
        return {};
    }

} // namespace poimodel
