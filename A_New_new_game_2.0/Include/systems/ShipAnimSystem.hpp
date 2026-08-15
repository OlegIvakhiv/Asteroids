/**
 * @file ShipAnimSystem.hpp
 * @brief Procedural animation + stagger state machine for the player ship
 *
 * Drives three visual-only outputs on TransformComponent:
 *   visualOffsetAngle / visualPivot / visualScale
 *
 * ...plus ONE gameplay-affecting behaviour: the stagger tumble, which owns
 * tf.rotation outright while the player is knocked out of control. That is
 * deliberate — a stagger that only *looked* like a tumble while the ship
 * still aimed straight would be a lie.
 *
 * Layers, in the order they stack:
 *   1. Stagger tumble      — overrides rotation entirely, blocks everything
 *   2. Turn sway           — damped spring, follow-through on fast rotations
 *   3. Turbo stretch       — continuous, blended
 *   4. Hit shudder         — short, on HP loss
 *   5. Dash animation      — bank / spin / wiggle
 *
 * All tuning lives in the `visuals` table in player.lua.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (stagger + sway)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <cmath>
#include <cstdlib>
#include <algorithm>

class ShipAnimSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_turboBlend = 0.f;
        m_hitFlashTimer = 0.f;
        m_lastHp = -1.f;
        m_sway = 0.f;
        m_swayVel = 0.f;
        m_lastRotation = 0.f;
        m_rotationSeeded = false;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;

        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        if (idx == (size_t)-1) return;

        auto& tf = m_em->transforms[idx];
        auto& ps = m_em->players[idx];
        auto& hp = m_em->healths[idx];

        // ====================================================================
        // 0. RESET THIS FRAME'S VISUAL OFFSETS
        //    Everything below is additive on a clean slate, so a cancelled or
        //    expired animation can never leave the ship stuck at an angle.
        // ====================================================================
        tf.visualOffsetAngle = 0.f;
        tf.visualPivot = { 0.f, 0.f };
        tf.visualScale = { 1.f, 1.f };

        if (ps.parryFlashTimer > 0.f)
            ps.parryFlashTimer = std::max(0.f, ps.parryFlashTimer - dt);

        // ====================================================================
        // 1. STAGGER (runs first — it can take over rotation completely)
        // ====================================================================
        const bool tumbling = updateStagger(dt, tf, ps);

        // ====================================================================
        // 2. TURN SWAY
        // ====================================================================
        updateSway(dt, tf, ps, tumbling);

        // ====================================================================
        // 3. TURBO STRETCH
        // ====================================================================
        const float turboStretch = cfg("turbo_stretch", 1.10f);
        const float target = ps.isTurbo ? 1.f : 0.f;
        const float rate = ps.isTurbo ? 9.f : 6.f;
        m_turboBlend += (target - m_turboBlend) * (1.f - std::exp(-rate * dt));

        const float s = (turboStretch - 1.f) * m_turboBlend;
        tf.visualScale.y *= 1.f + s;
        tf.visualScale.x *= 1.f - s * 0.45f;

        // ====================================================================
        // 4. HIT SHUDDER (HP-drop edge detection — no coupling to DamageSystem)
        // ====================================================================
        if (m_lastHp >= 0.f && hp.currentHp < m_lastHp - 0.01f) {
            m_hitFlashTimer = cfg("hit_shudder_duration", 0.22f);
        }
        m_lastHp = hp.currentHp;

        if (m_hitFlashTimer > 0.f) {
            m_hitFlashTimer = std::max(0.f, m_hitFlashTimer - dt);
            const float dur = cfg("hit_shudder_duration", 0.22f);
            const float u = m_hitFlashTimer / std::max(0.0001f, dur);
            const float amp = cfg("hit_shudder_angle", 6.f);
            tf.visualOffsetAngle += amp * std::sin(u * 6.28318f * 3.f) * u * u;
        }

        // ====================================================================
        // 5. DASH ANIMATION STATE MACHINE
        // ====================================================================
        if (tumbling) { ps.dashAnim = DashAnim::None; ps.dashAnimTimer = 0.f; return; }
        if (ps.dashAnim == DashAnim::None || ps.dashAnimDuration <= 0.f) return;

        ps.dashAnimTimer -= dt;
        if (ps.dashAnimTimer <= 0.f) {
            ps.dashAnimTimer = 0.f;
            ps.dashAnim = DashAnim::None;
            return;
        }

        const float t = std::clamp(1.f - (ps.dashAnimTimer / ps.dashAnimDuration), 0.f, 1.f);

        switch (ps.dashAnim) {

        case DashAnim::BankLeft:
        case DashAnim::BankRight: {
            const float maxAngle = cfg("dash_bank_angle", 26.f);
            const float pivotY = cfg("dash_bank_pivot_y", -22.f);
            const float flip = cfg("dash_bank_sign", 1.f);
            const float side = (ps.dashAnim == DashAnim::BankRight) ? 1.f : -1.f;

            const float attack = 0.16f;
            float k;
            if (t < attack) { const float u = t / attack; k = u * u * (3.f - 2.f * u); }
            else { const float u = (t - attack) / (1.f - attack); k = (1.f - u) * std::cos(u * 3.14159f * 1.5f); }

            tf.visualOffsetAngle += -side * flip * maxAngle * k;
            tf.visualPivot = { 0.f, pivotY };
            tf.visualScale.x *= 1.f + 0.14f * std::max(0.f, k);
            break;
        }

        case DashAnim::SpinBack: {
            const float degrees = cfg("backdash_spin_degrees", 360.f);
            const float ease = 1.f - std::pow(1.f - t, 3.f);

            tf.visualOffsetAngle += -degrees * ease;
            tf.visualPivot = { 0.f, 0.f };

            const float squash = std::sin(t * 3.14159f);
            tf.visualScale.x *= 1.f + 0.10f * squash;
            tf.visualScale.y *= 1.f - 0.10f * squash;
            break;
        }

        case DashAnim::WiggleForward: {
            const float amp = cfg("forward_wiggle_angle", 8.f);
            const float cycles = cfg("forward_wiggle_cycles", 2.5f);
            const float pivotY = cfg("dash_bank_pivot_y", -22.f);

            const float damp = (1.f - t) * (1.f - t);
            tf.visualOffsetAngle += amp * std::sin(t * 6.28318f * cycles) * damp;
            tf.visualPivot = { 0.f, pivotY };
            tf.visualScale.y *= 1.f + 0.16f * std::sin(t * 3.14159f);
            break;
        }

        default: break;
        }
    }

private:
    // ========================================================================
    // STAGGER
    //
    // Two phases:
    //   TUMBLE   — this system owns tf.rotation. Input fully blocked.
    //   RECOVER  — InputSystem resumes aiming, but with a ramped turn rate so
    //              the ship visibly swings back onto target instead of
    //              snapping. Movement / dash / parry / fire stay blocked.
    //
    // @return true while tumbling
    // ========================================================================
    bool updateStagger(float dt, TransformComponent& tf, PlayerComponent& ps) {
        const size_t idx = m_em->getEntityIndex(m_playerEntityId);

        if (ps.staggerTimer > 0.f) {
            ps.staggerTimer = std::max(0.f, ps.staggerTimer - dt);

            const float u = 1.f - (ps.staggerTimer / std::max(0.0001f, ps.staggerDuration)); // 0..1

            // Spin bleeds off exponentially — violent at first, drifting at the end.
            const float decay = std::exp(-cfg("stagger_spin_decay", 2.6f) * u);
            tf.rotation += ps.staggerSpinSpeed * decay * dt;

            while (tf.rotation > 360.f) tf.rotation -= 360.f;
            while (tf.rotation < 0.f)   tf.rotation += 360.f;

            // Keep the physics hull aligned with the tumbling visual.
            if (idx != (size_t)-1) {
                const b2BodyId body = m_em->physics[idx].bodyId;
                const b2Vec2 pos = b2Body_GetPosition(body);
                b2Body_SetTransform(body, pos, b2MakeRot(tf.rotation * 3.14159f / 180.f));
            }

            // Wobble + sustained low rumble while out of control.
            const float wob = 0.06f * std::sin(u * 34.f);
            tf.visualScale.x *= 1.f + wob;
            tf.visualScale.y *= 1.f - wob;
            m_em->cameraTrauma = std::max(m_em->cameraTrauma,
                cfg("stagger_trauma_floor", 0.18f) * (1.f - u));

            // Damage smoke venting off the hull.
            if ((rand() % 100) < 45) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    tf.position + sf::Vector2f(std::cos(a) * 14.f, std::sin(a) * 14.f),
                    sf::Vector2f(std::cos(a) * 45.f, std::sin(a) * 45.f),
                    sf::Color(255, static_cast<uint8_t>(120 + rand() % 60), 40, 200),
                    0.35f + (rand() % 25) / 100.f,
                    0.6f,
                    2.f + (rand() % 3)
                    });
            }

            if (ps.staggerTimer <= 0.f) {
                ps.staggerRecoverTimer = ps.staggerRecoverDuration;
            }
            return true;
        }

        if (ps.staggerRecoverTimer > 0.f) {
            ps.staggerRecoverTimer = std::max(0.f, ps.staggerRecoverTimer - dt);
            if (ps.staggerRecoverTimer <= 0.f) {
                ps.isStaggered = false;
                ps.staggerSpinSpeed = 0.f;
            }
        }
        return false;
    }

    // ========================================================================
    // TURN SWAY
    //
    // Measure how fast tf.rotation is actually changing, then drive a spring
    // whose rest target lags OPPOSITE the turn. When the turn stops, the
    // target snaps to zero and the underdamped spring carries the tail past
    // centre before settling — that overshoot is the whole effect.
    //
    // The gate is the important part. Slow rotations produce literally
    // nothing, and the parry spin / stagger tumble are excluded outright:
    // a 360-degree parry spin over 0.6s registers as ~600 deg/sec and would
    // otherwise peg the spring at its clamp for a second afterwards.
    // ========================================================================
    void updateSway(float dt, TransformComponent& tf, PlayerComponent& ps, bool tumbling) {
        if (!m_rotationSeeded) { m_lastRotation = tf.rotation; m_rotationSeeded = true; }

        // Wrap-safe angular velocity. Without the wrap, crossing 359 -> 1
        // reads as -358 degrees in one frame and slams the spring.
        float delta = tf.rotation - m_lastRotation;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;
        m_lastRotation = tf.rotation;

        const float angVel = (dt > 1e-5f) ? (delta / dt) : 0.f;   // degrees/sec

        const bool excluded = tumbling
            || ps.staggerRecoverTimer > 0.f
            || ps.parryAnimTimer > 0.f
            || ps.dashAnim == DashAnim::SpinBack;

        float targetSway = 0.f;
        if (!excluded) {
            const float lo = cfg("sway_min_speed", 90.f);    // below this: nothing at all
            const float hi = cfg("sway_full_speed", 420.f);
            const float mag = std::abs(angVel);

            float g = std::clamp((mag - lo) / std::max(1.f, hi - lo), 0.f, 1.f);
            g = g * g * (3.f - 2.f * g);   // smoothstep, so the effect eases in

            const float sign = (angVel > 0.f) ? 1.f : -1.f;
            targetSway = -sign * cfg("sway_max_angle", 9.f) * g * cfg("sway_sign", 1.f);
        }

        // Damped spring. Underdamped on purpose: that IS the overshoot.
        // Critical damping for k=90 would be c = 2*sqrt(90) ~= 19, so 11 leaves
        // roughly one visible bounce before it settles.
        const float k = cfg("sway_stiffness", 90.f);
        const float c = cfg("sway_damping", 11.f);

        m_swayVel += ((targetSway - m_sway) * k - m_swayVel * c) * dt;
        m_sway += m_swayVel * dt;

        const float limit = cfg("sway_max_angle", 9.f) * 2.f;
        m_sway = std::clamp(m_sway, -limit, limit);

        if (excluded && std::abs(m_sway) < 0.05f) { m_sway = 0.f; m_swayVel = 0.f; }

        tf.visualOffsetAngle += m_sway;

        // Only claim the pivot if no dash animation is going to want it.
        if (ps.dashAnim == DashAnim::None) {
            tf.visualPivot = { 0.f, cfg("dash_bank_pivot_y", -22.f) };
        }
    }

    float cfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    uint32_t m_playerEntityId = 0;

    float m_turboBlend = 0.f;
    float m_hitFlashTimer = 0.f;
    float m_lastHp = -1.f;

    // ---- Sway spring state ----
    float m_sway = 0.f;
    float m_swayVel = 0.f;
    float m_lastRotation = 0.f;
    bool  m_rotationSeeded = false;
};