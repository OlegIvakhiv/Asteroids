#pragma once

#include <SFML/Graphics.hpp>
#include <sol/sol.hpp>
#include <box2d/box2d.h>
#include <vector>
#include <memory>

#include "EntityManager.hpp"
#include "EntityFactory.hpp"
#include "core/FractureImpl.hpp"
#include "utils/GameConfig.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/GameState.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/DevState.hpp"
#include "utils/ZoneArchetypes.hpp"
#include "utils/LuaConfig.hpp"

// Include all system headers
#include "systems/ISystem.hpp"
#include "systems/InputSystem.hpp"
#include "systems/PhysicsSystem.hpp"
#include "systems/RenderSystem.hpp"
#include "systems/DamageSystem.hpp"
#include "systems/WeaponSystem.hpp"
#include "systems/AISystem.hpp"
#include "systems/EnemySystem.hpp"
#include "systems/ParticleSystem.hpp"
#include "systems/BackgroundSystem.hpp"
#include "systems/EffectsSystem.hpp"
#include "systems/DebugSystem.hpp"
#include "systems/ShipAnimSystem.hpp"
#include "systems/CameraSystem.hpp"
#include "systems/SpaceDustSystem.hpp"
#include "systems/HudSystem.hpp"
#include "systems/DebrisSystem.hpp"
#include "systems/ScrapSystem.hpp"          // scrap pickups + magnet
#include "systems/MenuSystem.hpp"
#include "systems/RefitSystem.hpp"
#include "systems/TurretSystem.hpp"          // turret AI
#include "systems/VentQTESystem.hpp"        // vent QTE and overdrive
#include "systems/DevSystem.hpp"            // tilde dev menu
#include "systems/ZoneSystem.hpp"           // zone visuals + background props
#include <iostream>
#include <cmath>
#include <algorithm>

class SystemManager {
public:
    SystemManager(sf::RenderWindow& window, sol::state& lua)
        : m_window(window), m_lua(lua) {
    }

    ~SystemManager() {
        if (b2World_IsValid(m_worldId)) {
            b2DestroyWorld(m_worldId);
        }
    }

    void init() {
        // 1. Box2D World
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = { 0.0f, 0.0f };
        m_worldId = b2CreateWorld(&worldDef);
        m_enemyRegistry.load(m_lua);

        // Zones are OPTIONAL, and are loaded here rather than in game.cpp so
        // there is ONE place that runs zones.lua. A failed load leaves
        // ZoneState::def() null and every consumer falls back to its pre-zone
        // behaviour, so an install without scripts/zones.lua still runs.
        reloadZones();

        // 2. Create Player
        m_playerEntityId = m_entityFactory.createPlayer(
            m_entityManager, { 640.f, 360.f }, m_lua, m_worldId
        );

        // 3. Background stars
        m_entityManager.initBackground(m_window.getSize(), 800);

        // 3.5 Seed the shared world view
        m_gameView = m_window.getDefaultView();
        m_gameView.setCenter(m_entityManager.transforms[
            m_entityManager.getEntityIndex(m_playerEntityId)].position);

        // 4. InputRegistry (static, called once)
        InputRegistry::init();

        // 5. Prepare context
        const SystemContext ctx = makeContext();

        // 6. Init all systems (order doesn't matter here)
        m_inputSystem.init(ctx);
        m_physicsSystem.init(ctx);
        m_renderSystem.init(ctx);
        m_damageSystem.init(ctx);
        m_ventQTESystem.init(ctx);          // added
        m_weaponSystem.init(ctx);
        m_aiSystem.init(ctx);
        m_turretSystem.init(ctx);
        m_enemySystem.init(ctx);
        m_particleSystem.init(ctx);
        m_backgroundSystem.init(ctx);
        m_effectsSystem.init(ctx);
        m_debugSystem.init(ctx);
        m_cameraSystem.init(ctx);
        m_shipAnimSystem.init(ctx);
        m_spaceDustSystem.init(ctx);
        m_hudSystem.init(ctx);
        // 8192, not 2048: this capacity is load-bearing (see reserveAll's
        // docs -- systems hold references across entity creation, so a
        // reallocation would dangle them). Headroom is cheap; a heap
        // corruption in a busy fight is not.
        m_entityManager.reserveAll(8192);
        m_debrisSystem.init(ctx);
        m_scrapSystem.init(ctx);
        m_menuSystem.init(ctx);
        m_zoneSystem.init(ctx);
        m_devSystem.init(ctx);
        m_devSystem.setSystems(&m_enemySystem, &m_aiSystem, &m_debugSystem, &m_zoneSystem);
        if (m_shipDesign.mountedGuns().empty() && m_shipDesign.mountedEngines().empty())
            m_shipDesign.autoMount();

        m_refitSystem.init(ctx);
        m_refitSystem.setDesign(&m_shipDesign);
        m_refitSystem.setLivery(&m_livery);

        m_state = GameState::MainMenu;
    }

    void update(float realDt) {
        // Before anything draws. The starfield is rendered ahead of the
        // world-space pass, so regenerating it any later would leave one frame
        // of the old sky under the new zone -- the exact frame the player is
        // looking at when they switch.
        m_zoneSystem.applyIfDirty();

        m_menuSystem.setState(m_state, m_entityManager.scrap);

        // ====================================================================
        // 1. MENU STATES (no game logic)
        // ====================================================================
        if (m_state == GameState::MainMenu || m_state == GameState::Tutorial) {
            m_menuSystem.update(realDt);
            // Mouse clicks on rows and buttons arrive through this channel
            const MenuAction clicked = m_menuSystem.takeClickAction();
            if (clicked != MenuAction::None) requestAction(clicked);
            return;
        }

        if (m_state == GameState::Refit) {
            m_refitSystem.update(realDt);
            if (m_refitSystem.wantsExit()) {
                m_refitSystem.clearExit();
                m_state = GameState::MainMenu;
                m_menuSystem.setState(m_state);   // this now calls primeInput()
            }
            return;
        }

        // ====================================================================
        // 2. PAUSED (draw frozen world, show menu)
        // ====================================================================
        if (m_state == GameState::Paused) {
            m_window.setView(m_cameraSystem.getWorldView());
            m_zoneSystem.update(0.f);
            m_debrisSystem.update(0.f);     // wreckage stays on screen, frozen
            m_scrapSystem.draw();
            m_particleSystem.update(0.f);
            m_renderSystem.update(0.f);
            m_debugSystem.update(0.f);

            // UI view – always matches current window size
            m_window.setView(sf::View(sf::FloatRect({ 0.f, 0.f },
                { static_cast<float>(m_window.getSize().x),
                  static_cast<float>(m_window.getSize().y) })));

            m_menuSystem.update(realDt);
            // Consume click actions for paused menu
            const MenuAction clicked = m_menuSystem.takeClickAction();
            if (clicked != MenuAction::None) requestAction(clicked);
            return;
        }

        // ====================================================================
        // 3. PLAYING / GAME OVER (run the simulation)
        // ====================================================================
        m_devSystem.frameBegin(realDt);

        // ----- 3a. LOGIC (scaled), under dev time control -----
        //
        // SPEED-UP RUNS MORE PASSES, NEVER A BIGGER STEP. PhysicsSystem steps
        // Box2D with the variable dt it is given, and that is only safe
        // because game.cpp clamps dt to 0.05s. Multiplying dt by 4 would hand
        // it 0.2s steps and bullets would tunnel through hulls. So 4x is four
        // ordinary passes; 1.5x is two passes of 0.75 frame each.
        //
        // DEV PAUSE SKIPS THE LOGIC BLOCK rather than passing dt = 0. Several
        // systems divide by dt or edge-detect input per call; zero-length
        // passes are an invitation to a NaN. Camera and rendering keep
        // running on real time, so free cam and overlays work while frozen.
        float dt = 0.f;
        if (m_dev.paused) {
            if (m_dev.stepFrames > 0) {
                --m_dev.stepFrames;
                dt = runLogicPass(1.f / 60.f);
            }
        }
        else {
            const float scale = std::clamp(m_dev.timeScale, 0.05f, 4.f);
            const int passes = std::max(1, static_cast<int>(std::ceil(scale - 0.001f)));
            const float passDt = std::min(realDt * scale / static_cast<float>(passes), dev::MAX_STEP);
            for (int k = 0; k < passes; ++k) dt += runLogicPass(passDt);
        }

        // ----- 3b. CAMERA + SCREEN FX (real time) -----
        m_cameraSystem.update(realDt);
        m_entityManager.updateFx(realDt);

        // ----- 3c. STARS (screen space, inheriting shake + zoom) -----
        m_window.setView(m_cameraSystem.makeStarView(m_entityManager.starFieldSize));
        m_backgroundSystem.update(dt);

        // ----- 3d. WORLD SPACE -----
        m_window.setView(m_cameraSystem.getWorldView());
        m_zoneSystem.update(dt);      // backdrop: behind dust, debris and ships
        m_spaceDustSystem.update(dt);
        m_debrisSystem.update(dt);
        m_scrapSystem.draw();
        m_particleSystem.update(dt);
        m_renderSystem.update(dt);
        m_debugSystem.update(dt);
        m_devSystem.drawWorld();

        // ----- 3e. SCREEN OVERLAY -----
        m_window.setView(sf::View(sf::FloatRect({ 0.f, 0.f },
            { static_cast<float>(m_window.getSize().x),
              static_cast<float>(m_window.getSize().y) })));

        m_renderSystem.drawScreenSpace();

        // ----- 3f. HUD -----
        m_hudSystem.update(realDt);

        // ----- 3g. GAME OVER OVERLAY -----
        if (m_state == GameState::GameOver) {
            m_menuSystem.update(realDt);
        }

        // ----- 3h. DEV MENU -- last, on top, and the only place dev actions
        //      run: every system has finished iterating by now, so creating
        //      and destroying entities here cannot pull a vector out from
        //      under anyone.
        m_devSystem.drawOverlay(realDt);
        serviceDevRequests();
    }

    /**
     * @brief Reload all three Lua scripts and rebuild the archetype cache.
     *
     * The one implementation behind F5 and the dev menu button. Without the
     * registry reload, enemy.lua edits silently do nothing.
     */
    bool reloadScripts() {
        // Invalidate every cached Lua table handle (luacfg::Table) and the
        // ClassFeel cache. Bumped BEFORE running the scripts on purpose: if a
        // script throws halfway, whatever globals it did replace are picked
        // up next frame -- the same as the old uncached reads behaved.
        luacfg::bumpEpoch();
        try {
            m_lua.script_file("scripts/player.lua");
            m_lua.script_file("scripts/asteroids.lua");
            m_lua.script_file("scripts/enemy.lua");
            m_enemyRegistry.load(m_lua);

            // zones.lua is reloaded in the same breath, and the registry MUST
            // be rebuilt with it: every ZoneDef holds a sol::table into the old
            // Lua tables and re-running the script replaces those. Same
            // stale-handle trap the enemy registry has.
            reloadZones();
            std::cout << "Scripts reloaded!" << std::endl;
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to reload Lua script: " << e.what() << std::endl;
            return false;
        }
    }

    // Accessors for main.cpp (if needed)
    EntityManager& getEntityManager() { return m_entityManager; }
    uint32_t getPlayerId() const { return m_playerEntityId; }
    DebugSystem& getDebugSystem() { return m_debugSystem; }
    HudSystem& getHudSystem() { return m_hudSystem; }
    GameState getState() const { return m_state; }
    MenuSystem& getMenuSystem() { return m_menuSystem; }
    RefitSystem& getRefitSystem() { return m_refitSystem; }
    DevSystem& getDevSystem() { return m_devSystem; }
    ZoneSystem& getZoneSystem() { return m_zoneSystem; }
    zonearch::ZoneState& getZoneState() { return m_zoneState; }
    const zonearch::ZoneRegistry& getZoneRegistry() const { return m_zoneRegistry; }

    /// What game.cpp clears the window to. Falls back to the original
    /// near-black when no zone is loaded.
    sf::Color voidColor() const {
        const zonearch::ZoneDef* z = m_zoneState.def();
        return z ? z->voidColor : sf::Color(2, 3, 5);
    }

    /**
     * @brief Load (or reload) zones.lua and rebuild the zone table.
     *
     * Kept separate from reloadScripts() so init() can call it once at startup,
     * where a missing file is a warning rather than a fatal error. Holds the
     * CURRENT zone by KEY across the reload: zone ids come from `zone_order`,
     * and an edit to that list would otherwise silently move you to a
     * different zone.
     */
    bool reloadZones() {
        const zonearch::ZoneDef* before = m_zoneState.def();
        const std::string key = before ? before->key : std::string();

        luacfg::bumpEpoch();   // see reloadScripts()
        try {
            m_lua.script_file("scripts/zones.lua");
        }
        catch (const std::exception& e) {
            std::cerr << "[SystemManager] scripts/zones.lua not loaded: "
                << e.what() << "\n  Running without zones.\n";
            return false;
        }

        m_zoneState.registry = &m_zoneRegistry;
        if (!m_zoneRegistry.load(m_lua)) return false;

        const uint8_t id = key.empty() ? m_zoneRegistry.defaultId()
            : m_zoneRegistry.idOf(key);
        m_zoneState.current = (id == zonearch::INVALID_ZONE)
            ? m_zoneRegistry.defaultId() : id;

        // Live props hold PropDef pointers into the registry just rebuilt.
        m_zoneState.dirty = true;
        return true;
    }

    /// Called by game.cpp when the menu confirms StartGame/RestartGame.
    void restart() {
        // 1. Free BodyUserData + clear all ECS vectors (does NOT touch Box2D)
        m_entityManager.reset();

        // 2. Tear down and recreate the Box2D world
        if (b2World_IsValid(m_worldId)) {
            b2DestroyWorld(m_worldId);
        }
        b2WorldDef worldDef = b2DefaultWorldDef();
        worldDef.gravity = { 0.0f, 0.0f };
        m_worldId = b2CreateWorld(&worldDef);

        // 3. Re-create the player
        m_playerEntityId = m_entityFactory.createPlayerFromDesign(
            m_entityManager, { 640.f, 360.f }, m_lua, m_worldId, m_shipDesign, m_livery);

        m_gameView.setCenter(m_entityManager.transforms[
            m_entityManager.getEntityIndex(m_playerEntityId)].position);

        // 4. Re-point every system's context at the NEW worldId/playerEntityId
        //    (systems cached these by value in init(), so they're stale otherwise)
        //    Dev flags survive the restart; anything keyed by entity id does not,
        //    because ids restart at 1.
        m_dev.onRestart();
        const SystemContext ctx = makeContext();

        m_inputSystem.init(ctx);
        m_physicsSystem.init(ctx);
        m_renderSystem.init(ctx);
        m_damageSystem.init(ctx);
        m_ventQTESystem.init(ctx);          // added
        m_weaponSystem.init(ctx);
        m_aiSystem.init(ctx);
        m_turretSystem.init(ctx);
        m_enemySystem.init(ctx);
        m_particleSystem.init(ctx);
        m_backgroundSystem.init(ctx);
        m_effectsSystem.init(ctx);
        m_debugSystem.init(ctx);
        m_cameraSystem.init(ctx);
        m_shipAnimSystem.init(ctx);
        m_spaceDustSystem.init(ctx);
        m_hudSystem.init(ctx);
        m_debrisSystem.init(ctx);
        m_scrapSystem.init(ctx);
        m_menuSystem.init(ctx);
        m_zoneSystem.init(ctx);
        m_devSystem.init(ctx);
        if (m_shipDesign.mountedGuns().empty() && m_shipDesign.mountedEngines().empty())
            m_shipDesign.autoMount();

        m_refitSystem.init(ctx);
        m_refitSystem.setDesign(&m_shipDesign);
        m_refitSystem.setLivery(&m_livery);

        m_state = GameState::Playing;
    }

    /// Central place all state transitions go through, so UI stays in sync.
    void requestAction(MenuAction action) {
        switch (action) {
        case MenuAction::StartGame:
        case MenuAction::RestartGame:
            restart();
            break;
        case MenuAction::ResumeGame:
            m_state = GameState::Playing;
            break;
        case MenuAction::QuitGame:
            m_window.close();
            break;
        case MenuAction::ShowTutorial:
            // Only record a real screen -- re-entering the tutorial from
            // itself must not make it its own return target.
            if (m_state != GameState::Tutorial)
                m_tutorialReturnTo = m_state;
            m_state = GameState::Tutorial;
            m_menuSystem.setState(m_state);
            break;
        case MenuAction::ShowRefit:
            m_state = GameState::Refit;
            m_menuSystem.setState(m_state);
            m_refitSystem.onEnter();
            break;
        case MenuAction::BackToMenu:
            // Closing the tutorial goes back where it came from. Every other
            // use of BackToMenu (ABANDON, RETURN AFTER DEATH) means the
            // terminal, and those are deliberate exits from the run.
            if (m_state == GameState::Tutorial) {
                m_state = m_tutorialReturnTo;
            }
            else {
                m_state = GameState::MainMenu;
            }
            m_menuSystem.setState(m_state);
            break;
        default:
            break;
        }
    }

    void togglePause() {
        if (m_state == GameState::Playing) m_state = GameState::Paused;
        else if (m_state == GameState::Paused) m_state = GameState::Playing;
    }

    void reloadEnemyRegistry() { m_enemyRegistry.load(m_lua); }

private:
    /// One place builds the context, so init() and restart() cannot drift --
    /// they had already been two hand-maintained copies of the same block.
    SystemContext makeContext() {
        SystemContext ctx;
        ctx.em = &m_entityManager;
        ctx.ef = &m_entityFactory;
        ctx.worldId = m_worldId;
        ctx.playerEntityId = m_playerEntityId;
        ctx.lua = &m_lua;
        ctx.window = &m_window;
        ctx.gameView = &m_gameView;
        ctx.enemyRegistry = &m_enemyRegistry;
        ctx.dev = &m_dev;
        ctx.zone = &m_zoneState;
        return ctx;
    }

    /**
     * @brief One simulation pass: every logic system, bracketed by the dev
     *        cheat hooks, then the player-death check.
     * @param passDt Real-seconds share of this pass. Hitstop scales it.
     * @return The scaled dt this pass actually simulated.
     */
    float runLogicPass(float passDt) {
        const float dt = m_entityManager.advanceTime(passDt);

        m_devSystem.beginPass();

        m_inputSystem.update(dt);
        m_physicsSystem.update(dt);
        m_shipAnimSystem.update(dt);
        m_effectsSystem.update(dt);
        m_physicsSystem.cleanup();
        m_enemySystem.update(dt);
        m_damageSystem.update(dt);
        m_ventQTESystem.update(dt);            // must set overdrive before weapon reads it
        m_weaponSystem.update(dt);
        m_aiSystem.update(dt);
        m_turretSystem.update(dt);
        m_scrapSystem.update(dt);           // after DamageSystem: this frame's drops exist

        m_devSystem.endPass();

        size_t playerIdx = m_entityManager.getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1 && m_state != GameState::GameOver) {
            // Edge-triggered: this block re-runs every pass while dead,
            // so anything stateful in here MUST check the transition.
            m_state = GameState::GameOver;
            m_hunterLosses++;
            m_menuSystem.setHunterLosses(m_hunterLosses);
        }
        return dt;
    }

    void serviceDevRequests() {
        if (m_dev.requestReloadScripts) {
            m_dev.requestReloadScripts = false;
            m_devSystem.reportReload(reloadScripts());
        }
    }

public:

    const enemyarch::EnemyRegistry& getEnemyRegistry() const { return m_enemyRegistry; }

private:
    sf::RenderWindow& m_window;
    sf::View m_gameView;
    sol::state& m_lua;
    enemyarch::EnemyRegistry m_enemyRegistry;

    b2WorldId m_worldId;
    EntityManager m_entityManager;
    EntityFactory m_entityFactory;
    uint32_t m_playerEntityId = 0;
    GameState m_state = GameState::MainMenu;

    /// Where ShowTutorial was requested from, so closing it returns there
    /// instead of dumping the player out of a live run.
    GameState m_tutorialReturnTo = GameState::MainMenu;

    int m_hunterLosses = 0;

    ship::ShipDesign m_shipDesign = ship::ShipDesign::stock();
    ship::Livery     m_livery;   ///< Paint, decals, cockpit. Persists across runs.
    RefitSystem m_refitSystem;

    // ---- All systems (default-constructible) ----
    InputSystem m_inputSystem;
    PhysicsSystem m_physicsSystem;
    RenderSystem m_renderSystem;
    DamageSystem m_damageSystem;
    VentQTESystem m_ventQTESystem;           // added
    WeaponSystem m_weaponSystem;
    AISystem m_aiSystem;
    TurretSystem m_turretSystem;
    EnemySystem m_enemySystem;
    ParticleSystem m_particleSystem;
    BackgroundSystem m_backgroundSystem;
    EffectsSystem m_effectsSystem;
    DebugSystem m_debugSystem;
    ShipAnimSystem  m_shipAnimSystem;
    CameraSystem    m_cameraSystem;
    SpaceDustSystem m_spaceDustSystem;
    HudSystem m_hudSystem;
    DebrisSystem m_debrisSystem;
    ScrapSystem  m_scrapSystem;
    MenuSystem m_menuSystem;

    // ---- Zones ----
    zonearch::ZoneRegistry m_zoneRegistry;
    zonearch::ZoneState    m_zoneState;   ///< Survives restarts, like DevState
    ZoneSystem             m_zoneSystem;

    // ---- Dev ----
    DevState  m_dev;         ///< Declared before DevSystem only for readability; no ctor dependency.
    DevSystem m_devSystem;
};