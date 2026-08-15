/**
 * @file CameraSystem.hpp
 * @brief Owns the world view: follow, look-ahead, speed zoom and trauma shake
 *
 * This system is the SINGLE owner of the game view. RenderSystem does not
 * centre the camera and InputSystem does not build a temporary view for mouse
 * mapping — both read the view this system writes.
 *
 * SHAKE MODEL (trauma):
 *   - Systems push trauma (0..1) into EntityManager::cameraTrauma.
 *   - Trauma decays linearly; actual shake = trauma^2, so small trauma is
 *     barely felt and large trauma is violent.
 *   - Offsets come from smooth 1D value noise, not rand().
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (star view fix)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <algorithm>

class CameraSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_view = ctx.gameView;

        m_baseSize = sf::Vector2f(m_window->getSize());
        m_zoom = 1.f;
        m_zoomKick = 0.f;
        m_time = 0.f;
        m_shakeOffset = { 0.f, 0.f };
        m_shakeAngle = 0.f;
        m_lastHp = -1.f;

        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        m_smoothCenter = (idx != (size_t)-1) ? m_em->transforms[idx].position
            : sf::Vector2f(0.f, 0.f);

        if (m_view) {
            m_view->setSize(m_baseSize);
            m_view->setCenter(m_smoothCenter);
            m_view->setRotation(sf::degrees(0.f));
        }
    }

    void update(float dt) override {
        if (!m_em || !m_window || !m_view) return;

        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        if (idx == (size_t)-1) return;

        const auto& tf = m_em->transforms[idx];
        const auto& ps = m_em->players[idx];
        const auto& hp = m_em->healths[idx];

        m_time += dt;

        // ====================================================================
        // 1. PLAYER SPEED (pixels/sec)
        // ====================================================================
        const b2Vec2 bv = b2Body_GetLinearVelocity(m_em->physics[idx].bodyId);
        const sf::Vector2f vel(bv.x * SCALE, bv.y * SCALE);
        const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);

        // ====================================================================
        // 2. TRAUMA SOURCES
        // ====================================================================

        // Damage: detected locally by watching HP, so DamageSystem needs no edits.
        if (m_lastHp >= 0.f && hp.currentHp < m_lastHp - 0.01f) {
            const float frac = std::clamp((m_lastHp - hp.currentHp) / std::max(1.f, hp.maxHp), 0.f, 1.f);
            m_em->addTrauma(cfg("shake_damage_base", 0.30f) + frac * 2.0f);
        }
        m_lastHp = hp.currentHp;

        // Turbo: a FLOOR rather than a per-frame add, so it can't stack into nausea.
        if (ps.isTurbo) {
            m_em->cameraTrauma = std::max(m_em->cameraTrauma, cfg("shake_turbo_floor", 0.22f));
        }

        // Weapon overheat vent: sustained rumble while locked out.
        if (ps.weaponOverheated) {
            m_em->cameraTrauma = std::max(m_em->cameraTrauma, cfg("shake_overheat_floor", 0.16f));
        }

        m_em->cameraTrauma = std::max(0.f, m_em->cameraTrauma - cfg("shake_decay", 1.4f) * dt);

        // ====================================================================
        // 3. SHAKE (trauma^2, smooth noise)
        // ====================================================================
        const float shake = m_em->cameraTrauma * m_em->cameraTrauma;
        const float freq = cfg("shake_frequency", 22.f);
        const float maxOff = cfg("shake_max_offset", 26.f);
        const float maxRot = cfg("shake_max_angle", 1.6f);

        m_shakeOffset.x = maxOff * shake * noise1(m_time * freq + 0.f);
        m_shakeOffset.y = maxOff * shake * noise1(m_time * freq + 137.f);
        m_shakeAngle = maxRot * shake * noise1(m_time * freq + 311.f);

        // ====================================================================
        // 4. ZOOM
        // ====================================================================
        const float refSpeed = cfg("zoom_reference_speed", 900.f);
        const float speedNorm = std::clamp(speed / refSpeed, 0.f, 1.f);

        float targetZoom = 1.f + speedNorm * cfg("zoom_speed_amount", 0.20f);
        if (ps.isTurbo) targetZoom += cfg("zoom_turbo_amount", 0.08f);

        m_zoomKick += (0.f - m_zoomKick) * (1.f - std::exp(-cfg("zoom_kick_release", 8.f) * dt));
        if (std::abs(m_em->cameraZoomKick) > 0.0001f) {
            m_zoomKick = m_em->cameraZoomKick;
            m_em->cameraZoomKick = 0.f;
        }
        targetZoom += m_zoomKick;

        m_zoom += (targetZoom - m_zoom) * (1.f - std::exp(-cfg("zoom_smoothing", 5.f) * dt));
        m_zoom = std::clamp(m_zoom, 0.75f, cfg("zoom_max", 1.32f));

        // ====================================================================
        // 5. FOLLOW + LOOK-AHEAD
        // ====================================================================
        sf::Vector2f lead(0.f, 0.f);
        if (speed > 1.f) {
            const float maxLead = cfg("camera_max_lookahead", 170.f);
            const float leadAmt = std::min(speed * cfg("camera_lookahead_factor", 0.20f), maxLead);
            lead = (vel / speed) * leadAmt;
        }

        const sf::Vector2f targetCenter = tf.position + lead;
        const float follow = cfg("camera_follow_speed", 8.f);
        m_smoothCenter += (targetCenter - m_smoothCenter) * (1.f - std::exp(-follow * dt));

        // ====================================================================
        // 6. COMMIT THE VIEW
        // ====================================================================
        m_view->setSize(m_baseSize * m_zoom);
        m_view->setCenter(m_smoothCenter + m_shakeOffset);
        m_view->setRotation(sf::degrees(m_shakeAngle));
        m_window->setView(*m_view);
    }

    // ========================================================================
    // ACCESSORS
    // ========================================================================

    const sf::View& getWorldView() const { return *m_view; }
    float getZoom()               const { return m_zoom; }
    sf::Vector2f getShakeOffset() const { return m_shakeOffset; }

    /**
     * @brief Build the view used to draw the parallax starfield
     * @param fieldSize The full extent stars are scattered across
     *                  (EntityManager::starFieldSize)
     *
     * The stars live in a FIXED rectangle [0, fieldSize] — not in world space.
     * The view must therefore be centred on the middle of that rectangle,
     * fieldSize * 0.5, and NOT on the window centre.
     *
     * This was the top/left gap: centred on windowSize * 0.5, zooming out grew
     * the visible region past x=0 and y=0 into a region where no stars were
     * ever generated. At zoom 1.32 on a 1920x1080 window that is 307px of
     * empty on the left and 173px on top. Right and bottom looked correct only
     * because the field happens to extend 1.5x in those directions.
     *
     * COVERAGE RULE: fieldSize >= windowSize * zoom_max, plus headroom for the
     * shake offset and rotation. Margin 1.5 vs zoom_max 1.32 is comfortable —
     * but if you raise zoom_max, raise the margin in initBackground() to match.
     */
    sf::View makeStarView(sf::Vector2f fieldSize) const {
        sf::View v;
        v.setSize(m_baseSize * m_zoom);
        v.setCenter(fieldSize * 0.5f + m_shakeOffset);
        v.setRotation(sf::degrees(m_shakeAngle));
        return v;
    }

    /**
     * @brief Generic screen-space view inheriting the camera's shake and zoom
     *
     * Fine for overlays that genuinely want to be centred on the window.
     * Do NOT use it for the starfield — see makeStarView().
     */
    sf::View makeScreenView(const sf::View& base) const {
        sf::View v = base;
        v.setSize(base.getSize() * m_zoom);
        v.setCenter(base.getCenter() + m_shakeOffset);
        v.setRotation(sf::degrees(m_shakeAngle));
        return v;
    }

private:
    float cfg(const char* key, float def) const {
        if (!m_lua) return def;
        sol::optional<sol::table> v = (*m_lua)["visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    // ---- Smooth 1D value noise ----
    static float hash1(int n) {
        n = (n << 13) ^ n;
        return 1.f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff)
            / 1073741824.f;
    }
    static float noise1(float x) {
        const int i = static_cast<int>(std::floor(x));
        const float f = x - static_cast<float>(i);
        const float u = f * f * (3.f - 2.f * f);
        return hash1(i) * (1.f - u) + hash1(i + 1) * u;
    }

    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    sf::View* m_view = nullptr;
    uint32_t m_playerEntityId = 0;

    sf::Vector2f m_baseSize;
    sf::Vector2f m_smoothCenter;
    sf::Vector2f m_shakeOffset;
    float m_shakeAngle = 0.f;
    float m_zoom = 1.f;
    float m_zoomKick = 0.f;
    float m_time = 0.f;
    float m_lastHp = -1.f;
};