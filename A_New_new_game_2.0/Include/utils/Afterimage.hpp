/**
 * @file Afterimage.hpp
 * @brief A hull-shaped ghost left behind where a ship just was.
 *
 * Used by the Bloodseeker's disengage: every sidestep, and every round that
 * passes through him, leaves one. The read it has to carry is "shooting him
 * right now is wasted" -- a spark or a ring says something was hit, an
 * outline of the ship standing where the round went says it MISSED.
 *
 * No new renderer. A ghost is a WreckShard with the whole hull as its one
 * piece: no velocity to speak of, no heat (so no embers), fill nearly clear
 * and the skin in the tint. DebrisSystem already fades shards linearly over
 * fadeTime, so lifetime == fadeTime gives a ghost that starts at full
 * strength and dissolves -- flat, hard-edged, on-style.
 *
 * It shares the wreckage budget on purpose: in a frame busy enough to have
 * hundreds of hull pieces in flight, ghosts are the first thing to go.
 *
 * @author Oleg Ivakhiv
 */

#pragma once

#include "core/EntityManager.hpp"
#include "core/EnemyArchetypes.hpp"
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>

namespace fx {

    /**
     * @param pos          world position the ghost stands at (usually where
     *                     the ship was this frame, before it moved)
     * @param rotationDeg  the ship's heading, so the ghost points where it did
     * @param drift        a little of the ship's motion, px/s -- 0 is fine
     * @param tint         skin colour; the fill is the same, nearly clear
     * @param life         seconds from full strength to gone
     */
    /// Any outline + triangles (local px): the player's ship uses this one.
    inline void afterimageShape(EntityManager& em, const std::vector<sf::Vector2f>& outline,
        const std::vector<sf::Vector2f>& tris, sf::Vector2f pos, float rotationDeg,
        sf::Vector2f drift, sf::Color tint, float life = 0.26f,
        float lineWidth = 1.7f, uint8_t fillAlpha = 60)
    {
        if (tris.size() < 3 || outline.size() < 3) return;
        if (em.wreckShards.size() > 700) return;   // spectacle budget, see above
        float r2 = 0.f;
        for (const auto& p : outline) r2 = std::max(r2, p.x * p.x + p.y * p.y);

        WreckShard s;
        s.position = pos;
        s.velocity = drift;
        s.rotation = rotationDeg;
        s.angularVelocity = 0.f;
        s.lifetime = life;
        s.fadeTime = life;
        s.drag = 6.f;
        s.heat = 0.f;
        s.coolRate = 1.f;
        s.radius = std::sqrt(r2);
        s.lineWidth = lineWidth;
        s.fill = sf::Color(tint.r, tint.g, tint.b, fillAlpha);
        s.skinColor = sf::Color(tint.r, tint.g, tint.b, 225);
        s.tris = tris;
        const size_t n = outline.size();
        s.skin.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            s.skin.push_back(outline[i]);
            s.skin.push_back(outline[(i + 1) % n]);
        }
        em.wreckShards.push_back(std::move(s));
    }

    inline void afterimage(EntityManager& em, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f pos, float rotationDeg, sf::Vector2f drift, sf::Color tint,
        float life = 0.26f)
    {
        if (adef.visualTris.size() < 3 || adef.visual.size() < 3) return;
        if (em.wreckShards.size() > 700) return;   // spectacle budget, see above

        WreckShard s;
        s.position = pos;
        s.velocity = drift;
        s.rotation = rotationDeg;
        s.angularVelocity = 0.f;
        s.lifetime = life;
        s.fadeTime = life;            // linear fade from the first frame
        s.drag = 6.f;                 // stops almost at once: it stays put
        s.heat = 0.f;                 // cold, so DebrisSystem sheds no embers
        s.coolRate = 1.f;
        s.radius = adef.radius;
        s.lineWidth = 1.7f;
        s.fill = sf::Color(tint.r, tint.g, tint.b, 60);
        s.skinColor = sf::Color(tint.r, tint.g, tint.b, 225);
        s.tris = adef.visualTris;

        const size_t n = adef.visual.size();
        s.skin.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {
            s.skin.push_back(adef.visual[i]);
            s.skin.push_back(adef.visual[(i + 1) % n]);
        }
        em.wreckShards.push_back(std::move(s));
    }

} // namespace fx
