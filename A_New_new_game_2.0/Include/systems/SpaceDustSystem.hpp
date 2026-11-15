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
 *  1. WORLD SPACE, TOROIDAL WRAP. The motes don't move � the camera does.
 *     Any mote that leaves the region around the camera is teleported to the
 *     opposite edge, so a fixed-size pool covers infinite space. O(n) per
 *     frame, no allocation, no spawning/despawning.
 *
 *  2. VELOCITY STREAKS, NOT DOTS. Each mote is drawn as a line whose length
 *     scales with the player's speed. At rest they're invisible specks; at
 *     turbo they're long streaks. This is the entire effect � dots alone do
 *     almost nothing.
 *
 *  3. FAKE DEPTH. Each mote has a `depth` factor scaling its size, alpha and
 *     streak length. True parallax would require the motes to move at
 *     different rates, which breaks the world-space assumption � and the
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
#include "utils/LuaConfig.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <random>
#include <algorithm>

class SpaceDustSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.drawTarget();
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_view = ctx.gameView;
        m_zone = ctx.zone;

        m_rng.seed(std::random_device{}());

        m_motes.clear();

        // Seed the field across the whole view round the player's start.
        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        const sf::Vector2f origin = (idx != (size_t)-1) ? m_em->transforms[idx].position
            : sf::Vector2f(0.f, 0.f);
        m_half = halfExtent();
        const int count = targetCount(m_half);
        m_motes.reserve(count);
        for (int i = 0; i < count; ++i) m_motes.push_back(makeMote(origin, m_half, { 0.f, 0.f }));
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
        // 0. THE FIELD FOLLOWS THE VIEW'S SIZE
        //    `dust_count` is the density at a 1920x1080 view. A bigger window,
        //    or the camera pulling out (a boss arena), opens a ring round the
        //    old box: it gets its share of new motes there, where the eye
        //    has not seen any yet. Zooming back in drops the ones now outside
        //    instead of wrapping them all onto the edges in a heap.
        // ====================================================================
        if (std::fabs(half.x - m_half.x) > m_half.x * 0.03f || std::fabs(half.y - m_half.y) > m_half.y * 0.03f) {
            resize(camCenter, m_half, half);
            m_half = half;
        }

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
        float depth = 1.f;       ///< 0.35..1.0 � scales size, alpha, streak
        float size = 2.f;        ///< Unused for lines, kept for a dot fallback
    };

    void draw(const sf::Vector2f& vel, float speed) {
        // ---- Speed gate: fade the whole field in as you accelerate ----
        const float fadeIn = cfg("dust_fade_in_speed", 120.f);
        const float fadeFull = cfg("dust_full_speed", 700.f);
        const float t = std::clamp((speed - fadeIn) / std::max(1.f, fadeFull - fadeIn), 0.f, 1.f);
        if (t <= 0.001f) return;

        // ---- Streak geometry ----
        // The motes are static; the CAMERA moves. So on screen a mote appears
        // to travel opposite to the player, and its trail extends BACK along
        // the player's velocity vector � i.e. where the mote came from.
        const float maxStreak = cfg("dust_max_streak", 90.f);
        const float streakPerSpeed = cfg("dust_streak_per_speed", 0.055f);

        sf::Vector2f dir(0.f, 0.f);
        if (speed > 0.001f) dir = vel / speed;
        const float streakLen = std::min(speed * streakPerSpeed, maxStreak);

        // ---- Tint: the zone owns it when one is loaded ----
        // Dust is the closest thing this game has to atmosphere, so it carries
        // more of a zone's identity than anything except the props. Falling
        // back to `visuals` keeps an install with no zones.lua identical.
        float baseAlpha = cfg("dust_alpha", 150.f);
        sf::Color tint(
            static_cast<uint8_t>(cfg("dust_color_r", 170.f)),
            static_cast<uint8_t>(cfg("dust_color_g", 200.f)),
            static_cast<uint8_t>(cfg("dust_color_b", 255.f)));

        if (m_zone) {
            if (m_zone->blend.active) {
                tint = m_zone->blend.dustColor;
                baseAlpha = m_zone->blend.dustAlpha;
            }
            else if (const zonearch::ZoneDef* z = m_zone->def()) {
                tint = z->dustColor;
                baseAlpha = z->dustAlpha;
            }
            // Near the Void the dust stops: there is nothing out there to drift.
            baseAlpha *= 1.f - std::clamp(m_zone->voidFade, 0.f, 1.f);
        }

        sf::VertexArray& va = m_verts;
        va.resize(m_motes.size() * 2);   // every vertex is overwritten below

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

    /// Motes for a box of this half-size at `dust_count` per 1920x1080 view.
    int targetCount(sf::Vector2f half) const {
        const float pad = cfg("dust_region_padding", 1.25f);
        const float ref = (1920.f * pad) * (1080.f * pad);
        const float n = cfg("dust_count", 240.f) * (4.f * half.x * half.y) / ref;
        return std::clamp(static_cast<int>(n), 1, static_cast<int>(cfg("dust_max", 1400.f)));
    }

    /// A mote somewhere in the box round `c`, outside `hole` (half-size;
    /// zero = anywhere).
    Mote makeMote(sf::Vector2f c, sf::Vector2f half, sf::Vector2f hole) {
        std::uniform_real_distribution<float> dx(-half.x, half.x);
        std::uniform_real_distribution<float> dy(-half.y, half.y);
        std::uniform_real_distribution<float> dd(0.35f, 1.0f);
        Mote m;
        sf::Vector2f o(dx(m_rng), dy(m_rng));
        for (int t = 0; t < 12 && std::fabs(o.x) < hole.x && std::fabs(o.y) < hole.y; ++t) o = { dx(m_rng), dy(m_rng) };
        m.position = c + o;
        m.depth = dd(m_rng);
        m.size = 1.0f + m.depth * 1.6f;
        return m;
    }

    void resize(sf::Vector2f c, sf::Vector2f oldHalf, sf::Vector2f half) {
        // Shrinking: drop what is now outside.
        m_motes.erase(std::remove_if(m_motes.begin(), m_motes.end(), [&](const Mote& m) {
            return std::fabs(m.position.x - c.x) > half.x || std::fabs(m.position.y - c.y) > half.y;
            }), m_motes.end());
        // Growing: fill the new ring (or top up anywhere after a shrink).
        const int want = targetCount(half);
        const sf::Vector2f hole(std::min(oldHalf.x, half.x), std::min(oldHalf.y, half.y));
        while (static_cast<int>(m_motes.size()) < want)
            m_motes.push_back(makeMote(c, half, (half.x > oldHalf.x || half.y > oldHalf.y) ? hole : sf::Vector2f(0.f, 0.f)));
        if (static_cast<int>(m_motes.size()) > want) m_motes.resize(static_cast<size_t>(want));
    }

    /// Half-size of the region motes are kept inside, padded for zoom-out.
    sf::Vector2f halfExtent() const {
        sf::Vector2f size = m_view ? m_view->getSize() : sf::Vector2f(m_window->getSize());
        const float pad = cfg("dust_region_padding", 1.25f);
        return { size.x * 0.5f * pad, size.y * 0.5f * pad };
    }

    /// Lua `visuals` table, cached per config epoch (see LuaConfig.hpp).
    luacfg::Table m_cfgVisuals{ "visuals" };
    float cfg(const char* key, float def) const {
        return m_cfgVisuals.get(m_lua, key, def);
    }

    /// Reused every frame: resize() keeps capacity, so after the first busy
    /// frame this never touches the allocator again.
    sf::VertexArray m_verts{ sf::PrimitiveType::Lines };

    EntityManager* m_em = nullptr;
    sf::RenderTarget* m_window = nullptr;   ///< ctx.drawTarget(): the window, or a hidden world's texture
    sol::state* m_lua = nullptr;
    sf::View* m_view = nullptr;
    zonearch::ZoneState* m_zone = nullptr;
    uint32_t m_playerEntityId = 0;

    std::vector<Mote> m_motes;
    sf::Vector2f m_half{ 0.f, 0.f };   ///< The box the field was last sized for
    mutable std::mt19937 m_rng;
};