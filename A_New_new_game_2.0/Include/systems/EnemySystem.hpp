/**
 * @file EnemySystem.hpp
 * @brief Enemy and asteroid spawner system
 *
 * Manages the spawning of asteroids and enemy ships (pirates) over time.
 * Uses separate timers for each type:
 * - Asteroids: spawn frequently in waves, with configurable types (SMALL, MEDIUM, LARGE, MAGMATIC)
 * - Enemy ships: spawn less frequently, up to a maximum count
 *
 * Spawns entities at a radius around the player, heading toward a random offset
 * near the player position (not directly at the player).
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (refactored)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include <SFML/System/Clock.hpp>
#include <cmath>
#include <cstdlib>

 /**
  * @class EnemySystem
  * @brief Spawns asteroids and enemy ships based on timers and population limits
  *
  * Uses two independent timers (asteroidSpawnClock, pirateSpawnClock) to
  * control spawn frequency. Spawn positions are calculated at a radius around
  * the player, with asteroids aimed toward a random offset near the player.
  * Supports magmatic (explosive) asteroid spawning with configurable chance.
  */
class EnemySystem : public ISystem {
public:
    /**
     * @brief Initialise the system with the global context
     * @param ctx SystemContext containing all engine dependencies
     *
     * Stores pointers to EntityManager, EntityFactory, Box2D world,
     * player ID, and Lua state. Resets spawn timers.
     */
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;

        // Reset spawn timers when system is initialised
        m_asteroidSpawnClock.restart();
        m_pirateSpawnClock.restart();
    }

    /**
     * @brief Update spawn timers and create new entities
     * @param dt Delta time in seconds (not used directly, but kept for interface)
     *
     * Called every frame from SystemManager. Checks spawn timers against
     * configured intervals and spawns new entities if conditions are met.
     * Counts current entities to respect maximum population limits.
     */
    void update(float dt) override {
        if (!m_em || !m_lua || !m_ef) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& playerTf = m_em->transforms[playerIdx];

        // ====================================================================
        // 1. LOAD CONFIGURATION FROM LUA
        // ====================================================================
        sol::table astSettings = (*m_lua)["spawn_settings"];
        float astInterval = astSettings["interval"].get_or(1.0f);
        int maxAstCount = astSettings["max_count"].get_or(40);
        float spawnRadius = astSettings["spawn_radius"].get_or(1500.f);

        // ====================================================================
        // 2. COUNT CURRENT ENTITIES
        // ====================================================================
        int currentAsteroids = 0;
        int currentPirates = 0;

        for (const auto& p : m_em->physics) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(p.bodyId);
            if (!ud) continue;
            BodyType type = ud->type;

            if (type == BodyType::Asteroid) currentAsteroids++;
            if (type == BodyType::Enemy) currentPirates++;
        }

        // ====================================================================
        // 3. ASTEROID SPAWNING
        // ====================================================================
        if (m_asteroidSpawnClock.getElapsedTime().asSeconds() > astInterval &&
            currentAsteroids < maxAstCount) {
            sol::table types = (*m_lua)["asteroid_types"];

            // ---- Select asteroid type ----
            const char* typeKeys[] = { "SMALL", "MEDIUM", "LARGE" };
            const char* selectedType = typeKeys[rand() % 3];

            // Check for magmatic asteroid spawn
            float magmaticChance = (*m_lua)["spawn_settings"]["magmatic_chance"].get_or(0.15f);
            bool isMagmatic = ((rand() % 100) / 100.f) < magmaticChance;

            if (isMagmatic && (*m_lua)["asteroid_types"]["MAGMATIC"].valid()) {
                selectedType = "MAGMATIC";
            }

            sol::table config = types[selectedType];

            // ---- Calculate spawn position ----
            float angle = (rand() % 360) * 3.14159f / 180.f;
            sf::Vector2f spawnPos = playerTf.position +
                sf::Vector2f(std::cos(angle) * spawnRadius,
                    std::sin(angle) * spawnRadius);

            // Aim toward random point near player (not directly at player)
            sf::Vector2f offset((rand() % 400) - 200.f, (rand() % 400) - 200.f);
            sf::Vector2f targetPos = playerTf.position + offset;
            sf::Vector2f dir = targetPos - spawnPos;
            float len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));

            // ---- Calculate speed from config ----
            sol::table speedRange = config["speed_range"];
            float speed = speedRange[1].get<float>() +
                (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

            // ---- Create the asteroid ----
            uint32_t asteroidEntityId = m_ef->createAsteroid(*m_em,
                spawnPos,
                (dir / len) * speed,
                config["base_size"],
                config,
                m_worldId);

            // ---- Mark as explosive if magmatic ----
            if (isMagmatic) {
                size_t asteroidIdx = m_em->getEntityIndex(asteroidEntityId);
                if (asteroidIdx != (size_t)-1) {
                    m_em->healths[asteroidIdx].isExplosive = true;
                    m_em->healths[asteroidIdx].explosionRadius =
                        config["explosion_radius"].get_or(150.0f);
                    m_em->healths[asteroidIdx].explosionDamage =
                        config["explosion_damage"].get_or(30.0f);
                }
            }

            // ---- Apply random spin ----
            size_t asteroidIdx = m_em->getEntityIndex(asteroidEntityId);
            if (asteroidIdx != (size_t)-1) {
                float randomSpin = ((rand() % 200) - 100.f) / 50.f;
                b2Body_SetAngularVelocity(m_em->physics[asteroidIdx].bodyId, randomSpin);
            }

            // Reset timer after spawning
            m_asteroidSpawnClock.restart();
        }

        // ====================================================================
        // 4. ENEMY SHIP SPAWNING
        // ====================================================================
        const float pirateInterval = 4.0f;   // Seconds between pirate spawn attempts
        const int maxPirates = 6;            // Maximum concurrent enemy ships

        if (m_pirateSpawnClock.getElapsedTime().asSeconds() > pirateInterval &&
            currentPirates < maxPirates) {
            // ---- Calculate spawn position ----
            float angle = (rand() % 360) * 3.14159f / 180.f;
            float pirateSpawnDist = 1200.f;   // Distance from player to spawn pirates
            sf::Vector2f spawnPos = playerTf.position +
                sf::Vector2f(std::cos(angle) * pirateSpawnDist,
                    std::sin(angle) * pirateSpawnDist);

            // ---- Create the enemy ship ----
            m_ef->createEnemy(*m_em, spawnPos, *m_lua, m_worldId);

            // Reset timer after spawning
            m_pirateSpawnClock.restart();
        }
    }

private:
    // ---- System dependencies (set via init) ----
    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;

    // ---- Spawn timers (persistent state) ----
    sf::Clock m_asteroidSpawnClock;   ///< Timer for asteroid spawning
    sf::Clock m_pirateSpawnClock;     ///< Timer for enemy ship spawning
};