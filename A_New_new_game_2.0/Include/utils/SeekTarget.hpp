/**
 * @file SeekTarget.hpp
 * @brief Victim picking for a parried Maniac rocket.
 *
 * A parried rocket no longer flies where it was batted (playtest: "the parry
 * mostly does nothing"). It picks a RANDOM victim -- any enemy ship or any
 * rock / wreck in range, never the player -- and then always gets there
 * (WeaponSystem: drunken weave that tightens as it closes, no collisions on
 * the way). Ships weigh more than rocks so a dense field does not eat most
 * parries, but rocks stay in: "random" has to sometimes mean the boulder.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "core/EntityManager.hpp"
#include <vector>
#include <cstdlib>

namespace seek {

    /// @return entity id of the victim, or 0 if nothing is in range.
    inline uint32_t pickTarget(const EntityManager& em, sf::Vector2f from, float range,
        uint32_t excludeA, uint32_t excludeB, float shipWeight = 3.f, float rockWeight = 1.f)
    {
        struct Cand { uint32_t id; float w; };
        std::vector<Cand> c;
        float total = 0.f;
        for (size_t j = 0; j < em.physics.size(); ++j) {
            if (!b2Body_IsValid(em.physics[j].bodyId)) continue;
            BodyUserData* ud = bodyUD(em.physics[j].bodyId);
            if (!ud) continue;
            float w = 0.f;
            if (ud->type == BodyType::Enemy) w = shipWeight;
            else if (ud->type == BodyType::Asteroid) w = rockWeight;
            if (w <= 0.f) continue;
            const uint32_t id = em.transforms[j].entityId;
            if (id == excludeA || id == excludeB) continue;
            if (em.healths[j].currentHp <= 0.f) continue;
            const sf::Vector2f d = em.transforms[j].position - from;
            if (d.x * d.x + d.y * d.y > range * range) continue;
            c.push_back({ id, w });
            total += w;
        }
        if (c.empty() || total <= 0.f) return 0;
        float roll = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * total;
        for (const auto& k : c) { roll -= k.w; if (roll <= 0.f) return k.id; }
        return c.back().id;
    }

} // namespace seek
