/**
 * @file FieldGen.hpp
 * @brief What each sub-zone puts INTO a generated level: placed fields of
 *        rocks, piles and wrecks, sleeping packs, comet-stream headings,
 *        and the navigation grid enemies route through.
 *
 * ============================================================================
 * DATA FROM zones.lua, PER SUB-ZONE
 * ============================================================================
 *   field = {                -- objects placed at generation, streamed in near the ship
 *     spacing      = 260,    -- px between candidate spots (jittered)
 *     fill         = 0.55,   -- share of the cell the noise lets through, 0..1
 *     density      = 0.6,    -- chance a spot in a dense patch is used
 *     noise_scale  = 4200,   -- px between patches
 *     types        = { LARGE = 50, MEDIUM = 40 },   -- weighted, asteroids.lua keys
 *     magma        = { type = "MAGMATIC", replaces = "MEDIUM", share = 0.3, scale = 2600 },
 *     clump        = { chance = 0.45, count = { 1, 3 }, gap = { 6, 40 },   -- smaller pieces
 *                      types = { MEDIUM = 75, SMALL = 25 } },             -- lying against a big one
 *     border_fade  = 3200,   -- px over which the field thins out toward a different sub-zone
 *     tunnels      = 2,      -- extra winding lanes beyond the ones joining neighbours
 *     tunnel_width = 700,    -- px of clear lane
 *     drift        = 0,      -- px/s: 0 = still (and enemies route around them)
 *     spin         = 0.15,   -- rad/s, max
 *     edge_margin  = 900,    -- px kept clear inside the Void's edge
 *   }
 *   packs = {                -- sleeping packs (wreck cemetery)
 *     count = { 2, 4 },  size = { 3, 5 },
 *     units = { RAIDER = 3, BERSERKER = 2 },
 *     cover = { WRECK_RAIDER = 2, WRECK_LIGHT = 1 },  -- the dead ships they hide among
 *     wake_radius = 650,
 *   }
 *   rock_mode = "field" | "stream" | "drift"   -- see EnemySystem
 *
 * Everything is rolled from the level's seed, so a level is the same level
 * every time it is generated -- including each object's SIZE, which is rolled
 * here and handed to createAsteroid, so nothing is ever generated overlapping
 * and a rock that streams out and back in comes back the same rock.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/LevelGen.hpp"
#include "utils/ZoneArchetypes.hpp"
#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>
#include <string>
#include <vector>

namespace fieldgen {

    namespace detail {

        using Pool = std::vector<std::pair<std::string, float>>;

        inline Pool readPool(sol::optional<sol::table> t) {
            Pool out;
            if (!t) return out;
            for (const auto& kv : *t)
                if (kv.first.is<std::string>() && kv.second.is<double>())
                    out.push_back({ kv.first.as<std::string>(), kv.second.as<float>() });
            std::sort(out.begin(), out.end());
            return out;
        }

        template <class Rng>
        const std::string& roll(const Pool& p, Rng& rng) {
            static const std::string none;
            float total = 0.f;
            for (const auto& e : p) total += std::max(0.f, e.second);
            if (total <= 0.f) return none;
            float r = std::uniform_real_distribution<float>(0.f, total)(rng);
            for (const auto& e : p) { r -= std::max(0.f, e.second); if (r <= 0.f) return e.first; }
            return p.back().first;
        }

        inline float segDist(sf::Vector2f p, sf::Vector2f a, sf::Vector2f b) {
            return levelgen::detail::segDist(p, a, b);
        }

        /// Same on every compiler (std::hash is not), so a seed is a seed everywhere.
        inline uint32_t fnv(const std::string& s) {
            uint32_t x = 2166136261u;
            for (unsigned char ch : s) { x ^= ch; x *= 16777619u; }
            return x;
        }

        /// Radius in px of an asteroids.lua type at a size roll (0..1 across
        /// its size_variance range) -- the same maths as createAsteroid.
        inline float radiusOf(sol::state& lua, const std::string& type, float sizeRoll = 1.f) {
            sol::optional<sol::table> t = lua["asteroid_types"][type];
            if (!t) return 30.f;
            const float mn = (*t)["size_variance_min"].get_or(0.85f), mx = (*t)["size_variance_max"].get_or(1.15f);
            return (*t)["base_size"].get_or(1.f) * 30.f * (mn + (mx - mn) * sizeRoll);
        }
    }

    /**
     * @brief Fill a freshly generated level.
     * Call after levelgen::generate(). Safe with zones missing (does nothing).
     */
    inline void populate(levelgen::Level& L, const zonearch::ZoneRegistry& reg, sol::state& lua) {
        using namespace detail;
        const size_t nCells = L.cells.size();
        L.placed.clear();
        L.placedTypes.clear();
        L.packs.clear();
        L.placedByCell.assign(nCells, {});
        L.cellStream.assign(nCells, std::numeric_limits<float>::quiet_NaN());
        L.navInit();
        if (nCells == 0) return;

        std::mt19937 rng(L.seed * 2654435761u + static_cast<uint32_t>(L.index) * 97u + 11u);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        const float S = L.cellSize, h = S * 0.5f;
        const auto has = [&](sf::Vector2i c) { for (const auto& q : L.cells) if (q == c) return true; return false; };
        const auto typeIndex = [&](const std::string& k) -> uint8_t {
            for (size_t i = 0; i < L.placedTypes.size(); ++i) if (L.placedTypes[i] == k) return static_cast<uint8_t>(i);
            L.placedTypes.push_back(k);
            return static_cast<uint8_t>(L.placedTypes.size() - 1);
        };

        // Level-wide spatial hash of everything placed, so nothing is ever
        // generated overlapping (Box2D would fling the pair apart on spawn).
        constexpr float HB = 256.f;
        std::unordered_map<int64_t, std::vector<int>> hash;
        const auto hkey = [](int x, int y) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(y); };
        const auto fits = [&](sf::Vector2f p, float r, float gap) {
            const int bx = static_cast<int>(std::floor(p.x / HB)), by = static_cast<int>(std::floor(p.y / HB));
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                auto it = hash.find(hkey(bx + dx, by + dy));
                if (it == hash.end()) continue;
                for (int i : it->second) {
                    const auto& o = L.placed[static_cast<size_t>(i)];
                    const float m = o.radius + r + gap;
                    const float ex = o.pos.x - p.x, ey = o.pos.y - p.y;
                    if (ex * ex + ey * ey < m * m) return false;
                }
            }
            return true;
        };

        // Each placed object's size is rolled HERE, not when it streams in, so
        // the generator knows exactly how big it is (piles can touch without
        // overlapping) and the same rock comes back the same size.
        const auto sizeRoll = [&]() { return (U(rng) + U(rng)) * 0.5f; };
        // `per_level = { a, b }` on a POI type: how many the whole level may
        // hold (the Dreadnought), rolled once, spent cell by cell.
        std::unordered_map<std::string, int> poiLevelLeft;

        for (size_t ci = 0; ci < nCells; ++ci) {
            const zonearch::ZoneDef* z = reg.byId(reg.idOf(L.cellZone[ci]));
            if (!z || !z->config.valid()) continue;
            const sol::table zc = z->config;
            const sf::Vector2f c = L.cellCentre(static_cast<int>(ci));

            // ---- Comet stream: one heading per cell ----
            if (zc["rock_mode"].get_or<std::string>("") == "stream")
                L.cellStream[ci] = U(rng) * 6.2831853f;

            sol::optional<sol::table> ft = zc["field"];
            sol::optional<sol::table> pt = zc["packs"];
            sol::optional<sol::table> poiT = zc["pois"];
            if (!ft && !pt && !poiT) continue;

            // ---- Lanes: centre to every neighbour's doorway, plus extras ----
            std::vector<std::pair<sf::Vector2f, sf::Vector2f>> lanes;
            const float laneW = ft ? (*ft)["tunnel_width"].get_or(700.f) : 0.f;
            const auto meander = [&](sf::Vector2f a, sf::Vector2f b) {
                const int n = 5;
                sf::Vector2f prev = a;
                const sf::Vector2f d = b - a;
                const sf::Vector2f nrm(-d.y, d.x);
                const float L2 = std::max(1.f, std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y));
                for (int k = 1; k <= n; ++k) {
                    const float t = static_cast<float>(k) / n;
                    const float wob = (k == n) ? 0.f : (U(rng) - 0.5f) * 0.35f;
                    const sf::Vector2f q = a + d * t + nrm / L2 * (wob * std::sqrt(d.x * d.x + d.y * d.y));
                    lanes.push_back({ prev, q });
                    prev = q;
                }
            };
            if (ft) {
                const sf::Vector2i g = L.cells[ci];
                const sf::Vector2i dirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (const auto& d : dirs)
                    if (has(g + d)) {
                        const sf::Vector2f door = c + sf::Vector2f(d.x * h, d.y * h)
                            + sf::Vector2f(d.y * (U(rng) - 0.5f) * S * 0.4f, d.x * (U(rng) - 0.5f) * S * 0.4f);
                        meander(c, door);
                    }
                const int extra = (*ft)["tunnels"].get_or(2);
                for (int k = 0; k < extra; ++k)
                    meander(c, c + sf::Vector2f((U(rng) - 0.5f) * S * 0.85f, (U(rng) - 0.5f) * S * 0.85f));
            }
            const auto inLane = [&](sf::Vector2f p, float r) {
                for (const auto& l : lanes) if (segDist(p, l.first, l.second) < laneW * 0.5f + r) return true;
                return false;
            };

            const float edgeMargin = ft ? (*ft)["edge_margin"].get_or(900.f) : 900.f;
            const auto keepClear = [&](sf::Vector2f p, float r) {
                const float ds = std::hypot(p.x - L.start.x, p.y - L.start.y);
                const float dr = std::hypot(p.x - L.rift.x, p.y - L.rift.y);
                return ds < 2600.f + r || dr < 2200.f + r || L.edgeDist(p) < edgeMargin + r;
            };

            const float drift = ft ? (*ft)["drift"].get_or(0.f) : 0.f;
            const float spin = ft ? (*ft)["spin"].get_or(0.15f) : 0.15f;
            const bool still = drift < 10.f;
            const auto place = [&](const std::string& type, sf::Vector2f p, float sr) {
                levelgen::Placed o;
                o.type = typeIndex(type);
                o.pos = p;
                o.sizeRoll = sr;
                o.radius = radiusOf(lua, type, sr);
                // Drift follows a slow current, so neighbours travel together
                // and a pile stays a pile while it moves.
                const float a = levelgen::detail::fbm(p.x / 9000.f + 3.f, p.y / 9000.f - 5.f, L.seed + 77u) * 6.2831853f * 1.5f
                              + (U(rng) - 0.5f) * 0.5f;
                o.vel = drift > 0.f ? sf::Vector2f(std::cos(a), std::sin(a)) * (drift * (0.3f + 0.7f * U(rng))) : sf::Vector2f(0.f, 0.f);
                // Every object lies at its own angle and turns, slowly: a field
                // where every hull points the same way and nothing turns reads
                // as wallpaper, not as debris.
                o.angle = U(rng) * 360.f;
                o.spin = (U(rng) < 0.5f ? -1.f : 1.f) * (0.25f + 0.75f * U(rng)) * spin;
                o.still = still;
                o.cell = static_cast<int>(ci);
                L.placedByCell[ci].push_back(static_cast<int>(L.placed.size()));
                hash[hkey(static_cast<int>(std::floor(p.x / HB)), static_cast<int>(std::floor(p.y / HB)))].push_back(static_cast<int>(L.placed.size()));
                if (still) L.navStamp(p, o.radius + 55.f, +1);
                L.placed.push_back(o);
            };

            // ---- Points of interest (scripts/pois.lua), before anything else
            //      claims the ground: their footprint stays clear of the field ----
            std::vector<std::pair<sf::Vector2f, float>> poiSpots;   // centre, keep-out radius
            if (poiT) {
                sol::optional<sol::table> types = lua["poi_types"];
                Pool want;   // key -> count range, read in a stable order
                for (const auto& kv : *poiT) if (kv.first.is<std::string>()) want.push_back({ kv.first.as<std::string>(), 0.f });
                std::sort(want.begin(), want.end());
                for (const auto& w : want) {
                    sol::optional<sol::table> range = (*poiT)[w.first];
                    sol::optional<sol::table> def;
                    if (types) def = (*types)[w.first].get<sol::optional<sol::table>>();
                    if (!def) { std::cerr << "[FieldGen] zones.lua names POI \"" << w.first << "\" but pois.lua has no such type.\n"; continue; }
                    const int lo = range ? (*range)[1].get_or(0) : 0;
                    const int hi = range ? std::max(lo, (*range)[2].get_or(lo)) : lo;
                    int count = std::uniform_int_distribution<int>(lo, hi)(rng);
                    // `min_depth`: not this close to the start (the Dreadnought).
                    if (ci < L.cellDepth.size() && L.cellDepth[ci] < (*def)["min_depth"].get_or(0)) count = 0;
                    sol::optional<sol::table> perLevel = (*def)["per_level"];
                    if (perLevel) {
                        auto left = poiLevelLeft.find(w.first);
                        if (left == poiLevelLeft.end()) {
                            const int plo = (*perLevel)[1].get_or(1);
                            const int phi = std::max(plo, (*perLevel)[2].get_or(plo));
                            left = poiLevelLeft.emplace(w.first, std::uniform_int_distribution<int>(plo, phi)(rng)).first;
                        }
                        count = std::min(count, left->second);
                    }
                    for (int k = 0, tries = 0; k < count && tries < 80; ++tries) {
                        sol::table d = *def;
                        // A variant, rolled once: its keys lie over the base's.
                        std::string variant;
                        sol::optional<sol::table> vt;
                        if (sol::optional<sol::table> vs = d["variants"]) {
                            Pool vp;
                            for (const auto& kv : *vs) if (kv.first.is<std::string>() && kv.second.is<sol::table>())
                                vp.push_back({ kv.first.as<std::string>(), kv.second.as<sol::table>()["weight"].get_or(1.f) });
                            std::sort(vp.begin(), vp.end());
                            variant = roll(vp, rng);
                            if (!variant.empty()) vt = (*vs)[variant].get<sol::optional<sol::table>>();
                        }
                        const auto pick = [&](const char* key) -> sol::optional<sol::table> {
                            if (vt) { sol::optional<sol::table> o = (*vt)[key]; if (o) return o; }
                            return d[key];
                        };
                        const float radius = d["radius"].get_or(700.f);
                        const float shield = d["shield"].get_or(0.f);
                        const float keep = std::max(radius, shield) + 250.f;
                        const sf::Vector2f p = c + sf::Vector2f((U(rng) - 0.5f) * S * 0.76f, (U(rng) - 0.5f) * S * 0.76f);
                        if (keepClear(p, keep + 600.f) || inLane(p, radius * 0.4f)) continue;
                        bool clash = false;
                        for (const auto& q : L.pois) if (std::hypot(p.x - q.pos.x, p.y - q.pos.y) < keep + std::max(q.radius, q.shield) + 2600.f) clash = true;
                        if (clash) continue;

                        levelgen::Poi poi;
                        poi.type = w.first;
                        poi.variant = variant;
                        poi.display = d["display"].get_or(w.first);
                        poi.icon = d["icon"].get_or(std::string("outpost"));
                        poi.pos = p;
                        poi.radius = radius;
                        poi.shield = shield;
                        poi.cell = static_cast<int>(ci);
                        const int pIdx = static_cast<int>(L.pois.size());
                        const float rot = U(rng) * 6.2831853f, cr = std::cos(rot), sr2 = std::sin(rot);
                        const float pdrift = d["drift"].get_or(0.f);
                        const float da = U(rng) * 6.2831853f;
                        const sf::Vector2f pvel = sf::Vector2f(std::cos(da), std::sin(da)) * pdrift;
                        const auto put = [&](const std::string& type, sf::Vector2f at, float angDeg) {
                            if (type.empty()) return;
                            const float sroll = sizeRoll();
                            const float rr = radiusOf(lua, type, sroll);
                            if (!fits(at, rr, 6.f)) return;
                            levelgen::Placed o;
                            o.type = typeIndex(type);
                            o.pos = at;
                            o.sizeRoll = sroll;
                            o.radius = rr;
                            o.vel = pvel;
                            o.angle = angDeg;
                            o.spin = pdrift > 0.f ? (U(rng) - 0.5f) * 0.02f : 0.f;
                            o.still = pdrift < 1.f;
                            o.cell = static_cast<int>(ci);
                            o.poi = pIdx;
                            L.placedByCell[ci].push_back(static_cast<int>(L.placed.size()));
                            hash[hkey(static_cast<int>(std::floor(at.x / HB)), static_cast<int>(std::floor(at.y / HB)))].push_back(static_cast<int>(L.placed.size()));
                            if (o.still) L.navStamp(at, rr + 55.f, +1);
                            poi.objects.push_back(static_cast<int>(L.placed.size()));
                            L.placed.push_back(o);
                        };
                        const auto putList = [&](sol::optional<sol::table> list) {
                            if (!list) return;
                            for (size_t q = 1; q <= list->size(); ++q) {
                                sol::optional<sol::table> e = (*list)[q];
                                if (!e) continue;
                                const std::string type = (*e)[1].get_or<std::string>("");
                                const float x = (*e)[2].get_or(0.f), y = (*e)[3].get_or(0.f);
                                const float a = (*e)[4].get_or(U(rng) * 360.f);
                                put(type, p + sf::Vector2f(x * cr - y * sr2, x * sr2 + y * cr), a + rot * 57.29578f);
                            }
                        };
                        putList(pick("parts"));
                        putList(pick("extra"));
                        if (sol::optional<sol::table> sc = pick("scatter")) {
                            const std::string type = (*sc)["type"].get_or<std::string>("");
                            const int a = (*sc)["count"][1].get_or(0), b = std::max(a, (*sc)["count"][2].get_or(a));
                            const float r0 = (*sc)["ring"][1].get_or(250.f), r1 = (*sc)["ring"][2].get_or(radius);
                            const int n = std::uniform_int_distribution<int>(a, b)(rng);
                            for (int s = 0, t2 = 0; s < n && t2 < n * 6; ++t2) {
                                const float ang = U(rng) * 6.2831853f, rad = r0 + U(rng) * (r1 - r0);
                                const size_t before = L.placed.size();
                                put(type, p + sf::Vector2f(std::cos(ang), std::sin(ang)) * rad, U(rng) * 360.f);
                                if (L.placed.size() > before) ++s;
                            }
                        }
                        if (sol::optional<sol::table> g = pick("guards")) {
                            const int a = (*g)["count"][1].get_or(0), b = std::max(a, (*g)["count"][2].get_or(a));
                            const int n = std::uniform_int_distribution<int>(a, b)(rng);
                            const Pool units = readPool((*g)["units"]);
                            const std::string lead = (*g)["lead"].get_or<std::string>("");
                            const bool lootLead = d["loot_guard"].valid();
                            const auto add = [&](const std::string& key, char loot, char sleeps) {
                                poi.guards.push_back(key); poi.guardLoot.push_back(loot); poi.guardSleeps.push_back(sleeps);
                            };
                            const auto addList = [&](const char* field, char sleeps) {
                                sol::optional<sol::table> l = (*g)[field];
                                if (!l) return;
                                for (size_t q = 1; q <= l->size(); ++q) {
                                    const std::string key = (*l)[q].get_or<std::string>("");
                                    if (!key.empty()) add(key, 0, sleeps);
                                }
                            };
                            // `lead` (counts toward `count`), then `count` random
                            // from `units`; `fixed` and `sleepers` always come on top.
                            if (!lead.empty()) add(lead, lootLead ? 1 : 0, 0);
                            for (int u = lead.empty() ? 0 : 1; u < n; ++u) {
                                const std::string& key = roll(units, rng);
                                if (key.empty()) break;
                                add(key, 0, 0);
                            }
                            addList("fixed", 0);
                            addList("sleepers", 1);
                            poi.guardsDormant = (*g)["dormant"].get_or(false);
                            poi.guardLeash = (*g)["leash"].get_or(900.f);
                        }
                        poiSpots.push_back({ p, keep });
                        L.pois.push_back(std::move(poi));
                        ++k;
                        if (perLevel) --poiLevelLeft[w.first];
                    }
                }
            }
            const auto inPoi = [&](sf::Vector2f q, float r) {
                for (const auto& s : poiSpots) if (std::hypot(q.x - s.first.x, q.y - s.first.y) < s.second + r) return true;
                return false;
            };

            // ---- Sleeping packs, and the wrecks they hide among ----
            std::vector<sf::Vector2f> siteCentres;
            std::vector<sf::Vector2f> sleeperSpots;
            if (pt) {
                sol::table pk = *pt;
                const int cMin = pk["count"][1].get_or(2), cMax = std::max(cMin, pk["count"][2].get_or(4));
                const int sMin = pk["size"][1].get_or(3), sMax = std::max(sMin, pk["size"][2].get_or(5));
                const Pool units = readPool(pk["units"]);
                const Pool cover = readPool(pk["cover"]);
                const float wake = pk["wake_radius"].get_or(650.f);
                const int count = std::uniform_int_distribution<int>(cMin, cMax)(rng);
                for (int k = 0, tries = 0; k < count && tries < 60; ++tries) {
                    const sf::Vector2f p = c + sf::Vector2f((U(rng) - 0.5f) * S * 0.8f, (U(rng) - 0.5f) * S * 0.8f);
                    if (keepClear(p, 900.f) || inLane(p, 500.f) || inPoi(p, 900.f)) continue;
                    bool near = false;
                    for (const auto& q : siteCentres) if (std::hypot(p.x - q.x, p.y - q.y) < 5000.f) near = true;
                    if (near) continue;
                    levelgen::PackSite site;
                    site.pos = p;
                    site.wakeRadius = wake;
                    site.cell = static_cast<int>(ci);
                    const int n = std::uniform_int_distribution<int>(sMin, sMax)(rng);
                    for (int u = 0; u < n; ++u) { const std::string& k2 = roll(units, rng); if (!k2.empty()) site.units.push_back(k2); }
                    // Sleepers and dead ships share one scatter: every sleeper
                    // lies in a gap BETWEEN wrecks, at the same spread, so none
                    // of them stands apart from the cover it is hiding in.
                    const int nCover = cover.empty() ? 0 : 6 + static_cast<int>(U(rng) * 5.f);
                    const int nSlots = static_cast<int>(site.units.size()) + nCover;
                    std::vector<bool> sleeper(static_cast<size_t>(nSlots), false);
                    for (size_t u = 0; u < site.units.size(); ++u) {
                        int s = static_cast<int>(U(rng) * nSlots) % nSlots;
                        while (sleeper[static_cast<size_t>(s)]) s = (s + 1) % nSlots;
                        sleeper[static_cast<size_t>(s)] = true;
                    }
                    for (int w = 0; w < nSlots; ++w) {
                        const float a = (w + U(rng) * 0.7f) * 6.2831853f / std::max(1, nSlots) + U(rng) * 0.3f;
                        const float r = 180.f + U(rng) * 470.f;
                        const sf::Vector2f wp = p + sf::Vector2f(std::cos(a), std::sin(a)) * r;
                        if (sleeper[static_cast<size_t>(w)]) {
                            site.slots.push_back(wp);
                            site.slotAngle.push_back(U(rng) * 360.f);
                            continue;
                        }
                        const std::string& wk = roll(cover, rng);
                        const float sr = sizeRoll();
                        if (!wk.empty() && fits(wp, radiusOf(lua, wk, sr), 40.f)) place(wk, wp, sr);
                    }
                    // Keep the field fill off the sleepers' spots.
                    for (const auto& q : site.slots) sleeperSpots.push_back(q);
                    L.packs.push_back(site);
                    siteCentres.push_back(p);
                    ++k;
                }
            }

            // ---- The field itself ----
            if (ft) {
                sol::table f = *ft;
                const float spacing = std::max(120.f, f["spacing"].get_or(260.f));
                const float fill = std::clamp(f["fill"].get_or(0.55f), 0.f, 1.f);
                const float density = std::clamp(f["density"].get_or(0.6f), 0.f, 1.f);
                const float nscale = std::max(500.f, f["noise_scale"].get_or(4200.f));
                const Pool types = readPool(f["types"]);
                std::string magmaType, magmaReplaces;
                float magmaShare = 0.f, magmaScale = 2600.f;
                if (sol::optional<sol::table> m = f["magma"]) {
                    magmaType = (*m)["type"].get_or<std::string>("MAGMATIC");
                    magmaReplaces = (*m)["replaces"].get_or<std::string>("MEDIUM");
                    magmaShare = (*m)["share"].get_or(0.3f);
                    magmaScale = (*m)["scale"].get_or(2600.f);
                }
                // One noise field per sub-zone, not per cell, so two neighbouring
                // cells of the same field join without a seam.
                // Clumps: a big rock with smaller ones lying against it, so a
                // pile reads as a pile and not as evenly sown gravel.
                float clumpChance = 0.f, gapMin = 6.f, gapMax = 40.f;
                int clumpMin = 1, clumpMax = 3;
                Pool clumpTypes;
                if (sol::optional<sol::table> cl = f["clump"]) {
                    clumpChance = (*cl)["chance"].get_or(0.4f);
                    clumpMin = (*cl)["count"][1].get_or(1);
                    clumpMax = std::max(clumpMin, (*cl)["count"][2].get_or(3));
                    gapMin = (*cl)["gap"][1].get_or(6.f);
                    gapMax = std::max(gapMin, (*cl)["gap"][2].get_or(40.f));
                    clumpTypes = readPool((*cl)["types"]);
                }
                const uint32_t ns = L.seed * 13u + (fnv(L.cellZone[ci]) & 0xffffu) * 101u + 5u;
                // Where the neighbour is something else, the field thins out over
                // a ragged band instead of stopping on the cell's straight edge.
                const float taperW = f["border_fade"].get_or(3200.f);
                const sf::Vector2i g = L.cells[ci];
                bool open[4];   // +x, -x, +y, -y: true = taper toward that side
                {
                    const sf::Vector2i dirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (int d = 0; d < 4; ++d) {
                        open[d] = true;
                        for (size_t cj = 0; cj < nCells; ++cj)
                            if (L.cells[cj] == g + dirs[d]) { open[d] = (L.cellZone[cj] != L.cellZone[ci]); break; }
                        // No neighbour at all: the Void's edge handles that side.
                        if (!has(g + dirs[d])) open[d] = false;
                    }
                }
                const auto borderK = [&](sf::Vector2f p) {
                    float m = 1e9f;
                    if (open[0]) m = std::min(m, (c.x + h) - p.x);
                    if (open[1]) m = std::min(m, p.x - (c.x - h));
                    if (open[2]) m = std::min(m, (c.y + h) - p.y);
                    if (open[3]) m = std::min(m, p.y - (c.y - h));
                    if (m > 1e8f) return 1.f;
                    const float wob = levelgen::detail::fbm(p.x / 1700.f - 9.f, p.y / 1700.f + 4.f, ns + 3u) * taperW * 0.45f;
                    const float t = std::clamp((m + wob) / taperW, 0.f, 1.f);
                    return t * t * (3.f - 2.f * t);
                };
                for (float y = c.y - h + spacing * 0.5f; y < c.y + h; y += spacing)
                    for (float x = c.x - h + spacing * 0.5f; x < c.x + h; x += spacing) {
                        const sf::Vector2f p(x + (U(rng) - 0.5f) * spacing * 0.9f, y + (U(rng) - 0.5f) * spacing * 0.9f);
                        const float n = 0.5f + 0.5f * levelgen::detail::fbm(p.x / nscale, p.y / nscale, ns);
                        const float k = std::clamp((n - (1.f - fill)) / 0.18f, 0.f, 1.f) * borderK(p);
                        if (k <= 0.f || U(rng) > k * density) continue;
                        std::string type = roll(types, rng);
                        if (type.empty()) continue;
                        const auto magmaOf = [&](std::string t, sf::Vector2f q) {
                            if (!magmaType.empty() && t == magmaReplaces) {
                                const float m = 0.5f + 0.5f * levelgen::detail::fbm(q.x / magmaScale + 31.f, q.y / magmaScale - 17.f, ns + 7u);
                                if (m > 1.f - magmaShare) t = magmaType;
                            }
                            return t;
                        };
                        const auto ok = [&](sf::Vector2f q, float r, float gap) {
                            if (keepClear(q, r) || inLane(q, r) || inPoi(q, r) || !fits(q, r, gap)) return false;
                            for (const auto& s : siteCentres) if (std::hypot(q.x - s.x, q.y - s.y) < 140.f + r) return false;
                            for (const auto& s : sleeperSpots) if (std::hypot(q.x - s.x, q.y - s.y) < 110.f + r) return false;
                            return true;
                        };
                        type = magmaOf(type, p);
                        const float sr = sizeRoll();
                        const float r = radiusOf(lua, type, sr);
                        if (!ok(p, r, 24.f)) continue;
                        place(type, p, sr);
                        const size_t prim = L.placed.size() - 1;
                        if (clumpTypes.empty() || U(rng) > clumpChance) continue;
                        const int nSat = std::uniform_int_distribution<int>(clumpMin, clumpMax)(rng);
                        float a = U(rng) * 6.2831853f;
                        for (int s = 0; s < nSat; ++s) {
                            a += 1.2f + U(rng) * 1.6f;
                            const std::string st = magmaOf(roll(clumpTypes, rng), p);
                            if (st.empty()) continue;
                            const float srs = sizeRoll();
                            const float rs = radiusOf(lua, st, srs);
                            const sf::Vector2f q = p + sf::Vector2f(std::cos(a), std::sin(a)) * (r + rs + gapMin + U(rng) * (gapMax - gapMin));
                            if (ok(q, rs, gapMin * 0.5f)) { place(st, q, srs); L.placed.back().vel = L.placed[prim].vel; }
                        }
                    }
            }
        }
    }

} // namespace fieldgen
