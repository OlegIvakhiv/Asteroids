/**
 * @file EnemyArchetypes.hpp
 * @brief Enemy unit archetypes and faction spawn tables, loaded from Lua.
 *
 * WHAT THIS SOLVES
 * ----------------
 * Before this file there was exactly one enemy in the game. Its hull was
 * hardcoded in EntityFactory, its stats lived in a single global Lua table
 * called `enemy_config`, and AISystem applied one behaviour profile to
 * everything that carried BodyType::Enemy. Adding a second unit type meant
 * either a second createXxx() function or branching inside AISystem -- both of
 * which multiply badly across a seven-unit roster.
 *
 * This registry replaces that with:
 *   - `enemy_archetypes` in Lua: one table per unit, inheriting shared defaults
 *   - `factions` in Lua: which units a faction fields, and at what weights
 *   - a stable uint8_t archetype id stored on EnemyComponent
 *
 * Adding a new unit is now a Lua-only edit. No recompile, F5-reloadable.
 *
 *
 * TWO HULLS PER SHIP
 * ------------------
 * Every archetype carries two polygons derived from ONE authored shape:
 *
 *   visual  -- the full-detail silhouette, exactly as authored. May be
 *              concave (jagged prows, exhaust teeth, asymmetric plating).
 *              Triangulated here at load time so RenderSystem can draw it
 *              as a vertex array instead of an sf::ConvexShape.
 *
 *   physics -- convex, <= 8 points, fed to b2ComputeHull. Box2D's
 *              B2_MAX_POLYGON_VERTICES is a hard limit of 8, so this is
 *              derived automatically rather than hand-authored: convex hull
 *              first, then greedy decimation down to 8.
 *
 * Deriving the collision hull instead of authoring it means the two can never
 * drift apart when a silhouette is tweaked, and it makes the 8-point ceiling
 * something the artist never has to think about.
 *
 * WHY sf::ConvexShape ISN'T ENOUGH FOR THE VISUAL
 * -----------------------------------------------
 * SFML builds a shape as a TriangleFan whose origin is the BOUNDING-BOX
 * CENTRE. That renders a concave polygon correctly only when every vertex is
 * visible from that centre. Shallow notches (the Raider's wing cutouts)
 * survive; deep ones (the Barge's exhaust teeth) get filled in, because the
 * fan sees straight through the gap. Ear clipping produces the real polygon.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics/Color.hpp>
#include <SFML/System/Vector2.hpp>
#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace enemyarch {

    // Box2D's B2_MAX_POLYGON_VERTICES. Not a suggestion -- b2ComputeHull
    // silently returns an invalid hull past this.
    inline constexpr int MAX_PHYSICS_POINTS = 8;

    // Sentinel for "no such archetype". Stored in a uint8_t, so 255.
    inline constexpr uint8_t INVALID_ARCHETYPE = 0xFF;

    // ========================================================================
    // GEOMETRY HELPERS
    // ========================================================================

    namespace geom {

        inline float cross(sf::Vector2f o, sf::Vector2f a, sf::Vector2f b) {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        inline float signedArea(const std::vector<sf::Vector2f>& p) {
            float a = 0.f;
            const size_t n = p.size();
            for (size_t i = 0; i < n; ++i) {
                const sf::Vector2f& c = p[i];
                const sf::Vector2f& d = p[(i + 1) % n];
                a += c.x * d.y - d.x * c.y;
            }
            return a * 0.5f;
        }

        /**
         * @brief Andrew's monotone chain convex hull.
         * @return Hull points. Winding is consistent but unspecified in screen
         *         space; callers that care should check signedArea().
         *
         * Duplicate and collinear points are dropped, which is what we want --
         * an authored silhouette often has near-collinear runs that would eat
         * slots in the 8-point budget for no collision benefit.
         */
        inline std::vector<sf::Vector2f> convexHull(std::vector<sf::Vector2f> pts) {
            if (pts.size() < 3) return pts;

            std::sort(pts.begin(), pts.end(), [](sf::Vector2f a, sf::Vector2f b) {
                return (a.x < b.x) || (a.x == b.x && a.y < b.y);
                });
            pts.erase(std::unique(pts.begin(), pts.end(), [](sf::Vector2f a, sf::Vector2f b) {
                return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f;
                }), pts.end());

            if (pts.size() < 3) return pts;

            std::vector<sf::Vector2f> h(pts.size() * 2);
            size_t k = 0;

            for (size_t i = 0; i < pts.size(); ++i) {
                while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.f) --k;
                h[k++] = pts[i];
            }
            const size_t lower = k + 1;
            for (size_t i = pts.size() - 1; i-- > 0; ) {
                while (k >= lower && cross(h[k - 2], h[k - 1], pts[i]) <= 0.f) --k;
                h[k++] = pts[i];
            }
            h.resize(k - 1);
            return h;
        }

        /**
         * @brief Reduce an already-convex polygon to at most maxPts vertices.
         *
         * Greedy: repeatedly delete whichever vertex costs the least area to
         * lose. Removing a vertex from a convex polygon leaves it convex, so
         * the result is always a valid Box2D hull.
         *
         * NOTE ON THE NET EFFECT. This step shrinks, but the convex-hull step
         * before it GROWS -- it fills in every notch. For a concave ship the
         * two do not cancel: the Barge's silhouette is 3110 units of area, its
         * convex hull is 3970, and the 8-point reduction only brings that back
         * to 3737. So its hitbox is ~20% larger than it looks, and shots into
         * the gaps between its exhaust teeth will register.
         *
         * That is unavoidable -- Box2D polygons are convex, full stop. It is
         * also the right direction for a tank, where "I hit it" is the honest
         * read. It would NOT be right for a unit meant to feel slippery, so
         * check the [EnemyRegistry] load line for any new archetype and pull
         * `hitbox_scale` below 1.0 if the ratio looks wrong for its role.
         */
        inline std::vector<sf::Vector2f> decimateConvex(std::vector<sf::Vector2f> h, int maxPts) {
            while (static_cast<int>(h.size()) > maxPts && h.size() > 3) {
                size_t best = 0;
                float bestCost = 1e30f;
                const size_t n = h.size();
                for (size_t i = 0; i < n; ++i) {
                    // Area of the triangle lost by deleting vertex i.
                    const sf::Vector2f& a = h[(i + n - 1) % n];
                    const sf::Vector2f& b = h[i];
                    const sf::Vector2f& c = h[(i + 1) % n];
                    const float cost = std::fabs(cross(a, b, c)) * 0.5f;
                    if (cost < bestCost) { bestCost = cost; best = i; }
                }
                h.erase(h.begin() + static_cast<long>(best));
            }
            return h;
        }

        inline bool pointInTriangle(sf::Vector2f p, sf::Vector2f a, sf::Vector2f b, sf::Vector2f c) {
            const float d1 = cross(a, b, p);
            const float d2 = cross(b, c, p);
            const float d3 = cross(c, a, p);
            const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            return !(neg && pos);
        }

        /**
         * @brief Ear-clipping triangulation for a simple polygon (concave OK).
         * @return Flat triangle list: indices [0,1,2], [3,4,5], ...
         *
         * Not the fastest algorithm in existence, but this runs once per
         * archetype at load time, on polygons of 10-30 points. Anything
         * cleverer would be optimising a cost that doesn't exist.
         *
         * If no ear is found in a full pass (degenerate or self-intersecting
         * input) it bails to a fan over whatever is left rather than spinning
         * forever. A slightly wrong ship is a bug report; a hang is a
         * disaster.
         */
        inline std::vector<sf::Vector2f> triangulate(const std::vector<sf::Vector2f>& polyIn) {
            std::vector<sf::Vector2f> out;
            if (polyIn.size() < 3) return out;

            std::vector<sf::Vector2f> p = polyIn;

            // Normalise winding to counter-clockwise in maths terms, so the
            // convexity test below has a single sign to check.
            if (signedArea(p) < 0.f) std::reverse(p.begin(), p.end());

            std::vector<int> idx(p.size());
            for (size_t i = 0; i < p.size(); ++i) idx[i] = static_cast<int>(i);

            int guard = static_cast<int>(p.size()) * 3;

            while (idx.size() > 3 && guard-- > 0) {
                bool clipped = false;
                const int n = static_cast<int>(idx.size());

                for (int i = 0; i < n; ++i) {
                    const sf::Vector2f& a = p[idx[(i + n - 1) % n]];
                    const sf::Vector2f& b = p[idx[i]];
                    const sf::Vector2f& c = p[idx[(i + 1) % n]];

                    if (cross(a, b, c) <= 0.f) continue;   // reflex vertex, not an ear

                    bool contains = false;
                    for (int j = 0; j < n; ++j) {
                        if (j == i || j == (i + n - 1) % n || j == (i + 1) % n) continue;
                        if (pointInTriangle(p[idx[j]], a, b, c)) { contains = true; break; }
                    }
                    if (contains) continue;

                    out.push_back(a); out.push_back(b); out.push_back(c);
                    idx.erase(idx.begin() + i);
                    clipped = true;
                    break;
                }
                if (!clipped) break;   // degenerate input -- fall through to the fan
            }

            // Remaining polygon (normally the final triangle) as a fan.
            for (size_t i = 1; i + 1 < idx.size(); ++i) {
                out.push_back(p[idx[0]]);
                out.push_back(p[idx[i]]);
                out.push_back(p[idx[i + 1]]);
            }
            return out;
        }

        /**
         * @brief Build a closed, thick outline as a triangle strip.
         *
         * sf::Shape::setOutlineThickness cannot be used for these hulls: SFML
         * offsets each vertex along the average of its two edge normals, which
         * overshoots badly at reflex vertices and produces visible spikes on
         * anything as jagged as the Barge.
         *
         * This is why the outline is rebuilt rather than cached: the shot
         * telegraph ramps thickness from 1.6 to ~6.6px as the wind-up
         * completes, and that ramp is the player's cue to move. Losing it to a
         * flat 1px line would quietly remove the single biggest fairness
         * feature the enemies have.
         *
         * @param poly  Polygon in COUNTER-CLOCKWISE (positive signed area)
         *              order -- ArchetypeDef::visual is normalised to this at
         *              load, so callers do not have to check.
         * @param width Total stroke width in local units.
         */
        inline std::vector<sf::Vector2f> outlineStrip(const std::vector<sf::Vector2f>& poly,
            float width) {
            std::vector<sf::Vector2f> out;
            const size_t n = poly.size();
            if (n < 3 || width <= 0.f) return out;

            const float h = width * 0.5f;
            const float miterLimit = width * 3.f;   // Sharp prows would otherwise
            // shoot a spike to infinity.

            out.reserve((n + 1) * 2);

            for (size_t i = 0; i <= n; ++i) {
                const sf::Vector2f& prev = poly[(i + n - 1) % n];
                const sf::Vector2f& cur = poly[i % n];
                const sf::Vector2f& next = poly[(i + 1) % n];

                auto unit = [](sf::Vector2f v) {
                    const float l = std::sqrt(v.x * v.x + v.y * v.y);
                    return (l > 1e-5f) ? sf::Vector2f(v.x / l, v.y / l) : sf::Vector2f(0.f, 0.f);
                };

                const sf::Vector2f d0 = unit(cur - prev);
                const sf::Vector2f d1 = unit(next - cur);

                // Outward normal of a CCW polygon edge (dx,dy) is (dy,-dx):
                // the interior lies to the left of each edge.
                const sf::Vector2f n0(d0.y, -d0.x);
                const sf::Vector2f n1(d1.y, -d1.x);

                sf::Vector2f m = n0 + n1;
                const float mLen = std::sqrt(m.x * m.x + m.y * m.y);
                if (mLen < 1e-4f) { m = n1; }
                else { m.x /= mLen; m.y /= mLen; }

                // Miter length correction: 1 / cos(theta/2), via the dot of the
                // miter direction with one edge normal.
                const float cosHalf = std::max(0.2f, m.x * n1.x + m.y * n1.y);
                float ext = h / cosHalf;
                ext = std::min(ext, miterLimit);

                out.push_back(cur + m * ext);   // outer
                out.push_back(cur - m * ext);   // inner
            }
            return out;
        }

    } // namespace geom

    // ========================================================================
    // ARCHETYPE DEFINITION
    // ========================================================================

    struct ArchetypeDef {
        uint8_t     id = INVALID_ARCHETYPE;   ///< Index into EnemyRegistry::all()
        std::string key;                      ///< "RAIDER" -- the Lua table key
        std::string display;                  ///< "Raider" -- for HUD / debug
        std::string faction;                  ///< "RAKSHARI"

        /// The merged stat table. AISystem reads this exactly the way it used
        /// to read the global `enemy_config`, so none of its 1600 lines had to
        /// change signature.
        sol::table  config;

        // ---- Geometry (all in local ship space, +Y is aft) ----
        std::vector<sf::Vector2f> visual;      ///< Authored silhouette, may be concave
        std::vector<sf::Vector2f> visualTris;  ///< Ear-clipped triangle list of `visual`
        std::vector<sf::Vector2f> physics;     ///< Convex, <= 8 points, for Box2D
        std::vector<sf::Vector2f> turrets;     ///< Local-space turret mount points

        sf::Color color{ 255, 50, 50 };

        // ---- Spawn director data ----
        float weight = 0.f;        ///< Relative roll weight. 0 == never spontaneous.
        int   maxActive = 4;       ///< Per-archetype concurrency cap
        int   threatCost = 1;      ///< Charged against the faction threat budget
        bool  summonOnly = false;  ///< Only spawnable by another entity, never by the director

        float radius = 25.f;       ///< Cached max vertex distance, for spawn spacing
    };

    // ========================================================================
    // FACTION DEFINITION
    // ========================================================================

    struct FactionDef {
        std::string key;               ///< "RAKSHARI"
        std::string display;           ///< "Rakshari Marauders"
        bool  active = false;          ///< Is this faction currently fielding units?

        float spawnInterval = 4.f;     ///< Seconds between spawn attempts
        int   maxActive = 8;           ///< Hard cap on live units from this faction
        int   maxThreat = 10;          ///< Budget cap -- see threatCost

        std::vector<uint8_t> units;    ///< Archetype ids this faction can field
    };

    // ========================================================================
    // REGISTRY
    // ========================================================================

    /**
     * @class EnemyRegistry
     * @brief Owns the loaded archetype and faction tables.
     *
     * Lives in main() and is handed to systems through SystemContext, matching
     * how every other dependency in this project is passed. No globals.
     *
     * HOT RELOAD: load() must be called again after every F5. Re-running a Lua
     * script builds BRAND NEW tables, so every cached sol::table in here points
     * at the old, orphaned ones until it is refreshed. Symptom if you forget:
     * edits appear to do nothing, which is exactly the sol2 silent-default trap
     * that has bitten this project before.
     *
     * ID STABILITY: ids come from `archetype_order` in Lua, NOT from table
     * iteration order (which is a hash order and would reshuffle between runs).
     * Live enemies store their id in EnemyComponent, so a reload that changes
     * the order would silently turn every live Raider into a Barge. load()
     * detects that and warns.
     */
    class EnemyRegistry {
    public:

        bool load(sol::state& lua) {
            const std::vector<std::string> previousOrder = orderSnapshot();

            m_archetypes.clear();
            m_factions.clear();
            m_byKey.clear();

            sol::object orderObj = lua["archetype_order"];
            if (!orderObj.valid() || !orderObj.is<sol::table>()) {
                std::cerr << "[EnemyRegistry] `archetype_order` missing from enemy.lua. "
                    "No enemies will spawn.\n";
                return false;
            }
            sol::table order = orderObj.as<sol::table>();
            sol::table defs = lua["enemy_archetypes"];

            for (size_t i = 1; i <= order.size(); ++i) {
                const std::string key = order[i].get_or<std::string>("");
                if (key.empty()) continue;

                sol::object entry = defs[key];
                if (!entry.valid() || !entry.is<sol::table>()) {
                    std::cerr << "[EnemyRegistry] archetype_order lists \"" << key
                        << "\" but enemy_archetypes has no such table. Skipped.\n";
                    continue;
                }
                if (m_archetypes.size() >= 255) {
                    std::cerr << "[EnemyRegistry] More than 254 archetypes. "
                        "EnemyComponent::archetype is a uint8_t. Stopping.\n";
                    break;
                }
                m_archetypes.push_back(buildArchetype(key, entry.as<sol::table>(),
                    static_cast<uint8_t>(m_archetypes.size())));
                m_byKey[key] = m_archetypes.back().id;
            }

            loadFactions(lua);

            const std::vector<std::string> newOrder = orderSnapshot();
            if (!previousOrder.empty() && previousOrder != newOrder) {
                std::cerr << "[EnemyRegistry] WARNING: archetype_order changed on reload. "
                    "Any enemy alive right now is holding a stale id and will behave as a "
                    "different unit. Restart the run.\n";
            }
            return !m_archetypes.empty();
        }

        // ---- Lookup ----

        const std::vector<ArchetypeDef>& all() const { return m_archetypes; }
        const std::vector<FactionDef>& factions() const { return m_factions; }

        const ArchetypeDef* byId(uint8_t id) const {
            return (id < m_archetypes.size()) ? &m_archetypes[id] : nullptr;
        }

        uint8_t idOf(const std::string& key) const {
            auto it = m_byKey.find(key);
            return (it == m_byKey.end()) ? INVALID_ARCHETYPE : it->second;
        }

        const ArchetypeDef* byKey(const std::string& key) const {
            return byId(idOf(key));
        }

        /// Falls back to archetype 0 rather than returning null. AISystem runs
        /// per-frame on every enemy, and a null check that fires there would
        /// mean an enemy silently freezing instead of misbehaving visibly.
        const ArchetypeDef& resolve(uint8_t id) const {
            const ArchetypeDef* d = byId(id);
            return d ? *d : m_archetypes.front();
        }

        bool empty() const { return m_archetypes.empty(); }

    private:

        std::vector<std::string> orderSnapshot() const {
            std::vector<std::string> v;
            v.reserve(m_archetypes.size());
            for (const auto& a : m_archetypes) v.push_back(a.key);
            return v;
        }

        static std::vector<sf::Vector2f> readPoints(const sol::table& t, const char* field) {
            std::vector<sf::Vector2f> out;
            sol::object o = t[field];
            if (!o.valid() || !o.is<sol::table>()) return out;

            sol::table arr = o.as<sol::table>();
            for (size_t i = 1; i <= arr.size(); ++i) {
                sol::object p = arr[i];
                if (!p.valid() || !p.is<sol::table>()) continue;
                sol::table pt = p.as<sol::table>();
                // Accepts both {x=..,y=..} and {.., ..} so hull points can be
                // pasted straight from a shape editor.
                const float x = pt["x"].valid() ? pt["x"].get_or(0.f) : pt[1].get_or(0.f);
                const float y = pt["y"].valid() ? pt["y"].get_or(0.f) : pt[2].get_or(0.f);
                out.push_back({ x, y });
            }
            return out;
        }

        ArchetypeDef buildArchetype(const std::string& key, sol::table t, uint8_t id) {
            ArchetypeDef d;
            d.id = id;
            d.key = key;
            d.config = t;
            d.display = t["display"].get_or(key);
            d.faction = t["faction"].get_or<std::string>("RAKSHARI");

            sol::object c = t["color"];
            if (c.valid() && c.is<sol::table>()) {
                sol::table ct = c.as<sol::table>();
                d.color = sf::Color(
                    static_cast<uint8_t>(std::clamp(ct["r"].get_or(255.f), 0.f, 255.f)),
                    static_cast<uint8_t>(std::clamp(ct["g"].get_or(50.f), 0.f, 255.f)),
                    static_cast<uint8_t>(std::clamp(ct["b"].get_or(50.f), 0.f, 255.f)));
            }

            d.weight = t["spawn_weight"].get_or(0.f);
            d.maxActive = t["max_active"].get_or(4);
            d.threatCost = t["threat_cost"].get_or(1);
            d.summonOnly = t["summon_only"].get_or(false);

            // ---- Geometry ----
            const float scale = t["scale"].get_or(1.0f);
            d.visual = readPoints(t, "hull");
            for (auto& v : d.visual) { v.x *= scale; v.y *= scale; }

            if (d.visual.size() < 3) {
                std::cerr << "[EnemyRegistry] \"" << key << "\" has fewer than 3 hull points. "
                    "Substituting a triangle so it is visibly wrong rather than invisible.\n";
                d.visual = { {0.f, -20.f}, {16.f, 16.f}, {-16.f, 16.f} };
            }

            // Normalise winding to positive signed area (counter-clockwise in
            // maths terms; visually clockwise in SFML's y-down space). Both
            // triangulate() and outlineStrip() assume this, so it is done ONCE
            // here rather than re-checked on every draw.
            if (geom::signedArea(d.visual) < 0.f)
                std::reverse(d.visual.begin(), d.visual.end());

            d.visualTris = geom::triangulate(d.visual);

            const float hitboxScale = t["hitbox_scale"].get_or(1.0f);
            d.physics = geom::decimateConvex(geom::convexHull(d.visual), MAX_PHYSICS_POINTS);
            if (hitboxScale != 1.0f) {
                for (auto& v : d.physics) { v.x *= hitboxScale; v.y *= hitboxScale; }
            }

            d.turrets = readPoints(t, "turrets");
            for (auto& v : d.turrets) { v.x *= scale; v.y *= scale; }

            float r2 = 0.f;
            for (const auto& v : d.visual) r2 = std::max(r2, v.x * v.x + v.y * v.y);
            d.radius = std::sqrt(r2);

            // Hitbox/silhouette ratio. >1 means the convex collision hull is
            // fatter than the drawn ship, which is what concavity always costs.
            // Anything much past ~1.25 on a unit that is supposed to feel
            // evasive wants either a less notched silhouette or a lower
            // hitbox_scale.
            const float silA = std::fabs(geom::signedArea(d.visual));
            const float hitA = std::fabs(geom::signedArea(d.physics));
            const float ratio = (silA > 0.f) ? hitA / silA : 1.f;

            std::cout << "[EnemyRegistry] " << key
                << "  visual=" << d.visual.size() << "pts"
                << "  tris=" << (d.visualTris.size() / 3)
                << "  physics=" << d.physics.size() << "pts"
                << "  hitbox/silhouette=" << ratio
                << "  turrets=" << d.turrets.size()
                << "  r=" << d.radius << "\n";

            return d;
        }

        void loadFactions(sol::state& lua) {
            sol::object fo = lua["factions"];
            if (!fo.valid() || !fo.is<sol::table>()) {
                std::cerr << "[EnemyRegistry] `factions` missing from enemy.lua. "
                    "Nothing will spawn.\n";
                return;
            }
            sol::table ft = fo.as<sol::table>();

            sol::object ao = lua["active_factions"];
            std::vector<std::string> activeList;
            if (ao.valid() && ao.is<sol::table>()) {
                sol::table at = ao.as<sol::table>();
                for (size_t i = 1; i <= at.size(); ++i)
                    activeList.push_back(at[i].get_or<std::string>(""));
            }

            for (auto& kv : ft) {
                if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;

                FactionDef f;
                f.key = kv.first.as<std::string>();
                sol::table t = kv.second.as<sol::table>();

                f.display = t["display"].get_or(f.key);
                f.spawnInterval = t["spawn_interval"].get_or(4.0f);
                f.maxActive = t["max_active"].get_or(8);
                f.maxThreat = t["max_threat"].get_or(10);
                f.active = std::find(activeList.begin(), activeList.end(), f.key) != activeList.end();

                for (const auto& a : m_archetypes)
                    if (a.faction == f.key) f.units.push_back(a.id);

                std::cout << "[EnemyRegistry] faction " << f.key
                    << (f.active ? " [ACTIVE]" : " [idle]")
                    << "  units=" << f.units.size()
                    << "  threat<=" << f.maxThreat << "\n";

                m_factions.push_back(std::move(f));
            }
        }

        std::vector<ArchetypeDef> m_archetypes;
        std::vector<FactionDef>   m_factions;
        std::unordered_map<std::string, uint8_t> m_byKey;
    };

} // namespace enemyarch