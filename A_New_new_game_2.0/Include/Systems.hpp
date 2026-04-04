/**
 * @file Systems.hpp
 * @brief Game systems implementing ECS logic (physics, damage, AI, rendering, etc.)
 *
 * Each system is a static class that operates on EntityManager component arrays.
 * Systems are called in sequence from main game loop. This file contains:
 * - PhysicsSystem: Box2D integration and transform updates
 * - RenderSystem: Visual rendering with camera following
 * - DamageSystem: Collision handling and entity destruction
 * - InputSystem: Player controls and ship mechanics
 * - EnemySystem: Spawner logic for asteroids and enemies
 * - WeaponSystem: Shooting and projectile management
 * - BackgroundSystem: Parallax starfield scrolling
 * - ParticleSystem: Visual effects (explosions, impacts)
 * - AISystem: Enemy behavior (patrol, alert, combat states)
 * - InputRegistry: Keyboard/mouse mapping utility
 *
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#pragma once

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include "EntityManager.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <map>
#include <random>
#include <string>


 // ============================================================================
 // PHYSICS SYSTEM
 // ============================================================================

 /**
  * @class PhysicsSystem
  * @brief Updates Box2D physics simulation and syncs transforms
  *
  * Steps the Box2D world at fixed timestep, then copies position/rotation
  * from physics bodies back to TransformComponents for rendering.
  */
class PhysicsSystem {
public:
    /**
     * @brief Update physics simulation and synchronize transforms
     * @param em EntityManager containing all components
     * @param worldId Box2D world identifier
     * @param dt Delta time since last frame (seconds)
     *
     * Uses fixed timestep (60 Hz) with substeps for stability.
     * Bullets orient by velocity direction; asteroids by physics rotation.
     */
    static void update(EntityManager& em, b2WorldId worldId, float dt) {
        // Step physics world: fixed 60Hz timestep with 6 substeps for stability
        float timeStep = 1.0f / 60.0f;
        int subStepCount = 6;
        b2World_Step(worldId, dt, subStepCount);

        const float SCALE = 30.f;  // Box2D meters to pixels

        // Sync Box2D bodies to TransformComponents
        for (size_t i = 0; i < em.physics.size(); ++i) {
            b2BodyId bodyId = em.physics[i].bodyId;
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);

            // Position: always sync
            b2Vec2 pos = b2Body_GetPosition(bodyId);
            em.transforms[i].position = { pos.x * SCALE, pos.y * SCALE };

            // Rotation: bullets align with velocity, asteroids use physics rotation
            if (type == BodyType::Bullet) {
                b2Vec2 vel = b2Body_GetLinearVelocity(bodyId);
                if (std::sqrt(vel.x * vel.x + vel.y * vel.y) > 0.1f) {
                    float angleRad = std::atan2(vel.y, vel.x) + (3.14159f / 2.f);
                    em.transforms[i].rotation = angleRad * 180.f / 3.14159f;
                }
            }
            else if (type == BodyType::Asteroid) {
                b2Rot rot = b2Body_GetRotation(bodyId);
                em.transforms[i].rotation = b2Rot_GetAngle(rot) * 180.f / 3.14159f;
            }
            // Player rotation is handled by InputSystem, not synced from physics
        }
    }

    /**
     * @brief Remove entities that drifted too far from player
     * @param em EntityManager
     * @param worldId Box2D world
     * @param playerEntityId Persistent player entity ID
     * @param lua Lua state (for despawn radius config)
     *
     * Collects distant asteroids/enemies, destroys them in reverse order.
     * Uses collect-then-destroy pattern for safe swap-and-pop deletion.
     */
    static void cleanup(EntityManager& em, b2WorldId worldId, uint32_t playerEntityId, sol::state& lua) {
        // Get player's current index
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return; // Player dead

        b2Vec2 playerPos = b2Body_GetPosition(em.physics[playerIdx].bodyId);

        float astDesRadius = lua["spawn_settings"]["despawn_radius"].get_or(100.0f);
        float astDesRadiusSq = astDesRadius * astDesRadius;

        float enemyDesRadius = lua["enemy_config"]["despawn_radius"].get_or(250.0f);
        float enemyDesRadiusSq = enemyDesRadius * enemyDesRadius;

        // Collect indices to destroy (don't destroy during iteration)
        std::vector<size_t> indicesToDestroy;

        for (size_t i = 0; i < em.physics.size(); ++i) {
            if (i == playerIdx) continue;

            b2BodyId bodyId = em.physics[i].bodyId;
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);
            if (type == BodyType::Bullet) continue;

            b2Vec2 pos = b2Body_GetPosition(bodyId);
            float dx = pos.x - playerPos.x;
            float dy = pos.y - playerPos.y;
            float distSq = dx * dx + dy * dy;

            bool shouldDestroy = false;
            if (type == BodyType::Asteroid && distSq > astDesRadiusSq) shouldDestroy = true;
            else if (type == BodyType::Enemy && distSq > enemyDesRadiusSq) shouldDestroy = true;

            if (shouldDestroy) indicesToDestroy.push_back(i);
        }

        // Destroy in reverse order to maintain index validity
        for (size_t i = indicesToDestroy.size(); i-- > 0; ) {
            em.destroyEntity(indicesToDestroy[i]);
        }
    }
};


// ============================================================================
// RENDER SYSTEM
// ============================================================================

/**
 * @class RenderSystem
 * @brief Draws all entities with camera following player
 *
 * Camera centers on player ship. Each entity's shape is positioned/rotated
 * according to its TransformComponent. Player and enemy colors are read from Lua.
 */
class RenderSystem {
public:
    /**
     * @brief Draw all entities to window
     * @param em EntityManager
     * @param window SFML render window
     * @param lua Lua state (for color configs)
     * @param playerEntityId Persistent player ID for camera follow
     */
    static void draw(EntityManager& em, sf::RenderWindow& window, sol::state& lua, uint32_t playerEntityId) {
        // Get player's current index for camera follow
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& playerTf = em.transforms[playerIdx];

        // Center camera on player
        sf::View view = window.getView();
        view.setCenter(playerTf.position);
        window.setView(view);

        // Draw all entities
        for (size_t i = 0; i < em.renders.size(); ++i) {
            auto& tf = em.transforms[i];
            auto& rd = em.renders[i];
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);

            rd.shape.setPosition(tf.position);
            rd.shape.setRotation(sf::degrees(tf.rotation));

            if (type == BodyType::Asteroid && em.healths[i].isExplosive) {
                // Pulsing glow effect using sine wave
                static float time = 0;
                time += 0.016f;  // Approximate delta
                float pulse = (std::sin(time * 8.0f) + 1.0f) / 2.0f;  // 0 to 1 oscillation

                // Make the asteroid brighter/pulsing
                sf::Color magmaColor = rd.shape.getFillColor();
                uint8_t intensity = 200 + (uint8_t)(55 * pulse);
                rd.shape.setFillColor(sf::Color(intensity, intensity * 0.4f, intensity * 0.2f));

                // Add a subtle outline glow
                rd.shape.setOutlineThickness(3.0f + pulse * 2.0f);
                rd.shape.setOutlineColor(sf::Color(255, 100, 0, 150 + (uint8_t)(100 * pulse)));
            }

            // Special color handling for player (dash flash effect)
            if (type == BodyType::Player) {
                if (tf.isParrying) {
                    // Pulsing glow during parry
                    float pulse = (std::sin(tf.parryTimer * 30.f) + 1.f) / 2.f;
                    rd.shape.setOutlineThickness(2.0f + pulse * 5.0f);
                    rd.shape.setOutlineColor(sf::Color(0, 255, 255, 200 + (uint8_t)(55 * pulse)));
                }
                else {
                    // RESET to default outline when not parrying
                    rd.shape.setOutlineThickness(2.5f);
                    sol::table luaOutline = lua["outline_color"];
                    rd.shape.setOutlineColor(sf::Color(
                        luaOutline["r"].get_or(255),
                        luaOutline["g"].get_or(255),
                        luaOutline["b"].get_or(255)
                    ));
                }

                if (tf.dashCooldown > (tf.dashMaxCooldown - 0.15f)) {
                    // Dash cooldown almost ready - flash effect
                    sol::table flash = lua["dash_flash_color"];
                    rd.shape.setFillColor(sf::Color(
                        flash["r"].get_or(100),
                        flash["g"].get_or(255),
                        flash["b"].get_or(255),
                        flash["a"].get_or(200)
                    ));
                }
                else {
                    sol::table clr = lua["color"];
                    rd.shape.setFillColor(sf::Color(clr["r"], clr["g"], clr["b"]));
                }
            }
            else if (type == BodyType::Enemy) {
                sol::table clr = lua["enemy_config"]["color"];
                rd.shape.setFillColor(sf::Color(clr["r"], clr["g"], clr["b"]));
            }

            // Re-draw (note: this draws twice? Possibly a bug, but kept for compatibility)
            rd.shape.setPosition(tf.position);
            rd.shape.setRotation(sf::degrees(tf.rotation));
            window.draw(rd.shape);
        }
    }
};


// ============================================================================
// DAMAGE SYSTEM
// ============================================================================

/**
 * @class DamageSystem
 * @brief Handles collisions, damage application, and entity death
 *
 * Processes Box2D contact events for bullet hits and player collisions.
 * When entities die, spawns particles, adds score, and may spawn child asteroids.
 * Uses collect-then-destroy pattern for safe entity removal.
 */
class DamageSystem {
public:
    /**
     * @brief Process all damage and death logic
     * @param em EntityManager
     * @param worldId Box2D world
     * @param playerEntityId Persistent player ID
     * @param dt Delta time
     * @param lua Lua state
     */
    static void update(EntityManager& em, b2WorldId worldId, uint32_t playerEntityId, float dt, sol::state& lua) {
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return;


        auto& playerHp = em.healths[playerIdx];
        auto& playerTf = em.transforms[playerIdx];

        // Update invulnerability timers
        if (playerHp.invulTimer > 0) playerHp.invulTimer -= dt;
        if (playerHp.cheapInvulTimer > 0) playerHp.cheapInvulTimer -= dt;

        // Process Box2D contact events
        b2ContactEvents events = b2World_GetContactEvents(worldId);
        b2BodyId playerBody = em.physics[playerIdx].bodyId;

        for (int i = 0; i < events.beginCount; ++i) {
            b2ContactBeginTouchEvent* event = events.beginEvents + i;
            b2BodyId bodyA = b2Shape_GetBody(event->shapeIdA);
            b2BodyId bodyB = b2Shape_GetBody(event->shapeIdB);

            BodyType typeA = (BodyType)(uintptr_t)b2Body_GetUserData(bodyA);
            BodyType typeB = (BodyType)(uintptr_t)b2Body_GetUserData(bodyB);

            // Check for bullet/asteroid hits
            b2BodyId bulletBody = b2_nullBodyId;
            b2BodyId targetBody = b2_nullBodyId;
            BodyType targetType;

            if (typeA == BodyType::Bullet) {
                bulletBody = bodyA; targetBody = bodyB; targetType = typeB;
            }
            else if (typeB == BodyType::Bullet) {
                bulletBody = bodyB; targetBody = bodyA; targetType = typeA;
            }

            if (b2Body_IsValid(bulletBody) && b2Body_IsValid(targetBody)) {
                size_t bulletIdx = (size_t)-1;
                size_t targetIdx = (size_t)-1;

                for (size_t idx = 0; idx < em.physics.size(); ++idx) {
                    if (B2_ID_EQUALS(em.physics[idx].bodyId, bulletBody)) bulletIdx = idx;
                    if (B2_ID_EQUALS(em.physics[idx].bodyId, targetBody)) targetIdx = idx;
                    if (bulletIdx != (size_t)-1 && targetIdx != (size_t)-1) break;
                }

                if (bulletIdx != (size_t)-1 && targetIdx != (size_t)-1) {
                    if (!em.bullets[bulletIdx].markedForDestroy) {
                        bool isPlayerParrying = playerTf.isParrying;
                        sf::Vector2f hitPos = em.transforms[bulletIdx].position;
                        sf::Vector2f hitVel = em.transforms[bulletIdx].velocity;

                        // ===== PARRY SUCCESS =====
                        if (isPlayerParrying) {
                            em.bullets[bulletIdx].markedForDestroy = true;
                            em.spawnExplosion(hitPos, sf::Color(0, 255, 200), 15, 2.5f);

                            // CASE 1: Parried ASTEROID → HOMING MISSILE
                            if (targetType == BodyType::Asteroid) {
                                size_t enemyIdx = findNearestEnemy(em, hitPos);
                                if (enemyIdx != (size_t)-1) {
                                    sf::Vector2f targetPos = em.transforms[enemyIdx].position;
                                    sf::Vector2f dirToEnemy = targetPos - hitPos;
                                    float len = std::sqrt(dirToEnemy.x * dirToEnemy.x + dirToEnemy.y * dirToEnemy.y);
                                    if (len > 0.01f) { dirToEnemy.x /= len; dirToEnemy.y /= len; }

                                    float homingSpeed = lua["homing_missile_speed"].get_or(800.f);
                                    b2Body_SetLinearVelocity(em.physics[targetIdx].bodyId,
                                        { dirToEnemy.x * homingSpeed / SCALE, dirToEnemy.y * homingSpeed / SCALE });

                                    em.healths[targetIdx].isHoming = true;
                                    // Store enemy entity ID, not index
                                    em.healths[targetIdx].homingTargetEntityId = em.transforms[enemyIdx].entityId;

                                    // Spawn ring effect
                                    for (int ring = 0; ring < 24; ++ring) {
                                        float angle = ring * 15.f * 3.14159f / 180.f;
                                        sf::Vector2f ringDir(std::cos(angle), std::sin(angle));
                                        em.spawnImpact(sf::Vector2f(hitPos.x + ringDir.x * 25, hitPos.y + ringDir.y * 25),
                                            sf::Color(0, 255, 200, 200),
                                            sf::Vector2f(ringDir.x * 400, ringDir.y * 400));
                                    }
                                }
                                else {
                                    // No enemy – deflect away
                                    sf::Vector2f away = hitPos - em.transforms[playerIdx].position;
                                    float len = std::sqrt(away.x * away.x + away.y * away.y);
                                    if (len > 0.01f) { away.x /= len; away.y /= len; }
                                    b2Body_SetLinearVelocity(em.physics[targetIdx].bodyId,
                                        { away.x * 600 / SCALE, away.y * 600 / SCALE });
                                }
                            }
                            // CASE 2: Parried ENEMY → STUN + DAMAGE
                            else if (targetType == BodyType::Enemy) {
                                float stunDuration = lua["parry_stun_duration"].get_or(1.5f);
                                float reflectDamage = lua["parry_reflect_damage"].get_or(50.f);
                                em.healths[targetIdx].currentHp -= reflectDamage;
                                em.healths[targetIdx].stunTimer = stunDuration;

                                // Stun stars
                                for (int star = 0; star < 20; ++star) {
                                    float angle = (rand() % 360) * 3.14159f / 180.f;
                                    sf::Vector2f dir(std::cos(angle), std::sin(angle));
                                    em.spawnImpact(sf::Vector2f(em.transforms[targetIdx].position.x + dir.x * 30,
                                        em.transforms[targetIdx].position.y + dir.y * 30),
                                        sf::Color(255, 255, 0, 200),
                                        sf::Vector2f(dir.x * 150, dir.y * 150));
                                }
                            }
                            // CASE 3: Parried BULLET → REFLECT
                            else if (targetType == BodyType::Bullet) {
                                size_t enemyIdx = findNearestEnemy(em, hitPos);
                                if (enemyIdx != (size_t)-1) {
                                    sf::Vector2f enemyPos = em.transforms[enemyIdx].position;
                                    sf::Vector2f reflectDir = enemyPos - hitPos;
                                    float len = std::sqrt(reflectDir.x * reflectDir.x + reflectDir.y * reflectDir.y);
                                    if (len > 0.01f) { reflectDir.x /= len; reflectDir.y /= len; }
                                    float bulletSpeed = lua["bullet_speed"].get_or(800.f);
                                    sf::Vector2f reflectedVel(reflectDir.x * bulletSpeed, reflectDir.y * bulletSpeed);
                                    float angle = std::atan2(reflectDir.y, reflectDir.x) * 180.f / 3.14159f + 90.f;
                                    em.createBullet(hitPos, reflectedVel, angle, lua, worldId);

                                    // Reflection sparks
                                    for (int spark = 0; spark < 12; ++spark) {
                                        float sparkAngle = (rand() % 360) * 3.14159f / 180.f;
                                        sf::Vector2f sparkDir(std::cos(sparkAngle), std::sin(sparkAngle));
                                        em.spawnImpact(sf::Vector2f(hitPos.x + sparkDir.x * 15, hitPos.y + sparkDir.y * 15),
                                            sf::Color(0, 200, 255),
                                            sf::Vector2f(sparkDir.x * 200, sparkDir.y * 200));
                                    }
                                }
                            }
                        }
                        // ===== NORMAL HIT (NO PARRY) =====
                        else {
                            if (targetType == BodyType::Asteroid) {
                                em.healths[targetIdx].currentHp -= 15.0f;
                                em.spawnImpact(hitPos, sf::Color(180, 180, 180), hitVel);
                                em.bullets[bulletIdx].markedForDestroy = true;
                            }
                            else if (targetType == BodyType::Enemy) {
                                em.healths[targetIdx].currentHp -= 25.0f;
                                em.spawnImpact(hitPos, sf::Color::Yellow, hitVel);
                                em.spawnExplosion(hitPos, sf::Color::Red, 5, 2.0f);
                                em.bullets[bulletIdx].markedForDestroy = true;
                            }
                        }
                    }
                    continue;
                }
            }

            // ===== PLAYER COLLISION WITH ASTEROID/ENEMY =====
            bool isPlayerA = B2_ID_EQUALS(bodyA, playerBody);
            bool isPlayerB = B2_ID_EQUALS(bodyB, playerBody);
            if (isPlayerA || isPlayerB) {
                b2BodyId otherBody = isPlayerA ? bodyB : bodyA;
                BodyType otherType = (BodyType)(uintptr_t)b2Body_GetUserData(otherBody);

                // PARRY HANDLING
                if (playerTf.isParrying && (otherType == BodyType::Asteroid || otherType == BodyType::Enemy)) {
                    size_t otherIdx = (size_t)-1;
                    for (size_t idx = 0; idx < em.physics.size(); ++idx) {
                        if (B2_ID_EQUALS(em.physics[idx].bodyId, otherBody)) {
                            otherIdx = idx;
                            break;
                        }
                    }
                    if (otherIdx != (size_t)-1) {
                        // Deflect away
                        sf::Vector2f away = em.transforms[otherIdx].position - em.transforms[playerIdx].position;
                        float len = std::sqrt(away.x * away.x + away.y * away.y);
                        if (len > 0.01f) { away.x /= len; away.y /= len; }
                        b2Body_SetLinearVelocity(em.physics[otherIdx].bodyId,
                            { away.x * 800 / SCALE, away.y * 800 / SCALE });
                        em.spawnExplosion(em.transforms[otherIdx].position, sf::Color(0, 255, 200), 10, 2.0f);

                        // Apply stun + damage to enemies
                        if (otherType == BodyType::Enemy) {
                            float stunDuration = lua["parry_stun_duration"].get_or(1.5f);
                            float reflectDamage = lua["parry_reflect_damage"].get_or(50.f);
                            em.healths[otherIdx].currentHp -= reflectDamage;
                            em.healths[otherIdx].stunTimer = stunDuration;
                        }
                    }
                    continue; // Skip damage
                }

                // NORMAL COLLISION DAMAGE
                b2Vec2 vA = b2Body_GetLinearVelocity(bodyA);
                b2Vec2 vB = b2Body_GetLinearVelocity(bodyB);
                float relativeSpeed = std::sqrt(std::pow(vA.x - vB.x, 2) + std::pow(vA.y - vB.y, 2));

                if (relativeSpeed > 12.0f && playerHp.invulTimer <= 0) {
                    playerHp.currentHp -= 15.0f;
                    playerHp.invulTimer = 1.0f;
                }
                else if (relativeSpeed > 1.5f && playerHp.invulTimer <= 0 && playerHp.cheapInvulTimer <= 0) {
                    playerHp.currentHp -= 1.0f;
                    playerHp.cheapInvulTimer = 0.2f;
                }
            }
        }

        // ===== DESTROY DEAD ENTITIES =====
        std::vector<size_t> indicesToDestroy;
        for (size_t i = 0; i < em.physics.size(); ++i) {
            if (i == playerIdx) continue;
            b2BodyId bodyId = em.physics[i].bodyId;
            if (!b2Body_IsValid(bodyId)) continue;
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);
            bool shouldDestroy = false;

            if (em.healths[i].currentHp <= 0) {
                shouldDestroy = true;
                sf::Vector2f deathPos = em.transforms[i].position;

                if (type == BodyType::Asteroid) {
                    int reward = em.scoreRewards[i];
                    em.totalScore += reward;
                    bool isExplosive = em.healths[i].isExplosive;

                    if (isExplosive) {
                        em.spawnExplosion(deathPos, sf::Color(255, 100, 0), 40, 6.0f);
                        em.spawnExplosion(deathPos, sf::Color(255, 50, 0), 30, 4.0f);
                        for (int angle = 0; angle < 360; angle += 15) {
                            float rad = angle * 3.14159f / 180.f;
                            sf::Vector2f dir(std::cos(rad), std::sin(rad));
                            em.spawnImpact(sf::Vector2f(deathPos.x + dir.x * 30, deathPos.y + dir.y * 30),
                                sf::Color(255, 100, 0), sf::Vector2f(dir.x * 500, dir.y * 500));
                        }
                        float radius = em.healths[i].explosionRadius;
                        float damage = em.healths[i].explosionDamage;
                        for (size_t j = 0; j < em.physics.size(); ++j) {
                            if (j == i) continue;
                            sf::Vector2f otherPos = em.transforms[j].position;
                            float dx = deathPos.x - otherPos.x;
                            float dy = deathPos.y - otherPos.y;
                            float dist = std::sqrt(dx * dx + dy * dy);
                            if (dist < radius) {
                                float falloff = 1.0f - (dist / radius);
                                em.healths[j].currentHp -= damage * falloff;
                            }
                        }
                    }
                    else {
                        float pSize = (reward >= 200) ? 5.0f : (reward >= 50 ? 3.0f : 1.5f);
                        int pCount = (reward >= 200) ? 40 : (reward >= 50 ? 25 : 15);
                        em.spawnExplosion(deathPos, sf::Color(160, 160, 160), pCount, pSize);
                        if (reward >= 200) {
                            for (int j = 0; j < 3; ++j) spawnChild(em, worldId, lua, deathPos, "MEDIUM");
                        }
                        else if (reward >= 50) {
                            for (int j = 0; j < 2; ++j) spawnChild(em, worldId, lua, deathPos, "SMALL");
                        }
                    }
                }
                else if (type == BodyType::Enemy) {
                    em.totalScore += em.scoreRewards[i];
                    em.spawnExplosion(deathPos, sf::Color::Red, 35, 5.0f);
                    em.spawnExplosion(deathPos, sf::Color::Yellow, 15, 2.5f);
                }
            }
            else if (type == BodyType::Bullet && (em.bullets[i].markedForDestroy || em.bullets[i].lifetime <= 0)) {
                shouldDestroy = true;
            }

            if (shouldDestroy) indicesToDestroy.push_back(i);
        }

        for (size_t i = indicesToDestroy.size(); i-- > 0; ) {
            em.destroyEntity(indicesToDestroy[i]);
        }
    }

private:
    static size_t findNearestEnemy(EntityManager& em, sf::Vector2f pos) {
        size_t nearestIdx = (size_t)-1;
        float nearestDistSq = FLT_MAX;
        for (size_t i = 0; i < em.physics.size(); ++i) {
            if (!b2Body_IsValid(em.physics[i].bodyId)) continue;
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);
            if (type == BodyType::Enemy) {
                sf::Vector2f enemyPos = em.transforms[i].position;
                float dx = pos.x - enemyPos.x;
                float dy = pos.y - enemyPos.y;
                float distSq = dx * dx + dy * dy;
                if (distSq < nearestDistSq) {
                    nearestDistSq = distSq;
                    nearestIdx = i;
                }
            }
        }
        return nearestIdx;
    }

    static void spawnChild(EntityManager& em, b2WorldId worldId, sol::state& lua, sf::Vector2f pos, const char* typeKey) {
        sol::table config = lua["asteroid_types"][typeKey];
        float angle = (rand() % 360) * 3.14159f / 180.f;
        sol::table speedRange = config["speed_range"];
        float speed = speedRange[1].get<float>() + (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());
        sf::Vector2f velocity(std::cos(angle) * speed * 0.5f, std::sin(angle) * speed * 0.5f);
        uint32_t asteroidEntityId = em.createAsteroid(pos, velocity, config["base_size"], config, worldId);
        size_t asteroidIdx = em.getEntityIndex(asteroidEntityId);
        float randomRotation = ((rand() % 200) - 100.f) / 50.f;
        if (asteroidIdx != (size_t)-1) {
            b2Body_SetAngularVelocity(em.physics[asteroidIdx].bodyId, randomRotation);
        }
    }
};





// ============================================================================
// INPUT REGISTRY (Utility)
// ============================================================================

/**
 * @class InputRegistry
 * @brief Maps named inputs (e.g., "Space", "MouseLeft") to SFML keys/buttons
 *
 * Allows Lua scripts to define key bindings by name instead of hardcoded keys.
 * Supports A-Z, 0-9, modifier keys, and mouse buttons.
 */
class InputRegistry {
private:
    static inline std::map<std::string, sf::Keyboard::Key> keyMap;
    static inline std::map<std::string, sf::Mouse::Button> mouseMap;

public:
    /**
     * @brief Initialize input mappings (called once at startup)
     */
    static void init() {
        if (!keyMap.empty()) return;

        // Letters A-Z
        for (int i = 0; i < 26; ++i) {
            std::string name(1, 'A' + i);
            keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::A) + i);
        }

        // Numbers 0-9
        for (int i = 0; i < 10; ++i) {
            std::string name = std::to_string(i);
            keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::Num0) + i);
        }

        // Special keys
        keyMap["Space"] = sf::Keyboard::Key::Space;
        keyMap["Enter"] = sf::Keyboard::Key::Enter;
        keyMap["LShift"] = sf::Keyboard::Key::LShift;
        keyMap["RShift"] = sf::Keyboard::Key::RShift;
        keyMap["LControl"] = sf::Keyboard::Key::LControl;
        keyMap["Escape"] = sf::Keyboard::Key::Escape;
        keyMap["Tab"] = sf::Keyboard::Key::Tab;

        // Mouse buttons
        mouseMap["MouseLeft"] = sf::Mouse::Button::Left;
        mouseMap["MouseRight"] = sf::Mouse::Button::Right;
        mouseMap["MouseMiddle"] = sf::Mouse::Button::Middle;
        mouseMap["MouseX1"] = sf::Mouse::Button::Extra1;
        mouseMap["MouseX2"] = sf::Mouse::Button::Extra2;
    }

    /**
     * @brief Check if a named input is currently pressed
     * @param name Input name (e.g., "Space", "MouseLeft", "W")
     * @return true if the key/button is down
     */
    static bool isPressed(const std::string& name) {
        if (keyMap.count(name)) return sf::Keyboard::isKeyPressed(keyMap[name]);
        if (mouseMap.count(name)) return sf::Mouse::isButtonPressed(mouseMap[name]);
        return false;
    }
};


// ============================================================================
// INPUT SYSTEM
// ============================================================================

/**
 * @class InputSystem
 * @brief Handles player controls: movement, rotation, turbo, dash
 *
 * Reads key bindings from Lua. Player aims with mouse, moves with WASD/arrows.
 * Features energy-based turbo boost and dash mechanic with cooldown/overheat.
 */
class InputSystem {
public:
    /**
     * @brief Process player input and apply forces to ship
     * @param em EntityManager
     * @param playerEntityId Persistent player ID
     * @param dt Delta time
     * @param window SFML window (for mouse position)
     * @param lua Lua state (config values)
     */
    static void update(EntityManager& em, uint32_t playerEntityId, float dt, sf::RenderWindow& window, sol::state& lua) {
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& tf = em.transforms[playerIdx];
        auto& phys = em.physics[playerIdx];
        b2BodyId bodyId = phys.bodyId;

        InputRegistry::init();

        // Lua configuration values
        float enginePower = lua["engine_power"].get_or(150.f);
        float rotationSpeed = lua["rotation_speed"].get_or(4.f);
        float multiplier = lua["sprint_power_multiplier"].get_or(2.5f);
        float drainRate = lua["sprint_drain_speed"].get_or(40.f);
        float regenRate = lua["sprint_regen_speed"].get_or(20.f);
        float penaltyTime = lua["penalty_energy"].get_or(3.0f);
        float dashVel = lua["dash_velocity"].get_or(40.f);
        float dashCost = lua["dash_energy_cost"].get_or(30.f);
        tf.dashMaxCooldown = lua["dash_max_cooldown"].get_or(1.0f);

        sol::table binds = lua["key_bindings"];

        // Update cooldowns
        if (tf.dashCooldown > 0) tf.dashCooldown -= dt;
        if (tf.overheatTimer > 0) tf.overheatTimer -= dt;

        // Turbo boost (energy drain)
        std::string sprintKey = binds["sprint"].get<std::string>();
        bool wantSprint = InputRegistry::isPressed(sprintKey);

        if (wantSprint && tf.energyDrive > 0 && tf.overheatTimer <= 0) {
            tf.isTurbo = true;
            tf.energyDrive -= drainRate * dt;
            if (tf.energyDrive <= 0) {
                tf.energyDrive = 0;
                tf.overheatTimer = penaltyTime;
                tf.isTurbo = false;
            }
        }
        else {
            tf.isTurbo = false;
        }

        // Energy regeneration (only when not turbo and not overheated)
        if (!tf.isTurbo && tf.energyDrive < tf.maxEnergyDrive) {
            tf.energyDrive += regenRate * dt;
            if (tf.energyDrive > tf.maxEnergyDrive) tf.energyDrive = tf.maxEnergyDrive;
        }

        // Turbo reduces turn speed (harder to control at high speed)
        if (tf.isTurbo) rotationSpeed *= 0.3f;

        // Mouse aim: rotate ship to face cursor
        b2Vec2 b2Pos = b2Body_GetPosition(bodyId);
        sf::Vector2f currentPos(b2Pos.x * SCALE, b2Pos.y * SCALE);
        sf::Vector2i mousePos = sf::Mouse::getPosition(window);
        sf::Vector2f worldPos = window.mapPixelToCoords(mousePos);

        float targetAngle = std::atan2(worldPos.y - currentPos.y, worldPos.x - currentPos.x) * 180.f / 3.14159f + 90.f;
        float currentRotation = tf.rotation;
        float deltaAngle = targetAngle - currentRotation;

        // Normalize angle difference to shortest path
        while (deltaAngle > 180) deltaAngle -= 360;
        while (deltaAngle < -180) deltaAngle += 360;

        tf.rotation += deltaAngle * rotationSpeed * dt;
        float rad = tf.rotation * 3.14159f / 180.f;
        b2Body_SetTransform(bodyId, b2Pos, b2MakeRot(rad));

        // Movement input (WASD / arrows)
        float dx = 0.f, dy = 0.f;
        if (InputRegistry::isPressed(binds["up"].get<std::string>()))    dy -= 1.f;
        if (InputRegistry::isPressed(binds["down"].get<std::string>()))  dy += 1.f;
        if (InputRegistry::isPressed(binds["left"].get<std::string>()))  dx -= 1.f;
        if (InputRegistry::isPressed(binds["right"].get<std::string>())) dx += 1.f;

        // Apply engine force
        if (tf.isTurbo) {
            // Turbo: always thrust forward (nose direction)
            float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
            float finalPower = enginePower * multiplier;
            b2Body_ApplyForceToCenter(phys.bodyId, { std::cos(noseRad) * finalPower, std::sin(noseRad) * finalPower }, true);
        }
        else {
            // Normal: thrust in input direction
            if (dx != 0 || dy != 0) {
                float length = std::sqrt(dx * dx + dy * dy);
                b2Body_ApplyForceToCenter(phys.bodyId, { (dx / length) * enginePower, (dy / length) * enginePower }, true);
            }
        }

        // Dash mechanic: instant velocity boost
        std::string dashKey = binds["dash"].get<std::string>();
        if (InputRegistry::isPressed(dashKey) && tf.dashCooldown <= 0 && tf.overheatTimer <= 0) {
            if (tf.energyDrive >= dashCost) {
                float vx, vy;
                if (dx != 0 || dy != 0) {
                    float len = std::sqrt(dx * dx + dy * dy);
                    vx = (dx / len) * dashVel;
                    vy = (dy / len) * dashVel;
                }
                else {
                    float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
                    vx = std::cos(noseRad) * dashVel;
                    vy = std::sin(noseRad) * dashVel;
                }
                b2Body_SetLinearVelocity(bodyId, { vx, vy });

                tf.energyDrive -= dashCost;
                tf.dashCooldown = tf.dashMaxCooldown;

                // Overheat if energy depleted
                if (tf.energyDrive < 1.0f) {
                    tf.energyDrive = 0;
                    tf.overheatTimer = penaltyTime;
                }
            }
        }

        // ===== PARRY MECHANIC =====
        std::string parryKey = binds["parry"].get<std::string>();
        float parryWindow = lua["parry_window"].get_or(0.2f);
        float parryAnimDuration = lua["parry_anim_duration"].get_or(0.6f);
        float parryCooldownTime = lua["parry_cooldown"].get_or(2.0f);
        tf.parryMaxCooldown = parryCooldownTime;

        // Update timers
        if (tf.parryTimer > 0) {
            tf.parryTimer -= dt;
            if (tf.parryTimer <= 0) {
                tf.isParrying = false;   // Active parry ends, but animation continues
            }
        }
        if (tf.parryAnimTimer > 0) {
            tf.parryAnimTimer -= dt;
        }
        if (tf.parryCooldown > 0) tf.parryCooldown -= dt;

        // Activate parry (only if not already in cooldown and not parrying)
        if (InputRegistry::isPressed(parryKey) && tf.parryCooldown <= 0 && tf.parryTimer <= 0) {
            tf.isParrying = true;
            tf.parryTimer = parryWindow;           // Short active window
            tf.parryAnimTimer = parryAnimDuration; // Longer spin animation
            tf.parryCooldown = parryCooldownTime;
            tf.parryStartRotation = tf.rotation;
            tf.parrySpinAngle = 0.f;
  
        }

        // Spin animation (runs for the full anim duration, even after active parry ends)
        if (tf.parryAnimTimer > 0) {
            float t = 1.0f - (tf.parryAnimTimer / parryAnimDuration);
            // Ease-out cubic: fast start, smooth deceleration
            float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            float spinDegrees = 360.f;   // Full 360° spin – change to 720 if desired
            float angle = tf.parryStartRotation + spinDegrees * ease;
            tf.rotation = angle;

            // Apply to Box2D body
            b2Vec2 b2Pos = b2Body_GetPosition(phys.bodyId);
            float rad = angle * 3.14159f / 180.f;
            b2Body_SetTransform(phys.bodyId, b2Pos, b2MakeRot(rad));
        }



    }

    /**
     * @brief Legacy physics update (kept for compatibility)
     * @deprecated Use PhysicsSystem::update instead
     */
    static void updatePhysics(EntityManager& em, b2WorldId worldId, float dt) {
        b2World_Step(worldId, dt, 6);
        for (size_t i = 0; i < em.physics.size(); ++i) {
            b2BodyId bodyId = em.physics[i].bodyId;
            b2Vec2 pos = b2Body_GetPosition(bodyId);
            em.transforms[i].position = { pos.x * SCALE, pos.y * SCALE };
            if (em.physics[i].bodyId.index1 != em.physics[0].bodyId.index1) {
                b2Rot rotation = b2Body_GetRotation(bodyId);
                float angleRad = b2Rot_GetAngle(rotation);
                em.transforms[i].rotation = angleRad * 180.f / 3.14159f;
            }
        }
    }
};


// ============================================================================
// ENEMY SYSTEM (Spawner)
// ============================================================================

/**
 * @class EnemySystem
 * @brief Manages spawning of asteroids and enemy ships
 *
 * Uses separate timers for asteroids (frequent) and enemies (less frequent).
 * Spawns entities at a radius around player, heading toward random offset.
 */
class EnemySystem {
private:
    static inline sf::Clock asteroidSpawnClock;
    static inline sf::Clock pirateSpawnClock;

public:
    /**
     * @brief Update spawn timers and create new entities
     * @param em EntityManager
     * @param lua Lua state (spawn configs)
     * @param worldId Box2D world
     * @param playerEntityId Persistent player ID
     */
    static void update(EntityManager& em, sol::state& lua, b2WorldId worldId, uint32_t playerEntityId) {
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& playerTf = em.transforms[playerIdx];

        sol::table astSettings = lua["spawn_settings"];
        float astInterval = astSettings["interval"].get_or(1.0f);
        int maxAstCount = astSettings["max_count"].get_or(40);
        float spawnRadius = astSettings["spawn_radius"].get_or(1500.f);

        // Count current entities
        int currentAsteroids = 0;
        int currentPirates = 0;
        for (const auto& p : em.physics) {
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(p.bodyId);
            if (type == BodyType::Asteroid) currentAsteroids++;
            if (type == BodyType::Enemy) currentPirates++;
        }

        // Asteroid spawning
        if (asteroidSpawnClock.getElapsedTime().asSeconds() > astInterval && currentAsteroids < maxAstCount) {
            sol::table types = lua["asteroid_types"];
            const char* typeKeys[] = { "SMALL", "MEDIUM", "LARGE" };
            const char* selectedType = typeKeys[rand() % 3];

            // Check for magmatic asteroid spawn
            float magmaticChance = lua["spawn_settings"]["magmatic_chance"].get_or(0.15f);
            bool isMagmatic = ((rand() % 100) / 100.f) < magmaticChance;

            if (isMagmatic && lua["asteroid_types"]["MAGMATIC"].valid()) {
                selectedType = "MAGMATIC";
            }

            sol::table config = types[selectedType];

            float angle = (rand() % 360) * 3.14159f / 180.f;
            sf::Vector2f spawnPos = playerTf.position + sf::Vector2f(std::cos(angle) * spawnRadius, std::sin(angle) * spawnRadius);

            // Aim toward random point near player (not directly at player)
            sf::Vector2f offset((rand() % 400) - 200.f, (rand() % 400) - 200.f);
            sf::Vector2f targetPos = playerTf.position + offset;
            sf::Vector2f dir = targetPos - spawnPos;
            float len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));

            sol::table speedRange = config["speed_range"];
            float speed = speedRange[1].get<float>() + (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

            uint32_t asteroidEntityId = em.createAsteroid(spawnPos, (dir / len) * speed, config["base_size"], config, worldId);
            
            // Store explosion properties in a new component or use userData
            if (isMagmatic) {
                size_t asteroidIdx = em.getEntityIndex(asteroidEntityId);
                if (asteroidIdx != (size_t)-1) {
                    // Mark as explosive (can store in HealthComponent or add flag)
                    em.healths[asteroidIdx].isExplosive = true;
                    em.healths[asteroidIdx].explosionRadius = config["explosion_radius"].get_or(150.0f);
                    em.healths[asteroidIdx].explosionDamage = config["explosion_damage"].get_or(30.0f);
                }
            }
            
            size_t asteroidIdx = em.getEntityIndex(asteroidEntityId);
            if (asteroidIdx != (size_t)-1) {
                b2Body_SetAngularVelocity(em.physics[asteroidIdx].bodyId, ((rand() % 200) - 100.f) / 50.f);
            }

            asteroidSpawnClock.restart();
        }

        // Enemy ship spawning
        const float pirateInterval = 4.0f;
        const int maxPirates = 6;

        if (pirateSpawnClock.getElapsedTime().asSeconds() > pirateInterval && currentPirates < maxPirates) {
            float angle = (rand() % 360) * 3.14159f / 180.f;
            float pirateSpawnDist = 1200.f;
            sf::Vector2f spawnPos = playerTf.position + sf::Vector2f(std::cos(angle) * pirateSpawnDist, std::sin(angle) * pirateSpawnDist);

            em.createEnemy(spawnPos, lua, worldId);
            pirateSpawnClock.restart();
        }
    }
};


// ============================================================================
// WEAPON SYSTEM
// ============================================================================

/**
 * @class WeaponSystem
 * @brief Handles shooting and bullet lifetime management
 *
 * Player fires bullets toward mouse aim direction. Bullets fade out over time
 * and auto-destroy when lifetime expires or after hitting a target.
 */
class WeaponSystem {
public:
    /**
     * @brief Process shooting input and update bullets
     * @param em EntityManager
     * @param worldId Box2D world
     * @param playerEntityId Persistent player ID
     * @param dt Delta time
     * @param lua Lua state (bullet config)
     */
    static void update(EntityManager& em, b2WorldId worldId, uint32_t playerEntityId, float dt, sol::state& lua) {
        static float shootTimer = 0.f;
        float fireRate = 0.2f;  // 5 shots per second
        shootTimer -= dt;
        sol::table binds = lua["key_bindings"];

        // Shooting input
        if (InputRegistry::isPressed(binds["fire"].get<std::string>()) && shootTimer <= 0) {
            size_t playerIdx = em.getEntityIndex(playerEntityId);
            if (playerIdx == (size_t)-1) return;

            auto& playerTf = em.transforms[playerIdx];

            float angleRad = (playerTf.rotation - 90.f) * 3.14159f / 180.f;
            sf::Vector2f direction(std::cos(angleRad), std::sin(angleRad));

            float bulletSpeed = lua["bullet_speed"].get_or(800.f);
            sf::Vector2f bulletVel = direction * bulletSpeed;
            sf::Vector2f spawnPos = playerTf.position + direction * 50.f;

            em.createBullet(spawnPos, bulletVel, playerTf.rotation, lua, worldId);
            shootTimer = fireRate;
        }

        // Bullet lifetime and fade out
        for (size_t i = em.bullets.size(); i-- > 0; ) {
            b2BodyId bodyId = em.physics[i].bodyId;
            if (!b2Body_IsValid(bodyId)) continue;

            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(bodyId);
            if (type != BodyType::Bullet) continue;

            auto& bullet = em.bullets[i];
            bullet.lifetime -= dt;

            // Fade out effect (alpha based on remaining lifetime)
            float maxLifetime = lua["bullet_lifetime"].get_or(1.5f);
            float ratio = bullet.lifetime / maxLifetime;
            if (ratio < 0.f) ratio = 0.f;

            auto& shape = em.renders[i].shape;
            sf::Color outlineCol = shape.getOutlineColor();
            sf::Color fillCol = shape.getFillColor();

            outlineCol.a = static_cast<std::uint8_t>(ratio * 255);
            fillCol.a = static_cast<std::uint8_t>(ratio * 255);

            shape.setOutlineColor(outlineCol);
            shape.setFillColor(fillCol);

            // Destroy expired bullets
            if (bullet.lifetime <= 0 || bullet.markedForDestroy) {
                em.destroyEntity(i);
            }
        }
    }
};


// ============================================================================
// BACKGROUND SYSTEM
// ============================================================================

/**
 * @class BackgroundSystem
 * @brief Parallax scrolling starfield
 *
 * Stars move opposite to player velocity with varying parallax factors.
 * Distant stars (low parallax) move slower, creating depth illusion.
 * Stars wrap around screen edges for infinite scrolling.
 */
class BackgroundSystem {
public:
    /**
     * @brief Update star positions based on player movement
     * @param em EntityManager
     * @param playerVelocity Current player velocity (pixels/sec)
     * @param windowSize Screen dimensions for wrapping
     * @param dt Delta time
     */
    static void update(EntityManager& em, sf::Vector2f playerVelocity, sf::Vector2u windowSize, float dt) {
        for (auto& star : em.stars) {
            star.position -= playerVelocity * dt * star.parallaxFactor;

            // Wrap around screen edges
            if (star.position.x < 0) star.position.x += windowSize.x;
            if (star.position.x > windowSize.x) star.position.x -= windowSize.x;
            if (star.position.y < 0) star.position.y += windowSize.y;
            if (star.position.y > windowSize.y) star.position.y -= windowSize.y;
        }
    }

    /**
     * @brief Draw all stars as a single vertex array
     * @param window SFML render window
     * @param em EntityManager
     */
    static void draw(sf::RenderWindow& window, EntityManager& em) {
        sf::VertexArray va(sf::PrimitiveType::Triangles);

        for (const auto& star : em.stars) {
            float r = star.size / 2.0f;
            sf::Vector2f p = star.position;

            // Each star is a quad (2 triangles = 6 vertices)
            sf::Vertex v0({ p.x - r, p.y - r }, star.color);
            sf::Vertex v1({ p.x + r, p.y - r }, star.color);
            sf::Vertex v2({ p.x + r, p.y + r }, star.color);
            sf::Vertex v3({ p.x - r, p.y + r }, star.color);

            va.append(v0); va.append(v1); va.append(v2);
            va.append(v2); va.append(v3); va.append(v0);
        }
        window.draw(va);
    }
};


// ============================================================================
// PARTICLE SYSTEM
// ============================================================================

/**
 * @class ParticleSystem
 * @brief Manages visual effects (explosions, sparks, debris)
 *
 * Particles have position, velocity, lifetime, and fading alpha.
 * Uses swap-and-pop for O(1) removal when particles expire.
 */
class ParticleSystem {
public:
    /**
     * @brief Update all particles (movement and lifetime)
     * @param em EntityManager
     * @param dt Delta time
     */
    static void update(EntityManager& em, float dt) {
        for (size_t i = em.particles.size(); i-- > 0; ) {
            auto& p = em.particles[i];
            p.position += p.velocity * dt;
            p.lifetime -= dt;

            if (p.lifetime <= 0) {
                // Swap with last and pop (O(1) removal)
                em.particles[i] = em.particles.back();
                em.particles.pop_back();
                continue;
            }

            // Fade out alpha based on remaining lifetime
            float ratio = p.lifetime / p.maxLifetime;
            p.color.a = static_cast<uint8_t>(255 * ratio);
        }
    }

    /**
     * @brief Draw all particles as a vertex array
     * @param window SFML render window
     * @param em EntityManager
     */
    static void draw(sf::RenderWindow& window, EntityManager& em) {
        if (em.particles.empty()) return;

        sf::VertexArray va(sf::PrimitiveType::Triangles, em.particles.size() * 6);

        for (size_t i = 0; i < em.particles.size(); ++i) {
            size_t idx = i * 6;
            const auto& p = em.particles[i];
            float s = p.size / 2.0f;

            // Each particle is a quad (2 triangles)
            va[idx + 0] = { {p.position.x - s, p.position.y - s}, p.color };
            va[idx + 1] = { {p.position.x + s, p.position.y - s}, p.color };
            va[idx + 2] = { {p.position.x - s, p.position.y + s}, p.color };

            va[idx + 3] = { {p.position.x + s, p.position.y - s}, p.color };
            va[idx + 4] = { {p.position.x + s, p.position.y + s}, p.color };
            va[idx + 5] = { {p.position.x - s, p.position.y + s}, p.color };
        }
        window.draw(va);
    }
};


// ============================================================================
// AI SYSTEM
// ============================================================================

/**
 * @class AISystem
 * @brief Enemy artificial intelligence with state machine
 *
 * Enemy states:
 * - PATROL: Random wandering, no awareness of player
 * - ALERT: Searching last known player position after losing sight
 * - COMBAT: Actively chasing and orbiting the player
 *
 * Uses steering behaviors for smooth movement and asteroid avoidance.
 * AI state is cached in unordered_map keyed by entityId (persists across swaps).
 */
class AISystem {
    static inline std::unordered_map<uint32_t, AIState> aiCache;

public:
    /**
     * @brief Update all enemy AI
     * @param em EntityManager
     * @param playerEntityId Persistent player ID
     * @param dt Delta time
     * @param lua Lua state (enemy config)
     */
    static void update(EntityManager& em, uint32_t playerEntityId, float dt, sol::state& lua) {
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) return;
        sf::Vector2f playerPos = em.transforms[playerIdx].position;

        sol::table config = lua["enemy_config"];
        float enginePower = config["engine_power"].get_or(150.0f);
        float maxSpeed = config["max_speed"].get_or(20.0f);
        float visionRange = 200.f;   // Detection range (pixels)
        float combatRange = 200.f;   // Preferred engagement distance


        // ===== HOMING MISSILE UPDATE =====
        for (size_t i = 0; i < em.physics.size(); ++i) {
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);
            if (type != BodyType::Asteroid) continue;

            if (em.healths[i].isHoming && em.healths[i].homingTargetEntityId != 0) {
                size_t targetIdx = em.getEntityIndex(em.healths[i].homingTargetEntityId);
                if (targetIdx == (size_t)-1 ||
                    (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[targetIdx].bodyId) != BodyType::Enemy) {
                    em.healths[i].isHoming = false;
                    continue;
                }

                // Get direction to target
                sf::Vector2f asteroidPos = em.transforms[i].position;
                sf::Vector2f targetPos = em.transforms[targetIdx].position;
                sf::Vector2f toTarget = targetPos - asteroidPos;
                float len = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
                if (len > 0.01f) toTarget /= len;

                // Apply homing force
                float turnRate = lua["homing_turn_rate"].get_or(3.0f);
                b2Vec2 currentVel = b2Body_GetLinearVelocity(em.physics[i].bodyId);
                sf::Vector2f desiredVel(toTarget.x * 800, toTarget.y * 800);

                b2Vec2 impulse = {
                    (desiredVel.x / SCALE - currentVel.x) * turnRate,
                    (desiredVel.y / SCALE - currentVel.y) * turnRate
                };
                b2Body_ApplyForceToCenter(em.physics[i].bodyId, impulse, true);

                // Spawn trail effect
                if (rand() % 3 == 0) {
                    em.spawnImpact(asteroidPos,
                        sf::Color(0, 255, 200, 150),
                        sf::Vector2f(-toTarget.x * 200, -toTarget.y * 200));
                }
            }
        }


        for (size_t i = 0; i < em.physics.size(); ++i) {
            BodyType type = (BodyType)(uintptr_t)b2Body_GetUserData(em.physics[i].bodyId);
            if (type != BodyType::Enemy) continue;


            auto& health = em.healths[i];

            // ===== STUN CHECK =====
            if (health.stunTimer > 0) {
                health.stunTimer -= dt;
                // Stunned enemies don't move
                continue;
            }

            auto& tf = em.transforms[i];
            uint32_t entityId = tf.entityId;
            auto& ai = aiCache[entityId];
            b2BodyId bodyId = em.physics[i].bodyId;

            sf::Vector2f enemyPos = tf.position;
            sf::Vector2f toPlayer = playerPos - enemyPos;
            float distToPlayer = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);
            bool canSeePlayer = (distToPlayer < visionRange);

            // ===== State Machine =====
            switch (ai.currentState) {
            case EnemyState::PATROL:
                if (canSeePlayer) {
                    ai.currentState = EnemyState::COMBAT;
                    ai.reactionTimer = 0.4f;  // Artificial reaction delay
                }
                break;

            case EnemyState::COMBAT:
                if (!canSeePlayer) {
                    ai.currentState = EnemyState::ALERT;
                    ai.lastKnownPlayerPos = playerPos;
                    ai.searchTimer = 5.0f;
                }
                else {
                    ai.lastKnownPlayerPos = playerPos;
                }
                break;

            case EnemyState::ALERT:
                if (canSeePlayer) {
                    ai.currentState = EnemyState::COMBAT;
                }
                else {
                    ai.searchTimer -= dt;
                    sf::Vector2f toLastKnown = ai.lastKnownPlayerPos - enemyPos;
                    float distToLast = std::sqrt(toLastKnown.x * toLastKnown.x + toLastKnown.y * toLastKnown.y);

                    if (ai.searchTimer <= 0 || distToLast < 100.f) {
                        ai.currentState = EnemyState::PATROL;
                    }
                }
                break;
            }

            // ===== Behavior Output (desired velocity) =====
            ai.reactionTimer -= dt;
            if (ai.reactionTimer <= 0) {
                ai.reactionTimer = 0.1f + (rand() % 10) / 100.f;

                sf::Vector2f targetVel(0.f, 0.f);

                if (ai.currentState == EnemyState::COMBAT) {
                    sf::Vector2f dir = toPlayer / distToPlayer;
                    if (distToPlayer > combatRange) {
                        targetVel = dir * maxSpeed * 10.f;  // Chase
                    }
                    else {
                        // Orbit around player (perpendicular movement + slight chase)
                        sf::Vector2f orbit(-dir.y, dir.x);
                        targetVel = (dir * 0.3f + orbit) * (maxSpeed * 8.f);
                    }
                }
                else if (ai.currentState == EnemyState::ALERT) {
                    sf::Vector2f toTarget = ai.lastKnownPlayerPos - enemyPos;
                    float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
                    if (d > 10.f) targetVel = (toTarget / d) * (maxSpeed * 12.f);
                }
                else {  // PATROL
                    ai.patrolWaitTimer -= 0.15f;
                    if (ai.patrolWaitTimer <= 0) {
                        float angle = (rand() % 360) * 3.14159f / 180.f;
                        ai.patrolTarget = enemyPos + sf::Vector2f(std::cos(angle), std::sin(angle)) * 300.f;
                        ai.patrolWaitTimer = 4.0f;
                    }

                    sf::Vector2f toTarget = ai.patrolTarget - enemyPos;
                    float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
                    if (d > 50.f) targetVel = (toTarget / d) * (maxSpeed * 5.f);
                }

                ai.smoothedDesiredVel = targetVel;
            }

            // ===== Obstacle Avoidance (asteroids) =====
            sf::Vector2f avoidance(0.f, 0.f);
            for (size_t j = 0; j < em.physics.size(); ++j) {
                if ((BodyType)(uintptr_t)b2Body_GetUserData(em.physics[j].bodyId) == BodyType::Asteroid) {
                    sf::Vector2f diff = enemyPos - em.transforms[j].position;
                    float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
                    if (d < 300.f) {
                        avoidance += (diff / d) * 600.f * (1.0f - d / 300.f);
                    }
                }
            }

            sf::Vector2f finalDesiredVel = ai.smoothedDesiredVel + avoidance;

            // ===== Steering (apply force to reach desired velocity) =====
            b2Vec2 currentVel = b2Body_GetLinearVelocity(bodyId);
            b2Vec2 desiredB2 = { finalDesiredVel.x / SCALE, finalDesiredVel.y / SCALE };
            b2Vec2 impulse = { desiredB2.x - currentVel.x, desiredB2.y - currentVel.y };

            float impulseLen = std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y);
            float maxForce = enginePower * dt;
            if (impulseLen > maxForce) {
                float scale = maxForce / impulseLen;
                impulse.x *= scale;
                impulse.y *= scale;
            }

            b2Body_ApplyForceToCenter(bodyId, { impulse.x * 50.0f, impulse.y * 50.0f }, true);

            // ===== Rotation (face movement direction or player in combat) =====
            float targetAngle = tf.rotation;

            if (std::abs(finalDesiredVel.x) > 10.f || std::abs(finalDesiredVel.y) > 10.f) {
                targetAngle = std::atan2(finalDesiredVel.y, finalDesiredVel.x) * 180.f / 3.14159f + 90.f;
            }
            if (ai.currentState == EnemyState::COMBAT) {
                targetAngle = std::atan2(toPlayer.y, toPlayer.x) * 180.f / 3.14159f + 90.f;
            }

            float rotSpeed = config["rotation_speed"].get_or(4.0f);
            float deltaAngle = targetAngle - tf.rotation;
            while (deltaAngle > 180) deltaAngle -= 360;
            while (deltaAngle < -180) deltaAngle += 360;

            float error = std::sin(ai.searchTimer * 2.0f) * 5.0f;  // Wobble in alert state
            tf.rotation += (deltaAngle + error) * rotSpeed * dt;

            b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId), b2MakeRot(tf.rotation * 3.14159f / 180.f));
        }
    }
};