/**
 * @file LevelGen.hpp
 * @brief Generated levels: sub-zone cells walled by the Void, plus the
 *        run-time state the map needs (fog of war, last-seen marks).
 *
 * ============================================================================
 * THE SHAPE
 * ============================================================================
 * A level is 8-12 square CELLS joined edge to edge, grown from the start cell
 * outward like a crystal. Each cell is one sub-zone -- a zone in zones.lua --
 * picked from the level KIND's pools by how far it is from the start, so a
 * MIXED level starts in open space and ends in the scrapyard.
 *
 * The Void is everything outside the cells. Its border is NOT the cells'
 * square outline: a signed distance field is baked once per level (positive
 * inside, negative outside, in px) and noise is added to it, so the edge
 * wanders in and out like a coastline. Every border question -- "how deep in
 * the Void am I", "may something spawn here", the map's outline -- reads
 * that one field. The outline itself is its zero contour, traced once with
 * marching squares.
 *
 * The rift goes in the cell farthest from the start (walking distance, not
 * straight-line), so reaching it means crossing the level.
 *
 * ============================================================================
 * PURE DATA
 * ============================================================================
 * No SFML drawing and no entities in here. LevelSystem draws, SystemManager
 * owns, EnemySystem asks inside(). A missing scripts/levels.lua leaves
 * LevelState inactive and every consumer behaves as if levels did not exist.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics/Color.hpp>
#include <SFML/System/Vector2.hpp>
#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <queue>
#include <limits>

namespace levelgen {

    // ========================================================================
    // CONFIG (scripts/levels.lua)
    // ========================================================================

    struct Subzone {
        std::string key;              ///< zone key in zones.lua ("YARD_CEMETERY")
        sf::Color   mapColor{ 40, 50, 60 };
    };

    struct Kind {
        std::string key, display, start, deepest;
        std::vector<std::pair<std::string, float>> nearPool, farPool;
        float farFrom = 1.f;
    };

    struct Config {
        float cellSize = 24000.f;
        int   cellsMin = 8, cellsMax = 12;
        float stretch = 0.55f;

        float edgeNoise = 2600.f, noiseScale = 9000.f, warnDistance = 3200.f;
        bool  damageOn = true;
        float damage = 5.f, damageInterval = 0.5f, damageDeep = 3.f, damageDeepAt = 2500.f;

        float riftRadius = 120.f, riftPull = 520.f;

        float scanRadius = 2600.f, fogCell = 1000.f, minimapRange = 6500.f;

        std::vector<Subzone> subzones;
        std::vector<Kind>    kinds;
        std::vector<std::string> cycle;
        bool loaded = false;

        const Subzone* subzone(const std::string& key) const {
            for (const auto& s : subzones) if (s.key == key) return &s;
            return nullptr;
        }
        const Kind* kind(const std::string& key) const {
            for (const auto& k : kinds) if (k.key == key) return &k;
            return nullptr;
        }
        /// The kind of the n-th level (0-based) in the endless cycle.
        const Kind* kindFor(int levelIndex) const {
            if (cycle.empty()) return kinds.empty() ? nullptr : &kinds.front();
            return kind(cycle[static_cast<size_t>(levelIndex) % cycle.size()]);
        }

        bool load(sol::state& lua) {
            loaded = false;
            sol::optional<sol::table> lt = lua["level"];
            if (!lt) return false;
            sol::table t = *lt;
            cellSize = std::max(4000.f, t["cell_size"].get_or(cellSize));
            cellsMin = std::max(2, t["cells_min"].get_or(cellsMin));
            cellsMax = std::max(cellsMin, t["cells_max"].get_or(cellsMax));
            stretch = std::clamp(t["stretch"].get_or(stretch), 0.f, 1.f);

            if (sol::optional<sol::table> b = t["border"]) {
                edgeNoise = (*b)["edge_noise"].get_or(edgeNoise);
                noiseScale = std::max(500.f, (*b)["noise_scale"].get_or(noiseScale));
                warnDistance = std::max(200.f, (*b)["warn_distance"].get_or(warnDistance));
                damageOn = (*b)["damage_enabled"].get_or(damageOn);
                damage = (*b)["damage"].get_or(damage);
                damageInterval = std::max(0.05f, (*b)["damage_interval"].get_or(damageInterval));
                damageDeep = std::max(1.f, (*b)["damage_deep"].get_or(damageDeep));
                damageDeepAt = std::max(1.f, (*b)["damage_deep_at"].get_or(damageDeepAt));
            }
            if (sol::optional<sol::table> r = t["rift"]) {
                riftRadius = (*r)["radius"].get_or(riftRadius);
                riftPull = (*r)["pull"].get_or(riftPull);
            }
            if (sol::optional<sol::table> m = t["map"]) {
                scanRadius = (*m)["scan_radius"].get_or(scanRadius);
                fogCell = std::max(200.f, (*m)["fog_cell"].get_or(fogCell));
                minimapRange = std::max(1000.f, (*m)["minimap_range"].get_or(minimapRange));
            }

            subzones.clear();
            if (sol::optional<sol::table> sz = t["subzones"]) {
                for (const auto& kv : *sz) {
                    if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
                    Subzone s;
                    s.key = kv.first.as<std::string>();
                    sol::table st = kv.second.as<sol::table>();
                    if (sol::optional<sol::table> c = st["map_color"])
                        s.mapColor = sf::Color((*c)["r"].get_or(40), (*c)["g"].get_or(50), (*c)["b"].get_or(60));
                    subzones.push_back(s);
                }
                std::sort(subzones.begin(), subzones.end(),
                    [](const Subzone& a, const Subzone& b) { return a.key < b.key; });
            }

            const auto readPool = [](sol::optional<sol::table> p) {
                std::vector<std::pair<std::string, float>> out;
                if (!p) return out;
                for (const auto& kv : *p)
                    if (kv.first.is<std::string>()) out.push_back({ kv.first.as<std::string>(), kv.second.as<float>() });
                // pairs() order is not stable across runs: sort, so a seed
                // always gives the same level.
                std::sort(out.begin(), out.end());
                return out;
            };
            kinds.clear();
            if (sol::optional<sol::table> ks = t["kinds"]) {
                for (const auto& kv : *ks) {
                    if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
                    sol::table kt = kv.second.as<sol::table>();
                    Kind k;
                    k.key = kv.first.as<std::string>();
                    k.display = kt["display"].get_or(k.key);
                    k.start = kt["start"].get_or(std::string());
                    k.deepest = kt["deepest"].get_or(std::string());
                    k.nearPool = readPool(kt["near"]);
                    k.farPool = readPool(kt["far"]);
                    if (k.farPool.empty()) k.farPool = k.nearPool;
                    k.farFrom = std::clamp(kt["far_from"].get_or(1.f), 0.f, 1.f);
                    if (!k.nearPool.empty()) kinds.push_back(k);
                }
                std::sort(kinds.begin(), kinds.end(), [](const Kind& a, const Kind& b) { return a.key < b.key; });
            }
            cycle.clear();
            if (sol::optional<sol::table> c = t["cycle"])
                for (size_t i = 1; i <= c->size(); ++i) {
                    const std::string k = (*c)[i].get_or<std::string>("");
                    if (kind(k)) cycle.push_back(k);
                }

            loaded = !kinds.empty() && !subzones.empty();
            if (!loaded) std::cerr << "[LevelGen] levels.lua has no usable kinds/subzones.\n";
            return loaded;
        }
    };

    // ========================================================================
    // THE LEVEL
    // ========================================================================

    // ---- What a sub-zone puts IN the level (filled by utils/FieldGen) ----

    /// One object placed at generation: a rock in a field, a pile in the yard,
    /// a dead ship in the cemetery. It only becomes an entity near the player
    /// (EnemySystem streams it in and out), so a level can hold thousands.
    struct Placed {
        sf::Vector2f pos;             ///< where it is (updated while live, so a drifter resumes where it went)
        sf::Vector2f vel;             ///< px/s drift
        float spin = 0.f;             ///< rad/s
        float angle = 0.f;            ///< degrees; rolled at generation, kept while live
        float radius = 30.f;          ///< px at its rolled size (nav and clearance)
        float sizeRoll = 0.5f;        ///< 0..1 across the type's size range; rolled once, at generation
        uint8_t type = 0;             ///< index into Level::placedTypes
        uint8_t state = 0;            ///< 0 waiting, 1 live, 2 gone (destroyed)
        bool  still = false;          ///< damped: shoved, it settles again
        int   cell = -1;
        int   poi = -1;               ///< index into Level::pois when it belongs to one (structures, caches, ore)
        uint32_t id = 0;              ///< entity id while live
    };

    /// A point of interest (scripts/pois.lua): structures, loot, guards and a
    /// shield, placed at generation; PoiSystem brings it to life.
    struct Poi {
        std::string type;             ///< poi_types key ("OUTPOST")
        std::string display;          ///< what the map calls it
        std::string icon;             ///< map glyph
        std::string variant;          ///< rolled sub-kind ("trap" / "convoy" for a distress call)
        sf::Vector2f pos;
        float radius = 600.f;         ///< footprint: nothing else is generated inside
        float shield = 0.f;           ///< px; 0 = none. Keeps outside rocks and junk out
        int   cell = -1;
        bool  discovered = false;     ///< on the map
        uint8_t state = 0;            ///< 0 waiting, 1 guards out, 2 cleared
        std::vector<std::string> guards;   ///< archetype keys
        std::vector<char> guardLoot;  ///< parallel: 1 = this guard carries the loot (the barge)
        std::vector<char> guardSleeps;///< parallel: 1 = laid out dormant (a docked Barge), whatever guardsDormant says
        bool  guardsDormant = false;
        float guardLeash = 900.f;
        std::vector<uint32_t> guardIds;
        std::vector<int> objects;     ///< indices into Level::placed
    };

    /// A sleeping pack in the wreck cemetery: dormant hulls among dead ones.
    /// Come close, or wake one, and they ALL come up together.
    struct PackSite {
        sf::Vector2f pos;
        std::vector<std::string> units;
        float wakeRadius = 650.f;
        uint8_t state = 0;            ///< 0 armed, 1 spawned (sleeping), 2 sprung
        std::vector<uint32_t> ids;
        std::vector<sf::Vector2f> slots;  ///< where each sleeper lies: in the gaps between its cover wrecks
        std::vector<float> slotAngle;     ///< and which way it faces (degrees)
        int cell = -1;
    };

    struct Level {
        int index = 0;                ///< 0-based: SECTOR 01 is index 0
        uint32_t seed = 0;
        std::string kind, kindDisplay;

        float cellSize = 24000.f;
        std::vector<sf::Vector2i> cells;        ///< grid coords; cell (0,0) is centred on the world origin
        std::vector<std::string>  cellZone;     ///< sub-zone key per cell
        std::vector<int>          cellDepth;    ///< walking distance from the start cell
        int startCell = 0, riftCell = 0;
        sf::Vector2f start, rift;

        // Signed distance field: +px inside, -px outside, noise included.
        sf::Vector2f sdfOrigin;
        int   sdfW = 0, sdfH = 0;
        float sdfStep = 800.f;
        std::vector<float> sdf;
        float boundsPad = 0.f;

        /// The Void border's outline, world px, as line segments.
        std::vector<std::pair<sf::Vector2f, sf::Vector2f>> outline;

        // ---- Contents (utils/FieldGen) ----
        std::vector<std::string> placedTypes;          ///< asteroids.lua keys, by Placed::type
        std::vector<Placed>      placed;
        std::vector<std::vector<int>> placedByCell;    ///< indices into `placed`
        std::vector<PackSite>    packs;
        std::vector<Poi>         pois;
        std::vector<float>       cellStream;           ///< per cell: stream heading (rad), NaN = none

        // ---- Navigation: what blocks a ship (static placed objects) ----
        // Counts, not flags, so a destroyed rock can give its cells back.
        sf::Vector2f navOrigin;
        int   navW = 0, navH = 0;
        float navStep = 160.f;
        std::vector<uint8_t> nav;
        bool hasNav() const { return !nav.empty(); }

        void navInit() {
            navStep = 160.f;
            navOrigin = boundsMin();
            const sf::Vector2f mx = boundsMax();
            navW = static_cast<int>(std::ceil((mx.x - navOrigin.x) / navStep)) + 1;
            navH = static_cast<int>(std::ceil((mx.y - navOrigin.y) / navStep)) + 1;
            nav.assign(static_cast<size_t>(navW) * navH, 0);
        }
        bool navCell(sf::Vector2f p, int& x, int& y) const {
            x = static_cast<int>(std::floor((p.x - navOrigin.x) / navStep));
            y = static_cast<int>(std::floor((p.y - navOrigin.y) / navStep));
            return x >= 0 && y >= 0 && x < navW && y < navH;
        }
        sf::Vector2f navCentre(int x, int y) const {
            return navOrigin + sf::Vector2f((x + 0.5f) * navStep, (y + 0.5f) * navStep);
        }
        bool navBlockedXY(int x, int y) const {
            if (x < 0 || y < 0 || x >= navW || y >= navH) return true;
            return nav[static_cast<size_t>(y) * navW + x] != 0;
        }
        bool navBlocked(sf::Vector2f p) const {
            if (nav.empty()) return false;
            int x, y;
            return navCell(p, x, y) && nav[static_cast<size_t>(y) * navW + x] != 0;
        }
        /// Anything blocked within r of p?
        bool navBlockedNear(sf::Vector2f p, float r) const {
            if (nav.empty()) return false;
            int x0, y0, x1, y1;
            navCell(p - sf::Vector2f(r, r), x0, y0);
            navCell(p + sf::Vector2f(r, r), x1, y1);
            for (int y = std::max(0, y0); y <= std::min(navH - 1, y1); ++y)
                for (int x = std::max(0, x0); x <= std::min(navW - 1, x1); ++x)
                    if (nav[static_cast<size_t>(y) * navW + x]) {
                        const sf::Vector2f c = navCentre(x, y);
                        if ((c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y) <= (r + navStep * 0.7f) * (r + navStep * 0.7f)) return true;
                    }
            return false;
        }
        /// Mark (+1) or release (-1) the cells a disc covers.
        void navStamp(sf::Vector2f p, float r, int delta) {
            if (nav.empty()) return;
            int x0, y0, x1, y1;
            navCell(p - sf::Vector2f(r, r), x0, y0);
            navCell(p + sf::Vector2f(r, r), x1, y1);
            for (int y = std::max(0, y0); y <= std::min(navH - 1, y1); ++y)
                for (int x = std::max(0, x0); x <= std::min(navW - 1, x1); ++x) {
                    const sf::Vector2f c = navCentre(x, y);
                    if ((c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y) > r * r) continue;
                    uint8_t& v = nav[static_cast<size_t>(y) * navW + x];
                    if (delta > 0) v = static_cast<uint8_t>(std::min(255, v + 1));
                    else if (v > 0) --v;
                }
        }
        /// Straight line a -> b clear of blocked cells?
        bool navClear(sf::Vector2f a, sf::Vector2f b) const {
            if (nav.empty()) return true;
            const sf::Vector2f d = b - a;
            const float L = std::sqrt(d.x * d.x + d.y * d.y);
            const int steps = std::max(1, static_cast<int>(L / (navStep * 0.5f)));
            for (int i = 1; i <= steps; ++i)
                if (navBlocked(a + d * (static_cast<float>(i) / steps))) return false;
            return true;
        }

        /**
         * @brief A* on the nav grid, inside a window around both ends.
         * @return false if no route (or the search ran out of budget).
         * The route comes back string-pulled: only the corners a ship
         * actually has to turn at.
         */
        bool navPath(sf::Vector2f from, sf::Vector2f to, std::vector<sf::Vector2f>& out, int budget = 9000) const {
            out.clear();
            if (nav.empty()) return false;
            int sx, sy, gx, gy;
            if (!navCell(from, sx, sy) || !navCell(to, gx, gy)) return false;
            // Goal inside a pile: the nearest open cell to it.
            if (navBlockedXY(gx, gy)) {
                bool found = false;
                for (int r = 1; r <= 8 && !found; ++r)
                    for (int dy = -r; dy <= r && !found; ++dy)
                        for (int dx = -r; dx <= r && !found; ++dx)
                            if (!navBlockedXY(gx + dx, gy + dy)) { gx += dx; gy += dy; found = true; }
                if (!found) return false;
            }
            const int pad = 30;
            const int wx0 = std::max(0, std::min(sx, gx) - pad), wy0 = std::max(0, std::min(sy, gy) - pad);
            const int wx1 = std::min(navW - 1, std::max(sx, gx) + pad), wy1 = std::min(navH - 1, std::max(sy, gy) + pad);
            const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
            if (ww * wh > 260 * 260) return false;
            std::vector<float> g(static_cast<size_t>(ww) * wh, std::numeric_limits<float>::max());
            std::vector<int> parent(static_cast<size_t>(ww) * wh, -1);
            const auto idx = [&](int x, int y) { return (y - wy0) * ww + (x - wx0); };
            const auto h = [&](int x, int y) {
                const float dx = static_cast<float>(std::abs(x - gx)), dy = static_cast<float>(std::abs(y - gy));
                return (dx + dy) + (1.4142f - 2.f) * std::min(dx, dy);
            };
            using Node = std::pair<float, int>;
            std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
            const int s = idx(sx, sy), goal = idx(gx, gy);
            g[static_cast<size_t>(s)] = 0.f;
            open.push({ h(sx, sy), s });
            int expanded = 0;
            bool reached = false;
            static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            while (!open.empty() && expanded < budget) {
                const auto [f, cur] = open.top(); open.pop();
                const int cx = cur % ww + wx0, cy = cur / ww + wy0;
                if (f - h(cx, cy) > g[static_cast<size_t>(cur)] + 0.001f) continue;
                ++expanded;
                if (cur == goal) { reached = true; break; }
                for (int k = 0; k < 8; ++k) {
                    const int nx = cx + DX[k], ny = cy + DY[k];
                    if (nx < wx0 || ny < wy0 || nx > wx1 || ny > wy1 || navBlockedXY(nx, ny)) continue;
                    if (k >= 4 && (navBlockedXY(cx + DX[k], cy) || navBlockedXY(cx, cy + DY[k]))) continue;   // no corner cutting
                    const int ni = idx(nx, ny);
                    const float ng = g[static_cast<size_t>(cur)] + (k < 4 ? 1.f : 1.4142f);
                    if (ng < g[static_cast<size_t>(ni)]) {
                        g[static_cast<size_t>(ni)] = ng;
                        parent[static_cast<size_t>(ni)] = cur;
                        open.push({ ng + h(nx, ny), ni });
                    }
                }
            }
            if (!reached) return false;
            std::vector<sf::Vector2f> raw;
            for (int c = goal; c != -1; c = parent[static_cast<size_t>(c)]) raw.push_back(navCentre(c % ww + wx0, c / ww + wy0));
            std::reverse(raw.begin(), raw.end());
            // String-pull.
            sf::Vector2f anchor = from;
            for (size_t i = 1; i < raw.size(); ++i)
                if (!navClear(anchor, raw[i])) { out.push_back(raw[i - 1]); anchor = raw[i - 1]; }
            out.push_back(raw.empty() ? to : raw.back());
            return true;
        }

        sf::Vector2f cellCentre(int i) const {
            return { cells[static_cast<size_t>(i)].x * cellSize, cells[static_cast<size_t>(i)].y * cellSize };
        }

        /// The cell a point is in, or -1 in the Void (ignores the noisy edge).
        int cellAt(sf::Vector2f p) const {
            const int gx = static_cast<int>(std::floor(p.x / cellSize + 0.5f));
            const int gy = static_cast<int>(std::floor(p.y / cellSize + 0.5f));
            for (size_t i = 0; i < cells.size(); ++i)
                if (cells[i].x == gx && cells[i].y == gy) return static_cast<int>(i);
            return -1;
        }

        /**
         * @brief How much each nearby cell "owns" a point, for blending across
         * sub-zone borders. Own cell 1; a neighbour 1 at the shared border,
         * falling to 0 `band` px past it; smoothed and normalised. Exactly on
         * a border two cells split it 50/50, so whatever is blended from these
         * weights is continuous -- nothing pops as the player crosses.
         * `out` gets (cell index, weight) pairs that sum to 1.
         */
        void cellWeights(sf::Vector2f p, float band, std::vector<std::pair<int, float>>& out) const {
            out.clear();
            if (cells.empty()) return;
            int c0 = cellAt(p);
            if (c0 < 0) c0 = nearestCell(p);
            const sf::Vector2i g = cells[static_cast<size_t>(c0)];
            const float h = cellSize * 0.5f;
            float sum = 0.f;
            for (size_t i = 0; i < cells.size(); ++i) {
                const sf::Vector2i d = cells[i] - g;
                if (std::abs(d.x) > 1 || std::abs(d.y) > 1) continue;
                const sf::Vector2f c = cellCentre(static_cast<int>(i));
                const float dx = std::max(0.f, std::fabs(p.x - c.x) - h);
                const float dy = std::max(0.f, std::fabs(p.y - c.y) - h);
                float w = static_cast<int>(i) == c0 ? 1.f : std::clamp(1.f - std::sqrt(dx * dx + dy * dy) / band, 0.f, 1.f);
                w = w * w * (3.f - 2.f * w);
                if (w <= 0.f) continue;
                out.push_back({ static_cast<int>(i), w });
                sum += w;
            }
            for (auto& e : out) e.second /= sum;
        }

        /// The nearest cell to a point -- for things just past the edge.
        int nearestCell(sf::Vector2f p) const {
            int best = -1; float bd = 1e30f;
            for (size_t i = 0; i < cells.size(); ++i) {
                const sf::Vector2f c = cellCentre(static_cast<int>(i));
                const float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
                if (d < bd) { bd = d; best = static_cast<int>(i); }
            }
            return best;
        }

        /// Signed distance to the Void's edge: +px inside, -px in the Void.
        float edgeDist(sf::Vector2f p) const {
            if (sdf.empty()) return 1e9f;
            const float fx = (p.x - sdfOrigin.x) / sdfStep, fy = (p.y - sdfOrigin.y) / sdfStep;
            const float cx = std::clamp(fx, 0.f, static_cast<float>(sdfW - 1) - 0.001f);
            const float cy = std::clamp(fy, 0.f, static_cast<float>(sdfH - 1) - 0.001f);
            const int x0 = static_cast<int>(cx), y0 = static_cast<int>(cy);
            const float tx = cx - x0, ty = cy - y0;
            const auto at = [&](int x, int y) { return sdf[static_cast<size_t>(y) * sdfW + x]; };
            const float v = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty)
                + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
            // Off the baked grid: keep falling off, so "far away" stays "deep".
            const float ox = std::max(0.f, std::fabs(fx - cx) * sdfStep), oy = std::max(0.f, std::fabs(fy - cy) * sdfStep);
            return v - std::sqrt(ox * ox + oy * oy);
        }

        bool inside(sf::Vector2f p, float margin = 0.f) const { return edgeDist(p) > margin; }

        /// World rectangle covering the whole level and a little Void.
        sf::Vector2f boundsMin() const { return sdfOrigin; }
        sf::Vector2f boundsMax() const {
            return { sdfOrigin.x + (sdfW - 1) * sdfStep, sdfOrigin.y + (sdfH - 1) * sdfStep };
        }
    };

    // ========================================================================
    // GENERATION
    // ========================================================================

    namespace detail {

        inline float hash2(int x, int y, uint32_t seed) {
            uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return (h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
        }
        /// Smooth value noise in [-1, 1].
        inline float vnoise(float x, float y, uint32_t seed) {
            const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            float tx = x - x0, ty = y - y0;
            tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
            const float a = hash2(x0, y0, seed), b = hash2(x0 + 1, y0, seed);
            const float c = hash2(x0, y0 + 1, seed), d = hash2(x0 + 1, y0 + 1, seed);
            return ((a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty) * 2.f - 1.f;
        }
        inline float fbm(float x, float y, uint32_t seed) {
            return 0.65f * vnoise(x, y, seed) + 0.35f * vnoise(x * 2.3f + 17.f, y * 2.3f - 9.f, seed + 101u);
        }

        inline float segDist(sf::Vector2f p, sf::Vector2f a, sf::Vector2f b) {
            const sf::Vector2f ab = b - a, ap = p - a;
            const float l2 = ab.x * ab.x + ab.y * ab.y;
            const float t = l2 > 0.f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.f, 1.f) : 0.f;
            const sf::Vector2f q = a + ab * t - p;
            return std::sqrt(q.x * q.x + q.y * q.y);
        }

        template <class Rng>
        const std::string& pick(const std::vector<std::pair<std::string, float>>& pool, Rng& rng,
            const std::vector<std::string>& avoid) {
            static const std::string none;
            if (pool.empty()) return none;
            float total = 0.f;
            std::vector<float> w(pool.size());
            for (size_t i = 0; i < pool.size(); ++i) {
                float x = std::max(0.f, pool[i].second);
                // Neighbours of the same sub-zone are allowed, just less likely:
                // a level reads as places, not as one big smear.
                for (const auto& a : avoid) if (a == pool[i].first) x *= 0.35f;
                w[i] = x; total += x;
            }
            if (total <= 0.f) return pool.front().first;
            float r = std::uniform_real_distribution<float>(0.f, total)(rng);
            for (size_t i = 0; i < pool.size(); ++i) { r -= w[i]; if (r <= 0.f) return pool[i].first; }
            return pool.back().first;
        }
    }

    /**
     * @brief Build level `index` of the run from `seed`.
     * Same config + index + seed = the same level, every time.
     */
    inline Level generate(const Config& cfg, int index, uint32_t seed) {
        Level L;
        L.index = index;
        L.seed = seed;
        L.cellSize = cfg.cellSize;
        const Kind* kind = cfg.kindFor(index);
        if (!kind) return L;
        L.kind = kind->key;
        L.kindDisplay = kind->display;

        std::mt19937 rng(seed ^ (0x9E3779B9u * static_cast<uint32_t>(index + 1)));
        const auto has = [&](sf::Vector2i c) {
            for (const auto& q : L.cells) if (q == c) return true;
            return false;
        };

        // ---- Grow the shape ----
        const int target = std::uniform_int_distribution<int>(cfg.cellsMin, cfg.cellsMax)(rng);
        L.cells.push_back({ 0, 0 });
        const sf::Vector2i dirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        int guard = 0;
        while (static_cast<int>(L.cells.size()) < target && guard++ < 2000) {
            // Stretch: grow from one of the newest cells (corridors), else
            // from anywhere (blobs).
            size_t from;
            if (std::uniform_real_distribution<float>(0.f, 1.f)(rng) < cfg.stretch) {
                const size_t n = L.cells.size();
                from = n - 1 - std::min<size_t>(n - 1, std::uniform_int_distribution<size_t>(0, 1)(rng));
            }
            else from = std::uniform_int_distribution<size_t>(0, L.cells.size() - 1)(rng);
            const sf::Vector2i c = L.cells[from] + dirs[std::uniform_int_distribution<int>(0, 3)(rng)];
            if (!has(c)) L.cells.push_back(c);
        }

        // ---- Walking distance from the start ----
        const size_t n = L.cells.size();
        L.cellDepth.assign(n, -1);
        L.cellDepth[0] = 0;
        std::vector<size_t> q{ 0 };
        for (size_t h = 0; h < q.size(); ++h) {
            for (const auto& d : dirs) {
                const sf::Vector2i nb = L.cells[q[h]] + d;
                for (size_t j = 0; j < n; ++j)
                    if (L.cells[j] == nb && L.cellDepth[j] < 0) { L.cellDepth[j] = L.cellDepth[q[h]] + 1; q.push_back(j); }
            }
        }
        int maxDepth = 0;
        for (int d : L.cellDepth) maxDepth = std::max(maxDepth, d);

        // ---- The rift: one of the deepest cells ----
        std::vector<int> deepest;
        for (size_t i = 0; i < n; ++i) if (L.cellDepth[i] == maxDepth) deepest.push_back(static_cast<int>(i));
        L.riftCell = deepest[std::uniform_int_distribution<size_t>(0, deepest.size() - 1)(rng)];
        L.startCell = 0;

        // ---- Sub-zones, nearest first so the neighbour rule sees them ----
        L.cellZone.assign(n, std::string());
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return L.cellDepth[a] < L.cellDepth[b]; });
        for (size_t i : order) {
            if (static_cast<int>(i) == L.startCell && !kind->start.empty()) { L.cellZone[i] = kind->start; continue; }
            if (static_cast<int>(i) == L.riftCell && !kind->deepest.empty()) { L.cellZone[i] = kind->deepest; continue; }
            std::vector<std::string> avoid;
            for (const auto& d : dirs)
                for (size_t j = 0; j < n; ++j)
                    if (L.cells[j] == L.cells[i] + d && !L.cellZone[j].empty()) avoid.push_back(L.cellZone[j]);
            const bool far = maxDepth > 0
                && static_cast<float>(L.cellDepth[i]) >= kind->farFrom * static_cast<float>(maxDepth) - 0.01f
                && kind->farFrom < 1.f;
            L.cellZone[i] = detail::pick(far ? kind->farPool : kind->nearPool, rng, avoid);
        }

        // ---- Start and rift positions ----
        const float S = cfg.cellSize;
        L.start = L.cellCentre(L.startCell);
        {
            const sf::Vector2f c = L.cellCentre(L.riftCell);
            std::uniform_real_distribution<float> off(-0.22f * S, 0.22f * S);
            L.rift = c + sf::Vector2f(off(rng), off(rng));
        }

        // ---- Signed distance field ----
        // Boundary = every cell side that faces the Void.
        std::vector<std::pair<sf::Vector2f, sf::Vector2f>> edges;
        const float h = S * 0.5f;
        for (size_t i = 0; i < n; ++i) {
            const sf::Vector2f c = L.cellCentre(static_cast<int>(i));
            if (!has(L.cells[i] + sf::Vector2i(1, 0)))  edges.push_back({ c + sf::Vector2f(h, -h), c + sf::Vector2f(h, h) });
            if (!has(L.cells[i] + sf::Vector2i(-1, 0))) edges.push_back({ c + sf::Vector2f(-h, -h), c + sf::Vector2f(-h, h) });
            if (!has(L.cells[i] + sf::Vector2i(0, 1)))  edges.push_back({ c + sf::Vector2f(-h, h), c + sf::Vector2f(h, h) });
            if (!has(L.cells[i] + sf::Vector2i(0, -1))) edges.push_back({ c + sf::Vector2f(-h, -h), c + sf::Vector2f(h, -h) });
        }
        int gx0 = 0, gx1 = 0, gy0 = 0, gy1 = 0;
        for (const auto& c : L.cells) { gx0 = std::min(gx0, c.x); gx1 = std::max(gx1, c.x); gy0 = std::min(gy0, c.y); gy1 = std::max(gy1, c.y); }
        L.boundsPad = cfg.warnDistance + cfg.edgeNoise + 4000.f;
        L.sdfStep = std::clamp(S / 30.f, 400.f, 1200.f);
        L.sdfOrigin = { gx0 * S - h - L.boundsPad, gy0 * S - h - L.boundsPad };
        L.sdfW = static_cast<int>(std::ceil(((gx1 - gx0 + 1) * S + 2 * L.boundsPad) / L.sdfStep)) + 1;
        L.sdfH = static_cast<int>(std::ceil(((gy1 - gy0 + 1) * S + 2 * L.boundsPad) / L.sdfStep)) + 1;
        L.sdf.assign(static_cast<size_t>(L.sdfW) * L.sdfH, 0.f);
        const uint32_t nseed = seed * 31u + static_cast<uint32_t>(index) * 7u + 3u;
        for (int y = 0; y < L.sdfH; ++y)
            for (int x = 0; x < L.sdfW; ++x) {
                const sf::Vector2f p = L.sdfOrigin + sf::Vector2f(x * L.sdfStep, y * L.sdfStep);
                float d = 1e30f;
                for (const auto& e : edges) d = std::min(d, detail::segDist(p, e.first, e.second));
                const bool in = L.cellAt(p) >= 0;
                float v = in ? d : -d;
                v += cfg.edgeNoise * detail::fbm(p.x / cfg.noiseScale, p.y / cfg.noiseScale, nseed);
                L.sdf[static_cast<size_t>(y) * L.sdfW + x] = v;
            }
        // The start and the rift are never swallowed by a bump of the edge.
        for (int y = 0; y < L.sdfH; ++y)
            for (int x = 0; x < L.sdfW; ++x) {
                const sf::Vector2f p = L.sdfOrigin + sf::Vector2f(x * L.sdfStep, y * L.sdfStep);
                for (const sf::Vector2f k : { L.start, L.rift }) {
                    const float dk = std::hypot(p.x - k.x, p.y - k.y);
                    const float keep = cfg.warnDistance + 3000.f - dk;   // >= this much inside near them
                    float& v = L.sdf[static_cast<size_t>(y) * L.sdfW + x];
                    if (keep > 0.f && v < keep) v = std::max(v, keep * 0.9f);
                }
            }

        // ---- Outline: the zero contour (marching squares) ----
        const auto P = [&](int x, int y) { return L.sdfOrigin + sf::Vector2f(x * L.sdfStep, y * L.sdfStep); };
        const auto V = [&](int x, int y) { return L.sdf[static_cast<size_t>(y) * L.sdfW + x]; };
        const auto lerpZ = [&](sf::Vector2f a, float va, sf::Vector2f b, float vb) {
            const float t = (va == vb) ? 0.5f : std::clamp(va / (va - vb), 0.f, 1.f);
            return a + (b - a) * t;
        };
        for (int y = 0; y + 1 < L.sdfH; ++y)
            for (int x = 0; x + 1 < L.sdfW; ++x) {
                const float v0 = V(x, y), v1 = V(x + 1, y), v2 = V(x + 1, y + 1), v3 = V(x, y + 1);
                const int code = (v0 > 0) | ((v1 > 0) << 1) | ((v2 > 0) << 2) | ((v3 > 0) << 3);
                if (code == 0 || code == 15) continue;
                const sf::Vector2f p0 = P(x, y), p1 = P(x + 1, y), p2 = P(x + 1, y + 1), p3 = P(x, y + 1);
                const sf::Vector2f eT = lerpZ(p0, v0, p1, v1), eR = lerpZ(p1, v1, p2, v2);
                const sf::Vector2f eB = lerpZ(p3, v3, p2, v2), eL = lerpZ(p0, v0, p3, v3);
                switch (code) {
                case 1: case 14: L.outline.push_back({ eL, eT }); break;
                case 2: case 13: L.outline.push_back({ eT, eR }); break;
                case 3: case 12: L.outline.push_back({ eL, eR }); break;
                case 4: case 11: L.outline.push_back({ eR, eB }); break;
                case 6: case 9:  L.outline.push_back({ eT, eB }); break;
                case 7: case 8:  L.outline.push_back({ eL, eB }); break;
                case 5:  L.outline.push_back({ eL, eT }); L.outline.push_back({ eR, eB }); break;
                case 10: L.outline.push_back({ eT, eR }); L.outline.push_back({ eB, eL }); break;
                default: break;
                }
            }
        return L;
    }

    // ========================================================================
    // RUN-TIME STATE (SystemManager owns one; systems get a pointer)
    // ========================================================================

    /// What the map remembers about something it saw.
    enum class MarkKind : uint8_t { Hostile, Hulk, Rift };

    struct Mark {
        sf::Vector2f pos;
        MarkKind kind = MarkKind::Hostile;
        float size = 1.f;     ///< relative blip size
    };

    struct LevelState {
        const Config* cfg = nullptr;
        Level level;
        bool active = false;

        // ---- Fog of war ----
        sf::Vector2f fogOrigin;
        int fogW = 0, fogH = 0;
        float fogCell = 1000.f;
        std::vector<uint8_t> fog;                  ///< 1 = seen at least once
        std::vector<uint8_t> cellVisited;          ///< per level cell: entered once
        std::unordered_map<uint32_t, Mark> marks;  ///< by entity id, last seen
        uint32_t fogVersion = 0;                   ///< bumps when fog changes (map caches)

        // ---- Live, written by LevelSystem ----
        int   playerCell = -1;
        float playerEdge = 1e9f;    ///< player's edgeDist
        float voidFade = 0.f;       ///< 0 deep inside .. 1 at / past the edge
        bool  mapOpen = false;
        bool  wantNextLevel = false;

        void begin(const Config& c, Level&& L) {
            cfg = &c;
            level = std::move(L);
            active = !level.cells.empty();
            fogCell = c.fogCell;
            fogOrigin = level.boundsMin();
            const sf::Vector2f mx = level.boundsMax();
            fogW = static_cast<int>(std::ceil((mx.x - fogOrigin.x) / fogCell)) + 1;
            fogH = static_cast<int>(std::ceil((mx.y - fogOrigin.y) / fogCell)) + 1;
            fog.assign(static_cast<size_t>(fogW) * fogH, 0);
            cellVisited.assign(level.cells.size(), 0);
            marks.clear();
            placedById.clear();
            ++fogVersion;
            playerCell = -1;
            playerEdge = 1e9f;
            voidFade = 0.f;
            mapOpen = false;
            wantNextLevel = false;
        }
        void end() { active = false; marks.clear(); }

        /// Dev: the whole map, as if every corner had been flown.
        void revealAll() {
            std::fill(fog.begin(), fog.end(), static_cast<uint8_t>(1));
            std::fill(cellVisited.begin(), cellVisited.end(), static_cast<uint8_t>(1));
            for (auto& p : level.pois) p.discovered = true;
            ++fogVersion;
        }

        /// May a body spawn here? (Inside, with a margin from the edge.)
        bool spawnable(sf::Vector2f p, float margin = 500.f, float clearance = 0.f) const {
            if (!active) return true;
            if (!level.inside(p, margin)) return false;
            return clearance <= 0.f || !level.navBlockedNear(p, clearance);
        }

        /// Entity id -> index in level.placed, for everything streamed in.
        std::unordered_map<uint32_t, int> placedById;

        /// The sub-zone a point is in (nearest cell past the edge).
        int cellOf(sf::Vector2f p) const {
            int c = level.cellAt(p);
            return c >= 0 ? c : level.nearestCell(p);
        }

        bool seen(int fx, int fy) const {
            if (fx < 0 || fy < 0 || fx >= fogW || fy >= fogH) return false;
            return fog[static_cast<size_t>(fy) * fogW + fx] != 0;
        }
        /// Reveal everything within r of p. @return true if anything new.
        bool reveal(sf::Vector2f p, float r) {
            bool changed = false;
            const int x0 = std::max(0, static_cast<int>((p.x - r - fogOrigin.x) / fogCell));
            const int x1 = std::min(fogW - 1, static_cast<int>((p.x + r - fogOrigin.x) / fogCell) + 1);
            const int y0 = std::max(0, static_cast<int>((p.y - r - fogOrigin.y) / fogCell));
            const int y1 = std::min(fogH - 1, static_cast<int>((p.y + r - fogOrigin.y) / fogCell) + 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const sf::Vector2f c = fogOrigin + sf::Vector2f((x + 0.5f) * fogCell, (y + 0.5f) * fogCell);
                    if ((c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y) > r * r) continue;
                    uint8_t& f = fog[static_cast<size_t>(y) * fogW + x];
                    if (!f) { f = 1; changed = true; }
                }
            if (changed) ++fogVersion;
            return changed;
        }
    };

} // namespace levelgen
