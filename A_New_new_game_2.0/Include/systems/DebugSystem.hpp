/**
 * @file DebugSystem.hpp
 * @brief Debug rendering system for visualising collision shapes
 *
 * Draws wireframe outlines of actual physics collision shapes.
 * Uses stored PhysicsShapeData for accurate representation.
 *
 * @author Oleg Ivakhiv
 * @version 1.2
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/components.hpp"
#include <SFML/Graphics.hpp>

class DebugSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_enabled = false;
    }

    void update(float dt) override {
        if (!m_em) return;

        // ---- EXPIRE AoE MARKERS FIRST, REGARDLESS OF DEBUG STATE ----
        // This loop used to sit below the !m_enabled guard, so with F3 OFF
        // nothing ever removed entries. Every magma explosion and rift burst
        // pushes 2 markers, and they accumulated for the entire session --
        // invisible, because they were never drawn.
        //
        // Expiry is bookkeeping, not rendering, so it belongs above the guard.
        for (size_t k = m_em->debugAoEs.size(); k-- > 0; ) {
            m_em->debugAoEs[k].lifetime -= dt;
            if (m_em->debugAoEs[k].lifetime <= 0.f) {
                m_em->debugAoEs[k] = m_em->debugAoEs.back();
                m_em->debugAoEs.pop_back();
            }
        }

        if (!m_window || !m_enabled) return;

        // Iterate over all physics bodies
        for (size_t i = 0; i < m_em->physics.size() && i < m_em->physicsShapes.size(); ++i) {
            b2BodyId bodyId = m_em->physics[i].bodyId;
            if (!b2Body_IsValid(bodyId)) continue;

            // Get body user data for type
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(bodyId);
            if (!ud) continue;

            // Get physics position and rotation
            b2Vec2 b2Pos = b2Body_GetPosition(bodyId);
            b2Rot b2Rot = b2Body_GetRotation(bodyId);
            float angle = b2Rot_GetAngle(b2Rot);

            sf::Vector2f pos(b2Pos.x * SCALE, b2Pos.y * SCALE);
            const auto& shapeData = m_em->physicsShapes[i];

            // ---- Draw based on shape type ----
            sf::Color color = getColorForBodyType(ud->type);

            if (shapeData.type == PhysicsShapeData::Type::Circle) {
                // ---- Circle ----
                sf::CircleShape circle(shapeData.radius);
                circle.setPosition(pos - sf::Vector2f(shapeData.radius, shapeData.radius));
                circle.setFillColor(sf::Color::Transparent);
                circle.setOutlineThickness(2.0f);
                circle.setOutlineColor(color);
                m_window->draw(circle);

                // Draw crosshair at center
                drawCrosshair(pos, color);
            }
            else if (shapeData.type == PhysicsShapeData::Type::Polygon) {
                // ---- Polygon ----
                if (shapeData.vertices.empty()) continue;

                sf::ConvexShape polygon;
                polygon.setPointCount(static_cast<int>(shapeData.vertices.size()));

                float cosA = std::cos(angle);
                float sinA = std::sin(angle);

                for (size_t j = 0; j < shapeData.vertices.size(); ++j) {
                    const auto& localVert = shapeData.vertices[j];
                    // Transform local → world
                    sf::Vector2f worldVert;
                    worldVert.x = pos.x + (localVert.x * cosA - localVert.y * sinA);
                    worldVert.y = pos.y + (localVert.x * sinA + localVert.y * cosA);
                    polygon.setPoint(static_cast<int>(j), worldVert);
                }

                polygon.setFillColor(sf::Color::Transparent);
                polygon.setOutlineThickness(2.0f);
                polygon.setOutlineColor(color);
                m_window->draw(polygon);

                // Draw center point
                drawCrosshair(pos, color);
            }
        }

        // Lifetime is already ticked above; this loop only DRAWS.
        for (auto it = m_em->debugAoEs.begin(); it != m_em->debugAoEs.end(); ) {
            float alphaRatio = it->lifetime / it->maxLifetime;
            sf::Color drawColor = it->color;
            drawColor.a = static_cast<uint8_t>(drawColor.a * alphaRatio);

            sf::CircleShape ring(it->radius);
            ring.setOrigin({ it->radius, it->radius });
            ring.setPosition(it->position);
            ring.setFillColor(sf::Color::Transparent);
            ring.setOutlineThickness(2.0f);
            ring.setOutlineColor(drawColor);

            m_window->draw(ring);
            ++it;
        }
    }

    void toggle() { m_enabled = !m_enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    bool m_enabled = false;

    sf::Color getColorForBodyType(BodyType type) const {
        switch (type) {
        case BodyType::Player:    return sf::Color::Cyan;
        case BodyType::Asteroid:  return sf::Color(255, 165, 0); // Orange
        case BodyType::Bullet:    return sf::Color::Yellow;
        case BodyType::Enemy:     return sf::Color::Red;
        default:                  return sf::Color::White;
        }
    }

    void drawCrosshair(sf::Vector2f pos, sf::Color color) const {
        const float size = 5.f;
        sf::VertexArray cross(sf::PrimitiveType::Lines, 4);
        cross[0] = { {pos.x - size, pos.y}, color };
        cross[1] = { {pos.x + size, pos.y}, color };
        cross[2] = { {pos.x, pos.y - size}, color };
        cross[3] = { {pos.x, pos.y + size}, color };
        m_window->draw(cross);
    }
};