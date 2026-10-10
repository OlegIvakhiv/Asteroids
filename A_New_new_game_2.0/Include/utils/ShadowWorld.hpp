/**
 * @file ShadowWorld.hpp
 * @brief A second, hidden copy of the game: real systems, AI pilot, own texture.
 *
 * The terminal's tactical feed and the field doctrine scenes used to be
 * drawings that LOOKED like the game. Playtest: "we have all the code to load
 * the main game -- why not a simulation with the AI flying player ships?"
 * This is that. A ShadowWorld owns its own EntityManager, EntityFactory,
 * Box2D world, camera and every gameplay system, wired exactly the way
 * SystemManager wires the real ones, and renders into its own
 * sf::RenderTexture that the menu draws into a panel.
 *
 * WHAT IS REAL: everything that happens in it. Movement, dash i-frames,
 * perfect dodge, parry re-arm, Rift Bolt charge and detonation, vent QTE,
 * squad turns, Berserker rams, Barge cones, scrap, debris -- the same code
 * paths a player hits, because the ship is flown through the same inputs.
 *
 * HOW IT IS FLOWN: the hunter reads a VirtualPad (InputRegistry) instead of
 * the keyboard. Whoever owns the world fills `pad()` before step(); the
 * pad is installed only for the duration of the logic passes, so the real
 * keyboard can never leak in and the pad can never leak out.
 *
 * WHAT IS CUT OFF from the real game -- each of these would otherwise be a
 * silent bug:
 *   - HunterRecord / codex: a ShadowWorld never sees SystemManager, so its
 *     kills and sightings go nowhere. codexKills is drained every pass.
 *   - Game over: the hidden hunter dying is just playerAlive() == false.
 *   - Zones: it gets its OWN ZoneState pointing at the real registry, never
 *     marked dirty, and no ZoneSystem -- so it reads zone tables (factions,
 *     asteroid mix) without ever regenerating the real sky.
 *   - Dev flags: its own DevState. The real dev menu cannot freeze the feed.
 *   - The window: draw systems get the texture through ctx.target. `window`
 *     stays the real one only because InputSystem's guard wants it non-null;
 *     the pad's aim point means it is never read.
 *
 * LIFETIME: systems keep pointers into this object (em, gameView, dev, zone),
 * so a ShadowWorld must not move after build(). Hold it in a unique_ptr.
 * build() can be called again on the same object to restart the scene: like
 * SystemManager::restart(), it resets the ECS, recreates the Box2D world and
 * re-inits every system, without giving back the reserved capacity.
 *
 * Not built here, on purpose: HUD (optional, `hud` flag -- doctrine wants the
 * real energy / heat / QTE bars, the feed does not), ZoneSystem, DevSystem,
 * MenuSystem, RefitSystem, DebugSystem.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <sol/sol.hpp>
#include <box2d/box2d.h>
#include <memory>
#include <algorithm>
#include <cmath>

#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/DevState.hpp"
#include "utils/ZoneArchetypes.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/ShipLivery.hpp"
#include "systems/ISystem.hpp"
#include "systems/InputSystem.hpp"
#include "systems/PhysicsSystem.hpp"
#include "systems/RenderSystem.hpp"
#include "systems/DamageSystem.hpp"
#include "systems/VentQTESystem.hpp"
#include "systems/WeaponSystem.hpp"
#include "systems/AISystem.hpp"
#include "systems/TurretSystem.hpp"
#include "systems/EnemySystem.hpp"
#include "systems/ParticleSystem.hpp"
#include "systems/BackgroundSystem.hpp"
#include "systems/EffectsSystem.hpp"
#include "systems/ShipAnimSystem.hpp"
#include "systems/CameraSystem.hpp"
#include "systems/SpaceDustSystem.hpp"
#include "systems/HudSystem.hpp"
#include "systems/DebrisSystem.hpp"
#include "systems/ScrapSystem.hpp"

class ShadowWorld {
public:
    /// Everything a scene decides before the first frame.
    struct Setup {
        sf::Vector2u      size{ 1280, 720 };     ///< texture = camera view at zoom 1, px
        sf::Vector2f      spawn{ 0.f, 0.f };     ///< hunter start, world px
        float             spawnAngle = 0.f;      ///< degrees, 0 = nose up (screen -Y)
        ship::ShipDesign  design = ship::ShipDesign::preset(ship::HullClass::Medium);
        ship::Livery      livery;
        bool director  = false;                  ///< faction spawn director (real enemy waves)
        bool asteroids = false;                  ///< the real asteroid spawner
        bool hud       = false;                  ///< draw the real HUD into the texture
        bool god       = false;                  ///< hunter takes no damage (EntityManager::godMode)
        int  stars     = 260;
        std::size_t capacity = 4096;             ///< ECS reserve -- load-bearing, see reserveAll()
    };

    ShadowWorld() = default;
    ShadowWorld(const ShadowWorld&) = delete;
    ShadowWorld& operator=(const ShadowWorld&) = delete;
    ~ShadowWorld() { teardown(); }

    /**
     * @brief (Re)build the world. Safe to call again to restart a scene.
     * @param zoneReg  The real zone registry (may be null: no zones).
     * @param zoneId   Which zone's tables to read.
     */
    bool build(sf::RenderWindow& window, sol::state& lua,
        const enemyarch::EnemyRegistry& registry,
        const zonearch::ZoneRegistry* zoneReg, uint8_t zoneId,
        const sf::Font* font, const Setup& s)
    {
        m_window = &window;
        m_lua = &lua;
        m_registry = &registry;
        m_setup = s;

        if (!m_rt || m_rt->getSize() != s.size) {
            m_rt = std::make_unique<sf::RenderTexture>();
            if (!m_rt->resize(s.size)) { m_rt.reset(); return false; }
            m_rt->setSmooth(true);
        }

        // ---- 1. Fresh ECS + Box2D (SystemManager::restart, in miniature) ----
        teardown();
        if (!m_reserved) { m_em.reserveAll(s.capacity); m_reserved = true; }
        b2WorldDef wd = b2DefaultWorldDef();
        wd.gravity = { 0.f, 0.f };
        m_worldId = b2CreateWorld(&wd);
        m_built = true;

        // ---- 2. Own zone + dev state: read-only views of the real game ----
        m_zone.registry = zoneReg;
        m_zone.current = zoneId;
        m_zone.dirty = false;                 // nothing here may ever regenerate the sky
        m_dev = DevState{};
        m_dev.directorOn = s.director;
        m_dev.asteroidSpawnerOn = s.asteroids;

        // ---- 3. Hunter ----
        m_playerId = m_ef.createPlayerFromDesign(m_em, s.spawn, lua, m_worldId, s.design, s.livery);
        {
            const std::size_t pi = m_em.getEntityIndex(m_playerId);
            if (pi != (std::size_t)-1) {
                m_em.transforms[pi].rotation = s.spawnAngle;
                b2Body_SetTransform(m_em.physics[pi].bodyId, b2Body_GetPosition(m_em.physics[pi].bodyId),
                    b2MakeRot(s.spawnAngle * 3.14159265f / 180.f));
            }
        }
        m_em.godMode = s.god;
        m_em.initBackground(s.size, s.stars);

        m_view = m_rt->getDefaultView();
        m_view.setCenter(s.spawn);

        // ---- 4. Systems, same order as SystemManager::init ----
        SystemContext ctx;
        ctx.em = &m_em;
        ctx.ef = &m_ef;
        ctx.worldId = m_worldId;
        ctx.playerEntityId = m_playerId;
        ctx.lua = &lua;
        ctx.window = &window;            // InputSystem's null guard only; aim comes from the pad
        ctx.gameView = &m_view;
        ctx.enemyRegistry = &registry;
        ctx.dev = &m_dev;
        ctx.zone = &m_zone;
        ctx.target = m_rt.get();

        m_input.init(ctx);
        m_physics.init(ctx);
        m_render.init(ctx);
        m_damage.init(ctx);
        m_vent.init(ctx);
        m_weapon.init(ctx);
        m_ai.init(ctx);
        m_turret.init(ctx);
        m_enemy.init(ctx);
        m_particle.init(ctx);
        m_background.init(ctx);
        m_effects.init(ctx);
        m_shipAnim.init(ctx);
        m_camera.init(ctx);
        m_dust.init(ctx);
        m_debris.init(ctx);
        m_scrapSys.init(ctx);
        if (s.hud) { m_hud.init(ctx); m_hud.setFont(font); }

        m_time = 0.f;
        m_kills = 0;
        return true;
    }

    bool ready() const { return m_built && m_rt != nullptr; }

    /**
     * @brief Advance one frame. The pad is the hunter's input for all of it.
     *
     * Same split as SystemManager: logic in fixed-ish passes (never a step
     * longer than 1/30 s, so Box2D cannot tunnel), camera and screen FX on
     * real time.
     */
    void step(float realDt) {
        if (!ready()) return;
        realDt = std::clamp(realDt, 0.f, 0.05f);
        if (realDt <= 0.f) return;
        m_lastDt = 0.f;
        {
            InputRegistry::VirtualScope scope(&m_pad);
            const int passes = realDt > 1.f / 30.f ? 2 : 1;
            for (int k = 0; k < passes; ++k) m_lastDt += logicPass(realDt / static_cast<float>(passes));
        }
        m_camera.update(realDt);
        m_em.updateFx(realDt);
        m_time += realDt;
    }

    /// Draw the frame into the texture (does not touch the window).
    void render(sf::Color voidColor = sf::Color(3, 5, 8)) {
        if (!ready()) return;
        sf::RenderTexture& t = *m_rt;
        t.clear(voidColor);
        t.setView(m_camera.makeStarView(m_em.starFieldSize));
        m_background.update(m_lastDt);
        t.setView(m_camera.getWorldView());
        m_dust.update(m_lastDt);
        m_debris.update(m_lastDt);
        m_scrapSys.draw();
        m_particle.update(m_lastDt);
        m_render.update(m_lastDt);
        t.setView(t.getDefaultView());
        m_render.drawScreenSpace();
        if (m_setup.hud) m_hud.update(m_lastDt);
        t.display();
    }

    // ---- Access for the pilot and the scene director ----
    VirtualPad& pad() { return m_pad; }
    EntityManager& em() { return m_em; }
    const EntityManager& em() const { return m_em; }
    EntityFactory& factory() { return m_ef; }
    EnemySystem& enemies() { return m_enemy; }
    const AISystem& ai() const { return m_ai; }
    DevState& dev() { return m_dev; }
    b2WorldId world() const { return m_worldId; }
    uint32_t playerId() const { return m_playerId; }
    std::size_t playerIndex() const { return m_em.getEntityIndex(m_playerId); }
    bool playerAlive() const { return m_built && m_em.getEntityIndex(m_playerId) != (std::size_t)-1; }
    const sf::View& worldView() const { return m_camera.getWorldView(); }
    sf::View& view() { return m_view; }
    const sf::Texture* texture() const { return m_rt ? &m_rt->getTexture() : nullptr; }
    sf::Vector2u size() const { return m_rt ? m_rt->getSize() : sf::Vector2u{}; }
    float time() const { return m_time; }
    int kills() const { return m_kills; }
    sol::state& lua() { return *m_lua; }
    const enemyarch::EnemyRegistry& registry() const { return *m_registry; }

    /// Spawn a unit by archetype key, ignoring the director (EnemySystem::summon).
    uint32_t summon(const std::string& key, sf::Vector2f pos) { return m_enemy.summon(key, pos); }
    /// Spawn one asteroid of an asteroids.lua type.
    uint32_t rock(const std::string& type, sf::Vector2f pos, sf::Vector2f vel = { 0.f, 0.f }) {
        return m_enemy.spawnAsteroid(type, pos, vel);
    }

private:
    float logicPass(float passDt) {
        const float dt = m_em.advanceTime(passDt);
        m_input.update(dt);
        m_physics.update(dt);
        m_shipAnim.update(dt);
        m_effects.update(dt);
        m_physics.cleanup();
        m_enemy.update(dt);
        m_damage.update(dt);
        m_vent.update(dt);
        m_weapon.update(dt);
        m_ai.update(dt);
        m_turret.update(dt);
        m_scrapSys.update(dt);
        // Nobody records a hidden world's kills: count them, then drop them.
        m_kills += static_cast<int>(m_em.codexKills.size());
        m_em.codexKills.clear();
        return dt;
    }

    void teardown() {
        if (!m_built) return;
        m_em.reset();
        if (b2World_IsValid(m_worldId)) b2DestroyWorld(m_worldId);
        m_built = false;
    }

    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    Setup m_setup;
    std::unique_ptr<sf::RenderTexture> m_rt;

    EntityManager m_em;
    EntityFactory m_ef;
    b2WorldId m_worldId{};
    uint32_t m_playerId = 0;
    bool m_built = false, m_reserved = false;
    sf::View m_view;
    zonearch::ZoneState m_zone;
    DevState m_dev;
    VirtualPad m_pad;
    float m_time = 0.f, m_lastDt = 0.f;
    int m_kills = 0;

    InputSystem m_input;
    PhysicsSystem m_physics;
    RenderSystem m_render;
    DamageSystem m_damage;
    VentQTESystem m_vent;
    WeaponSystem m_weapon;
    AISystem m_ai;
    TurretSystem m_turret;
    EnemySystem m_enemy;
    ParticleSystem m_particle;
    BackgroundSystem m_background;
    EffectsSystem m_effects;
    ShipAnimSystem m_shipAnim;
    CameraSystem m_camera;
    SpaceDustSystem m_dust;
    HudSystem m_hud;
    DebrisSystem m_debris;
    ScrapSystem m_scrapSys;
};
