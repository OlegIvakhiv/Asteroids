/**
 * @file EffectsSystem.hpp
 * @brief Visual effects system for thrusters, dash bursts, and parry sparks
 *
 * Spawns particle effects for:
 * - Player ship thrusters (normal and turbo modes)
 * - Player dash bursts (cyan radial streaks + flash)
 * - Player parry ring sparks
 * - Enemy ship thrusters (red-orange exhaust)
 *
 * All effects are added to the EntityManager's particle list and are
 * rendered by the ParticleSystem.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (refactored)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/GameConfig.hpp"
#include <cmath>
#include <cstdlib>

 /**
  * @class EffectsSystem
  * @brief Spawns visual particle effects for ships and actions
  *
  * This system does not render particles directly – it only creates
  * new particle entries in the EntityManager. The actual rendering
  * is handled by ParticleSystem.
  *
  * Effects include:
  * - Player engine exhaust (cool blue or hot white-blue for turbo)
  * - Dash burst (cyan radial streaks with afterimage flash)
  * - Parry ring sparks (cyan particles around the parry radius)
  * - Enemy engine exhaust (red-orange)
  */
class EffectsSystem : public ISystem {
public:
    /**
     * @brief Initialise the system with the global context
     * @param ctx SystemContext containing all engine dependencies
     *
     * Stores pointers to EntityManager and player ID.
     * Resets the dash detection state.
     */
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_playerEntityId = ctx.playerEntityId;
        m_lastDashCooldown = 0.f;
    }

    /**
     * @brief Spawn visual effects based on current game state
     * @param dt Delta time in seconds
     *
     * Called every frame. Iterates over all physics bodies and:
     * - For the player: spawns thruster particles, dash burst, parry sparks
     * - For enemies: spawns thruster particles
     */
    void update(float dt) override {
        if (!m_em) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);

        // ====================================================================
        // Iterate over all physics bodies
        // ====================================================================
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud) continue;

            b2Vec2 vel = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
            float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);

            // ---- PLAYER effects ----
            if (ud->type == BodyType::Player && i == playerIdx) {
                auto& tf = m_em->transforms[i];
                auto& playerStats = m_em->players[i];
                float rot = tf.rotation * 3.14159f / 180.f;

                // Ship nose direction (forward) and right vector
                sf::Vector2f forward(std::sin(rot), -std::cos(rot));
                sf::Vector2f right(std::cos(rot), std::sin(rot));

                // Two engine nozzle positions (matching the ship shape stabilizers)
                sf::Vector2f nozzleL = tf.position - forward * 20.f - right * 18.f;
                sf::Vector2f nozzleR = tf.position - forward * 20.f + right * 18.f;

                bool moving = (speed > 1.f);
                bool turbo = playerStats.isTurbo;

                // ----- THRUSTER PARTICLES -----
                if (moving || turbo) {
                    int count = turbo ? 3 : 1;
                    for (int n = 0; n < count; ++n) {
                        // Use initializer list to iterate over both nozzles
                        sf::Vector2f nozzles[2] = { nozzleL, nozzleR };
                        for (auto& nozzle : nozzles) {
                            float spread = ((rand() % 40) - 20) * 3.14159f / 180.f;
                            sf::Vector2f dir = -forward;
                            sf::Vector2f pVel = {
                                (dir.x * std::cos(spread) - dir.y * std::sin(spread)) * (120.f + rand() % 80),
                                (dir.x * std::sin(spread) + dir.y * std::cos(spread)) * (120.f + rand() % 80)
                            };

                            float life = turbo ? 0.25f + (rand() % 15) / 100.f
                                : 0.12f + (rand() % 8) / 100.f;

                            // Turbo: hot white-blue core. Normal: cool blue
                            sf::Color col = turbo
                                ? sf::Color(180, 220, 255, 220)
                                : sf::Color(60, 160, 255, 180);

                            float sz = turbo ? 4.f + (rand() % 3) : 2.5f + (rand() % 2);

                            m_em->particles.push_back({
                                m_em->nextEntityId++,
                                nozzle,
                                pVel,
                                col,
                                life,
                                life,
                                sz
                                });
                        }
                    }
                }

                // ----- DASH BURST -----
                // Detect a fresh dash: dashCooldown jumps from 0 to dashMaxCooldown.
                // We compare against previous frame's value with a threshold.
                bool justDashed = (playerStats.dashCooldown > m_lastDashCooldown + 0.05f);
                m_lastDashCooldown = playerStats.dashCooldown;

                if (justDashed) {
                    // Radial burst of cyan streaks
                    for (int n = 0; n < 20; ++n) {
                        float angle = (rand() % 360) * 3.14159f / 180.f;
                        sf::Vector2f dir(std::cos(angle), std::sin(angle));
                        float spd = 300.f + rand() % 200;
                        m_em->particles.push_back({
                            m_em->nextEntityId++,
                            tf.position,
                            dir * spd,
                            sf::Color(0, 220, 255, 230),
                            0.2f, 0.2f,
                            3.5f + (rand() % 3)
                            });
                    }
                    // Afterimage: bright flash at ship center
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        tf.position,
                        { 0.f, 0.f },
                        sf::Color(100, 255, 255, 200),
                        0.15f, 0.15f,
                        22.f
                        });
                }

                // ----- PARRY SPARKS (ring edge) -----
                // Spawn small particles around the parry radius while parrying.
                if (playerStats.isParrying && rand() % 3 == 0) {
                    float angle = (rand() % 360) * 3.14159f / 180.f;
                    sf::Vector2f rimPos = tf.position + sf::Vector2f(
                        std::cos(angle) * 55.f,
                        std::sin(angle) * 55.f
                    );
                    sf::Vector2f rimVel(std::cos(angle) * 80.f, std::sin(angle) * 80.f);
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        rimPos,
                        rimVel,
                        sf::Color(0, 255, 220, 200),
                        0.15f, 0.15f,
                        2.5f
                        });
                }
            }

            // ---- ENEMY THRUSTER ----
            else if (ud->type == BodyType::Enemy) {
                if (speed < 0.5f) continue;  // not moving, no exhaust

                auto& tf = m_em->transforms[i];
                float rot = tf.rotation * 3.14159f / 180.f;
                sf::Vector2f forward(std::sin(rot), -std::cos(rot));

                // Single rear exhaust point
                sf::Vector2f nozzle = tf.position - forward * 22.f;

                if (rand() % 2 == 0) {  // 50% chance per frame — subtle
                    float spread = ((rand() % 50) - 25) * 3.14159f / 180.f;
                    sf::Vector2f dir = -forward;
                    sf::Vector2f pVel = {
                        (dir.x * std::cos(spread) - dir.y * std::sin(spread)) * (80.f + rand() % 60),
                        (dir.x * std::sin(spread) + dir.y * std::cos(spread)) * (80.f + rand() % 60)
                    };

                    // Red-orange exhaust to match enemy color
                    int rVar = 200 + rand() % 55;
                    int gVar = 40 + rand() % 40;
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        nozzle,
                        pVel,
                        sf::Color(rVar, gVar, 0, 180),
                        0.1f + (rand() % 8) / 100.f,
                        0.15f,
                        2.f + (rand() % 2)
                        });
                }
            }
        }
    }

private:
    // ---- System dependencies (set via init) ----
    EntityManager* m_em = nullptr;
    uint32_t m_playerEntityId = 0;

    // ---- Persistent state for dash detection ----
    float m_lastDashCooldown = 0.f;   ///< Previous frame's dash cooldown value
};