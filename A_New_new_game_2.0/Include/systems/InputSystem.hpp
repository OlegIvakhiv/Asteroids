/**
 * @file InputSystem.hpp
 * @brief Handles player controls: movement, rotation, turbo, dash, and parry
 *
 * CHANGES in 1.6: poise tuning written to PlayerComponent every frame (hot
 * reload), poise regeneration, hyperarmor flag during a heavy dodge burst.
 *
 * CHANGES in 1.5 (playtest pass):
 *  - DODGE REWRITE (refit ships). The old dash set 40 m/s and let linear
 *    damping glide it out: a very long "fat roll" with no i-frames. Now:
 *      * a controlled burst -- velocity falls linearly from a peak to a
 *        carry speed over dash_duration, covering dash_distance exactly
 *      * then a DRIFT: the carry bleeds back to your entry speed over
 *        dash_drift. Capped along the dodge only -- you can steer out of it.
 *      * CCD (bullet mode) only while bursting: the peak is fast enough to
 *        tunnel through a small asteroid otherwise
 *      * real i-frames from the press (dash_iframes)
 *      * recovery counts from the END of the burst (dash_recovery)
 *      * every number is a class tier -- see ClassTuning.hpp
 *  - BREAK-OUT DODGE: dodging is allowed during stagger RECOVERY and ends it.
 *    The tumble itself still owns the ship.
 *  - Parry window x class (light wider, heavy narrower). Outcome unchanged.
 *  - Regen x class. dashEnergyCost published for the HUD.
 *  - Legacy ships keep the 1.3 dash exactly.
 *
 * CHANGES in 1.4 (refit kit):
 *  - When the player was built from a ShipDesign (kit.valid), movement reads
 *    the mounted DRIVES instead of a flat engine_power:
 *      * every drive pushes along its own direction; the input direction gets
 *        the sum of what each drive contributes toward it
 *      * RCS gives a class-dependent floor in every direction, so reversing
 *        and strafing on a stern-only ship is weaker, never impossible
 *      * turbo uses forward drive thrust only
 *  - rotation_speed x agility, dash_velocity x dashScale (impulse / mass),
 *    sprint_regen_speed x regenScale (free reactor units).
 *  - Off-axis drives drift the nose under forward thrust. Aim fights it.
 *  - Legacy ships (kit.valid == false) are untouched.
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
 * @version 1.5
 */

#pragma once

#include "ISystem.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/GameConfig.hpp"
#include "utils/ClassTuning.hpp"
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
        m_fwdThrottle = 0.f;
        m_dashFinishing = false;
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

        // ---- Refit kit: the hull scales the Lua baseline, never replaces it ----
        const ship::KitProfile& kit = playerStats.kit;
        const bool refit = kit.valid;
        const ship::ClassFeel feel = ship::classFeelFor(*m_lua, kit);
        if (refit) {
            rotationSpeed *= kit.agilityScale;
            regenRate *= kit.regenScale * feel.regen;
            dashCost = feel.dashEnergyCost;
        }
        else {
            // Legacy dash owns its cooldown from Lua; a refit dodge sets it
            // per burst (duration + recovery) so the HUD and flash stay right.
            playerStats.dashMaxCooldown = (*m_lua)["dash_max_cooldown"].get_or(1.0f);
        }
        playerStats.dashEnergyCost = dashCost;

        // ---- Poise tuning (refit ships; legacy stays at 0 / 1 / 1) ----
        if (refit) {
            if (playerStats.poiseMax != feel.poise)            // spawn or F5 reload
                playerStats.poise = std::min(feel.poise, playerStats.poise + (feel.poise - playerStats.poiseMax));
            playerStats.poiseMax = feel.poise;
            playerStats.poiseRegen = feel.poiseRegen;
            playerStats.poiseDelay = feel.poiseDelay;
            playerStats.knockbackScale = feel.knockback;
            playerStats.tumbleScale = feel.tumble;
            playerStats.damageTakenScale = 1.f - feel.damageReduction;
            playerStats.hyperarmorDamageScale = 1.f - feel.hyperarmorReduction;
        }
        playerStats.hyperarmor = refit && feel.hyperarmor > 0.5f && playerStats.dashTimer > 0.f;
        if (playerStats.poiseHitFlash > 0.f)   playerStats.poiseHitFlash -= dt;
        if (playerStats.poiseBreakFlash > 0.f) playerStats.poiseBreakFlash -= dt;
        if (playerStats.poiseRegenTimer > 0.f) playerStats.poiseRegenTimer -= dt;
        else if (playerStats.poise < playerStats.poiseMax)
            playerStats.poise = std::min(playerStats.poiseMax, playerStats.poise + playerStats.poiseRegen * dt);

        float parryWindow = (*m_lua)["parry_window"].get_or(0.2f)
            * (refit ? feel.parryWindow : 1.f);
        float parryAnimDuration = (*m_lua)["parry_anim_duration"].get_or(0.6f);
        float parryCooldownTime = (*m_lua)["parry_cooldown"].get_or(2.0f);
        playerStats.parryMaxCooldown = parryCooldownTime;

        sol::table binds = (*m_lua)["key_bindings"];

        // ====================================================================
        // 2. TIMERS  (always tick, even while staggered)
        // ====================================================================
        if (playerStats.dashCooldown > 0)    playerStats.dashCooldown -= dt;
        if (playerStats.staggerImmuneTimer > 0) playerStats.staggerImmuneTimer -= dt;
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
            if (playerStats.dashTimer > 0.f) b2Body_SetBullet(bodyId, false);
            playerStats.isTurbo = false;
            playerStats.yawDriftVel = 0.f;   // the tumble owns rotation now
            playerStats.dashTimer = 0.f;
            playerStats.dashDriftTimer = 0.f;
            m_fwdThrottle = 0.f;
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
        // transform, which is a one-frame lag nobody can perceive � and far
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

        // ---- Yaw drift: off-axis drives pull the nose while thrusting ----
        // Integrated from LAST frame's throttle (movement is read below). One
        // frame of lag on a drift is invisible; a second SetTransform is not free.
        if (refit) {
            const float inertia = std::max(0.01f, kit.inertiaKgM2);
            playerStats.yawDriftVel += (kit.yawTorqueFwdNm / inertia) * 57.29578f
                * m_fwdThrottle * rcfg("yaw_drift_scale", 0.2f) * dt;
            playerStats.yawDriftVel *= std::exp(-rcfg("yaw_drift_damping", 3.0f) * dt);
            const float cap = rcfg("yaw_drift_max", 240.f);
            playerStats.yawDriftVel = std::clamp(playerStats.yawDriftVel, -cap, cap);
            tf.rotation += playerStats.yawDriftVel * dt;
        }
        m_fwdThrottle = 0.f;

        b2Body_SetTransform(bodyId, b2Pos, b2MakeRot(tf.rotation * 3.14159f / 180.f));

        // RECOVERY: aiming is back, the engines are not. The ONE thing allowed
        // is a break-out dodge (section 9), which ends the recovery.

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
        //
        // Skipped while recovering, and while a dodge burst is driving the
        // velocity (the force would be overwritten next frame anyway, and
        // feeding it into the yaw drift would twist every dodge).
        // ====================================================================
        if (recovering || playerStats.dashTimer > 0.f) {
            // no thrust
        }
        else if (playerStats.isTurbo) {
            float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
            // Turbo is the drives at full burn -- RCS does not contribute, so a
            // flat wide stern out-sprints a ship of angled quarter nozzles.
            float finalPower = (refit ? kit.forwardForceN : enginePower) * multiplier;
            b2Body_ApplyForceToCenter(phys.bodyId,
                { std::cos(noseRad) * finalPower, std::sin(noseRad) * finalPower },
                true);
            m_fwdThrottle = multiplier;
        }
        else if (playerStats.riftCharging) {
            // No thrust while charging the Rift Shot. (This branch previously
            // computed an unused `finalPower` local � removed.)
        }
        else {
            if (dx != 0 || dy != 0) {
                float length = std::sqrt(dx * dx + dy * dy);
                const float ux = dx / length, uy = dy / length;

                float force = enginePower;
                if (refit) {
                    // World input -> ship-local. Local forward is (0,-1).
                    const float r = tf.rotation * 3.14159f / 180.f;
                    const float cr = std::cos(r), sr = std::sin(r);
                    const float lx = ux * cr + uy * sr;
                    const float ly = -ux * sr + uy * cr;

                    // Each drive contributes only toward where it points.
                    float authority = 0.f;
                    for (int e = 0; e < kit.engineCount; ++e) {
                        const float along = kit.engineDir[e].x * lx + kit.engineDir[e].y * ly;
                        if (along > 0.f) authority += kit.engineForceN[e] * along;
                    }
                    // rcs_scale is THE knob for how much facing matters:
                    // 1 = class default, 2 = medium reverses at full power.
                    force = std::max(authority, kit.rcsForceN * rcfg("rcs_scale", 1.0f));
                    m_fwdThrottle = std::max(0.f, -ly);
                }

                b2Body_ApplyForceToCenter(phys.bodyId, { ux * force, uy * force }, true);
            }
        }

        // ====================================================================
        // 9. DODGE
        //
        // Refit ships: a controlled burst (see header). Legacy ships: the 1.3
        // velocity kick, untouched.
        // ====================================================================
        std::string dashKey = binds["dash"].get<std::string>();
        bool dashDown = InputRegistry::isPressed(dashKey);
        bool dashPressedThisFrame = dashDown && !m_dashWasDown;
        m_dashWasDown = dashDown;

        // ---- Burst finished last step: hand control back ----
        if (m_dashFinishing) {
            m_dashFinishing = false;
            b2Body_SetBullet(bodyId, false);
        }

        // ---- Burst in progress ----
        if (playerStats.dashTimer > 0.f) stepDash(playerStats, bodyId, dt);

        // ---- Drift: momentum bleeds off, steering stays yours ----
        // Only the component ALONG the dodge is capped, and only from above.
        // Thrust sideways or backwards acts normally the whole time.
        else if (playerStats.dashDriftTimer > 0.f && dt > 0.f) {
            const float u = 1.f - playerStats.dashDriftTimer / std::max(0.001f, playerStats.dashDriftDuration);
            const float cap = playerStats.dashExitSpeed
                + (playerStats.dashEntrySpeed - playerStats.dashExitSpeed) * u;
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float along = (v.x * playerStats.dashDir.x + v.y * playerStats.dashDir.y) * SCALE;
            if (along > cap) {
                const float cut = (along - cap) / SCALE;
                b2Body_SetLinearVelocity(bodyId, { v.x - playerStats.dashDir.x * cut,
                                                   v.y - playerStats.dashDir.y * cut });
            }
            playerStats.dashDriftTimer -= dt;
        }

        if (dashPressedThisFrame &&
            playerStats.dashTimer <= 0 &&
            playerStats.dashCooldown <= 0 &&
            playerStats.overheatTimer <= 0 &&
            playerStats.energyDrive >= dashCost) {

            float vx, vy;
            if (dx != 0 || dy != 0) {
                float len = std::sqrt(dx * dx + dy * dy);
                vx = dx / len;
                vy = dy / len;
            }
            else {
                float noseRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
                vx = std::cos(noseRad);
                vy = std::sin(noseRad);
            }

            float animScale = 1.f;
            if (refit) {
                // Linear fall from peak to the carry covers T * (peak + carry) / 2.
                // Solve for the peak that lands on dash_distance.
                const float T = feel.dashDuration;
                const float avg = feel.dashDistancePx / T;

                // Carry: a share of the burst, or what you already had if that
                // was more. Drift then returns you to your entry speed -- the
                // dodge adds a slide, not permanent momentum.
                const b2Vec2 v0 = b2Body_GetLinearVelocity(bodyId);
                const float along = std::clamp((v0.x * vx + v0.y * vy) * SCALE, 0.f, avg);
                const float carry = std::max(along, avg * feel.dashCarry);
                const float peak = 2.f * avg - carry;

                playerStats.dashDir = { vx, vy };
                playerStats.shoulderUsed = false;
                playerStats.dashPeakSpeed = peak;
                playerStats.dashExitSpeed = carry;
                playerStats.dashEntrySpeed = along;
                playerStats.dashDriftDuration = feel.dashDrift;
                playerStats.dashDriftTimer = 0.f;
                playerStats.dashDuration = T;
                playerStats.dashTimer = T;
                m_dashFinishing = false;
                b2Body_SetBullet(bodyId, true);
                stepDash(playerStats, bodyId, dt);   // this frame's physics step is the first slice

                auto& php = m_em->healths[playerIdx];
                php.invulTimer = std::max(php.invulTimer, feel.dashIframes);

                playerStats.dashMaxCooldown = T + feel.dashRecovery;
                playerStats.dashCooldown = playerStats.dashMaxCooldown;
                animScale = std::clamp(T / 0.20f, 0.7f, 1.4f);

                // Break-out: dodging out of a stagger recovery ends it.
                if (recovering) playerStats.staggerRecoverTimer = 0.f;
            }
            else {
                b2Body_SetLinearVelocity(bodyId, { vx * dashVel, vy * dashVel });
                playerStats.dashCooldown = playerStats.dashMaxCooldown;
            }
            vx *= 100.f; vy *= 100.f;   // classification below only needs a direction

            playerStats.energyDrive -= dashCost;

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
                playerStats.dashAnimDuration = animCfg("backdash_spin_duration", 0.55f) * animScale;
            }
            else if (std::abs(sd) > std::abs(f)) {
                playerStats.dashAnim = (sd > 0.f) ? DashAnim::BankRight : DashAnim::BankLeft;
                playerStats.dashAnimDuration = animCfg("dash_bank_duration", 0.45f) * animScale;
            }
            else {
                playerStats.dashAnim = DashAnim::WiggleForward;
                playerStats.dashAnimDuration = animCfg("forward_wiggle_duration", 0.35f) * animScale;
            }
            playerStats.dashAnimTimer = playerStats.dashAnimDuration;

            m_em->addTrauma(animCfg("shake_dash", 0.35f));
            m_em->cameraZoomKick = -animCfg("zoom_dash_punch", 0.07f);

            if (playerStats.energyDrive < 1.0f) {
                playerStats.energyDrive = 0;
                playerStats.overheatTimer = penaltyTime;
            }
        }

        // Recovery ends here: no parry out of a stagger, only the dodge.
        if (recovering) return;

        // ====================================================================
        // 10. PARRY MECHANIC
        // ====================================================================
        std::string parryKey = binds["parry"].get<std::string>();

        if (InputRegistry::isPressed(parryKey) &&
            playerStats.parryCooldown <= 0 &&
            playerStats.parryTimer <= 0) {
            playerStats.isParrying = true;
            playerStats.parryTimer = parryWindow;
            playerStats.parryWindowTotal = parryWindow;   // DamageSystem's perfect-parry test
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
    /// Lua `refit` table. Every key optional; defaults are the tuned values.
    float rcfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["refit"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    /**
     * @brief Drive one physics step of a dodge burst.
     *
     * Velocity is set so this step covers EXACTLY the distance the profile
     * covers over the same slice of time: v = (S(t+h) - S(t)) / dt, with
     * S(t) the integral of the linear fall from peak to exit. Sampling the
     * speed instead lost the partial last frame -- ~10% short at 60fps and
     * framerate-dependent. The final slice blends in the exit speed for the
     * part of the step past the end of the burst.
     */
    void stepDash(PlayerComponent& ps, b2BodyId body, float dt) {
        if (dt <= 0.f) return;   // hitstop freeze: hold the velocity, spend no time
        const float T = std::max(0.001f, ps.dashDuration);
        const float a = ps.dashExitSpeed, b = ps.dashPeakSpeed;
        const auto S = [&](float t) { return a * t + (b - a) * (t - t * t / (2.f * T)); };

        const float elapsed = T - ps.dashTimer;
        const float slice = std::min(dt, ps.dashTimer);
        float dist = S(elapsed + slice) - S(elapsed);
        if (slice < dt) dist += a * (dt - slice);

        const float v = dist / dt;
        b2Body_SetLinearVelocity(body, { ps.dashDir.x * v / SCALE, ps.dashDir.y * v / SCALE });

        ps.dashTimer -= dt;
        if (ps.dashTimer <= 0.f) {
            ps.dashTimer = 0.f;
            ps.dashDriftTimer = ps.dashDriftDuration;
            m_dashFinishing = true;   // CCD stays on through this last step
        }
    }

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
    float m_fwdThrottle = 0.f;        ///< 0..turbo multiplier, forward share of last frame's thrust
    bool  m_dashFinishing = false;    ///< Last burst step ran; drop CCD next frame

};