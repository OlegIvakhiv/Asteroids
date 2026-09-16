/**
 * @file ParticleSystem.hpp
 * @brief Manages visual particle effects (explosions, sparks, debris)
 *
 * Particles are short-lived visual elements with position, velocity,
 * lifetime, and fading alpha. They are stored in EntityManager and
 * updated/drawn by this system. Uses swap-and-pop for O(1) removal
 * when particles expire. Particles are drawn in world space using
 * a single vertex array for performance.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (refactored)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>

 /**
  * @class ParticleSystem
  * @brief Updates and renders all active particles
  *
  * Particles have position, velocity, lifetime, and fading alpha.
  * Uses swap-and-pop for O(1) removal when particles expire.
  * All particles are drawn as a single vertex array for performance.
  */
class ParticleSystem : public ISystem {
public:
    /// No legitimate particle is bigger than a ship. Anything past this is a
    /// bug upstream, and clamping keeps it from taking the frame with it.
    static constexpr float MAX_PARTICLE_SIZE = 48.f;

    /**
     * @brief Initialise the system with the global context
     * @param ctx SystemContext containing all engine dependencies
     *
     * Stores pointers to EntityManager and SFML window.
     */
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
    }

    /**
     * @brief Update and render all particles
     * @param dt Delta time in seconds
     *
     * Called every frame from SystemManager. Performs:
     * 1. Moves particles (position += velocity * dt)
     * 2. Reduces lifetime and fades alpha
     * 3. Removes expired particles (swap-and-pop)
     * 4. Draws all remaining particles as a vertex array
     *
     * Note: Particles are drawn in world space, so the view should
     * be set to the game view before calling this method.
     */
    void update(float dt) override {
        if (!m_em || !m_window) return;

        // ====================================================================
        // 1. UPDATE PARTICLES
        // ====================================================================
        // Iterate backwards for safe removal (swap-and-pop)
        for (size_t i = m_em->particles.size(); i-- > 0; ) {
            auto& p = m_em->particles[i];

            // Move
            p.position += p.velocity * dt;
            p.lifetime -= dt;

            // Remove if expired
            if (p.lifetime <= 0) {
                // Swap with last and pop (O(1) removal)
                m_em->particles[i] = m_em->particles.back();
                m_em->particles.pop_back();
                continue;
            }

            // Fade alpha based on remaining lifetime.
            // maxLifetime is guarded: a particle spawned with 0 there used to
            // divide by zero, and the resulting inf/NaN alpha is undefined
            // behaviour on the cast.
            const float ratio = std::clamp(
                p.lifetime / std::max(0.0001f, p.maxLifetime), 0.f, 1.f);
            p.color.a = static_cast<uint8_t>(255.f * ratio);
        }

        // ====================================================================
        // 2. DRAW PARTICLES
        // ====================================================================
        drawParticles();
    }

    /**
     * @brief Draw all particles as a single vertex array
     *
     * Called internally from update(). Uses sf::VertexArray with
     * sf::PrimitiveType::Triangles (each particle is a quad = 2 triangles).
     * This is more performant than drawing each particle individually.
     *
     * Particles are drawn in world space (pixel coordinates) using
     * the currently active view.
     */
    void drawParticles() const {
        if (!m_window || m_em->particles.empty()) return;

        sf::VertexArray va(sf::PrimitiveType::Triangles, m_em->particles.size() * 6);

        for (size_t i = 0; i < m_em->particles.size(); ++i) {
            size_t idx = i * 6;
            const auto& p = m_em->particles[i];

            // ================================================================
            // SANITY GUARD
            // ================================================================
            // Every particle shares ONE vertex array, so a single bad particle
            // is not a local glitch: a NaN or absurd coordinate produces a
            // triangle that smears across the whole map, and an unwritten
            // vertex sits at world (0,0) and draws a sliver from the origin to
            // wherever the effect was. Either one looks like "the particles
            // stretched out". Degenerate particles are collapsed to zero-area
            // quads at the origin instead of being skipped -- skipping would
            // leave exactly the unwritten vertices this is guarding against.
            const bool finite =
                std::isfinite(p.position.x) && std::isfinite(p.position.y) &&
                std::isfinite(p.size);
            const float halfSize = finite
                ? std::clamp(p.size, 0.f, MAX_PARTICLE_SIZE) * 0.5f : 0.f;
            const sf::Vector2f c = finite ? p.position : sf::Vector2f(0.f, 0.f);
            const sf::Color col = finite ? p.color : sf::Color(0, 0, 0, 0);

            // Each particle is a quad (2 triangles = 6 vertices)
            // Triangle 1: v0-v1-v2
            va[idx + 0] = { {c.x - halfSize, c.y - halfSize}, col };
            va[idx + 1] = { {c.x + halfSize, c.y - halfSize}, col };
            va[idx + 2] = { {c.x - halfSize, c.y + halfSize}, col };

            // Triangle 2: v1-v3-v2
            va[idx + 3] = { {c.x + halfSize, c.y - halfSize}, col };
            va[idx + 4] = { {c.x + halfSize, c.y + halfSize}, col };
            va[idx + 5] = { {c.x - halfSize, c.y + halfSize}, col };
        }

        m_window->draw(va);
    }

private:
    // ---- System dependencies (set via init) ----
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
};