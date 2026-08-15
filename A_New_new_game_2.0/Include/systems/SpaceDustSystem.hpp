/**
 * @file SpaceDustSystem.hpp
 * @brief Near-field motion particles that give empty space a speed reference
 *
 * The starfield already sells DEPTH. What it can't sell is SPEED, because
 * it lives in screen space and every star moves the same way. This system
 * fills the gap: a cloud of motes that sit in WORLD space right next to the
 * ship, so they whip past you and give the eye something to measure against.
 *
 * Key design decisions:
 *
 *  1. WORLD SPACE, TOROIDAL WRAP. The motes don't move — the camera does.
 *     Any mote that leaves the region around the camera is teleported to the
 *     opposite edge, so a fixed-size pool covers infinite space. O(n) per
 *     frame, no allocation, no spawning/despawning.
 *
 *  2. VELOCITY STREAKS, NOT DOTS. Each mote is drawn as a line whose length
 *     scales with the player's speed. At rest they're invisible specks; at
 *     turbo they're long streaks. This is the entire effect — dots alone do
 *     almost nothing.
 *
 *  3. FAKE DEPTH. Each mote has a `depth` factor scaling its size, alpha and
 *     streak length. True parallax would require the motes to move at
 *     different rates, which breaks the world-space assumption — and the
 *     starfield already covers real parallax anyway.
 *
 *  4. SPEED-GATED ALPHA. Below a threshold the whole field fades out, so
 *     drifting slowly through space stays clean and readable.
 *
 * Drawn as ONE vertex array (2 verts per mote), matching the existing
 * single-draw-call convention of ParticleSystem and BackgroundSystem.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <random>
#include <algorithm>

class SpaceDustSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_view = ctx.gameView;

        m_rng.seed(std::random_device{}());

        const int count = static_cast<int>(cfg("dust_count", 240.f));
        m_motes.clear();
        m_motes.reserve(count);

        // Seed the field across a generous box around the player's start.
        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        const sf::Vector2f origin = (idx != (size_t)-1) ? m_em->transforms[idx].position
            : sf::Vector2f(0.f, 0.f);
        const sf::Vector2f half = halfExtent();

        std::uniform_real_distribution<float> dx(-half.x, half.x);
        std::uniform_real_distribution<float> dy(-half.y, half.y);
        std::uniform_real_distribution<float> dd(0.35f, 1.0f);

        for (int i = 0; i < count; ++i) {
            Mote m;
            m.position = origin + sf::Vector2f(dx(m_rng), dy(m_rng));
            m.depth = dd(m_rng);
            m.size = 1.0f + m.depth * 1.6f;
            m_motes.push_back(m);
        }
    }

    void update(float dt) override {
        (void)dt;
        if (!m_em || !m_window || m_motes.empty()) return;

        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        if (idx == (size_t)-1) return;

        // ---- Camera centre & player velocity ----
        const sf::Vector2f camCenter = m_view ? m_view->getCenter()
            : m_em->transforms[idx].position;

        const b2Vec2 bv = b2Body_GetLinearVelocity(m_em->physics[idx].bodyId);
        const sf::Vector2f vel(bv.x * SCALE, bv.y * SCALE);
        const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);

        const sf::Vector2f half = halfExtent();

        // ====================================================================
        // 1. TOROIDAL WRAP AROUND THE CAMERA
        //    A small perpendicular jitter on wrap stops the same motes from
        //    retracing an identical corridor, which is otherwise very visible
        //    when you fly in a straight line.
        // ====================================================================
        std::uniform_real_distribution<float> jx(-half.x, half.x);
        std::uniform_real_distribution<float> jy(-half.y, half.y);

        for (auto& m : m_motes) {
            sf::Vector2f d = m.position - camCenter;

            if (d.x > half.x) { m.position.x -= half.x * 2.f; m.position.y = camCenter.y + jy(m_rng); }
            else if (d.x < -half.x) { m.position.x += half.x * 2.f; m.position.y = camCenter.y + jy(m_rng); }

            d = m.position - camCenter;
            if (d.y > half.y) { m.position.y -= half.y * 2.f; m.position.x = camCenter.x + jx(m_rng); }
            else if (d.y < -half.y) { m.position.y += half.y * 2.f; m.position.x = camCenter.x + jx(m_rng); }
        }

        // ====================================================================
        // 2. DRAW
        // ====================================================================
        draw(vel, speed);
    }

private:
    struct Mote {
        sf::Vector2f position;   ///< World position (pixels)
        float depth = 1.f;       ///< 0.35..1.0 — scales size, alpha, streak
        float size = 2.f;        ///< Unused for lines, kept for a dot fallback
    };

    void draw(const sf::Vector2f& vel, float speed) const {
        // ---- Speed gate: fade the whole field in as you accelerate ----
        const float fadeIn = cfg("dust_fade_in_speed", 120.f);
        const float fadeFull = cfg("dust_full_speed", 700.f);
        const float t = std::clamp((speed - fadeIn) / std::max(1.f, fadeFull - fadeIn), 0.f, 1.f);
        if (t <= 0.001f) return;

        // ---- Streak geometry ----
        // The motes are static; the CAMERA moves. So on screen a mote appears
        // to travel opposite to the player, and its trail extends BACK along
        // the player's velocity vector — i.e. where the mote came from.
        const float maxStreak = cfg("dust_max_streak", 90.f);
        const float streakPerSpeed = cfg("dust_streak_per_speed", 0.055f);

        sf::Vector2f dir(0.f, 0.f);
        if (speed > 0.001f) dir = vel / speed;
        const float streakLen = std::min(speed * streakPerSpeed, maxStreak);

        const auto baseAlpha = cfg("dust_alpha", 150.f);
        const sf::Color tint(
            static_cast<uint8_t>(cfg("dust_color_r", 170.f)),
            static_cast<uint8_t>(cfg("dust_color_g", 200.f)),
            static_cast<uint8_t>(cfg("dust_color_b", 255.f)));

        sf::VertexArray va(sf::PrimitiveType::Lines, m_motes.size() * 2);

        for (size_t i = 0; i < m_motes.size(); ++i) {
            const Mote& m = m_motes[i];
            const float len = streakLen * m.depth;

            const uint8_t aHead = static_cast<uint8_t>(
                std::clamp(baseAlpha * t * m.depth, 0.f, 255.f));

            sf::Color head = tint; head.a = aHead;
            sf::Color tail = tint; tail.a = static_cast<uint8_t>(aHead / 6);  // fades to nothing

            va[i * 2 + 0] = sf::Vertex{ m.position, head };
            va[i * 2 + 1] = sf::Vertex{ m.position + dir * len, tail };
        }

        m_window->draw(va);
    }

    /// Half-size of the region motes are kept inside, padded for zoom-out.
    sf::Vector2f halfExtent() const {
        sf::Vector2f size = m_view ? m_view->getSize() : sf::Vector2f(m_window->getSize());
        const float pad = cfg("dust_region_padding", 1.25f);
        return { size.x * 0.5f * pad, size.y * 0.5f * pad };
    }

    float cfg(const char* key, float def) const {
        if (!m_lua) return def;
        sol::optional<sol::table> v = (*m_lua)["visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    sf::View* m_view = nullptr;
    uint32_t m_playerEntityId = 0;

    std::vector<Mote> m_motes;
    mutable std::mt19937 m_rng;
};