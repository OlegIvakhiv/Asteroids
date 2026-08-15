/**
 * @file DebrisSystem.hpp
 * @brief Decorative, non-physical rock shards thrown off by fracturing asteroids
 *
 * WHY THIS EXISTS SEPARATELY FROM PARTICLES AND FROM ASTEROIDS:
 *
 *   Particles are dots. They read as dust, sparks and smoke — fine for an
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
 * Everything renders as ONE vertex array of triangles — same single-draw-call
 * convention as ParticleSystem and BackgroundSystem.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <algorithm>

class DebrisSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
    }

    void update(float dt) override {
        if (!m_em || !m_window) return;

        // ====================================================================
        // 1. SIMULATE
        // ====================================================================
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
            // none, of course — but a chunk that drifts off-screen at constant
            // speed reads as a bug, and a slight decay reads as "settling".
            const float decay = std::exp(-0.55f * dt);
            d.velocity *= decay;
            d.angularVelocity *= decay;
        }

        if (m_em->debris.empty()) return;

        // ====================================================================
        // 2. DRAW — fan-triangulate each chunk into one vertex array
        // ====================================================================
        size_t triCount = 0;
        for (const auto& d : m_em->debris) {
            if (d.pointCount >= 3) triCount += (d.pointCount - 2);
        }
        if (triCount == 0) return;

        sf::VertexArray va(sf::PrimitiveType::Triangles, triCount * 3);
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

private:
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
};