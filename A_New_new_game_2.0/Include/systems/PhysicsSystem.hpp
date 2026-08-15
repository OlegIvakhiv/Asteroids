/**
 * @file PhysicsSystem.hpp
 * @brief Box2D physics integration and world cleanup
 *
 * Steps the Box2D world at a fixed timestep and synchronises
 * physics body positions/rotations back to TransformComponents.
 * Also removes entities that have drifted too far from the player.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (refactored)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <cmath>
#include <vector>

 /**
  * @class PhysicsSystem
  * @brief Updates Box2D physics simulation and syncs transforms
  *
  * Steps the Box2D world at fixed timestep, then copies position/rotation
  * from physics bodies back to TransformComponents for rendering.
  * Also provides a cleanup routine to remove distant entities.
  */
class PhysicsSystem : public ISystem {
public:
    /**
     * @brief Initialise the system with the global context
     * @param ctx SystemContext containing all engine dependencies
     *
     * Stores pointers to EntityManager, Box2D world, player ID, and Lua state.
     */
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
    }

    /**
     * @brief Step physics simulation and synchronise transforms
     * @param dt Delta time since last frame (seconds)
     *
     * Uses fixed timestep (60 Hz) with substeps for stability.
     * Bullets orient by velocity direction; asteroids by physics rotation.
     */
    void update(float dt) override {
        if (!m_em) return;

        // Step the physics world with a VARIABLE timestep.
        //
        // The comment here used to claim a fixed 60Hz step and declared
        //     float timeStep = 1.0f / 60.0f;
        // ...which was then never used -- `dt` was passed instead. So the sim
        // has always been variable-step. The dead local is removed rather than
        // wired up, because switching to a true fixed step needs an accumulator
        // AND all the hitstop time-scaling in SystemManager to be reworked
        // (a scaled dt no longer maps to a whole number of fixed steps).
        //
        // Variable-step is acceptable here ONLY because game.cpp clamps dt to
        // 0.05s. Without that clamp an alt-tab would tunnel bodies through
        // each other. Do not remove the clamp.
        int subStepCount = 6;
        b2World_Step(m_worldId, dt, subStepCount);

        // Sync Box2D bodies to TransformComponents
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            b2BodyId bodyId = m_em->physics[i].bodyId;
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid; // fallback

            // Position: always sync
            b2Vec2 pos = b2Body_GetPosition(bodyId);
            m_em->transforms[i].position = { pos.x * SCALE, pos.y * SCALE };

            // Rotation: bullets align with velocity, asteroids use physics rotation
            if (type == BodyType::Bullet) {
                b2Vec2 vel = b2Body_GetLinearVelocity(bodyId);
                if (std::sqrt(vel.x * vel.x + vel.y * vel.y) > 0.1f) {
                    float angleRad = std::atan2(vel.y, vel.x) + (3.14159f / 2.f);
                    m_em->transforms[i].rotation = angleRad * 180.f / 3.14159f;
                }
            }
            else if (type == BodyType::Asteroid) {
                b2Rot rot = b2Body_GetRotation(bodyId);
                m_em->transforms[i].rotation = b2Rot_GetAngle(rot) * 180.f / 3.14159f;
            }
            // Player rotation is handled by InputSystem, not synced from physics
        }
    }

    /**
     * @brief Remove entities that drifted too far from player
     *
     * Collects distant asteroids/enemies, destroys them in reverse order.
     * Uses collect-then-destroy pattern for safe swap-and-pop deletion.
     * Called separately from SystemManager after the physics step.
     */
    void cleanup() {
        if (!m_em || !m_lua) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return; // Player dead

        b2Vec2 playerPos = b2Body_GetPosition(m_em->physics[playerIdx].bodyId);

        float astDesRadius = (*m_lua)["spawn_settings"]["despawn_radius"].get_or(100.0f);
        float astDesRadiusSq = astDesRadius * astDesRadius;

        float enemyDesRadius = (*m_lua)["enemy_config"]["despawn_radius"].get_or(250.0f);
        float enemyDesRadiusSq = enemyDesRadius * enemyDesRadius;

        // Collect indices to destroy (don't destroy during iteration)
        std::vector<size_t> indicesToDestroy;

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (i == playerIdx) continue;

            b2BodyId bodyId = m_em->physics[i].bodyId;
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid;
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
            m_em->destroyEntity(indicesToDestroy[i]);
        }
    }

private:
    EntityManager* m_em = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
};