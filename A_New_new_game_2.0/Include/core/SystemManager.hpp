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
#include "systems/MenuSystem.hpp"

class SystemManager {
public:
    SystemManager(sf::RenderWindow& window, sol::state& lua)
        : m_window(window), m_lua(lua) {}

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
        SystemContext ctx;
        ctx.em = &m_entityManager;
        ctx.ef = &m_entityFactory;
        ctx.worldId = m_worldId;
        ctx.playerEntityId = m_playerEntityId;
        ctx.lua = &m_lua;
        ctx.window = &m_window;
        ctx.gameView = &m_gameView;

        // 6. Init all systems (order doesn't matter here)
        m_inputSystem.init(ctx);
        m_physicsSystem.init(ctx);
        m_renderSystem.init(ctx);
        m_damageSystem.init(ctx);
        m_weaponSystem.init(ctx);
        m_aiSystem.init(ctx);
        m_enemySystem.init(ctx);
        m_particleSystem.init(ctx);
        m_backgroundSystem.init(ctx);
        m_effectsSystem.init(ctx);
        m_debugSystem.init(ctx);
        m_cameraSystem.init(ctx);
        m_shipAnimSystem.init(ctx);
        m_spaceDustSystem.init(ctx);
        m_debugSystem.init(ctx);
        m_hudSystem.init(ctx);
        // 8192, not 2048: this capacity is load-bearing (see reserveAll's
        // docs -- systems hold references across entity creation, so a
        // reallocation would dangle them). Headroom is cheap; a heap
        // corruption in a busy fight is not.
        m_entityManager.reserveAll(8192);
        m_debrisSystem.init(ctx);
        m_menuSystem.init(ctx);
        m_state = GameState::MainMenu;
    }

    void update(float realDt) {
        m_menuSystem.setState(m_state, m_entityManager.totalScore);

        // ====================================================================
        // 1. MENU STATES (no game logic)
        // ====================================================================
        if (m_state == GameState::MainMenu || m_state == GameState::Tutorial) {
            m_menuSystem.update(realDt);
            return;
        }

        // ====================================================================
        // 2. PAUSED (draw frozen world, show menu)
        // ====================================================================
        if (m_state == GameState::Paused) {
            m_window.setView(m_cameraSystem.getWorldView());
            m_particleSystem.update(0.f);
            m_renderSystem.update(0.f);
            m_debugSystem.update(0.f);

            m_window.setView(m_window.getDefaultView());
            m_menuSystem.update(realDt);
            return;
        }

        // ====================================================================
        // 3. PLAYING / GAME OVER (run the simulation)
        // ====================================================================
        const float dt = m_entityManager.advanceTime(realDt);

        // ----- 3a. LOGIC (scaled) -----
        m_inputSystem.update(dt);
        m_physicsSystem.update(dt);
        m_shipAnimSystem.update(dt);
        m_effectsSystem.update(dt);
        m_physicsSystem.cleanup();
        m_enemySystem.update(dt);
        m_damageSystem.update(dt);
        m_weaponSystem.update(dt);
        m_aiSystem.update(dt);

        size_t playerIdx = m_entityManager.getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) {
            m_state = GameState::GameOver;
        }

        // ----- 3b. CAMERA + SCREEN FX (real time) -----
        m_cameraSystem.update(realDt);
        m_entityManager.updateFx(realDt);

        // ----- 3c. STARS (screen space, inheriting shake + zoom) -----
        m_window.setView(m_cameraSystem.makeStarView(m_entityManager.starFieldSize));
        m_backgroundSystem.update(dt);

        // ----- 3d. WORLD SPACE -----
        m_window.setView(m_cameraSystem.getWorldView());
        m_spaceDustSystem.update(dt);
        m_debrisSystem.update(dt);
        m_particleSystem.update(dt);
        m_renderSystem.update(dt);
        m_debugSystem.update(dt);

        // ----- 3e. SCREEN OVERLAY -----
        m_window.setView(m_window.getDefaultView());
        m_renderSystem.drawScreenSpace();

        // ----- 3f. HUD -----
        m_hudSystem.update(realDt);

        // ----- 3g. GAME OVER OVERLAY -----
        if (m_state == GameState::GameOver) {
            m_menuSystem.update(realDt);
        }
    }


  
    // Accessors for main.cpp (if needed)
    EntityManager& getEntityManager() { return m_entityManager; }
    uint32_t getPlayerId() const { return m_playerEntityId; }
    DebugSystem& getDebugSystem() { return m_debugSystem; }
    HudSystem& getHudSystem() { return m_hudSystem; }
    GameState getState() const { return m_state; }
    MenuSystem& getMenuSystem() { return m_menuSystem; }

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
        m_playerEntityId = m_entityFactory.createPlayer(
            m_entityManager, { 640.f, 360.f }, m_lua, m_worldId);

        m_gameView.setCenter(m_entityManager.transforms[
            m_entityManager.getEntityIndex(m_playerEntityId)].position);

        // 4. Re-point every system's context at the NEW worldId/playerEntityId
        //    (systems cached these by value in init(), so they're stale otherwise)
        SystemContext ctx;
        ctx.em = &m_entityManager;
        ctx.ef = &m_entityFactory;
        ctx.worldId = m_worldId;
        ctx.playerEntityId = m_playerEntityId;
        ctx.lua = &m_lua;
        ctx.window = &m_window;
        ctx.gameView = &m_gameView;

        m_inputSystem.init(ctx);
        m_physicsSystem.init(ctx);
        m_renderSystem.init(ctx);
        m_damageSystem.init(ctx);
        m_weaponSystem.init(ctx);
        m_aiSystem.init(ctx);
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
        m_menuSystem.init(ctx);

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
            m_state = GameState::Tutorial;
            m_menuSystem.setState(m_state);
            break;
        case MenuAction::BackToMenu:
            m_state = GameState::MainMenu;
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



private:
    sf::RenderWindow& m_window;
    sf::View m_gameView;
    sol::state& m_lua;

    b2WorldId m_worldId;
    EntityManager m_entityManager;
    EntityFactory m_entityFactory;
    uint32_t m_playerEntityId = 0;
    GameState m_state = GameState::MainMenu;

    // ---- All systems (default-constructible) ----
    InputSystem m_inputSystem;
    PhysicsSystem m_physicsSystem;
    RenderSystem m_renderSystem;
    DamageSystem m_damageSystem;
    WeaponSystem m_weaponSystem;
    AISystem m_aiSystem;
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
    MenuSystem m_menuSystem;
};