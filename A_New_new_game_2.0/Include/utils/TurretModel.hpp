/**
 * @file TurretModel.hpp
 * @brief One turret model, shared by the live gun and the dead one.
 *
 * Until 1.0 the live turret (RenderSystem: octagon + barrel bar, sized by
 * `turret_size`) and the dead one (WreckDetail: a ring 5.2 authored units
 * across + a thin drooping barrel) were two different drawings at two
 * different sizes. A dormant Barge's turret was a third of the size of the
 * gun that woke up on it, and a dead one shrank on the frame it died.
 *
 * Now both draw THESE polygons at THIS size. RenderSystem colours them live
 * (heat ramp on the barrels, a charge lane that fills down the housing),
 * WreckDetail bakes them cold and slumped off the firing line.
 *
 * Frame: forward = -Y, origin = the mount point, all sizes x `s`
 * (`turret_size`, in pixels at the live archetype's scale).
 *
 *     ||  ||        twin barrels + muzzle brakes   (BARREL, BRAKE)
 *    [======]       mantlet                         (MANTLET)
 *   /  |##|  \      housing, charge lane down the   (HOUSING, LANE)
 *   |  |##|  |      middle
 *   \________/
 *     [____]        rear block / counterweight      (REAR)
 *
 * The geometry is FIXED: nothing grows or shrinks with the charge. The tell
 * is colour (barrels heat from the muzzle back, the lane fills rear to front,
 * a glow sits on each muzzle), so the silhouette is the same ship in every
 * state, dormant included.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <vector>

namespace turretmodel {

    enum class Part { Rear, Barrel, Brake, Mantlet, Housing, Lane };

    struct Poly {
        Part part;
        std::vector<sf::Vector2f> pts;   ///< Convex, local frame, already x s
    };

    /// Length of the barrels from the mount, in units of s. Muzzles sit here.
    constexpr float kMuzzle = 2.25f;
    /// Barrel centre lines, +-x, in units of s.
    constexpr float kBarrelX = 0.30f;

    namespace detail {
        inline std::vector<sf::Vector2f> scaled(std::initializer_list<sf::Vector2f> p, float s,
            float dx = 0.f) {
            std::vector<sf::Vector2f> out;
            out.reserve(p.size());
            for (const auto& v : p) out.push_back({ (v.x + dx) * s, v.y * s });
            return out;
        }
    }

    /// Every part, in draw order (back to front).
    inline std::vector<Poly> parts(float s) {
        using detail::scaled;
        std::vector<Poly> out;
        out.push_back({ Part::Rear, scaled({ { -0.55f, 0.60f }, { 0.55f, 0.60f },
            { 0.45f, 1.10f }, { -0.45f, 1.10f } }, s) });
        for (float side : { -1.f, 1.f }) {
            const float x = side * kBarrelX;
            out.push_back({ Part::Barrel, scaled({ { -0.12f, -0.90f }, { 0.12f, -0.90f },
                { 0.12f, -kMuzzle + 0.20f }, { -0.12f, -kMuzzle + 0.20f } }, s, x) });
            out.push_back({ Part::Brake, scaled({ { -0.17f, -kMuzzle + 0.22f }, { 0.17f, -kMuzzle + 0.22f },
                { 0.17f, -kMuzzle }, { -0.17f, -kMuzzle } }, s, x) });
        }
        out.push_back({ Part::Mantlet, scaled({ { -0.70f, -0.80f }, { 0.70f, -0.80f },
            { 0.62f, -1.15f }, { -0.62f, -1.15f } }, s) });
        out.push_back({ Part::Housing, scaled({ { -0.85f, -0.55f }, { -0.55f, -0.85f },
            { 0.55f, -0.85f }, { 0.85f, -0.55f }, { 0.85f, 0.50f }, { 0.60f, 0.80f },
            { -0.60f, 0.80f }, { -0.85f, 0.50f } }, s) });
        out.push_back({ Part::Lane, scaled({ { -0.14f, 0.45f }, { 0.14f, 0.45f },
            { 0.14f, -0.55f }, { -0.14f, -0.55f } }, s) });
        return out;
    }

    /// The lit share of the charge lane: rear edge to `t` of the way forward.
    inline std::vector<sf::Vector2f> laneFill(float s, float t) {
        const float y0 = 0.45f, y1 = 0.45f - 1.0f * t;
        return { { -0.14f * s, y0 * s }, { 0.14f * s, y0 * s },
                 { 0.14f * s, y1 * s }, { -0.14f * s, y1 * s } };
    }

    /// Muzzle points, local frame.
    inline std::vector<sf::Vector2f> muzzles(float s) {
        return { { -kBarrelX * s, -kMuzzle * s }, { kBarrelX * s, -kMuzzle * s } };
    }

} // namespace turretmodel
