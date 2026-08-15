/**
 * @file InputSystem.hpp
 * @brief Handles player controls: movement, rotation, turbo, dash, and parry
 *
 * CHANGES in 1.3:
 *  - STAGGER GATING. While tumbling, all input is dropped. During recovery,
 *    aiming is restored on a ramp (slow -> full) so the ship visibly swings
 *    back onto the cursor, but movement / dash / parry stay locked.
 *  - Parry whiff detection moved OUT of the animation block. It was nested
 *    inside `if (parryAnimTimer > 0)`, where `isAnimating` is always true, so
 *    the `wasAnimating && !isAnimating` transition could never fire.
 *  - Dash is edge-triggered (1.2) and classified relative to the ship (1.2).
 *  - Mouse aim maps through the shared camera view (1.2).
 *
 * @author Oleg Ivakhiv
 * @version 1.3
 */

#pragma once

#include "ISystem.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/GameConfig.hpp"
#include <cmath>
#include <algorithm>

class InputSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_playerEntityId = ctx.playerEntityId;
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_gameView = ctx.gameView;
        m_wasAnimating = false;
        m_dashWasDown = false;
    }

    void update(float dt) override {
        if (!m_em || !m_window || !m_lua) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& tf = m_em->transforms[playerIdx];
        auto& playerStats = m_em->players[playerIdx];
        auto& phys = m_em->physics[playerIdx];
        b2BodyId bodyId = phys.bodyId;

        InputRegistry::init();

        // ====================================================================
        // 1. LOAD CONFIGURATION FROM LUA
        // ====================================================================
        float enginePower = (*m_lua)["engine_power"].get_or(150.f);
        float rotationSpeed = (*m_lua)["rotation_speed"].get_or(4.f);
        float multiplier = (*m_lua)["sprint_power_multiplier"].get_or(2.5f);
        float drainRate = (*m_lua)["sprint_drain_speed"].get_or(40.f);
        float regenRate = (*m_lua)["sprint_regen_speed"].get_or(20.f);
        float penaltyTime = (*m_lua)["penalty_energy"].get_or(3.0f);
        float dashVel = (*m_lua)["dash_velocity"].get_or(40.f);
        float dashCost = (*m_lua)["dash_energy_cost"].get_or(30.f);
        playerStats.dashMaxCooldown = (*m_lua)["dash_max_cooldown"].get_or(1.0f);

        float parryWindow = (*m_lua)["parry_window"].get_or(0.2f);
        float parryAnimDuration = (*m_lua)["parry_anim_duration"].get_or(0.6f);
        float parryCooldownTime = (*m_lua)["parry_cooldown"].get_or(2.0f);
        playerStats.parryMaxCooldown = parryCooldownTime;

        sol::table binds = (*m_lua)["key_bindings"];

        // ====================================================================
        // 2. TIMERS  (always tick, even while staggered)
        // ====================================================================
        if (playerStats.dashCooldown > 0)    playerStats.dashCooldown -= dt;
        if (playerStats.overheatTimer > 0)   playerStats.overheatTimer -= dt;
        if (playerStats.parryCooldown > 0)   playerStats.parryCooldown -= dt;

        if (playerStats.parryTimer > 0) {
            playerStats.parryTimer -= dt;
            if (playerStats.parryTimer <= 0) playerStats.isParrying = false;
        }
        if (playerStats.parryAnimTimer > 0) playerStats.parryAnimTimer -= dt;

        // ---- Parry whiff detection (moved out of the animation block) ----
        if (playerStats.parryWhiffRecovery) {
            playerStats.parryWhiffTimer -= dt;
            if (playerStats.parryWhiffTimer <= 0.f) {
                playerStats.parryWhiffRecovery = false;
                playerStats.parryWhiffTimer = 0.f;
            }
        }
        {
            const bool isAnimating = (playerStats.parryAnimTimer > 0.f);
            if (m_wasAnimating && !isAnimating && !playerStats.parryHitSomething) {
                playerStats.parryWhiffRecovery = true;
                playerStats.parryWhiffTimer = (*m_lua)["parry_whiff_duration"].get_or(0.5f);
            }
            if (!isAnimating) playerStats.parryHitSomething = false;
            m_wasAnimating = isAnimating;
        }

        // ====================================================================
        // 3. STAGGER GATE
        //
        // TUMBLE: nothing responds. The ship is genuinely out of control and
        //         ShipAnimSystem owns tf.rotation.
        // ====================================================================
        if (playerStats.staggerTimer > 0.f) {
            playerStats.isTurbo = false;
            playerStats.riftCharging = false;
            playerStats.isParrying = false;
            playerStats.parryTimer = 0.f;
            playerStats.parryAnimTimer = 0.f;

            // Energy still trickles back so you aren't punished twice.
            if (playerStats.energyDrive < playerStats.maxEnergyDrive)
                playerStats.energyDrive = std::min(playerStats.maxEnergyDrive,
                    playerStats.energyDrive + regenRate * 0.5f * dt);
            return;
        }

        // ====================================================================
        // 4. TURBO BOOST (Energy Drain)
        // ====================================================================
        const bool recovering = (playerStats.staggerRecoverTimer > 0.f);

        std::string sprintKey = binds["sprint"].get<std::string>();
        bool wantSprint = InputRegistry::isPressed(sprintKey) && !recovering;

        if (wantSprint && playerStats.energyDrive > 0 && playerStats.overheatTimer <= 0) {
            playerStats.isTurbo = true;
            playerStats.energyDrive -= drainRate * dt;
            if (playerStats.energyDrive <= 0) {
                playerStats.energyDrive = 0;
                playerStats.overheatTimer = penaltyTime;
                playerStats.isTurbo = false;
            }
        }
        else {
            playerStats.isTurbo = false;
        }

        // ====================================================================
        // 5. ENERGY REGENERATION
        // ====================================================================
        if (!playerStats.isTurbo && playerStats.energyDrive < playerStats.maxEnergyDrive) {
            playerStats.energyDrive += regenRate * dt;
            if (playerStats.energyDrive > playerStats.maxEnergyDrive) {
                playerStats.energyDrive = playerStats.maxEnergyDrive;
            }
        }

        // ====================================================================
        // 6. MOUSE AIM
        //
        // Mapped through the SHARED camera view. It holds last frame's
        // transform, which is a one-frame lag nobody can perceive — and far
        // better than an unshaken view, which would slide the crosshair away
        // from the cursor every time the screen shakes or zooms.
        // ====================================================================
        if (playerStats.isTurbo) rotationSpeed *= 0.3f;

        if (recovering) {
            // Ramp the turn rate back up: the ship swings onto target rather
            // than snapping to it the instant the tumble ends.
            const float u = 1.f - (playerStats.staggerRecoverTimer /
                std::max(0.0001f, playerStats.staggerRecoverDuration));
            rotationSpeed *= 0.25f + 0.75f * u * u;
        }

        sf::View savedView = m_window->getView();
        m_window->setView(m_gameView ? *m_gameView : savedView);
        sf::Vector2i mousePos = sf::Mouse::getPosition(*m_window);
        sf::Vector2f worldPos = m_window->mapPixelToCoords(mousePos);
        m_window->setView(savedView);

        b2Vec2 b2Pos = b2Body_GetPosition(bodyId);
        sf::Vector2f currentPos(b2Pos.x * SCALE, b2Pos.y * SCALE);
        float targetAngle = std::atan2(worldPos.y - currentPos.y, worldPos.x - currentPos.x) * 180.f / 3.14159f + 90.f;

        float deltaAngle = targetAngle - tf.rotation;
        while (deltaAngle > 180) deltaAngle -= 360;
        while (deltaAngle < -180) deltaAngle += 360;

        tf.rotation += deltaAngle * rotationSpeed * dt;
        b2Body_SetTransform(bodyId, b2Pos, b2MakeRot(tf.rotation * 3.14159f / 180.f));

        // RECOVERY: aiming is back, but the ship is still not manoeuvrable.
        if (recovering) return;

        // ====================================================================
        // 7. MOVEMENT INPUT (WASD / Arrows)
        // ====================================================================
        float dx = 0.f, dy = 0.f;
        if (InputRegistry::isPressed(binds["up"].get<std::string>()))    dy -= 1.f;
        if (InputRegistry::isPressed(binds["down"].get<std::string>()))  dy += 1.f;
        if (InputRegistry::isPressed(binds["left"].get<std::string>()))  dx -= 1.f;
        if (InputRegistry::isPressed(binds["right"].get<std::string>())) dx += 1.f;

        // ====================================================================
        // 8. APPLY ENGINE FORCE
        // ====================================================================
        if (playerStats.isTurbo) {
            float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
            float finalPower = enginePower * multiplier;
            b2Body_ApplyForceToCenter(phys.bodyId,
                { std::cos(noseRad) * finalPower, std::sin(noseRad) * finalPower },
                true);
        }
        else if (playerStats.riftCharging) {
            // No thrust while charging the Rift Shot. (This branch previously
            // computed an unused `finalPower` local — removed.)
        }
        else {
            if (dx != 0 || dy != 0) {
                float length = std::sqrt(dx * dx + dy * dy);
                b2Body_ApplyForceToCenter(phys.bodyId,
                    { (dx / length) * enginePower, (dy / length) * enginePower },
                    true);
            }
        }

        // ====================================================================
        // 9. DASH MECHANIC (edge-triggered instant velocity boost)
        // ====================================================================
        std::string dashKey = binds["dash"].get<std::string>();
        bool dashDown = InputRegistry::isPressed(dashKey);
        bool dashPressedThisFrame = dashDown && !m_dashWasDown;
        m_dashWasDown = dashDown;

        if (dashPressedThisFrame &&
            playerStats.dashCooldown <= 0 &&
            playerStats.overheatTimer <= 0 &&
            playerStats.energyDrive >= dashCost) {

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

            playerStats.energyDrive -= dashCost;
            playerStats.dashCooldown = playerStats.dashMaxCooldown;

            // ---- Classify the dash relative to the SHIP ----
            // Local +Y points backwards (nose vertex is (0,-30)), so:
            //     forward = ( sin r, -cos r )
            //     right   = ( cos r,  sin r )
            const float r = tf.rotation * 3.14159f / 180.f;
            const sf::Vector2f forward(std::sin(r), -std::cos(r));
            const sf::Vector2f rightV(std::cos(r), std::sin(r));

            const float invLen = 1.f / std::max(0.0001f, std::sqrt(vx * vx + vy * vy));
            const sf::Vector2f dashDir(vx * invLen, vy * invLen);

            const float f = dashDir.x * forward.x + dashDir.y * forward.y;
            const float sd = dashDir.x * rightV.x + dashDir.y * rightV.y;

            if (f < -0.85f) {
                playerStats.dashAnim = DashAnim::SpinBack;
                playerStats.dashAnimDuration = animCfg("backdash_spin_duration", 0.55f);
            }
            else if (std::abs(sd) > std::abs(f)) {
                playerStats.dashAnim = (sd > 0.f) ? DashAnim::BankRight : DashAnim::BankLeft;
                playerStats.dashAnimDuration = animCfg("dash_bank_duration", 0.45f);
            }
            else {
                playerStats.dashAnim = DashAnim::WiggleForward;
                playerStats.dashAnimDuration = animCfg("forward_wiggle_duration", 0.35f);
            }
            playerStats.dashAnimTimer = playerStats.dashAnimDuration;

            m_em->addTrauma(animCfg("shake_dash", 0.35f));
            m_em->cameraZoomKick = -animCfg("zoom_dash_punch", 0.07f);

            if (playerStats.energyDrive < 1.0f) {
                playerStats.energyDrive = 0;
                playerStats.overheatTimer = penaltyTime;
            }
        }

        // ====================================================================
        // 10. PARRY MECHANIC
        // ====================================================================
        std::string parryKey = binds["parry"].get<std::string>();

        if (InputRegistry::isPressed(parryKey) &&
            playerStats.parryCooldown <= 0 &&
            playerStats.parryTimer <= 0) {
            playerStats.isParrying = true;
            playerStats.parryTimer = parryWindow;
            playerStats.parryAnimTimer = parryAnimDuration;
            playerStats.parryCooldown = parryCooldownTime;
            playerStats.parryStartRotation = tf.rotation;
            playerStats.parrySpinAngle = 0.f;
            playerStats.parryHitSomething = false;   // reset for this attempt

            m_em->addTrauma(animCfg("shake_parry_start", 0.18f));
        }

        // Spin animation (visual, but driven through the real rotation so the
        // hull orientation matches what the player sees).
        if (playerStats.parryAnimTimer > 0) {
            float t = 1.0f - (playerStats.parryAnimTimer / parryAnimDuration);
            float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            float angle = playerStats.parryStartRotation + 360.f * ease;
            tf.rotation = angle;

            b2Vec2 b2Pos2 = b2Body_GetPosition(phys.bodyId);
            b2Body_SetTransform(phys.bodyId, b2Pos2, b2MakeRot(angle * 3.14159f / 180.f));
        }
    }

private:
    float animCfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    EntityManager* m_em = nullptr;
    uint32_t m_playerEntityId = 0;
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    sf::View* m_gameView = nullptr;   ///< Owned by SystemManager, written by CameraSystem

    bool m_wasAnimating = false;      ///< Parry animation state last frame
    bool m_dashWasDown = false;       ///< Edge detection for the dash key
};