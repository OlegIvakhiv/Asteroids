/**
 * @file BackgroundSystem.hpp
 * @brief Parallax scrolling starfield background system
 *
 * Manages a field of stars that move opposite to player velocity with
 * varying parallax factors. Distant stars (low parallax) move slower,
 * creating a depth illusion. Stars wrap around screen edges for infinite
 * scrolling. Rendered as a single vertex array for performance.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (refactored)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/GameConfig.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>

 /**
  * @class BackgroundSystem
  * @brief Parallax scrolling starfield system
  *
  * Stars move opposite to player velocity with varying parallax factors.
  * Distant stars (low parallax) move slower, creating depth illusion.
  * Stars wrap around screen edges for infinite scrolling.
  * Rendered as a vertex array for maximum performance (single draw call).
  */
class BackgroundSystem : public ISystem {
public:
    /**
     * @brief Initialise the system with the global context
     * @param ctx SystemContext containing all engine dependencies
     *
     * Stores pointers to EntityManager, SFML window, and player ID.
     * The stars are already generated in EntityManager::initBackground().
     */
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_playerEntityId = ctx.playerEntityId;
    }

    /**
     * @brief Update and render the background starfield
     * @param dt Delta time in seconds
     *
     * Called every frame from SystemManager. Performs:
     * 1. Fetches player velocity from the physics body
     * 2. Moves stars opposite to player velocity (parallax)
     * 3. Wraps stars around screen edges
     * 4. Draws all stars as a single vertex array
     *
     * Note: Stars are drawn in screen space (using the default view),
     * not in world space, to keep them fixed relative to the screen.
     */
    void update(float dt) override {
        if (!m_em || !m_window) return;

        // ---- 1. Get player velocity ----
        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        b2Vec2 b2Vel = b2Body_GetLinearVelocity(m_em->physics[playerIdx].bodyId);
        sf::Vector2f playerVelocity(b2Vel.x * SCALE, b2Vel.y * SCALE);

        // ---- 2. Update star positions (parallax) ----
        const sf::Vector2f bounds = m_em->starFieldSize;
        for (auto& star : m_em->stars) {
            star.position -= playerVelocity * dt * star.parallaxFactor;

            if (star.position.x < 0)        star.position.x += bounds.x;
            if (star.position.x > bounds.x) star.position.x -= bounds.x;
            if (star.position.y < 0)        star.position.y += bounds.y;
            if (star.position.y > bounds.y) star.position.y -= bounds.y;
        }

        // ---- 3. Draw stars as a single vertex array ----
        // Stars use the default view (screen space), not the game view.
        // The SystemManager should set the default view before calling draw.
        drawStars();
    }

    /**
     * @brief Draw all stars as a single vertex array
     *
     * Called internally from update(). Uses a vertex array with
     * sf::PrimitiveType::Triangles (each star is a quad = 2 triangles).
     * This is more performant than drawing each star individually.
     *
     * The stars are drawn in screen space (pixel coordinates) using
     * the default view, so they stay fixed relative to the screen.
     */
    void drawStars() const {
        if (!m_window || m_em->stars.empty()) return;

        sf::VertexArray va(sf::PrimitiveType::Triangles);

        // Reserve memory for better performance (6 vertices per star)
        va.resize(m_em->stars.size() * 6);

        size_t vertexIndex = 0;
        for (const auto& star : m_em->stars) {
            float halfSize = star.size / 2.0f;
            sf::Vector2f p = star.position;

            // Each star is a quad (2 triangles = 6 vertices)
            // Triangle 1: v0-v1-v2
            va[vertexIndex + 0] = { {p.x - halfSize, p.y - halfSize}, star.color };  // top-left
            va[vertexIndex + 1] = { {p.x + halfSize, p.y - halfSize}, star.color };  // top-right
            va[vertexIndex + 2] = { {p.x - halfSize, p.y + halfSize}, star.color };  // bottom-left

            // Triangle 2: v1-v3-v2
            va[vertexIndex + 3] = { {p.x + halfSize, p.y - halfSize}, star.color };  // top-right
            va[vertexIndex + 4] = { {p.x + halfSize, p.y + halfSize}, star.color };  // bottom-right
            va[vertexIndex + 5] = { {p.x - halfSize, p.y + halfSize}, star.color };  // bottom-left

            vertexIndex += 6;
        }

        m_window->draw(va);
    }

private:
    // ---- System dependencies (set via init) ----
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    uint32_t m_playerEntityId = 0;
};