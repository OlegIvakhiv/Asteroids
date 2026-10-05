/**
 * @file DebrisSystem.hpp
 * @brief Decorative, non-physical pieces: rock shards and ship wreckage
 *
 * WHY THIS EXISTS SEPARATELY FROM PARTICLES AND FROM ASTEROIDS:
 *
 *   Particles are dots. They read as dust, sparks and smoke -- fine for an
 *   impact, useless for conveying that a solid object came apart. What sells a
 *   fracture is seeing angular, tumbling PIECES with straight edges.
 *
 *   But making every fragment a real Box2D asteroid is a trap: a large rock
 *   breaking into a dozen physical bodies triples the collision load, and the
 *   player now has a dozen new things that can hit them. That turns a
 *   satisfying kill into a punishment.
 *
 *   So the split is: a FEW real physical children (gameplay), plus MANY
 *   decorative chunks (spectacle). The decorative ones are drawn from the same
 *   polygon the parent actually had, so they look like genuine pieces of that
 *   specific rock rather than generic confetti.
 *
 * CHANGED in 1.1 -- SHIP WRECKAGE
 *
 *   A destroyed ship used to vanish behind its explosion. It now breaks into
 *   pieces of its own hull (EntityManager::wreckShards, cut by
 *   utils/ShipShatter.hpp) that tumble away, hold for a couple of seconds,
 *   and fade. Same contract as the rock shards: nothing collides.
 *
 *   A ship piece draws three layers, all flat:
 *     fill  -- the hull colour, charred dark
 *     skin  -- the ship's own outline where the piece still has it
 *     scar  -- the fracture face, white-orange hot at the break and cooling
 *              to raw metal over about a second. A temperature, not a glow:
 *              it is a colour change on a hard line, no bloom.
 *   While a piece is hot it sheds the odd ember into the particle list.
 *
 * Everything renders as vertex arrays of triangles -- one draw for all ship
 * pieces, one for all rock shards -- same single-draw-call convention as
 * ParticleSystem and BackgroundSystem.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (ship wreckage)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class DebrisSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
    }

    void update(float dt) override {
        if (!m_em || !m_window) return;

        simulateRocks(dt);
        simulateWreck(dt);

        // Ship pieces UNDER rock shards: a hull section is the bigger, slower
        // object, and the rock chips should read as passing in front of it.
        drawWreck();
        drawRocks();
    }

private:
    // ========================================================================
    // ROCK SHARDS
    // ========================================================================

    void simulateRocks(float dt) {
        for (size_t i = m_em->debris.size(); i-- > 0; ) {
            auto& d = m_em->debris[i];

            d.lifetime -= dt;
            if (d.lifetime <= 0.f) {
                d = m_em->debris.back();
                m_em->debris.pop_back();
                continue;
            }

            d.position += d.velocity * dt;
            d.rotation += d.angularVelocity * dt;

            // Light drag so chunks settle instead of flying forever. Space has
            // none, of course -- but a chunk that drifts off-screen at constant
            // speed reads as a bug, and a slight decay reads as "settling".
            const float decay = std::exp(-0.55f * dt);
            d.velocity *= decay;
            d.angularVelocity *= decay;
        }
    }

    void drawRocks() {
        if (m_em->debris.empty()) return;

        // Fan-triangulate each chunk into one vertex array
        size_t triCount = 0;
        for (const auto& d : m_em->debris) {
            if (d.pointCount >= 3) triCount += (d.pointCount - 2);
        }
        if (triCount == 0) return;

        sf::VertexArray& va = m_verts;
        va.resize(triCount * 3);   // every vertex is overwritten below
        size_t v = 0;

        for (const auto& d : m_em->debris) {
            if (d.pointCount < 3) continue;

            const float rad = d.rotation * 3.14159f / 180.f;
            const float cs = std::cos(rad);
            const float sn = std::sin(rad);

            // Fade over the last 40% of life, so chunks dissolve rather than
            // vanishing mid-flight.
            const float t = std::clamp(d.lifetime / std::max(0.0001f, d.maxLifetime), 0.f, 1.f);
            const float fade = std::clamp(t / 0.4f, 0.f, 1.f);

            sf::Color c = d.color;
            c.a = static_cast<uint8_t>(std::clamp(static_cast<float>(d.color.a) * fade, 0.f, 255.f));

            auto worldPt = [&](int idx) {
                const sf::Vector2f p = d.points[idx];
                return sf::Vector2f(
                    d.position.x + (p.x * cs - p.y * sn),
                    d.position.y + (p.x * sn + p.y * cs));
                };

            for (int k = 1; k + 1 < d.pointCount; ++k) {
                va[v++] = sf::Vertex{ worldPt(0),     c };
                va[v++] = sf::Vertex{ worldPt(k),     c };
                va[v++] = sf::Vertex{ worldPt(k + 1), c };
            }
        }

        m_window->draw(va);
    }

    // ========================================================================
    // SHIP WRECKAGE
    // ========================================================================

    void simulateWreck(float dt) {
        auto& ws = m_em->wreckShards;
        for (size_t i = ws.size(); i-- > 0; ) {
            auto& s = ws[i];

            s.lifetime -= dt;
            if (s.lifetime <= 0.f) {
                if (i != ws.size() - 1) s = std::move(ws.back());
                ws.pop_back();
                continue;
            }

            s.position += s.velocity * dt;
            s.rotation += s.angularVelocity * dt;
            const float decay = std::exp(-s.drag * dt);
            s.velocity *= decay;
            s.angularVelocity *= std::exp(-s.drag * 0.8f * dt);
            s.heat = std::max(0.f, s.heat - s.coolRate * dt);

            // ---- Embers off the break, while it is still hot ----
            // Sparse on purpose: one every ~0.07s from a hot piece is enough to
            // say "this just burned", and a whole kill stays under a few dozen.
            if (dt > 0.f && s.heat > 0.3f && !s.scar.empty()) {
                s.emberTimer -= dt;
                if (s.emberTimer <= 0.f) {
                    s.emberTimer = 0.05f + (rand() % 50) / 1000.f;
                    // From a point on a fracture face, not from the centre:
                    // the sparks come off where the metal tore.
                    const size_t k = static_cast<size_t>(rand()) % s.scar.size();
                    const float rad = s.rotation * 3.14159f / 180.f;
                    const float cs = std::cos(rad), sn = std::sin(rad);
                    const sf::Vector2f lp = s.scar[k];
                    const sf::Vector2f wp(s.position.x + lp.x * cs - lp.y * sn,
                        s.position.y + lp.x * sn + lp.y * cs);
                    const float a = (rand() % 360) * 3.14159f / 180.f;
                    const float sp = 15.f + rand() % 35;
                    const float life = 0.25f + (rand() % 25) / 100.f;
                    m_em->particles.push_back({ wp,
                        s.velocity * 0.4f + sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                        sf::Color(255, static_cast<uint8_t>(120 + rand() % 80), 40, 220),
                        life, life, 1.5f + (rand() % 2) });
                }
            }
        }
    }

    /// A segment as a quad -- sf::Lines is always 1px and vanishes under
    /// camera zoom-out, same reason RenderSystem::drawSegments exists.
    static void segQuad(sf::VertexArray& va, size_t& v, sf::Vector2f a, sf::Vector2f b,
        float halfW, sf::Color c)
    {
        sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        // A degenerate segment still writes its six vertices: skipping would
        // leave them at (0,0) and draw a sliver from the world origin.
        const sf::Vector2f n = (l < 1e-4f) ? sf::Vector2f(0.f, 0.f)
            : sf::Vector2f(-d.y / l * halfW, d.x / l * halfW);
        va[v++] = sf::Vertex{ a - n, c }; va[v++] = sf::Vertex{ a + n, c }; va[v++] = sf::Vertex{ b + n, c };
        va[v++] = sf::Vertex{ a - n, c }; va[v++] = sf::Vertex{ b + n, c }; va[v++] = sf::Vertex{ b - n, c };
    }

    static sf::Color lerp(sf::Color a, sf::Color b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return sf::Color(
            static_cast<uint8_t>(a.r + (b.r - a.r) * t),
            static_cast<uint8_t>(a.g + (b.g - a.g) * t),
            static_cast<uint8_t>(a.b + (b.b - a.b) * t),
            static_cast<uint8_t>(a.a + (b.a - a.a) * t));
    }

    static sf::Color withAlpha(sf::Color c, float k) {
        c.a = static_cast<uint8_t>(std::clamp(c.a * k, 0.f, 255.f));
        return c;
    }

    void drawWreck() {
        const auto& ws = m_em->wreckShards;
        if (ws.empty()) return;

        size_t count = 0;
        for (const auto& s : ws) count += s.tris.size() + (s.skin.size() / 2 + s.scar.size() / 2) * 6;
        if (count == 0) return;

        sf::VertexArray& va = m_wreckVerts;
        va.resize(count);
        size_t v = 0;

        // Hot face: white-orange. Cold face: the raw metal under the paint,
        // a lighter shade of the hull -- the cut stays visible as a fresh edge
        // after it stops burning.
        const sf::Color hot(255, 200, 120, 255);
        const sf::Color warm(255, 110, 40, 255);

        // Painter's order PER PIECE (fill, then its edges), so a piece that
        // tumbles over another covers that one's outline instead of being
        // crossed by it.
        for (const auto& s : ws) {
            const float rad = s.rotation * 3.14159f / 180.f;
            const float cs = std::cos(rad), sn = std::sin(rad);
            const auto W = [&](sf::Vector2f p) {
                return sf::Vector2f(s.position.x + p.x * cs - p.y * sn,
                    s.position.y + p.x * sn + p.y * cs);
                };

            const float alpha = std::clamp(s.lifetime / std::max(0.0001f, s.fadeTime), 0.f, 1.f);
            const sf::Color fill = withAlpha(s.fill, alpha);
            const sf::Color skin = withAlpha(s.skinColor, alpha);

            const sf::Color cold(
                static_cast<uint8_t>(std::min(255.f, s.fill.r * 2.1f + 20.f)),
                static_cast<uint8_t>(std::min(255.f, s.fill.g * 2.1f + 18.f)),
                static_cast<uint8_t>(std::min(255.f, s.fill.b * 2.1f + 16.f)), 230);
            const sf::Color scarCol = withAlpha(s.heat > 0.5f
                ? lerp(warm, hot, (s.heat - 0.5f) * 2.f)
                : lerp(cold, warm, s.heat * 2.f), alpha);

            for (const auto& p : s.tris) va[v++] = sf::Vertex{ W(p), fill };

            const float hw = s.lineWidth * 0.5f;
            for (size_t k = 0; k + 1 < s.skin.size(); k += 2)
                segQuad(va, v, W(s.skin[k]), W(s.skin[k + 1]), hw, skin);
            // The break is drawn a touch heavier than the skin while hot:
            // the eye should land on where the ship TORE.
            const float scarHw = hw * (1.f + 0.4f * s.heat);
            for (size_t k = 0; k + 1 < s.scar.size(); k += 2)
                segQuad(va, v, W(s.scar[k]), W(s.scar[k + 1]), scarHw, scarCol);
        }

        m_window->draw(va);
    }

    /// Reused every frame: resize() keeps capacity, so after the first busy
    /// frame this never touches the allocator again.
    sf::VertexArray m_verts{ sf::PrimitiveType::Triangles };
    sf::VertexArray m_wreckVerts{ sf::PrimitiveType::Triangles };

    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
};
