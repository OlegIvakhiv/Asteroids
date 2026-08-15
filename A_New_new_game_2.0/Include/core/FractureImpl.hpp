/**
 * @file FractureImpl.hpp
 * @brief Out-of-line definition of EntityManager::fractureAsteroid
 *
 * WHY THIS IS IN ITS OWN FILE:
 *   EntityManager needs to call EntityFactory::createAsteroid, and
 *   EntityFactory includes EntityManager. Defining the body inside
 *   EntityManager.hpp would be a circular include. Declaring it there and
 *   defining it here — included AFTER EntityFactory.hpp — breaks the cycle
 *   without a forward-declaration dance or a pointer indirection.
 *
 * Include this ONCE, from SystemManager.hpp, after both headers.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "EntityManager.hpp"
#include "EntityFactory.hpp"
#include <cmath>
#include <algorithm>

inline void EntityManager::fractureAsteroid(size_t idx, sf::Vector2f impactDir,
    int physicalChildren, EntityFactory* ef,
    sol::state* lua, b2WorldId worldId)
{
    if (idx >= transforms.size()) return;

    const sf::Vector2f origin = transforms[idx].position;
    const uint8_t tier = healths[idx].asteroidTier;
    const float parentRadius = std::max(6.f, healths[idx].visualRadius);

    // Inherit the parent's momentum — fragments continuing along the original
    // trajectory is most of what makes a break look physical.
    sf::Vector2f parentVel(0.f, 0.f);
    if (b2Body_IsValid(physics[idx].bodyId)) {
        const b2Vec2 pv = b2Body_GetLinearVelocity(physics[idx].bodyId);
        parentVel = { pv.x * SCALE, pv.y * SCALE };
    }

    // Normalise the impact direction; fall back to random if the blow had none.
    float il = std::sqrt(impactDir.x * impactDir.x + impactDir.y * impactDir.y);
    if (il < 0.01f) {
        const float a = (rand() % 360) * 3.14159f / 180.f;
        impactDir = { std::cos(a), std::sin(a) };
    }
    else {
        impactDir /= il;
    }

    const sf::Color parentCol = renders[idx].shape.getFillColor();

    // ========================================================================
    // 1. CUT THE PARENT POLYGON INTO WEDGES
    //
    // Fan-triangulate the actual outline from its centroid. Each wedge is a
    // real piece of the rock that just died, which is the entire reason this
    // reads as cracking rather than as generic debris.
    // ========================================================================
    const std::vector<sf::Vector2f>& verts = physicsShapes[idx].vertices;
    const int n = static_cast<int>(verts.size());

    if (n >= 3) {
        for (int i = 0; i < n; ++i) {
            const sf::Vector2f& a = verts[i];
            const sf::Vector2f& b = verts[(i + 1) % n];

            // Wedge: centroid -> edge start -> edge midpoint-pushed-out -> edge end.
            // The pushed midpoint gives each shard a slightly convex outer face
            // instead of a flat one, which catches the eye as it tumbles.
            const sf::Vector2f mid((a.x + b.x) * 0.5f * 1.12f, (a.y + b.y) * 0.5f * 1.12f);
            const sf::Vector2f pts[4] = { {0.f, 0.f}, a, mid, b };

            // Shard centroid, used both as spawn offset and throw direction.
            sf::Vector2f c((a.x + b.x + mid.x) / 3.f, (a.y + b.y + mid.y) / 3.f);
            const float cl = std::sqrt(c.x * c.x + c.y * c.y);
            sf::Vector2f dir = (cl > 0.01f) ? sf::Vector2f(c.x / cl, c.y / cl) : impactDir;

            // Bias outward throw along the impact vector: shards on the far
            // side of the blow fly hardest. This is what gives the break a
            // direction instead of a firework's symmetry.
            const float align = 0.55f + 0.45f * (dir.x * impactDir.x + dir.y * impactDir.y);
            const float speed = (70.f + rand() % 150) * align + parentRadius * 1.4f;

            const float life = 0.65f + (rand() % 70) / 100.f;

            // Shards are darker than the parent surface — you're seeing the
            // rock's unweathered interior.
            sf::Color sc(
                static_cast<uint8_t>(parentCol.r * 0.72f),
                static_cast<uint8_t>(parentCol.g * 0.72f),
                static_cast<uint8_t>(parentCol.b * 0.75f),
                255);

            spawnDebris(
                origin + c * 0.5f,
                parentVel * 0.55f + dir * speed,
                ((rand() % 2) ? 1.f : -1.f) * (90.f + rand() % 340),
                pts, 4, sc, life);
        }
    }

    // ========================================================================
    // 2. DUST AT THE FRACTURE PLANE
    // ========================================================================
    const int dustCount = 10 + static_cast<int>(parentRadius * 0.55f);
    for (int i = 0; i < dustCount; ++i) {
        const float a = (rand() % 360) * 3.14159f / 180.f;
        const float sp = 40.f + rand() % 160;
        const float lf = 0.35f + (rand() % 45) / 100.f;
        particles.push_back({
            nextEntityId++,
            origin + sf::Vector2f(std::cos(a), std::sin(a)) * (parentRadius * 0.6f),
            parentVel * 0.3f + sf::Vector2f(std::cos(a), std::sin(a)) * sp,
            sf::Color(150, 145, 140, 190),
            lf, lf,
            1.5f + rand() % 4
            });
    }

    // ========================================================================
    // 3. PHYSICAL CHILDREN
    //
    // Spawned in a FAN perpendicular to the impact, not at random angles, so
    // the pieces visibly separate along the crack rather than overlapping and
    // shoving each other apart with collision resolution.
    // ========================================================================
    if (physicalChildren <= 0 || !ef || !lua) return;

    const char* childKey = (tier >= 2) ? "MEDIUM" : "SMALL";
    sol::optional<sol::table> childCfg = (*lua)["asteroid_types"][childKey];
    if (!childCfg) return;

    // Perpendicular to the impact = the crack line.
    const sf::Vector2f perp(-impactDir.y, impactDir.x);

    for (int i = 0; i < physicalChildren; ++i) {
        // Spread across the crack: -1 .. +1
        const float t = (physicalChildren == 1) ? 0.f
            : (2.f * i / static_cast<float>(physicalChildren - 1) - 1.f);

        sf::Vector2f dir(
            perp.x * t + impactDir.x * 0.45f,
            perp.y * t + impactDir.y * 0.45f);
        const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (dl > 0.01f) dir /= dl;

        // Offset far enough that children don't spawn inside each other —
        // overlapping bodies get violently separated by Box2D and the burst
        // looks like an explosion instead of a break.
        const sf::Vector2f spawnPos = origin + dir * (parentRadius * 0.62f);

        sol::table cfg = *childCfg;
        sol::table speedRange = cfg["speed_range"];
        const float lo = speedRange[1].get_or(6.f);
        const float hi = speedRange[2].get_or(8.f);
        const float sp = lo + (rand() % 100 / 100.f) * (hi - lo);

        const sf::Vector2f vel = parentVel / SCALE * 0.4f + dir * sp * 0.7f;

        // Big parent -> chunky children. Forcing the size roll here is why
        // createAsteroid takes sizeRollOverride: a LARGE rock should not
        // produce three runt MEDIUMs.
        const float roll = (tier >= 2) ? (0.55f + (rand() % 45) / 100.f)
            : (0.35f + (rand() % 50) / 100.f);

        const uint32_t childId = ef->createAsteroid(*this, spawnPos, vel,
            cfg["base_size"].get_or(0.5f), cfg, worldId, roll);

        const size_t cIdx = getEntityIndex(childId);
        if (cIdx != (size_t)-1) {
            b2Body_SetAngularVelocity(physics[cIdx].bodyId,
                ((rand() % 2) ? 1.f : -1.f) * (2.f + (rand() % 300) / 100.f));
        }
    }
}