/**
 * @file DevState.hpp
 * @brief Every dev-menu flag in one plain struct.
 *
 * ============================================================================
 * WHY IT LIVES HERE AND NOT ON EntityManager
 * ============================================================================
 * EntityManager::reset() runs on every restart and wipes time, camera and
 * entity state. A god-mode flag stored there would silently switch itself off
 * the first time you died -- exactly when you wanted it. SystemManager owns
 * this struct and hands a pointer out through SystemContext, the same way
 * every other dependency travels. It survives restarts by construction.
 *
 * The one exception is EntityManager::godMode, which damagePlayer() has to
 * see. DevSystem copies it across every frame; nothing in gameplay writes it.
 *
 * ============================================================================
 * SHIPPING
 * ============================================================================
 * Define VH_SHIPPING for a release build. The struct still exists (systems
 * test `m_dev && m_dev->flag`, which stays false), but the tilde key is dead,
 * so nothing can ever set a flag. One gate, not forty #ifdefs.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/System/Vector2.hpp>
#include <unordered_set>
#include <cstdint>

#if !defined(VH_SHIPPING)
#define VH_DEV 1
#endif

namespace dev {

    /// Hard ceiling for anything the dev menu creates, and for the
    /// no-cooldown fire rate. EntityManager reserves 8192 slots and that
    /// reserve is LOAD-BEARING: systems hold `auto&` into the SoA vectors
    /// across entity-creating calls, so a reallocation dangles them. 6000
    /// leaves 2000 for fracture cascades, bullets and debris the spawn causes.
    inline constexpr size_t ENTITY_BUDGET = 6000;

    /// Must match EntityManager::reserveAll(). Only used for the stats readout.
    inline constexpr size_t ENTITY_RESERVE = 8192;

    /// Minimum seconds between player shots under NO COOLDOWNS. Zero would be
    /// 60+ bullets a second per gun, which is a reserve blow-out, not a test.
    inline constexpr float NO_CD_FIRE_INTERVAL = 0.05f;

    /// The simulation never takes a single step larger than this. Mirrors the
    /// 0.05s clamp in game.cpp -- variable-step Box2D tunnels above it.
    inline constexpr float MAX_STEP = 0.05f;

} // namespace dev

struct DevState {
    // ---- Menu ----
    bool menuOpen = false;

    // ---- Player cheats ----
    bool godMode = false;
    bool infiniteEnergy = false;   ///< energy, engine overheat AND weapon heat
    bool noCooldowns = false;      ///< dash, parry, rift; fire floored at NO_CD_FIRE_INTERVAL
    bool oneShot = false;          ///< any HP an enemy loses this pass becomes all of it

    // ---- World ----
    bool directorOn = true;        ///< faction spawn director
    bool asteroidSpawnerOn = true;

    // ---- Enemy control ----
    bool freezeAllAI = false;
    std::unordered_set<uint32_t> aiDisabled;   ///< per-enemy AI off, by PERSISTENT id
    uint32_t selectedEnemy = 0;                ///< persistent id, 0 = none

    bool isAIFrozen(uint32_t entityId) const {
        return freezeAllAI || aiDisabled.count(entityId) != 0;
    }

    // ---- Time ----
    bool  paused = false;          ///< dev pause: logic frozen, camera + render live
    int   stepFrames = 0;          ///< queued single steps while paused
    float timeScale = 1.f;         ///< 0.25 .. 4.0. >1 runs extra passes, never a bigger step

    // ---- Camera ----
    bool         freeCam = false;
    bool         freeCamActive = false;   ///< CameraSystem-owned: seeded this session
    sf::Vector2f freeCamCenter{ 0.f, 0.f };
    float        viewScale = 1.f;         ///< 0.5 .. 2.0, >1 shows more world

    // ---- Visual debug ----
    bool showAILabels = false;
    bool showVisionCones = false;
    bool showAimLines = false;
    bool showStats = false;

    // ---- Requests to SystemManager (consumed there, same frame) ----
    bool requestReloadScripts = false;

    /// Anything keyed by entity id must go on restart: ids restart at 1, so a
    /// stale "AI off" would land on an unrelated unit in the next run.
    void onRestart() {
        aiDisabled.clear();
        selectedEnemy = 0;
        freeCamActive = false;
        stepFrames = 0;
    }
};