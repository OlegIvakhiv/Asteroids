/**
 * @file ShipLivery.hpp
 * @brief Paint, decals and cockpits -- everything purely cosmetic on a ship.
 *
 * Pure data and geometry. No rendering, no Box2D, no Lua, no UI: the renderer
 * and the refit bay both build their vertices from decalGeometry() here, so a
 * detail can never look different in the editor than in the fight.
 *
 * ============================================================================
 * WHAT IS PAINTABLE
 * ============================================================================
 *   hull / outline     the ship's own colours
 *   plasma             bolts and muzzle flash
 *   thrust / turbo     drive exhaust, normal and sprinting
 *   dodge              the dash burst
 *   parry              parry ring, arcs and the flash on a successful parry
 *   cockpit            the canopy glow
 *
 * The RIFT is deliberately absent. Its violet is how every ship on screen
 * says "that is the heavy weapon", and a player who paints it pink is only
 * lying to themselves.
 *
 * ============================================================================
 * DECALS
 * ============================================================================
 * Simple figures -- line, bar, oval, triangle, ring -- with position, size,
 * angle, thickness (0 = filled), colour, depth (under or over the hull) and
 * an optional mirror. They are drawn through the hull's transform, so they
 * bank, spin and squash with the ship.
 *
 * A decal must sit inside the model ENVELOPE (see ShipDesign): paint may not
 * be used to fake a silhouette bigger than the one the enemy has to hit.
 *
 * ============================================================================
 * PLATES
 * ============================================================================
 * Armour panels: any simple polygon (<= 16 points), stamped from a primitive
 * and then reshaped point by point. Same idea as the enemy archetype plates
 * (EnemyArchetypes.hpp), so a player ship can be dressed the way enemies are.
 *
 * A plate is stored as a SHAPE normalised to its own bounding box, plus the
 * same four numbers a decal has (pos, w, h, angle). That way one gizmo moves,
 * turns and sizes figures, plates and the canopy alike, and editing a point
 * only changes the shape.
 *
 * ============================================================================
 * INK: TONE OR COLOUR
 * ============================================================================
 * Plates and figures are inked one of two ways:
 *   TONE    the LIVE hull fill multiplied by `shade`. Panels flash white on a
 *           parry, glow with heat, flicker on stagger -- with the hull, as
 *           enemy plates do. This is what keeps a dressed ship from looking
 *           like stickers on a shape.
 *   COLOUR  a fixed colour, for markings that should stay put (stripes, kill
 *           marks, a faction badge).
 * Alpha always comes from the item's own colour, so either ink can be
 * see-through.
 *
 * ============================================================================
 * ONE MESH, TWO DRAWERS
 * ============================================================================
 * liveryPass() builds a pass (under or over the hull) as local-space
 * vertices. RenderSystem transforms them by the hull; the refit bay maps them
 * onto its canvas. Neither draws livery any other way, so the editor cannot
 * show something the fight will not.
 *
 * @author Oleg Ivakhiv
 * @version 1.2 -- plates, tonal ink, hull alpha, shared liveryPass()
 */

#pragma once

#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Vertex.hpp>
#include <SFML/System/Vector2.hpp>
#include "ShipDesign.hpp"
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ship {

    inline constexpr int MAX_DECALS = 20;
    inline constexpr int MAX_PLATES = 16;
    inline constexpr int MAX_PLATE_POINTS = 16;

    /// A see-through hull is allowed; an invisible one is not. The outline
    /// stays opaque in any case, so the silhouette always reads.
    inline constexpr std::uint8_t HULL_MIN_ALPHA = 64;

    /// Tone range. Below ~0.2 a panel is black whatever the hull does;
    /// above ~1.6 every hull colour clips to white.
    inline constexpr float SHADE_MIN = 0.2f;
    inline constexpr float SHADE_MAX = 1.6f;

    enum class DecalKind : std::uint8_t { Line = 0, Bar, Oval, Tri, Ring };
    inline constexpr int DECAL_KIND_COUNT = 5;

    inline const char* decalKindName(DecalKind k) {
        switch (k) {
        case DecalKind::Line: return "LINE";
        case DecalKind::Bar:  return "BAR";
        case DecalKind::Oval: return "OVAL";
        case DecalKind::Tri:  return "TRI";
        default:              return "RING";
        }
    }

    struct Decal {
        DecalKind    kind = DecalKind::Bar;
        sf::Vector2f pos;                       ///< Local pixels, ship space
        float        w = 16.f;
        float        h = 6.f;
        float        angle = 0.f;               ///< Degrees, clockwise
        float        thickness = 0.f;           ///< 0 = filled, else outline width
        sf::Color    color{ 40, 245, 255, 255 };
        bool         over = true;               ///< Above the hull, or under it
        bool         mirrored = false;          ///< Also drawn at -x
        bool         tonal = false;             ///< TONE ink: live hull fill x shade
        float        shade = 0.6f;              ///< Used when tonal
    };

    /// Stamp shapes a new plate starts from. Every one is editable afterwards.
    enum class PlateStamp : std::uint8_t { Rect = 0, Tri, Fin, Half, Chevron, Hex };
    inline constexpr int PLATE_STAMP_COUNT = 6;

    inline const char* plateStampName(PlateStamp s) {
        switch (s) {
        case PlateStamp::Rect:    return "PANEL";
        case PlateStamp::Tri:     return "WEDGE";
        case PlateStamp::Fin:     return "FIN";
        case PlateStamp::Half:    return "HALF";
        case PlateStamp::Chevron: return "CHEVRON";
        default:                  return "HEX";
        }
    }

    struct Plate {
        /// Outline normalised to the plate's own box: every point in
        /// [-0.5, 0.5] on both axes. Local = (x * w, y * h), rotated by angle.
        std::vector<sf::Vector2f> shape;
        sf::Vector2f pos;                       ///< Box centre, ship space
        float        w = 18.f;
        float        h = 10.f;
        float        angle = 0.f;               ///< Degrees, clockwise
        bool         tonal = true;              ///< TONE ink by default, like enemy plates
        float        shade = 0.72f;
        sf::Color    color{ 40, 245, 255, 255 };///< COLOUR ink; its alpha is used by both inks
        bool         accent = false;            ///< Thin outline in the live outline colour
        bool         over = true;
        bool         mirrored = false;
    };

    /// Canopies. Purely decorative -- they do not move a mount or a hitbox.
    enum class CockpitStyle : std::uint8_t { None = 0, Bubble, Visor, Twin, Slit, Dome };
    inline constexpr int COCKPIT_STYLE_COUNT = 6;

    inline const char* cockpitStyleName(CockpitStyle s) {
        switch (s) {
        case CockpitStyle::None:   return "NONE";
        case CockpitStyle::Bubble: return "BUBBLE";
        case CockpitStyle::Visor:  return "VISOR";
        case CockpitStyle::Twin:   return "TWIN";
        case CockpitStyle::Slit:   return "SLIT";
        default:                   return "DOME";
        }
    }

    struct Cockpit {
        CockpitStyle style = CockpitStyle::None;
        sf::Vector2f pos{ 0.f, -6.f };
        float        w = 13.f;
        float        h = 17.f;
        float        angle = 0.f;   ///< Degrees. Same handles as a decal.
    };

    /// Defaults reproduce the stock ship exactly, so an unpainted hull is unchanged.
    struct Paint {
        sf::Color hull{ 40, 100, 255, 255 };
        sf::Color outline{ 255, 255, 255, 255 };
        sf::Color plasma{ 120, 230, 255, 255 };
        sf::Color thrust{ 60, 160, 255, 180 };
        sf::Color turbo{ 180, 220, 255, 220 };
        sf::Color dodge{ 0, 220, 255, 230 };
        sf::Color parry{ 0, 255, 200, 255 };
        sf::Color cockpit{ 180, 245, 255, 235 };
        /// Anything the player turns into their own projectile: a parried
        /// asteroid, a Rift-hijacked rock, their trails and hijack burst.
        sf::Color homing{ 0, 255, 200, 255 };
    };

    struct Livery {
        Paint              paint;
        std::vector<Decal> decals;
        Cockpit            cockpit;
        std::vector<Plate> plates;              ///< Drawn before figures in each pass
    };

    /// Editor palette. Terminal-bright, plus enough neutrals to make panelling.
    inline const sf::Color* liveryPalette(int& count) {
        static const sf::Color k[] = {
            { 40, 245, 255 }, { 20, 150, 190 }, { 40, 100, 255 }, { 120, 90, 255 },
            { 175,  95, 255 }, { 255,  60, 170 }, { 255,  48,   0 }, { 255, 130,  40 },
            { 255, 214,   0 }, { 180, 250,  70 }, {  80, 240, 130 }, {   0, 255, 200 },
            { 236, 240, 245 }, { 150, 160, 175 }, {  70,  80,  95 }, {  12,  16,  24 },
        };
        count = static_cast<int>(sizeof(k) / sizeof(k[0]));
        return k;
    }

    // ============================================================================
    // GEOMETRY
    // ============================================================================

    namespace detail {

        /// Rotated rectangle -> two triangles, appended to `out`.
        inline void quadTris(std::vector<sf::Vector2f>& out, sf::Vector2f c,
            sf::Vector2f half, float cosA, float sinA) {
            const sf::Vector2f ex{ half.x * cosA, half.x * sinA };
            const sf::Vector2f ey{ -half.y * sinA, half.y * cosA };
            const sf::Vector2f a{ c.x - ex.x - ey.x, c.y - ex.y - ey.y };
            const sf::Vector2f b{ c.x + ex.x - ey.x, c.y + ex.y - ey.y };
            const sf::Vector2f d{ c.x + ex.x + ey.x, c.y + ex.y + ey.y };
            const sf::Vector2f e{ c.x - ex.x + ey.x, c.y - ex.y + ey.y };
            out.push_back(a); out.push_back(b); out.push_back(d);
            out.push_back(a); out.push_back(d); out.push_back(e);
        }

        /// Closed ring of points -> filled fan, or a band of width `thick`.
        inline void ringTris(std::vector<sf::Vector2f>& out,
            const std::vector<sf::Vector2f>& ring, sf::Vector2f c, float thick) {
            const std::size_t n = ring.size();
            if (n < 3) return;
            if (thick <= 0.f) {
                for (std::size_t i = 0; i < n; ++i) {
                    out.push_back(c); out.push_back(ring[i]); out.push_back(ring[(i + 1) % n]);
                }
                return;
            }
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f a = ring[i], b = ring[(i + 1) % n];
                const auto inward = [&](sf::Vector2f p) {
                    sf::Vector2f d{ c.x - p.x, c.y - p.y };
                    const float l = std::sqrt(d.x * d.x + d.y * d.y);
                    if (l < 0.001f) return p;
                    const float t = std::min(thick, l);
                    return sf::Vector2f{ p.x + d.x / l * t, p.y + d.y / l * t };
                    };
                const sf::Vector2f ai = inward(a), bi = inward(b);
                out.push_back(a); out.push_back(b); out.push_back(bi);
                out.push_back(a); out.push_back(bi); out.push_back(ai);
            }
        }

    } // namespace detail

    /**
     * @brief Triangles for one decal, in ship-local pixels.
     *
     * @param mirrorX draw the mirrored copy instead of the original.
     * Appends; never clears. Both the game and the editor call this.
     */
    inline void decalGeometry(const Decal& d, std::vector<sf::Vector2f>& out, bool mirrorX = false) {
        const float sx = mirrorX ? -1.f : 1.f;
        const sf::Vector2f c{ d.pos.x * sx, d.pos.y };
        const float ang = (mirrorX ? -d.angle : d.angle) * 3.14159265f / 180.f;
        const float ca = std::cos(ang), sa = std::sin(ang);
        const float w = std::max(1.f, d.w), h = std::max(1.f, d.h);
        const float th = std::max(0.f, d.thickness);

        switch (d.kind) {
        case DecalKind::Line:
            detail::quadTris(out, c, { w * 0.5f, std::max(1.f, th > 0.f ? th : h * 0.5f) * 0.5f }, ca, sa);
            break;

        case DecalKind::Bar:
            if (th <= 0.f) { detail::quadTris(out, c, { w * 0.5f, h * 0.5f }, ca, sa); }
            else {
                // Four bands: an outlined rectangle.
                const float hw = w * 0.5f, hh = h * 0.5f, t = std::min(th, std::min(hw, hh));
                const auto band = [&](sf::Vector2f off, sf::Vector2f half) {
                    detail::quadTris(out, { c.x + off.x * ca - off.y * sa,
                                            c.y + off.x * sa + off.y * ca }, half, ca, sa);
                    };
                band({ 0.f, -hh + t * 0.5f }, { hw, t * 0.5f });
                band({ 0.f,  hh - t * 0.5f }, { hw, t * 0.5f });
                band({ -hw + t * 0.5f, 0.f }, { t * 0.5f, hh - t });
                band({ hw - t * 0.5f, 0.f }, { t * 0.5f, hh - t });
            }
            break;

        case DecalKind::Oval:
        case DecalKind::Ring: {
            const int seg = 22;
            std::vector<sf::Vector2f> ring;
            ring.reserve(seg);
            for (int i = 0; i < seg; ++i) {
                const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
                const float x = std::cos(a) * w * 0.5f, y = std::sin(a) * h * 0.5f;
                ring.push_back({ c.x + x * ca - y * sa, c.y + x * sa + y * ca });
            }
            const float bandW = (d.kind == DecalKind::Ring) ? std::max(1.f, th > 0.f ? th : 2.f) : th;
            detail::ringTris(out, ring, c, bandW);
            break;
        }

        case DecalKind::Tri: {
            const sf::Vector2f p[3] = { { 0.f, -h * 0.5f }, { w * 0.5f, h * 0.5f }, { -w * 0.5f, h * 0.5f } };
            std::vector<sf::Vector2f> ring;
            for (const auto& q : p)
                ring.push_back({ c.x + q.x * ca - q.y * sa, c.y + q.x * sa + q.y * ca });
            if (mirrorX) std::swap(ring[1], ring[2]);
            detail::ringTris(out, ring, c, th);
            break;
        }
        }
    }

    /// The canopy, as (dark rim, glass) triangle lists.
    inline void cockpitGeometry(const Cockpit& cp,
        std::vector<sf::Vector2f>& glass, std::vector<sf::Vector2f>& rim) {
        if (cp.style == CockpitStyle::None) return;
        const float w = std::max(3.f, cp.w), h = std::max(3.f, cp.h);
        const std::size_t glass0 = glass.size(), rim0 = rim.size();

        const auto oval = [&](sf::Vector2f c, float ow, float oh, std::vector<sf::Vector2f>& out, float th) {
            std::vector<sf::Vector2f> ring;
            for (int i = 0; i < 20; ++i) {
                const float a = 6.2831853f * static_cast<float>(i) / 20.f;
                ring.push_back({ c.x + std::cos(a) * ow * 0.5f, c.y + std::sin(a) * oh * 0.5f });
            }
            detail::ringTris(out, ring, c, th);
            };
        const auto poly = [&](std::vector<sf::Vector2f> ring, std::vector<sf::Vector2f>& out, float th) {
            sf::Vector2f c{ 0.f, 0.f };
            for (const auto& p : ring) { c.x += p.x; c.y += p.y; }
            c.x /= static_cast<float>(ring.size()); c.y /= static_cast<float>(ring.size());
            detail::ringTris(out, ring, c, th);
            };

        switch (cp.style) {
        case CockpitStyle::Bubble:
            oval(cp.pos, w + 3.f, h + 3.f, rim, 0.f);
            oval(cp.pos, w, h, glass, 0.f);
            break;
        case CockpitStyle::Visor:
            poly({ { cp.pos.x - w * 0.5f, cp.pos.y + h * 0.5f }, { cp.pos.x - w * 0.34f, cp.pos.y - h * 0.5f },
                   { cp.pos.x + w * 0.34f, cp.pos.y - h * 0.5f }, { cp.pos.x + w * 0.5f, cp.pos.y + h * 0.5f } }, rim, 0.f);
            poly({ { cp.pos.x - w * 0.38f, cp.pos.y + h * 0.32f }, { cp.pos.x - w * 0.24f, cp.pos.y - h * 0.34f },
                   { cp.pos.x + w * 0.24f, cp.pos.y - h * 0.34f }, { cp.pos.x + w * 0.38f, cp.pos.y + h * 0.32f } }, glass, 0.f);
            break;
        case CockpitStyle::Twin:
            oval({ cp.pos.x - w * 0.32f, cp.pos.y }, w * 0.55f, h * 0.8f, rim, 0.f);
            oval({ cp.pos.x + w * 0.32f, cp.pos.y }, w * 0.55f, h * 0.8f, rim, 0.f);
            oval({ cp.pos.x - w * 0.32f, cp.pos.y }, w * 0.36f, h * 0.6f, glass, 0.f);
            oval({ cp.pos.x + w * 0.32f, cp.pos.y }, w * 0.36f, h * 0.6f, glass, 0.f);
            break;
        case CockpitStyle::Slit:
            poly({ { cp.pos.x - w * 0.5f, cp.pos.y }, { cp.pos.x - w * 0.2f, cp.pos.y - h * 0.5f },
                   { cp.pos.x + w * 0.2f, cp.pos.y - h * 0.5f }, { cp.pos.x + w * 0.5f, cp.pos.y },
                   { cp.pos.x, cp.pos.y + h * 0.22f } }, rim, 0.f);
            poly({ { cp.pos.x - w * 0.32f, cp.pos.y - h * 0.06f }, { cp.pos.x - w * 0.14f, cp.pos.y - h * 0.36f },
                   { cp.pos.x + w * 0.14f, cp.pos.y - h * 0.36f }, { cp.pos.x + w * 0.32f, cp.pos.y - h * 0.06f } }, glass, 0.f);
            break;
        case CockpitStyle::Dome:
            oval(cp.pos, w + 4.f, h + 4.f, rim, 0.f);
            oval(cp.pos, w, h, glass, 0.f);
            oval({ cp.pos.x, cp.pos.y - h * 0.1f }, w * 0.55f, h * 0.5f, rim, 1.5f);
            break;
        default: break;
        }

        // Rotate everything the styles just built, about the canopy's centre.
        if (std::fabs(cp.angle) > 0.01f) {
            const float a = cp.angle * 3.14159265f / 180.f;
            const float ca = std::cos(a), sa = std::sin(a);
            const auto spin = [&](std::vector<sf::Vector2f>& v, std::size_t from) {
                for (std::size_t i = from; i < v.size(); ++i) {
                    const float x = v[i].x - cp.pos.x, y = v[i].y - cp.pos.y;
                    v[i] = { cp.pos.x + x * ca - y * sa, cp.pos.y + x * sa + y * ca };
                }
                };
            spin(glass, glass0);
            spin(rim, rim0);
        }
    }

    // ============================================================================
    // RULES
    // ============================================================================

    /// Worst-case reach of a decal from its centre.
    inline float decalRadius(const Decal& d) {
        const float w = std::max(1.f, d.w) * 0.5f, h = std::max(1.f, d.h) * 0.5f;
        return std::sqrt(w * w + h * h);
    }

    /**
     * @brief A decal must stay inside the model envelope -- EXACT test.
     *
     * Paint cannot fake a silhouette bigger than the hull the enemy must hit.
     * Tests every vertex the decal actually draws (mirror copy included).
     * The envelope is the convex hitbox scaled about its centroid, so it is
     * convex, and a triangle whose three corners are inside it is inside it.
     *
     * The old bounding-circle test treated a 40x3 stripe as a 40 px disc:
     * rotating a stripe near the edge was refused while it visibly fitted.
     * The refit bay uses THIS function.
     */
    inline bool decalFits(const Decal& d, const std::vector<sf::Vector2f>& envelope) {
        if (envelope.size() < 3) return true;
        std::vector<sf::Vector2f> tris;
        decalGeometry(d, tris, false);
        if (d.mirrored) decalGeometry(d, tris, true);
        for (const auto& p : tris)
            if (!ship::detail::pointInPolygon(p, envelope, 0.5f)) return false;
        return true;
    }

    /// The canopy's rim and glass, same exact rule as a decal.
    inline bool cockpitFits(const Cockpit& cp, const std::vector<sf::Vector2f>& envelope) {
        if (envelope.size() < 3 || cp.style == CockpitStyle::None) return true;
        std::vector<sf::Vector2f> glass, rim;
        cockpitGeometry(cp, glass, rim);
        for (const auto& p : rim)   if (!ship::detail::pointInPolygon(p, envelope, 0.5f)) return false;
        for (const auto& p : glass) if (!ship::detail::pointInPolygon(p, envelope, 0.5f)) return false;
        return true;
    }

    /**
     * @brief Load-time acceptance: the exact test OR the legacy bounding circle.
     *
     * ShipFile drops decals that fail this. Accepting either rule means no
     * ship saved before the exact test can lose art on import, and nothing
     * the editor now allows can be dropped either.
     */
    inline bool decalInside(const Decal& d, const std::vector<sf::Vector2f>& envelope) {
        if (envelope.size() < 3) return true;
        if (decalFits(d, envelope)) return true;
        const float r = decalRadius(d);
        const float xs[2] = { d.pos.x, -d.pos.x };
        for (int m = 0; m < (d.mirrored ? 2 : 1); ++m) {
            const sf::Vector2f c{ xs[m], d.pos.y };
            const sf::Vector2f probes[5] = { c, { c.x + r, c.y }, { c.x - r, c.y },
                                             { c.x, c.y + r }, { c.x, c.y - r } };
            for (const auto& p : probes)
                if (!ship::detail::pointInPolygon(p, envelope, 0.5f)) return false;
        }
        return true;
    }

    inline void clampDecal(Decal& d) {
        d.w = std::clamp(d.w, 2.f, 90.f);
        d.h = std::clamp(d.h, 2.f, 90.f);
        d.thickness = std::clamp(d.thickness, 0.f, 12.f);
        while (d.angle < 0.f)    d.angle += 360.f;
        while (d.angle >= 360.f) d.angle -= 360.f;
    }


    // ============================================================================
    // PLATES
    // ============================================================================

    inline void clampPlate(Plate& p) {
        p.w = std::clamp(p.w, 2.f, 140.f);
        p.h = std::clamp(p.h, 2.f, 140.f);
        p.shade = std::clamp(p.shade, SHADE_MIN, SHADE_MAX);
        while (p.angle < 0.f)    p.angle += 360.f;
        while (p.angle >= 360.f) p.angle -= 360.f;
    }

    /// Ship-space outline of a plate (or of its mirror copy).
    inline std::vector<sf::Vector2f> plateWorld(const Plate& p, bool mirrorX = false) {
        std::vector<sf::Vector2f> out;
        out.reserve(p.shape.size());
        const float a = p.angle * 3.14159265f / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        for (const auto& s : p.shape) {
            const float lx = s.x * p.w, ly = s.y * p.h;
            sf::Vector2f q{ p.pos.x + lx * ca - ly * sa, p.pos.y + lx * sa + ly * ca };
            if (mirrorX) q.x = -q.x;
            out.push_back(q);
        }
        return out;
    }

    /// Ship space -> the plate's normalised shape space (inverse of plateWorld).
    inline sf::Vector2f plateToShape(const Plate& p, sf::Vector2f world) {
        const float a = p.angle * 3.14159265f / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        const sf::Vector2f d{ world.x - p.pos.x, world.y - p.pos.y };
        return { (d.x * ca + d.y * sa) / std::max(0.01f, p.w),
                 (-d.x * sa + d.y * ca) / std::max(0.01f, p.h) };
    }

    /**
     * @brief Refit the box to the shape after its points changed.
     *
     * The ship-space outline is unchanged -- only how it is split between
     * shape and box -- so it can run every frame of a point drag without the
     * plate creeping. Keeps the gizmo box tight around what is drawn.
     */
    inline void normalizePlate(Plate& p) {
        if (p.shape.empty()) return;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const auto& s : p.shape) {
            const float lx = s.x * p.w, ly = s.y * p.h;
            x0 = std::min(x0, lx); x1 = std::max(x1, lx);
            y0 = std::min(y0, ly); y1 = std::max(y1, ly);
        }
        const float nw = std::max(1.f, x1 - x0), nh = std::max(1.f, y1 - y0);
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        const float a = p.angle * 3.14159265f / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        for (auto& s : p.shape) s = { (s.x * p.w - cx) / nw, (s.y * p.h - cy) / nh };
        p.pos = { p.pos.x + cx * ca - cy * sa, p.pos.y + cx * sa + cy * ca };
        p.w = nw;
        p.h = nh;
    }

    /// A fresh plate of a stamp shape, centred on `pos`.
    inline Plate plateStamp(PlateStamp kind, sf::Vector2f pos) {
        Plate p;
        p.pos = pos;
        switch (kind) {
        case PlateStamp::Rect:
            p.shape = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            p.w = 18.f; p.h = 10.f;
            break;
        case PlateStamp::Tri:
            p.shape = { { 0.f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            p.w = 14.f; p.h = 14.f;
            break;
        case PlateStamp::Fin:          // swept: a long trailing edge, a short root
            p.shape = { { -0.5f, -0.5f }, { 0.5f, 0.25f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            p.w = 14.f; p.h = 18.f;
            break;
        case PlateStamp::Half: {       // flat edge aft, dome forward
            for (int i = 0; i <= 8; ++i) {
                const float t = 3.14159265f + 3.14159265f * static_cast<float>(i) / 8.f;
                p.shape.push_back({ 0.5f * std::cos(t), 0.5f + std::sin(t) });
            }
            p.w = 16.f; p.h = 8.f;
            break;
        }
        case PlateStamp::Chevron:
            p.shape = { { 0.f, -0.5f }, { 0.5f, 0.1f }, { 0.5f, 0.5f }, { 0.f, -0.1f },
                        { -0.5f, 0.5f }, { -0.5f, 0.1f } };
            p.w = 16.f; p.h = 12.f;
            break;
        default:
            for (int i = 0; i < 6; ++i) {
                const float t = 3.14159265f / 6.f + 6.2831853f * static_cast<float>(i) / 6.f;
                p.shape.push_back({ 0.5f * std::cos(t), 0.5f * std::sin(t) });
            }
            p.w = 14.f; p.h = 12.f;
            break;
        }
        normalizePlate(p);   // fit the box to the shape exactly
        return p;
    }

    /// Every point of the plate (and its mirror copy) inside the envelope.
    /// The envelope is convex, so a concave plate with all corners inside is inside.
    inline bool plateFits(const Plate& p, const std::vector<sf::Vector2f>& envelope) {
        if (envelope.size() < 3) return true;
        for (int m = 0; m < (p.mirrored ? 2 : 1); ++m)
            for (const auto& q : plateWorld(p, m == 1))
                if (!ship::detail::pointInPolygon(q, envelope, 0.5f)) return false;
        return true;
    }

    /// 3..16 points and no self-crossing: ear clipping needs a simple polygon.
    inline bool plateValid(const Plate& p) {
        if (p.shape.size() < 3 || p.shape.size() > static_cast<std::size_t>(MAX_PLATE_POINTS)) return false;
        return ship::detail::isSimplePolygon(plateWorld(p, false));
    }

    /**
     * @brief Turn a figure into an editable plate with the same look.
     *
     * Outlined figures (a ring, an outlined bar) come out solid with the
     * accent outline on: a plate is a filled panel.
     */
    inline Plate plateFromDecal(const Decal& d) {
        Plate p;
        p.pos = d.pos;
        p.angle = d.angle;
        p.w = std::max(1.f, d.w);
        p.h = std::max(1.f, d.h);
        switch (d.kind) {
        case DecalKind::Line:
            p.h = std::max(1.f, d.thickness > 0.f ? d.thickness : d.h * 0.5f);
            p.shape = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            break;
        case DecalKind::Bar:
            p.shape = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            break;
        case DecalKind::Tri:
            p.shape = { { 0.f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            break;
        default:   // Oval, Ring
            for (int i = 0; i < MAX_PLATE_POINTS; ++i) {
                const float t = 6.2831853f * static_cast<float>(i) / static_cast<float>(MAX_PLATE_POINTS);
                p.shape.push_back({ 0.5f * std::cos(t), 0.5f * std::sin(t) });
            }
            break;
        }
        p.accent = (d.kind == DecalKind::Ring) || (d.kind != DecalKind::Line && d.thickness > 0.f);
        p.tonal = d.tonal;
        p.shade = d.shade;
        p.color = d.color;
        p.over = d.over;
        p.mirrored = d.mirrored;
        clampPlate(p);
        return p;
    }

    // ============================================================================
    // INK
    // ============================================================================

    /// The hull colour of THIS frame, scaled. Alpha is the item's own.
    inline sf::Color tone(sf::Color fill, float shade, std::uint8_t alpha) {
        const auto ch = [&](std::uint8_t c) {
            return static_cast<std::uint8_t>(std::clamp(static_cast<float>(c) * shade, 0.f, 255.f));
            };
        return { ch(fill.r), ch(fill.g), ch(fill.b), alpha };
    }

    inline sf::Color inkOf(const Decal& d, sf::Color liveFill) {
        return d.tonal ? tone(liveFill, d.shade, d.color.a) : d.color;
    }
    inline sf::Color inkOf(const Plate& p, sf::Color liveFill) {
        return p.tonal ? tone(liveFill, p.shade, p.color.a) : p.color;
    }

    namespace detail {

        /// Closed polygon stroke, centred on the edges, mitred, as triangles.
        inline void strokeClosed(const std::vector<sf::Vector2f>& P, float w,
            std::vector<sf::Vector2f>& out) {
            const std::size_t n = P.size();
            if (n < 3 || w <= 0.f) return;
            const auto normalOf = [&](std::size_t i) {
                const sf::Vector2f e{ P[(i + 1) % n].x - P[i].x, P[(i + 1) % n].y - P[i].y };
                const float l = std::max(0.0001f, std::sqrt(e.x * e.x + e.y * e.y));
                return sf::Vector2f{ e.y / l, -e.x / l };
                };
            std::vector<sf::Vector2f> off(n);
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f n1 = normalOf((i + n - 1) % n), n2 = normalOf(i);
                const float d = std::max(1.f + n1.x * n2.x + n1.y * n2.y, 0.0001f);
                sf::Vector2f m{ (n1.x + n2.x) / d, (n1.y + n2.y) / d };
                const float ml = std::sqrt(m.x * m.x + m.y * m.y);
                if (ml > 4.f) { m.x *= 4.f / ml; m.y *= 4.f / ml; }   // cap spikes at 4x
                off[i] = { m.x * w * 0.5f, m.y * w * 0.5f };
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                const sf::Vector2f a0{ P[i].x + off[i].x, P[i].y + off[i].y }, a1{ P[i].x - off[i].x, P[i].y - off[i].y };
                const sf::Vector2f b0{ P[j].x + off[j].x, P[j].y + off[j].y }, b1{ P[j].x - off[j].x, P[j].y - off[j].y };
                out.push_back(a0); out.push_back(b0); out.push_back(b1);
                out.push_back(a0); out.push_back(b1); out.push_back(a1);
            }
        }

    } // namespace detail

    /// Width of a plate's accent outline, local pixels.
    inline constexpr float PLATE_ACCENT_PX = 1.2f;

    /// The live colours of the frame being drawn. The refit bay passes the
    /// paint; the game passes whatever the hull is showing right now.
    struct LiveInk {
        sf::Color fill;
        sf::Color outline;
    };

    /**
     * @brief One pass of the livery as LOCAL-space triangles.
     *
     * Order inside a pass: plates, then figures; the OVER pass ends with the
     * canopy. Appends; never clears. Callers transform the positions.
     */
    inline void liveryPass(const Livery& lv, const LiveInk& ink, bool over, std::vector<sf::Vertex>& out) {
        std::vector<sf::Vector2f> tris;
        for (const auto& p : lv.plates) {
            if (p.over != over || p.shape.size() < 3) continue;
            const sf::Color c = inkOf(p, ink.fill);
            const sf::Color ac(ink.outline.r, ink.outline.g, ink.outline.b,
                static_cast<std::uint8_t>(std::min<int>(ink.outline.a, 220) * c.a / 255));
            for (int m = 0; m < (p.mirrored ? 2 : 1); ++m) {
                const std::vector<sf::Vector2f> poly = plateWorld(p, m == 1);
                for (const auto& v : ship::detail::triangulate(poly)) out.push_back({ v, c });
                if (p.accent) {
                    tris.clear();
                    detail::strokeClosed(poly, PLATE_ACCENT_PX, tris);
                    for (const auto& v : tris) out.push_back({ v, ac });
                }
            }
        }
        for (const auto& d : lv.decals) {
            if (d.over != over) continue;
            tris.clear();
            decalGeometry(d, tris, false);
            if (d.mirrored) decalGeometry(d, tris, true);
            const sf::Color c = inkOf(d, ink.fill);
            for (const auto& v : tris) out.push_back({ v, c });
        }
        if (over && lv.cockpit.style != CockpitStyle::None) {
            std::vector<sf::Vector2f> glass, rim;
            cockpitGeometry(lv.cockpit, glass, rim);
            const sf::Color rimCol(14, 18, 26, 235);
            for (const auto& v : rim)   out.push_back({ v, rimCol });
            for (const auto& v : glass) out.push_back({ v, lv.paint.cockpit });
        }
    }

    /// HSV -> RGB. h in [0,360), s and v in [0,1]. For the colour picker.
    inline sf::Color fromHSV(float h, float s, float v, std::uint8_t a = 255) {
        h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f);
        s = std::clamp(s, 0.f, 1.f);
        v = std::clamp(v, 0.f, 1.f);
        const float c = v * s;
        const float x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f));
        const float m = v - c;
        float r = 0.f, g = 0.f, b = 0.f;
        if (h < 60.f) { r = c; g = x; }
        else if (h < 120.f) { r = x; g = c; }
        else if (h < 180.f) { g = c; b = x; }
        else if (h < 240.f) { g = x; b = c; }
        else if (h < 300.f) { r = x; b = c; }
        else { r = c; b = x; }
        return sf::Color(static_cast<std::uint8_t>((r + m) * 255.f),
            static_cast<std::uint8_t>((g + m) * 255.f),
            static_cast<std::uint8_t>((b + m) * 255.f), a);
    }

    /// RGB -> HSV, so the picker can open on the colour already in use.
    inline void toHSV(sf::Color c, float& h, float& s, float& v) {
        const float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
        const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        const float d = mx - mn;
        v = mx;
        s = (mx <= 0.0001f) ? 0.f : d / mx;
        if (d <= 0.0001f) { h = 0.f; return; }
        if (mx == r)      h = 60.f * std::fmod((g - b) / d, 6.f);
        else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
        else              h = 60.f * ((r - g) / d + 4.f);
        if (h < 0.f) h += 360.f;
    }

    inline std::string colorToHex(sf::Color c) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
        return std::string(buf);
    }

    inline sf::Color colorFromHex(const std::string& s, sf::Color fallback = sf::Color::White) {
        if (s.size() < 7 || s[0] != '#') return fallback;
        const auto hx = [&](std::size_t i) -> int {
            const char c = s[i];
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
            };
        int v[8]{};
        const std::size_t n = (s.size() >= 9) ? 8u : 6u;
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = hx(i + 1);
            if (v[i] < 0) return fallback;
        }
        return sf::Color(
            static_cast<std::uint8_t>(v[0] * 16 + v[1]),
            static_cast<std::uint8_t>(v[2] * 16 + v[3]),
            static_cast<std::uint8_t>(v[4] * 16 + v[5]),
            n == 8 ? static_cast<std::uint8_t>(v[6] * 16 + v[7]) : 255);
    }

} // namespace ship