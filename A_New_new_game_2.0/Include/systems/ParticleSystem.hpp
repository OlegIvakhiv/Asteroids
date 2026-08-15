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

            // Fade alpha based on remaining lifetime
            float ratio = p.lifetime / p.maxLifetime;
            p.color.a = static_cast<uint8_t>(255 * ratio);
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
            float halfSize = p.size / 2.0f;

            // Each particle is a quad (2 triangles = 6 vertices)
            // Triangle 1: v0-v1-v2
            va[idx + 0] = { {p.position.x - halfSize, p.position.y - halfSize}, p.color };
            va[idx + 1] = { {p.position.x + halfSize, p.position.y - halfSize}, p.color };
            va[idx + 2] = { {p.position.x - halfSize, p.position.y + halfSize}, p.color };

            // Triangle 2: v1-v3-v2
            va[idx + 3] = { {p.position.x + halfSize, p.position.y - halfSize}, p.color };
            va[idx + 4] = { {p.position.x + halfSize, p.position.y + halfSize}, p.color };
            va[idx + 5] = { {p.position.x - halfSize, p.position.y + halfSize}, p.color };
        }

        m_window->draw(va);
    }

private:
    // ---- System dependencies (set via init) ----
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
};