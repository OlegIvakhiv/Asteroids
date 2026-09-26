/**
 * @file ZoneArchetypes.hpp
 * @brief Zone definitions loaded from zones.lua -- the "tileset" layer.
 *
 * ============================================================================
 * WHAT A ZONE IS (AND IS NOT, YET)
 * ============================================================================
 * A zone is a PRESET, not a bounded region. It answers "what does this stretch
 * of space look like, and what lives in it" -- sky colour, star density, dust
 * tint, which rocks drift through, which factions the director may field, and
 * what junk hangs in the background.
 *
 * It does NOT yet answer "where does it end". There are no zone boundaries, no
 * transitions and no run state, because the engine has none: the director
 * spawns in unbounded space around the player and PhysicsSystem culls by
 * distance from the player. Boundaries are the next layer up and they need a
 * run-state object to carry hull, score and loot across a transition. Adding
 * them here first would mean guessing that object's shape.
 *
 * The payoff of doing it in this order is the test in the header of zones.lua:
 * zone number three should be a Lua and art change with NO new C++. If it
 * isn't, this layer isn't finished.
 *
 * ============================================================================
 * WHY IT MIRRORS EnemyArchetypes
 * ============================================================================
 * Deliberately the same shape as enemyarch::EnemyRegistry, down to the
 * `zone_order` list: an explicit order array gives every zone a stable uint8_t
 * id that survives an F5 reload, because Lua table iteration order is a hash
 * order and would reshuffle between runs. One pattern to learn, not two.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EnemyArchetypes.hpp"   // enemyarch::geom::triangulate
#include <SFML/Graphics.hpp>
#include <sol/sol.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <cstdint>

namespace zonearch {

    inline constexpr uint8_t INVALID_ZONE = 0xFF;

    /// Hard ceiling on the live background prop pool. Props are drawn in ONE
    /// vertex array and never collide with anything, so this is a fill-rate
    /// budget rather than an entity budget -- they are not entities and never
    /// touch EntityManager's reserve.
    inline constexpr int MAX_PROPS = 160;

    // ========================================================================
    // PROP
    // ========================================================================

    /**
     * @struct PropDef
     * @brief One authored background silhouette: a hulk, a spar, a container.
     *
     * Authored as a polygon point list in Lua, exactly like a ship hull in
     * enemy.lua. That is on purpose -- it is the drawing pipeline this project
     * already has, it needs no sprite atlas, no texture loading and no new art
     * tooling, and it stays inside the "дешево і сердито" constraint.
     *
     * SHAPE CONSTRAINT: the fill is drawn as a triangle fan from the polygon's
     * centroid, so a shape must be STAR-SHAPED about its centroid (every vertex
     * visible from the middle). Junk silhouettes naturally are. A shape with a
     * deep hook or a C-profile will fill wrong -- author it with `fill = false`
     * and let the outline carry it, or split it into two props.
     */
     /**
      * @struct PropPart
      * @brief One polygon inside a prop, with its own colours.
      *
      * A prop used to BE a polygon. That was enough for a floating girder and
      * hopeless for anything built: a station is a rock, plus mismatched armour
      * bolted over it, plus spikes, plus a lit furnace -- each a different
      * colour, drawn in order. Parts are that list.
      *
      * `tris` is ear-clipped once at load by enemyarch::geom::triangulate, the
      * same routine that handles concave ship hulls. It replaces the old
      * centroid triangle fan, which silently folded over itself on any shape
      * with a deep notch -- which is to say, on exactly the shapes that make
      * scrap look like scrap.
      */
    struct PropPart {
        std::vector<sf::Vector2f> points;  ///< Local pixels, ring order
        std::vector<sf::Vector2f> tris;    ///< Ear-clipped at load, 3 per triangle
        sf::Color fill{ 10, 8, 8, 255 };
        sf::Color line{ 74, 44, 32, 255 };
        bool  filled = true;
        bool  outlined = true;

        /// 0 = inert. Above 0 draws a pulsing halo behind the part, faked as
        /// three scaled translucent copies -- no shader, no render texture,
        /// and it is the only thing in a zone that emits light rather than
        /// reflecting it. Keep it rare: a lit window is a landmark's whole
        /// claim to being inhabited, and stops meaning anything if the entire
        /// station glows.
        float glow = 0.f;
    };

    struct PropDef {
        uint8_t     id = 0;
        std::string key;                   ///< "HULK_CUT" -- the Lua table key
        std::vector<PropPart> parts;

        /// Size CLASS, multiplied by the layer's random scale.
        ///
        /// Without this every prop drew from one shared range, so a chunk of
        /// debris and a ship's hull could come out the same size on screen --
        /// and once two silhouettes are the same size the eye stops telling
        /// them apart at all. A chunk is always small, a hulk is always big.
        float scale = 1.f;

        /// A prop drawn by a C++ model instead of `parts`. Some landmarks
        /// move -- chains sway, turrets track, a furnace breathes -- and a
        /// static polygon list cannot. Currently: "RAKSHARI_CITADEL"
        /// (utils/CitadelModel.hpp). Empty = an ordinary parts prop.
        std::string builtin;
        bool builtinGlow = true;       ///< `furnace_glow`  -- the lab's glow toggle
        bool builtinTrophies = true;   ///< `trophy_chains` -- the lab's chains toggle

        bool valid() const { return !parts.empty() || !builtin.empty(); }
    };

    // ========================================================================
    // ZONE
    // ========================================================================

    /// One weighted entry in a zone's asteroid table.
    struct RockEntry {
        std::string key;        ///< An asteroid_types key from asteroids.lua
        float       weight = 1.f;
    };

    /**
     * @struct FactionSlot
     * @brief A faction this zone permits, and how hard it may push.
     *
     * A faction with no slot in a zone does not spawn there at all. This
     * REPLACES the global `active_factions` list once a zone is loaded: which
     * faction fields units is a property of where you are, not of the session.
     *
     * The scales multiply the faction's own numbers rather than replacing them,
     * so a zone stays correct after enemy.lua is retuned. `threat_scale` is the
     * one that actually shapes a fight -- see the budget note in EnemySystem.
     */
    struct FactionSlot {
        std::string key;                 ///< "RAKSHARI"
        float intervalScale = 1.f;       ///< >1 = slower spawns
        float threatScale = 1.f;       ///< >1 = a heavier field
        float activeScale = 1.f;       ///< >1 = more bodies at once
    };

    /**
     * @struct PropLayer
     * @brief One parallax band of background geometry.
     *
     * A zone has two: near junk, and far landmarks. They are the same
     * machinery with very different numbers -- junk is small, plentiful and
     * moderately close; a landmark is enormous, rare and almost stationary.
     * Splitting them is what lets a Rakshari base sit behind the scrap
     * instead of competing with it for the same depth band.
     */
    struct PropLayer {
        int   count = 0;
        float depthMin = 0.25f;   ///< 0 = infinitely far, 1 = welded to the world
        float depthMax = 0.55f;
        float scaleMin = 0.8f;
        float scaleMax = 2.2f;
        float spinMax = 4.f;      ///< deg/sec, signed per prop
        /// Wrap-box padding, in view-sizes. Bigger = props recycle further
        /// off screen. A landmark wants a large one: it is the only thing on
        /// screen big enough for a teleport to be noticeable.
        float pad = 1.6f;

        /// Alpha at the layer's far and near depth edges.
        ///
        /// This used to be one hardcoded curve for everything, which pinned a
        /// landmark at 40% no matter how much it was meant to be seen. Junk
        /// wants to sink into the background; a base is the thing the player
        /// is supposed to look at and wonder about.
        float alphaFar = 0.30f;
        float alphaNear = 0.55f;

        std::vector<uint8_t> props;   ///< Prop ids, equal weight

        bool active() const { return count > 0 && !props.empty(); }
    };

    struct ZoneDef {
        uint8_t     id = INVALID_ZONE;
        std::string key;                 ///< "RAKSHARI" -- the Lua table key
        std::string display;             ///< "Rakshari Scrapyard" -- for HUD / dev

        // ---- Sky ----
        sf::Color voidColor{ 2, 3, 5 };    ///< Window clear colour
        int       starCount = 800;
        sf::Color starTint{ 255, 255, 255 }; ///< Multiplied into each star's colour
        float     starAlphaScale = 1.f;

        // ---- Dust ----
        sf::Color dustColor{ 170, 200, 255 };
        float     dustAlpha = 150.f;

        // ---- Rocks ----
        float rockInterval = 0.5f;
        int   rockMaxCount = 60;
        float rockSpawnRadius = 2500.f;
        std::vector<RockEntry> rocks;

        // ---- Factions ----
        std::vector<FactionSlot> factions;

        // ---- Background geometry, near to far ----
        PropLayer junk;        ///< Drifting scrap, close enough to read
        PropLayer landmarks;   ///< Stations and bases, the farthest layer

        /// The raw Lua table, for anything not promoted to a field above.
        sol::table config;

        /// Pick a rock type by weight. Empty table returns an empty string,
        /// which EnemySystem treats as "spawn nothing this tick".
        const std::string& rollRock() const {
            static const std::string none;
            if (rocks.empty()) return none;

            float total = 0.f;
            for (const auto& r : rocks) total += std::max(0.f, r.weight);
            if (total <= 0.f) return rocks.front().key;

            float roll = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * total;
            for (const auto& r : rocks) {
                roll -= std::max(0.f, r.weight);
                if (roll <= 0.f) return r.key;
            }
            return rocks.back().key;
        }

        /// The slot for a faction key, or nullptr when this zone bars it.
        const FactionSlot* slotFor(const std::string& factionKey) const {
            for (const auto& f : factions)
                if (f.key == factionKey) return &f;
            return nullptr;
        }
    };

    // ========================================================================
    // REGISTRY
    // ========================================================================

    /**
     * @class ZoneRegistry
     * @brief Owns the loaded zone and prop tables.
     *
     * Lives in SystemManager and travels to systems through SystemContext,
     * matching how enemyarch::EnemyRegistry gets around.
     *
     * HOT RELOAD: load() must be called again after every F5, from the same
     * place that reloads the enemy registry. Re-running zones.lua rebuilds the
     * Lua tables, and every sol::table held here would otherwise point at the
     * old ones -- the same stale-handle trap enemy archetypes have.
     */
    class ZoneRegistry {
    public:
        bool load(sol::state& lua) {
            const std::vector<std::string> previousOrder = orderSnapshot();

            m_zones.clear();
            m_props.clear();
            m_byKey.clear();
            m_propByKey.clear();

            loadProps(lua);

            sol::object orderObj = lua["zone_order"];
            if (!orderObj.valid() || !orderObj.is<sol::table>()) {
                std::cerr << "[ZoneRegistry] `zone_order` missing from zones.lua. "
                    "Falling back to the pre-zone defaults.\n";
                return false;
            }
            sol::table order = orderObj.as<sol::table>();

            sol::object defsObj = lua["zones"];
            if (!defsObj.valid() || !defsObj.is<sol::table>()) {
                std::cerr << "[ZoneRegistry] `zones` missing from zones.lua.\n";
                return false;
            }
            sol::table defs = defsObj.as<sol::table>();

            for (size_t i = 1; i <= order.size(); ++i) {
                const std::string key = order[i].get_or<std::string>("");
                if (key.empty()) continue;

                sol::object entry = defs[key];
                if (!entry.valid() || !entry.is<sol::table>()) {
                    std::cerr << "[ZoneRegistry] zone_order lists \"" << key
                        << "\" but `zones` has no such table. Skipped.\n";
                    continue;
                }
                if (m_zones.size() >= 254) {
                    std::cerr << "[ZoneRegistry] More than 254 zones. "
                        "Zone ids are uint8_t. Stopping.\n";
                    break;
                }
                m_zones.push_back(buildZone(key, entry.as<sol::table>(),
                    static_cast<uint8_t>(m_zones.size())));
                m_byKey[key] = m_zones.back().id;
            }

            m_defaultKey = lua["default_zone"].get_or<std::string>("");

            const std::vector<std::string> newOrder = orderSnapshot();
            if (!previousOrder.empty() && previousOrder != newOrder) {
                std::cerr << "[ZoneRegistry] WARNING: zone_order changed on reload. "
                    "The active zone id now points at a different zone.\n";
            }

            for (const auto& z : m_zones) {
                std::cout << "[ZoneRegistry] zone " << z.key
                    << "  rocks=" << z.rocks.size()
                    << "  factions=" << z.factions.size()
                    << "  junk=" << z.junk.count
                    << "  landmarks=" << z.landmarks.count << "\n";
            }
            return !m_zones.empty();
        }

        // ---- Lookup ----

        const std::vector<ZoneDef>& all() const { return m_zones; }
        const std::vector<PropDef>& props() const { return m_props; }
        bool empty() const { return m_zones.empty(); }

        uint8_t idOf(const std::string& key) const {
            auto it = m_byKey.find(key);
            return (it == m_byKey.end()) ? INVALID_ZONE : it->second;
        }

        /// The zone for an id, or nullptr. Never throws on a stale id.
        const ZoneDef* byId(uint8_t id) const {
            return (id < m_zones.size()) ? &m_zones[id] : nullptr;
        }

        const PropDef* propById(uint8_t id) const {
            return (id < m_props.size()) ? &m_props[id] : nullptr;
        }

        /// `default_zone` from Lua, else the first zone in the order.
        uint8_t defaultId() const {
            if (!m_defaultKey.empty()) {
                const uint8_t id = idOf(m_defaultKey);
                if (id != INVALID_ZONE) return id;
            }
            return m_zones.empty() ? INVALID_ZONE : 0;
        }

    private:
        std::vector<std::string> orderSnapshot() const {
            std::vector<std::string> v;
            v.reserve(m_zones.size());
            for (const auto& z : m_zones) v.push_back(z.key);
            return v;
        }

        static sf::Color readColor(const sol::table& t, const char* key, sf::Color def) {
            sol::optional<sol::table> c = t[key];
            if (!c) return def;
            return sf::Color(
                static_cast<std::uint8_t>(std::clamp((*c)["r"].get_or(static_cast<int>(def.r)), 0, 255)),
                static_cast<std::uint8_t>(std::clamp((*c)["g"].get_or(static_cast<int>(def.g)), 0, 255)),
                static_cast<std::uint8_t>(std::clamp((*c)["b"].get_or(static_cast<int>(def.b)), 0, 255)),
                static_cast<std::uint8_t>(std::clamp((*c)["a"].get_or(static_cast<int>(def.a)), 0, 255)));
        }

        void loadProps(sol::state& lua) {
            sol::object po = lua["zone_props"];
            if (!po.valid() || !po.is<sol::table>()) return;   // a zone may have none
            sol::table pt = po.as<sol::table>();

            for (auto& kv : pt) {
                if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;

                PropDef p;
                p.key = kv.first.as<std::string>();
                sol::table t = kv.second.as<sol::table>();
                p.scale = std::max(0.05f, t["scale"].get_or(1.f));
                p.builtin = t["builtin"].get_or<std::string>("");
                p.builtinGlow = t["furnace_glow"].get_or(true);
                p.builtinTrophies = t["trophy_chains"].get_or(true);

                // Two shapes: a `parts` list for anything built, or a bare
                // `points` list for a single-polygon prop. The short form is
                // kept because a lump of debris does not deserve two levels
                // of table nesting to say one thing.
                sol::optional<sol::table> parts = t["parts"];
                if (parts) {
                    for (size_t i = 1; i <= parts->size(); ++i) {
                        sol::optional<sol::table> pt = (*parts)[i];
                        if (!pt) continue;
                        PropPart part = readPart(*pt, p.key);
                        if (!part.points.empty()) p.parts.push_back(std::move(part));
                    }
                }
                else {
                    PropPart part = readPart(t, p.key);
                    if (!part.points.empty()) p.parts.push_back(std::move(part));
                }

                if (!p.valid()) {
                    std::cerr << "[ZoneRegistry] prop \"" << p.key
                        << "\" has no usable geometry. Skipped.\n";
                    continue;
                }

                p.id = static_cast<uint8_t>(m_props.size());
                m_propByKey[p.key] = p.id;
                m_props.push_back(std::move(p));
                if (m_props.size() >= 254) break;
            }
        }

        PropLayer buildLayer(const std::string& zoneKey, sol::object obj,
            float defDepthMin, float defDepthMax, float defPad,
            float defAlphaFar, float defAlphaNear)
        {
            PropLayer L;
            L.depthMin = defDepthMin;
            L.depthMax = defDepthMax;
            L.pad = defPad;
            L.alphaFar = defAlphaFar;
            L.alphaNear = defAlphaNear;
            if (!obj.valid() || !obj.is<sol::table>()) return L;
            sol::table t = obj.as<sol::table>();

            L.count = std::clamp(t["count"].get_or(0), 0, MAX_PROPS);
            L.depthMin = t["depth_min"].get_or(defDepthMin);
            L.depthMax = t["depth_max"].get_or(defDepthMax);
            L.scaleMin = t["scale_min"].get_or(0.8f);
            L.scaleMax = t["scale_max"].get_or(2.2f);
            L.spinMax = t["spin_max"].get_or(4.f);
            L.pad = t["pad"].get_or(defPad);
            L.alphaFar = std::clamp(t["alpha_far"].get_or(defAlphaFar), 0.f, 1.f);
            L.alphaNear = std::clamp(t["alpha_near"].get_or(defAlphaNear), 0.f, 1.f);

            sol::optional<sol::table> use = t["use"];
            if (use) {
                for (size_t i = 1; i <= use->size(); ++i) {
                    const std::string pk = (*use)[i].get_or<std::string>("");
                    auto it = m_propByKey.find(pk);
                    if (it == m_propByKey.end()) {
                        std::cerr << "[ZoneRegistry] zone \"" << zoneKey
                            << "\" uses prop \"" << pk << "\" which does not exist.\n";
                        continue;
                    }
                    L.props.push_back(it->second);
                }
            }
            // A count with nothing to draw is a typo, not a blank layer.
            if (L.props.empty()) L.count = 0;

            // Depth 0 would pin a prop to the camera and never wrap; clamp
            // rather than trust the data.
            L.depthMin = std::clamp(L.depthMin, 0.02f, 1.f);
            L.depthMax = std::clamp(L.depthMax, L.depthMin, 1.f);
            L.scaleMax = std::max(L.scaleMin, L.scaleMax);
            L.pad = std::clamp(L.pad, 1.1f, 4.f);
            return L;
        }

        /// One polygon plus its colours. Triangulated here, once, so nothing
        /// at draw time has to know or care whether a shape is concave.
        PropPart readPart(const sol::table& t, const std::string& propKey) {
            PropPart part;

            sol::optional<sol::table> pts = t["points"];
            if (pts) {
                for (size_t i = 1; i <= pts->size(); ++i) {
                    sol::optional<sol::table> v = (*pts)[i];
                    if (!v) continue;
                    part.points.push_back({ (*v)[1].get_or(0.f), (*v)[2].get_or(0.f) });
                }
            }
            // Two points cannot enclose anything; a part that made it this far
            // with a typo'd point list would draw as a stray line.
            if (part.points.size() < 3) {
                if (!part.points.empty())
                    std::cerr << "[ZoneRegistry] prop \"" << propKey
                    << "\" has a part with fewer than 3 points. Dropped.\n";
                part.points.clear();
                return part;
            }

            part.fill = readColor(t, "fill", part.fill);
            part.line = readColor(t, "line", part.line);
            part.filled = t["filled"].get_or(true);
            part.outlined = t["outlined"].get_or(true);
            part.glow = std::clamp(t["glow"].get_or(0.f), 0.f, 4.f);

            if (part.filled) part.tris = enemyarch::geom::triangulate(part.points);
            return part;
        }

        ZoneDef buildZone(const std::string& key, sol::table t, uint8_t id) {
            ZoneDef z;
            z.id = id;
            z.key = key;
            z.display = t["display"].get_or(key);
            z.config = t;

            z.voidColor = readColor(t, "void_color", z.voidColor);
            z.starCount = std::clamp(t["star_count"].get_or(800), 0, 4000);
            z.starTint = readColor(t, "star_tint", z.starTint);
            z.starAlphaScale = t["star_alpha_scale"].get_or(1.f);

            z.dustColor = readColor(t, "dust_color", z.dustColor);
            z.dustAlpha = t["dust_alpha"].get_or(150.f);

            z.rockInterval = t["rock_interval"].get_or(0.5f);
            z.rockMaxCount = t["rock_max_count"].get_or(60);
            z.rockSpawnRadius = t["rock_spawn_radius"].get_or(2500.f);

            sol::optional<sol::table> rocks = t["rocks"];
            if (rocks) {
                for (auto& kv : *rocks) {
                    if (!kv.first.is<std::string>()) continue;
                    RockEntry r;
                    r.key = kv.first.as<std::string>();
                    r.weight = kv.second.is<float>() ? kv.second.as<float>() : 1.f;
                    if (r.weight > 0.f) z.rocks.push_back(std::move(r));
                }
            }

            sol::optional<sol::table> facs = t["factions"];
            if (facs) {
                for (auto& kv : *facs) {
                    if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
                    FactionSlot f;
                    f.key = kv.first.as<std::string>();
                    sol::table ft = kv.second.as<sol::table>();
                    f.intervalScale = ft["interval_scale"].get_or(1.f);
                    f.threatScale = ft["threat_scale"].get_or(1.f);
                    f.activeScale = ft["active_scale"].get_or(1.f);
                    z.factions.push_back(std::move(f));
                }
            }

            z.junk = buildLayer(key, t["props"], 0.25f, 0.55f, 1.6f, 0.30f, 0.55f);
            z.landmarks = buildLayer(key, t["landmarks"], 0.05f, 0.12f, 2.6f, 0.55f, 0.75f);

            return z;
        }

        std::vector<ZoneDef> m_zones;
        std::vector<PropDef> m_props;
        std::unordered_map<std::string, uint8_t> m_byKey;
        std::unordered_map<std::string, uint8_t> m_propByKey;
        std::string m_defaultKey;
    };

    // ========================================================================
    // RUNTIME STATE
    // ========================================================================

    /**
     * @struct ZoneState
     * @brief Which zone is live right now, and whether it still needs applying.
     *
     * Systems cache the SystemContext by value in init(), so the CURRENT zone
     * cannot be a plain value in the context -- it would freeze at whatever it
     * was when the game started. This is the same pointer-to-mutable-state
     * pattern DevState uses, for the same reason.
     *
     * Setting `current` never applies anything by itself. ZoneSystem watches
     * `dirty` and does the work (regenerate stars, reseed props, push tints) on
     * the next frame, in one place, at a moment it knows is safe.
     */
    struct ZoneState {
        const ZoneRegistry* registry = nullptr;
        uint8_t current = INVALID_ZONE;
        bool    dirty = true;

        /// The live zone, or nullptr when zones.lua failed to load. EVERY
        /// consumer must handle nullptr: that is the "engine behaves exactly
        /// as it did before zones existed" path, and it is what runs if a
        /// scripts/zones.lua is missing from an install.
        const ZoneDef* def() const {
            return registry ? registry->byId(current) : nullptr;
        }

        void set(uint8_t id) {
            if (id == current) return;
            current = id;
            dirty = true;
        }

        void setByKey(const std::string& key) {
            if (!registry) return;
            const uint8_t id = registry->idOf(key);
            if (id != INVALID_ZONE) set(id);
        }
    };

} // namespace zonearch