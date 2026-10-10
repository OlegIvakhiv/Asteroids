/**
 * @file ScrapSystem.hpp
 * @brief Loose scrap: drift, the player's magnet, pickup, and drawing.
 *
 * ============================================================================
 * WHAT SCRAP IS
 * ============================================================================
 * Score is gone. Kills (and salvage) drop SCRAP -- the one currency the
 * upgrade, shop and crafting layers will all spend. It drops as a burst of
 * small amber cubes (EntityManager::spawnScrap) that the player has to fly
 * near to collect: the reward sits where the fight was, so taking it is a
 * small decision rather than a number ticking up on its own.
 *
 * ============================================================================
 * THE MAGNET
 * ============================================================================
 * Requiring physical contact with a 5px cube is a chore, so the ship carries
 * a short-range magnet (`scrap.magnet_radius`). Inside it a cube stops
 * drifting and homes:
 *
 *   target velocity = player velocity + (direction to player) x home speed
 *
 * and its velocity is blended toward that target every frame. Matching the
 * PLAYER'S velocity first is what makes it reliable: a pure "accelerate
 * toward the player" pull can never catch a ship flying away at turbo, and
 * at short range it overshoots and orbits. Home speed rises as the cube gets
 * closer, so the last stretch is a snap, not a crawl.
 *
 * `pickup_delay` keeps a fresh drop out of the magnet for a beat. Without it
 * a kill at point blank is collected the frame it happens and the player
 * never sees the burst -- which is the part that says "that was worth it".
 *
 * Uncollected scrap blinks for `blink_time` and is gone after `lifetime`:
 * the field cannot fill up with cubes, and lingering at a dead fight has a
 * clock on it.
 *
 * ============================================================================
 * FRAME PLACEMENT
 * ============================================================================
 * update() is LOGIC -- it changes the balance -- so it runs inside the
 * simulation pass (scaled time, skipped while paused or dev-frozen).
 * draw() is called from the world pass, and from the paused branch so the
 * cubes stay visible behind the pause menu.
 *
 * Every cube is two quads (rim, then face) in one vertex array: one draw.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/LuaConfig.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class ScrapSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.drawTarget();
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
    }

    /// Simulation. Run in the logic pass.
    void update(float dt) override {
        if (!m_em || dt <= 0.f) return;
        auto& list = m_em->scrapPickups;
        if (list.empty()) return;

        const float magnetR = cfg("magnet_radius", 150.f);
        const float collectR = cfg("collect_radius", 26.f);
        const float homeNear = cfg("home_speed_near", 620.f);
        const float homeFar = cfg("home_speed_far", 240.f);
        const float grip = cfg("magnet_grip", 12.f);
        const float delay = cfg("pickup_delay", 0.35f);
        const float drag = cfg("drift_drag", 1.6f);
        const float life = cfg("lifetime", 30.f);

        // ---- The player, if there is one ----
        const size_t p = m_em->getEntityIndex(m_playerEntityId);
        const bool alive = (p != (size_t)-1) && b2Body_IsValid(m_em->physics[p].bodyId);
        sf::Vector2f pPos, pVel;
        if (alive) {
            pPos = m_em->transforms[p].position;
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[p].bodyId);
            pVel = { v.x * SCALE, v.y * SCALE };
        }

        const float driftDecay = std::exp(-drag * dt);
        const float gripK = 1.f - std::exp(-grip * dt);

        for (size_t i = list.size(); i-- > 0; ) {
            auto& s = list[i];
            s.age += dt;
            s.pulled = false;

            if (s.age >= life) { removeAt(i); continue; }

            if (alive && s.age >= delay) {
                sf::Vector2f d = pPos - s.position;
                const float dist = std::sqrt(d.x * d.x + d.y * d.y);

                // ---- Collected ----
                if (dist <= collectR) {
                    m_em->scrap += s.value;
                    collectFx(s.position, pVel);
                    removeAt(i);
                    continue;
                }

                // ---- In the magnet: home on the ship ----
                if (dist < magnetR) {
                    d /= std::max(0.001f, dist);
                    const float closeness = 1.f - dist / magnetR;            // 0 edge .. 1 hull
                    const float home = homeFar + (homeNear - homeFar) * closeness * closeness;
                    const sf::Vector2f target = pVel + d * home;
                    s.velocity += (target - s.velocity) * gripK;
                    s.spin *= 1.f + 2.f * dt;                                // spins up as it's drawn in
                    s.spin = std::clamp(s.spin, -900.f, 900.f);
                    s.pulled = true;
                }
            }

            if (!s.pulled) s.velocity *= driftDecay;
            s.position += s.velocity * dt;
            s.rotation += s.spin * dt;
        }
    }

    /// Draw every cube in one call. Run in the world view.
    void draw() {
        if (!m_em || !m_window) return;
        const auto& list = m_em->scrapPickups;
        if (list.empty()) return;

        const float life = cfg("lifetime", 30.f);
        const float blink = cfg("blink_time", 5.f);

        // Amber face, pale rim: the HTML lab's cube, in the game's own amber
        // accent. Flat, no glow -- the rim is what lifts it off the starfield.
        const sf::Color face(255, 170, 0);
        const sf::Color faceHot(255, 214, 110);
        const sf::Color rim(255, 244, 214);

        size_t n = 0;
        for (const auto& s : list) n += s.pulled ? 18 : 12;
        m_verts.resize(n);
        size_t v = 0;

        for (const auto& s : list) {
            // Last seconds: blink, faster as it runs out. Hidden half the
            // time rather than faded, so it reads as a timer, not as dimming.
            bool visible = true;
            const float left = life - s.age;
            if (left < blink) {
                const float hz = 2.f + 6.f * (1.f - left / std::max(0.01f, blink));
                visible = std::fmod(s.age * hz, 1.f) < 0.6f;
            }
            const uint8_t a = visible ? 255 : 0;

            const float rad = s.rotation * 3.14159f / 180.f;
            const float cs = std::cos(rad), sn = std::sin(rad);

            // Streak behind a cube the magnet has hold of: it shows the pull
            // working before the cube arrives, and it shows where the magnet
            // edge is without drawing a ring around the ship.
            if (s.pulled) {
                const float sp = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y);
                const sf::Vector2f dir = (sp > 1.f) ? s.velocity / sp : sf::Vector2f(0.f, 0.f);
                const sf::Vector2f tail = s.position - dir * std::min(22.f, sp * 0.035f);
                const sf::Vector2f nrm(-dir.y * s.size * 0.3f, dir.x * s.size * 0.3f);
                const sf::Color head(255, 200, 90, static_cast<uint8_t>(a * 0.7f));
                const sf::Color clear(255, 160, 40, 0);
                m_verts[v++] = sf::Vertex{ s.position + nrm, head };
                m_verts[v++] = sf::Vertex{ s.position - nrm, head };
                m_verts[v++] = sf::Vertex{ tail, clear };
                m_verts[v++] = sf::Vertex{ tail, clear };
                m_verts[v++] = sf::Vertex{ tail, clear };
                m_verts[v++] = sf::Vertex{ tail, clear };
            }

            const auto square = [&](float half, sf::Color c) {
                const sf::Vector2f ax(cs * half, sn * half), ay(-sn * half, cs * half);
                const sf::Vector2f p0 = s.position - ax - ay, p1 = s.position + ax - ay;
                const sf::Vector2f p2 = s.position + ax + ay, p3 = s.position - ax + ay;
                m_verts[v++] = sf::Vertex{ p0, c }; m_verts[v++] = sf::Vertex{ p1, c }; m_verts[v++] = sf::Vertex{ p2, c };
                m_verts[v++] = sf::Vertex{ p0, c }; m_verts[v++] = sf::Vertex{ p2, c }; m_verts[v++] = sf::Vertex{ p3, c };
                };
            const float half = s.size * 0.5f;
            sf::Color r = rim;  r.a = a;
            sf::Color f = s.pulled ? faceHot : face;  f.a = a;
            square(half + 0.9f, r);
            square(half, f);
        }

        m_window->draw(m_verts);
    }

private:
    void removeAt(size_t i) {
        auto& list = m_em->scrapPickups;
        list[i] = list.back();
        list.pop_back();
    }

    /// A tiny amber blink where the cube went in. Small on purpose: several
    /// of these land in the same few frames after every kill.
    void collectFx(sf::Vector2f at, sf::Vector2f pVel) {
        for (int k = 0; k < 3; ++k) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = 40.f + rand() % 60;
            m_em->particles.push_back({ at,
                pVel + sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                sf::Color(255, 210, 110, 230), 0.18f, 0.18f, 2.f });
        }
    }

    /// Lua `scrap` table (player.lua), cached per config epoch.
    luacfg::Table m_cfg{ "scrap" };
    float cfg(const char* key, float def) const { return m_cfg.get(m_lua, key, def); }

    sf::VertexArray m_verts{ sf::PrimitiveType::Triangles };

    EntityManager* m_em = nullptr;
    sf::RenderTarget* m_window = nullptr;   ///< ctx.drawTarget(): the window, or a hidden world's texture
    sol::state* m_lua = nullptr;
    uint32_t m_playerEntityId = 0;
};
