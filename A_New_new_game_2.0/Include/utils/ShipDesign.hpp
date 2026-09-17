/**
 * @file ShipDesign.hpp
 * @brief Player-authored hull: class frame, geometry, mounts, reactor, derived kit.
 *
 * Pure data and math. No rendering, no Box2D, no Lua, no UI. This is what the
 * refit editor manipulates, what EntityFactory consumes, and what the stat
 * readout displays. describe() prints everything with no window open.
 *
 * ============================================================================
 * COORDINATE CONTRACT
 * ============================================================================
 * Local pixels, same space as RenderComponent::shape and PhysicsShapeData.
 * The ship faces -Y. Forward is up on screen. Conversion to Box2D metres
 * happens by dividing by ShipTuning::pixelsPerMetre, which MUST equal SCALE.
 *
 * ============================================================================
 * HULL CLASSES -- THE FRAME
 * ============================================================================
 * Every design belongs to LIGHT, MEDIUM or HEAVY. A class is a build frame:
 *
 *   - a rectangle every hull point must stay inside   (no enormous cube)
 *   - an area band, i.e. tonnage                     (no paper-thin needle)
 *   - weapon / drive slot caps
 *   - a reactor range that grows with area inside the band
 *   - RCS strength (how well the ship moves where engines do not point)
 *   - a minimum forward acceleration                  (no parked fortress)
 *
 * The bands do not overlap and the reactor ranges are near-continuous across
 * them, so a big MEDIUM and a small HEAVY are close relatives rather than an
 * exploitable seam.
 *
 * ============================================================================
 * MOUNT RULES
 * ============================================================================
 * Guns mount on VERTICES with forward clearance. Engines mount on EDGES whose
 * outward normal faces aft. Unchanged from 1.0, plus:
 *
 *   - every mount costs REACTOR UNITS; a full reactor refuses the next mount
 *   - guns need BARREL CLEARANCE from each other (kills the chisel-nose stack)
 *   - mounts add MASS at their position, so wide mounts cost turn rate (m r^2)
 *
 * Reactor units not spent on mounts become energy regeneration. That is the
 * anti-Rambo rule: four guns and four drives do not fit, and a nearly full
 * reactor starves dash, turbo, plasma and Rift of energy.
 *
 * ============================================================================
 * THE RIFT ROLE
 * ============================================================================
 * Exactly one mounted gun is SPINAL: the Rift Shot fires from it. Auto-picked
 * as the gun nearest the centreline (ties -> most forward), reassignable.
 * A lone gun is always SHARED: it fires plasma too, and charging the Rift
 * interrupts plasma. With two or more guns the spinal mount is DEDICATED by
 * default and plasma keeps firing from the rest while the Rift charges.
 *
 * ============================================================================
 * THE KIT
 * ============================================================================
 * kit() turns stats into what the starting gear reads at runtime. Everything
 * that already has a Lua value stays in Lua -- the kit only supplies ratios
 * against the reference build (stock hull, auto-mounted), so that build
 * handles EXACTLY as the pre-refit ship did and every existing tuning number
 * keeps its meaning.
 *
 * @author Oleg Ivakhiv
 * CHANGED in 2.2 -- DECORATIVE MODEL:
 *  The hitbox (<= 8 convex points) is what Box2D collides with. The MODEL is
 *  what the player sees: any simple polygon, concave allowed, up to 64 points.
 *  Three rules keep it honest -- what you see must never hide what hits you:
 *    1. it COVERS the hitbox completely (no invisible hull)
 *    2. it stays inside the ENVELOPE -- the hitbox scaled x1.5 about its centroid
 *    3. its area is at most 150% of the hitbox's
 *  A spike on the model above a gun mount becomes that gun's MUZZLE: plasma
 *  and heat glow leave from the tip. An untouched model simply follows the
 *  hitbox. A model broken by a later hitbox edit is flagged, and the game
 *  draws the hitbox until it is fixed.
 *
 * CHANGED in 2.1 (playtest pass):
 *  - Energy pool is nearly flat across sizes (exponent 0.5 -> 0.2). Size buys
 *    armour; batteries stopped deciding who can dodge and shoot at once.
 *  - Mobility spread compressed: accel ratio^0.6, agility floor 0.70. Heavy is
 *    slower, not helpless.
 *  - Dash moved out of the kit -- it is a class tier now (ClassTuning.hpp).
 *  - Heat capacity moved out of the kit -- class thermal profile instead.
 *  - KitProfile carries the hull class.
 *
 * @version 2.2
 */

#pragma once

#include <SFML/System/Vector2.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace ship {

    // ============================================================================
    // CONSTANTS
    // ============================================================================

    /// b2ComputeHull's limit. Not a design preference -- a hard engine cap.
    inline constexpr int MAX_HULL_POINTS = 8;
    inline constexpr int MIN_HULL_POINTS = 3;

    /// Array sizes in KitProfile. Class caps sit at or below these.
    inline constexpr int MAX_GUN_MOUNTS = 4;
    inline constexpr int MAX_DECOR_POINTS = 64;
    inline constexpr int MAX_ENGINE_MOUNTS = 4;

    inline constexpr float FORWARD_X = 0.f;
    inline constexpr float FORWARD_Y = -1.f;

    // ============================================================================
    // HULL CLASSES
    // ============================================================================

    enum class HullClass : std::uint8_t { Light = 0, Medium = 1, Heavy = 2 };
    inline constexpr int HULL_CLASS_COUNT = 3;

    struct HullClassSpec {
        const char* name;          ///< "LIGHT"
        const char* pattern;       ///< Name of the preset hull for this class
        float halfWidthPx;         ///< Frame: |x| <= this
        float foreYPx;             ///< Frame: y >= this (negative, forward)
        float aftYPx;              ///< Frame: y <= this
        float minAreaPx2;          ///< Tonnage band
        float maxAreaPx2;
        int   maxGuns;
        int   maxEngines;
        int   reactorMinUnits;     ///< At minAreaPx2
        int   reactorMaxUnits;     ///< At maxAreaPx2
        float rcsFraction;         ///< Any-direction thrust, as a share of forward thrust
        float minAccelRatio;       ///< Forward accel vs reference build; below = UNDERPOWERED
    };

    inline const HullClassSpec& classSpec(HullClass c) {
        static const HullClassSpec k[HULL_CLASS_COUNT] = {
            //  name      pattern         hw    fore   aft   minA   maxA  guns eng  rMin rMax  rcs    minAcc
            { "LIGHT",  "INTERCEPTOR",  22.f, -44.f, 30.f,  500.f, 1200.f, 2, 2,   5,   7, 0.65f, 0.80f },
            { "MEDIUM", "STANDARD",     32.f, -40.f, 34.f, 1200.f, 2600.f, 3, 3,   8,  11, 0.50f, 0.60f },
            { "HEAVY",  "BASTION",      44.f, -48.f, 42.f, 2600.f, 4600.f, 4, 4,  12,  15, 0.35f, 0.45f },
        };
        return k[static_cast<int>(c)];
    }

    // ============================================================================
    // TUNING
    // ============================================================================

    struct ShipTuning {
        // ---- Unit bridge ----
        float pixelsPerMetre = 30.f;   ///< MUST match SCALE in the physics headers.
        float density = 3.0f;          ///< Hull density, kg/m^2
        float linearDamping = 1.0f;    ///< Top-speed readout only

        // ---- Hull-derived pools. Calibrated so the stock hull reads 100/100. ----
        float referenceAreaPx2 = 1858.f;
        float hpAtReference = 100.f;
        float hpAreaExponent = 0.85f;      ///< Sublinear: big hulls tank, but not linearly
        float energyAtReference = 100.f;
        float energyAreaExponent = 0.20f;  ///< Nearly flat: size buys armour, not batteries

        // ---- Thrust ----
        float thrustPerEngine = 55.f;      ///< Flat per-engine contribution (raw units)
        float thrustPerEdgePx = 1.1f;      ///< Wider stern mounts more motor
        float thrustStackExponent = 0.6f;  ///< Total ~ N^0.6: the 4th engine adds little

        // ---- Reactor ----
        int   gunUpkeepUnits = 2;
        int   engineUpkeepUnits = 2;
        int   reactorBonusUnits = 0;       ///< Hook for run upgrades (reactor cores)
        float regenFloorFraction = 0.25f;  ///< Regen share left with a FULL reactor

        // ---- Mount masses ----
        float gunMassKg = 0.35f;
        float engineMassKg = 0.25f;
        float engineMassPerEdgePx = 0.01f; ///< Wide engines are heavy engines

        // ---- Mount validity ----
        float engineRearMinNormalY = 0.25f;
        float maxGunInteriorAngleDeg = 135.f;
        float minEdgePx = 6.f;
        float minGunSpacingPx = 16.f;      ///< Barrel clearance between mounted guns
        float minAreaPx2 = 400.f;          ///< Absolute sanity floor under any class

        // ---- Kit clamps: keep extremes playable ----
        float agilityMin = 0.70f, agilityMax = 1.40f;
        float riftPowerMin = 0.85f, riftPowerMax = 1.20f;
        float plasmaHeatPerExtraGun = 0.12f;  ///< Per-shot heat +12% for each extra plasma gun

        // ---- Decorative model ----
        float decorMaxAreaRatio = 1.5f;    ///< Model area / hitbox area
        float decorEnvelopeScale = 1.5f;   ///< Model points stay inside hitbox x this, about its centroid
        float muzzleSearchPx = 5.f;        ///< A model point this close to a gun's line can be its muzzle

        /// Accel ratios are raised to this power. 1.0 = pure physics;
        /// 0.6 turns a x1.57 light / x0.68 heavy into x1.31 / x0.79.
        float mobilityExponent = 0.6f;
    };

    // ============================================================================
    // MOUNTS
    // ============================================================================

    enum class MountKind { Gun, Engine };

    struct MountSlot {
        MountKind    kind = MountKind::Gun;
        int          index = 0;        ///< Vertex index (Gun) or edge start index (Engine)
        sf::Vector2f position;         ///< Local pixels. Vertex, or edge midpoint.
        sf::Vector2f outward;          ///< Unit outward direction
        bool         valid = false;
        float        interiorAngleDeg = 0.f;  ///< Gun only
        float        edgeLengthPx = 0.f;      ///< Engine only
        const char* reason = "";      ///< Why invalid. Shown in the editor.
    };

    // ============================================================================
    // DERIVED STATS
    // ============================================================================

    struct ShipStats {
        HullClass hullClass = HullClass::Medium;

        // ---- Geometry & mass ----
        float areaPx2 = 0.f;
        float hullMassKg = 0.f;
        float massKg = 0.f;            ///< Hull + mounts. What Box2D gets.
        float inertiaKgM2 = 0.f;       ///< About the TOTAL centre of mass
        float agility = 0.f;           ///< 1 / inertia. Compare against reference().
        float radiusPx = 0.f;
        sf::Vector2f hullCentroid;     ///< Local pixels
        sf::Vector2f centreOfMass;     ///< Local pixels, hull + mounts

        // ---- Pools ----
        float hpMax = 0.f;
        float energyMax = 0.f;

        // ---- Reactor ----
        int   reactorUnits = 0;
        int   reactorUsed = 0;
        float regenFraction = 0.f;     ///< floor + (1 - floor) * free / units

        // ---- Thrust (raw units -- only ratios against reference() mean anything) ----
        float forwardThrust = 0.f;     ///< Engines, straight ahead
        float strafeThrust = 0.f;      ///< Weaker side, RCS floor included
        float reverseThrust = 0.f;     ///< RCS only -- engines never face forward
        float rcsThrust = 0.f;
        float accel = 0.f;             ///< forwardThrust / mass
        float strafeAccel = 0.f;
        float reverseAccel = 0.f;
        float topSpeed = 0.f;

        sf::Vector2f thrustPoint;      ///< Forward-thrust-weighted engine position
        float lateralOffsetPx = 0.f;   ///< Signed. Non-zero = ship pulls under thrust.

        // ---- Weapons ----
        int  gunSlots = 0;             ///< Geometrically valid vertices
        int  gunCount = 0;
        int  engineCount = 0;
        int  spinalVertex = -1;        ///< Vertex the Rift fires from, -1 if unarmed
        bool riftShared = true;        ///< Effective: spinal also fires plasma
        int  primaryCount = 0;         ///< Guns that fire plasma
    };

    // ============================================================================
    // RUNTIME KIT
    // ============================================================================

    /**
     * @brief Everything the starting gear reads from the hull, ready for systems.
     *
     * Forces are in newtons, already calibrated. Everything named *Scale is a
     * multiplier on an existing Lua value; 1.0 means "as the pre-refit ship".
     */
    struct KitProfile {
        bool valid = false;
        HullClass hullClass = HullClass::Medium;

        // ---- Body ----
        float        massKg = 0.f;
        float        inertiaKgM2 = 0.f;
        sf::Vector2f centreOfMassPx;

        // ---- Movement ----
        int          engineCount = 0;
        sf::Vector2f enginePosPx[MAX_ENGINE_MOUNTS];
        sf::Vector2f engineDir[MAX_ENGINE_MOUNTS];      ///< Local, unit, direction the ship is PUSHED
        float        engineForceN[MAX_ENGINE_MOUNTS] = {};
        float        forwardForceN = 0.f;               ///< Turbo reads this
        float        rcsForceN = 0.f;                   ///< Floor in every direction
        float        yawTorqueFwdNm = 0.f;              ///< At full forward thrust. >0 turns clockwise.

        float agilityScale = 1.f;      ///< rotation_speed multiplier
        float regenScale = 1.f;        ///< sprint_regen_speed multiplier (reactor share)

        // ---- Weapons ----
        int          gunCount = 0;
        sf::Vector2f gunPosPx[MAX_GUN_MOUNTS];
        sf::Vector2f gunMuzzlePx[MAX_GUN_MOUNTS];      ///< Where bolts leave: a model spike tip, or the mount
        int          spinalSlot = 0;                    ///< Index into gunPosPx
        bool         riftShared = true;
        int          primarySlots[MAX_GUN_MOUNTS] = {}; ///< Plasma cycle order, left to right
        int          primaryCount = 0;
        float        fireIntervalScale = 1.f;           ///< fire_rate multiplier
        float        shotHeatScale = 1.f;               ///< shot_heat multiplier
        float        riftPower = 1.f;                   ///< Rift damage / burst multiplier
        float        riftCostScale = 1.f;               ///< rift_energy_cost multiplier
        float        shotEnergyScale = 1.f;             ///< shot_energy_cost multiplier (1/sqrt guns)
    };

    // ============================================================================
    // VALIDATION
    // ============================================================================

    struct ValidationResult {
        bool ok = false;
        bool tooFewPoints = false;
        bool tooManyPoints = false;
        bool outOfFrame = false;
        bool tooSmall = false;
        bool tooLarge = false;
        bool wasConcave = false;       ///< True if normalise() had to discard points
        bool noEngine = false;
        bool noGun = false;
        bool reactorOverload = false;
        bool underpowered = false;
        std::string message;
    };

    // ============================================================================
    // SMALL MATH
    // ============================================================================

    namespace detail {

        inline float cross(sf::Vector2f a, sf::Vector2f b) { return a.x * b.y - a.y * b.x; }
        inline float dot(sf::Vector2f a, sf::Vector2f b) { return a.x * b.x + a.y * b.y; }
        inline float len(sf::Vector2f a) { return std::sqrt(a.x * a.x + a.y * a.y); }

        inline sf::Vector2f norm(sf::Vector2f a) {
            const float l = len(a);
            return (l > 1e-6f) ? sf::Vector2f{ a.x / l, a.y / l } : sf::Vector2f{ 0.f, 0.f };
        }

        inline float signedArea(const std::vector<sf::Vector2f>& p) {
            float a = 0.f;
            const std::size_t n = p.size();
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f& c = p[i];
                const sf::Vector2f& d = p[(i + 1) % n];
                a += c.x * d.y - d.x * c.y;
            }
            return a * 0.5f;
        }

        /// Area-weighted centroid. NOT the vertex average.
        inline sf::Vector2f centroid(const std::vector<sf::Vector2f>& p) {
            const float a = signedArea(p);
            if (std::fabs(a) < 1e-4f) {
                sf::Vector2f s{ 0.f, 0.f };
                for (const auto& v : p) { s.x += v.x; s.y += v.y; }
                const float n = static_cast<float>(std::max<std::size_t>(1, p.size()));
                return { s.x / n, s.y / n };
            }
            float cx = 0.f, cy = 0.f;
            const std::size_t n = p.size();
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f& c = p[i];
                const sf::Vector2f& d = p[(i + 1) % n];
                const float w = c.x * d.y - d.x * c.y;
                cx += (c.x + d.x) * w;
                cy += (c.y + d.y) * w;
            }
            return { cx / (6.f * a), cy / (6.f * a) };
        }

        /**
         * @brief Polar moment of a solid polygon about the local ORIGIN, per unit density.
         *
         * Standard triangle-fan formula. Winding-independent via the area sign.
         * Units follow the input: feed metres, get m^4.
         */
        inline float polygonInertiaAboutOrigin(const std::vector<sf::Vector2f>& p) {
            const float a = signedArea(p);
            if (std::fabs(a) < 1e-8f) return 0.f;
            float sum = 0.f;
            const std::size_t n = p.size();
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f& c = p[i];
                const sf::Vector2f& d = p[(i + 1) % n];
                const float w = cross(c, d);
                sum += w * (dot(c, c) + dot(c, d) + dot(d, d));
            }
            return std::fabs(sum / 12.f);
        }

        /// Andrew's monotone chain.
        inline std::vector<sf::Vector2f> convexHull(std::vector<sf::Vector2f> pts) {
            if (pts.size() < 3) return pts;

            std::sort(pts.begin(), pts.end(), [](sf::Vector2f a, sf::Vector2f b) {
                return (a.x < b.x) || (a.x == b.x && a.y < b.y);
                });
            pts.erase(std::unique(pts.begin(), pts.end(), [](sf::Vector2f a, sf::Vector2f b) {
                return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f;
                }), pts.end());
            if (pts.size() < 3) return pts;

            const int n = static_cast<int>(pts.size());
            std::vector<sf::Vector2f> h(2 * n);
            int k = 0;

            for (int i = 0; i < n; ++i) {
                while (k >= 2 && cross({ h[k - 1].x - h[k - 2].x, h[k - 1].y - h[k - 2].y },
                    { pts[i].x - h[k - 2].x,   pts[i].y - h[k - 2].y }) <= 0.f) --k;
                h[k++] = pts[i];
            }
            for (int i = n - 2, t = k + 1; i >= 0; --i) {
                while (k >= t && cross({ h[k - 1].x - h[k - 2].x, h[k - 1].y - h[k - 2].y },
                    { pts[i].x - h[k - 2].x,   pts[i].y - h[k - 2].y }) <= 0.f) --k;
                h[k++] = pts[i];
            }
            h.resize(std::max(0, k - 1));
            return h;
        }

        /// Does a ray from vertex V in direction d re-enter the hull?
        inline bool directionBlocked(sf::Vector2f prev, sf::Vector2f v,
            sf::Vector2f next, sf::Vector2f d) {
            const sf::Vector2f a{ prev.x - v.x, prev.y - v.y };
            const sf::Vector2f b{ next.x - v.x, next.y - v.y };

            const float cab = cross(a, b);
            if (std::fabs(cab) < 1e-4f) return true;

            const float cad = cross(a, d);
            const float cdb = cross(d, b);
            const bool s = (cab > 0.f);
            return ((cad > 0.f) == s) && ((cdb > 0.f) == s);
        }

        /// Point-to-segment distance.
        inline float segDist(sf::Vector2f p, sf::Vector2f a, sf::Vector2f b) {
            const sf::Vector2f ab{ b.x - a.x, b.y - a.y };
            const float l2 = dot(ab, ab);
            const float t = (l2 > 1e-8f) ? std::clamp(dot({ p.x - a.x, p.y - a.y }, ab) / l2, 0.f, 1.f) : 0.f;
            return len({ a.x + ab.x * t - p.x, a.y + ab.y * t - p.y });
        }

        /// Even-odd ray cast. Concave-safe. Points within `tol` of an edge count as inside.
        inline bool pointInPolygon(sf::Vector2f p, const std::vector<sf::Vector2f>& poly, float tol = 0.5f) {
            const std::size_t n = poly.size();
            if (n < 3) return false;
            bool in = false;
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const sf::Vector2f& a = poly[i];
                const sf::Vector2f& b = poly[j];
                if (segDist(p, a, b) <= tol) return true;
                if (((a.y > p.y) != (b.y > p.y)) &&
                    (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) in = !in;
            }
            return in;
        }

        /// Strictly inside a convex polygon, by more than `margin`.
        inline bool strictlyInsideConvex(sf::Vector2f p, const std::vector<sf::Vector2f>& poly, float margin = 0.25f) {
            const std::size_t n = poly.size();
            if (n < 3) return false;
            const float sgn = (signedArea(poly) > 0.f) ? 1.f : -1.f;
            for (std::size_t i = 0; i < n; ++i) {
                const sf::Vector2f& a = poly[i];
                const sf::Vector2f& b = poly[(i + 1) % n];
                const sf::Vector2f e = norm({ b.x - a.x, b.y - a.y });
                if (sgn * cross(e, { p.x - a.x, p.y - a.y }) <= margin) return false;
            }
            return true;
        }

        /// Segments cross at a single interior point. Touching and collinear
        /// overlap do NOT count -- a model that shares an edge with the hitbox
        /// is legal.
        inline bool segmentsCrossProperly(sf::Vector2f a, sf::Vector2f b, sf::Vector2f c, sf::Vector2f d) {
            const float eps = 1e-3f;
            const float o1 = cross({ b.x - a.x, b.y - a.y }, { c.x - a.x, c.y - a.y });
            const float o2 = cross({ b.x - a.x, b.y - a.y }, { d.x - a.x, d.y - a.y });
            const float o3 = cross({ d.x - c.x, d.y - c.y }, { a.x - c.x, a.y - c.y });
            const float o4 = cross({ d.x - c.x, d.y - c.y }, { b.x - c.x, b.y - c.y });
            return (o1 * o2 < -eps) && (o3 * o4 < -eps);
        }

        /// No two non-adjacent edges cross, and no zero-length edges.
        inline bool isSimplePolygon(const std::vector<sf::Vector2f>& p) {
            const std::size_t n = p.size();
            if (n < 3) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (len({ p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y }) < 0.5f) return false;
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (j == i + 1 || (i == 0 && j == n - 1)) continue;
                    if (segmentsCrossProperly(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return false;
                }
            }
            return true;
        }

        /**
         * @brief Ear clipping. Returns a flat triangle list (3 points each).
         *
         * O(n^2) on <= 64 points, once per ship spawn. Falls back to a fan if
         * the polygon is degenerate, so the caller always gets something drawable.
         */
        inline std::vector<sf::Vector2f> triangulate(std::vector<sf::Vector2f> p) {
            std::vector<sf::Vector2f> tris;
            if (p.size() < 3) return tris;
            if (signedArea(p) < 0.f) std::reverse(p.begin(), p.end());

            std::vector<int> idx(p.size());
            for (std::size_t i = 0; i < p.size(); ++i) idx[i] = static_cast<int>(i);

            const auto inTri = [](sf::Vector2f q, sf::Vector2f a, sf::Vector2f b, sf::Vector2f c) {
                const float d1 = cross({ b.x - a.x, b.y - a.y }, { q.x - a.x, q.y - a.y });
                const float d2 = cross({ c.x - b.x, c.y - b.y }, { q.x - b.x, q.y - b.y });
                const float d3 = cross({ a.x - c.x, a.y - c.y }, { q.x - c.x, q.y - c.y });
                return d1 >= 0.f && d2 >= 0.f && d3 >= 0.f;
                };

            int guard = static_cast<int>(idx.size()) * static_cast<int>(idx.size()) + 8;
            while (idx.size() > 3 && guard-- > 0) {
                bool clipped = false;
                const int n = static_cast<int>(idx.size());
                for (int k = 0; k < n; ++k) {
                    const sf::Vector2f a = p[idx[(k - 1 + n) % n]], b = p[idx[k]], c = p[idx[(k + 1) % n]];
                    if (cross({ b.x - a.x, b.y - a.y }, { c.x - b.x, c.y - b.y }) <= 1e-4f) continue;  // reflex
                    bool empty = true;
                    for (int m = 0; m < n && empty; ++m) {
                        const int v = idx[m];
                        if (v == idx[(k - 1 + n) % n] || v == idx[k] || v == idx[(k + 1) % n]) continue;
                        if (inTri(p[v], a, b, c)) empty = false;
                    }
                    if (!empty) continue;
                    tris.push_back(a); tris.push_back(b); tris.push_back(c);
                    idx.erase(idx.begin() + k);
                    clipped = true;
                    break;
                }
                if (!clipped) break;
            }
            if (idx.size() == 3) {
                tris.push_back(p[idx[0]]); tris.push_back(p[idx[1]]); tris.push_back(p[idx[2]]);
            }
            else if (idx.size() > 3) {   // degenerate leftovers: fan them
                for (std::size_t k = 1; k + 1 < idx.size(); ++k) {
                    tris.push_back(p[idx[0]]); tris.push_back(p[idx[k]]); tris.push_back(p[idx[k + 1]]);
                }
            }
            return tris;
        }

        inline float interiorAngleDeg(sf::Vector2f prev, sf::Vector2f v, sf::Vector2f next) {
            const sf::Vector2f a = norm({ prev.x - v.x, prev.y - v.y });
            const sf::Vector2f b = norm({ next.x - v.x, next.y - v.y });
            const float c = std::clamp(dot(a, b), -1.f, 1.f);
            return std::acos(c) * 57.2957795f;
        }

    } // namespace detail

    // ============================================================================
    // DECOR CHECK
    // ============================================================================

    struct DecorCheck {
        bool  ok = true;
        bool  simple = true;
        bool  covers = true;
        bool  inEnvelope = true;
        bool  areaOk = true;
        float areaRatio = 1.f;
        int   violations = 0;      ///< Severity score; edits may not raise it
        const char* reason = "";
    };

    // ============================================================================
    // SHIP DESIGN
    // ============================================================================

    class ShipDesign {
    public:
        // ---- Construction ---------------------------------------------------

        /// The stock hull (MEDIUM), matching EntityFactory's legacy collision points.
        static ShipDesign stock() {
            return fromPoints({
                {   0.f, -30.f },   // nose
                {  28.f,  15.f },   // right wing tip
                {  18.f,  28.f },   // right stern
                { -18.f,  28.f },   // left stern
                { -28.f,  15.f }    // left wing tip
                }, HullClass::Medium);
        }

        static ShipDesign fromPoints(std::vector<sf::Vector2f> pts,
            HullClass cls = HullClass::Medium, bool symmetric = true) {
            ShipDesign d;
            d.m_class = cls;
            d.m_points = std::move(pts);
            d.m_symmetric = symmetric;
            d.normalise();
            return d;
        }

        /// The starting hull for each class, auto-mounted. What the class buttons load.
        static ShipDesign preset(HullClass c) {
            ShipDesign d;
            switch (c) {
            case HullClass::Light:
                d = fromPoints({ {   0.f, -34.f }, {  14.f,  16.f }, {   8.f,  26.f },
                                 {  -8.f,  26.f }, { -14.f,  16.f } }, HullClass::Light);
                d.autoMount();
                // Needle nose spike (the Rift muzzle) and swept tail fins.
                d.setDecor({ {0,-48}, {3,-32}, {8,-12}, {15,12}, {20,21}, {12,24}, {9,30}, {0,28},
                             {-9,30}, {-12,24}, {-20,21}, {-15,12}, {-8,-12}, {-3,-32} });
                return d;
            case HullClass::Medium:
                d = stock();
                d.autoMount();
                // Nose spike plus a spike above each wing gun: every muzzle is visible.
                d.setDecor({ {0,-42}, {5,-26}, {20,2}, {28,-3}, {31,10}, {36,20}, {24,24}, {20,34},
                             {8,30}, {0,32}, {-8,30}, {-20,34}, {-24,24}, {-36,20}, {-31,10},
                             {-28,-3}, {-20,2}, {-5,-26} });
                return d;
            case HullClass::Heavy:
                // Broad arrowhead. Convexity makes wide polygons blunt at every
                // corner, so a heavy that wants three guns has to keep a nose
                // and two genuinely swept wing tips.
                d = fromPoints({ {   0.f, -42.f }, {  38.f,  18.f }, {  26.f,  40.f },
                                 { -26.f,  40.f }, { -38.f,  18.f } }, HullClass::Heavy);
                d.autoMount();
                // Stepped armour shoulders, wing-gun spikes, blade wings, engine skirts.
                d.setDecor({ {0,-56}, {10,-30}, {24,-8}, {33,2}, {38,-4}, {43,12}, {50,22}, {40,30},
                             {32,46}, {14,44}, {0,48}, {-14,44}, {-32,46}, {-40,30}, {-50,22},
                             {-43,12}, {-38,-4}, {-33,2}, {-24,-8}, {-10,-30} });
                return d;
            }
            d.autoMount();
            return d;
        }

        /// Reference build every kit ratio is measured against: stock hull, auto-mounted.
        static const ShipStats& reference() {
            static const ShipStats ref = [] {
                ShipDesign d = stock();
                d.autoMount();
                return d.m_stats;
                }();
            return ref;
        }

        // ---- Class ----------------------------------------------------------

        HullClass hullClass() const { return m_class; }
        const HullClassSpec& spec() const { return classSpec(m_class); }

        /// True if the CURRENT hull already satisfies a class's frame and tonnage.
        bool fitsClass(HullClass c) const {
            const HullClassSpec& s = classSpec(c);
            for (const auto& p : m_points) if (!inFrame(p, s)) return false;
            const float a = std::fabs(detail::signedArea(m_points));
            return a >= s.minAreaPx2 && a <= s.maxAreaPx2;
        }

        /**
         * @brief Switch class, keeping the hull if it fits.
         * @return false (and no change) if the hull does not fit -- the editor
         *         then loads that class's preset instead of silently mangling it.
         */
        bool setHullClass(HullClass c) {
            if (!fitsClass(c)) return false;
            m_class = c;
            rebuild();
            trimMountsToClass();
            return true;
        }

        // ---- Editing --------------------------------------------------------

        const std::vector<sf::Vector2f>& points() const { return m_points; }
        int pointCount() const { return static_cast<int>(m_points.size()); }
        bool symmetric() const { return m_symmetric; }

        /// Why the last edit or mount was refused. Shown by the editor.
        const char* lastRejectReason() const { return m_reject; }

        void setSymmetric(bool on) {
            m_symmetric = on;
            if (on) enforceSymmetry();
        }

        /**
         * @brief Move a point, respecting symmetry, frame, convexity and tonnage.
         *
         * Rejection rather than silent correction. Tonnage uses a "never worse"
         * rule: a move that leaves the band is refused unless it moves the area
         * TOWARD the band, so a hull that is somehow outside can always be fixed.
         */
        bool movePoint(int i, sf::Vector2f pos) {
            if (i < 0 || i >= pointCount()) return false;

            const std::vector<sf::Vector2f> backup = m_points;
            const float oldArea = std::fabs(detail::signedArea(m_points));

            // Partner BEFORE the move. Looked up after, a fast drag (>24px from
            // the mirror position) lost its partner and snapped to the axis.
            const int partner = m_symmetric ? mirrorPartner(i) : -1;
            m_points[i] = pos;
            if (m_symmetric) {
                if (partner >= 0) m_points[partner] = { -pos.x, pos.y };
                else              m_points[i].x = 0.f;
            }

            const char* why = nullptr;
            if (!inFrame(m_points[i], spec()) ||
                (partner >= 0 && !inFrame(m_points[partner], spec()))) why = frameReason();
            else if (!isConvex())                                        why = "WOULD BREAK HULL";
            else if (!areaAcceptable(oldArea, std::fabs(detail::signedArea(m_points))))
                why = tonnageReason(std::fabs(detail::signedArea(m_points)));

            if (why) { m_points = backup; m_reject = why; return false; }
            rebuild();
            return true;
        }

        bool addPoint(sf::Vector2f pos) {
            if (pointCount() >= MAX_HULL_POINTS) { m_reject = "HULL FULL - 8 POINT LIMIT"; return false; }
            if (!inFrame(pos, spec())) { m_reject = frameReason(); return false; }

            const std::vector<sf::Vector2f> backup = m_points;
            const float oldArea = std::fabs(detail::signedArea(m_points));

            // Splice into the edge it stretches least.
            const int n = pointCount();
            int best = 0; float bestCost = 1e9f;
            for (int i = 0; i < n; ++i) {
                const sf::Vector2f& a = m_points[i];
                const sf::Vector2f& b = m_points[(i + 1) % n];
                const float cost = detail::len({ pos.x - a.x, pos.y - a.y })
                    + detail::len({ b.x - pos.x, b.y - pos.y })
                    - detail::len({ b.x - a.x, b.y - a.y });
                if (cost < bestCost) { bestCost = cost; best = i; }
            }
            m_points.insert(m_points.begin() + best + 1, pos);

            const char* why = nullptr;
            if (!isConvex()) why = "POINT WOULD BREAK HULL";
            else if (!areaAcceptable(oldArea, std::fabs(detail::signedArea(m_points))))
                why = tonnageReason(std::fabs(detail::signedArea(m_points)));
            if (why) { m_points = backup; m_reject = why; return false; }

            remapAfterInsert(best + 1);
            rebuild();
            return true;
        }

        bool removePoint(int i) {
            if (i < 0 || i >= pointCount()) return false;
            if (pointCount() <= MIN_HULL_POINTS) { m_reject = "MINIMUM 3 POINTS"; return false; }

            const std::vector<sf::Vector2f> backup = m_points;
            const float oldArea = std::fabs(detail::signedArea(m_points));
            m_points.erase(m_points.begin() + i);

            if (!areaAcceptable(oldArea, std::fabs(detail::signedArea(m_points)))) {
                m_reject = tonnageReason(std::fabs(detail::signedArea(m_points)));
                m_points = backup;
                return false;
            }
            remapAfterErase(i);
            rebuild();
            return true;
        }

        int mirrorPartner(int i) const {
            if (i < 0 || i >= pointCount()) return -1;
            if (std::fabs(m_points[i].x) < 1.5f) return -1;
            const sf::Vector2f m{ -m_points[i].x, m_points[i].y };
            int best = -1;
            float bestD = 1e9f;
            for (int j = 0; j < pointCount(); ++j) {
                if (j == i) continue;
                const float d = detail::len({ m_points[j].x - m.x, m_points[j].y - m.y });
                if (d < bestD) { bestD = d; best = j; }
            }
            return (bestD < 24.f) ? best : -1;
        }

        /// Snap symmetric by mirroring the left half onto the right.
        void enforceSymmetry() {
            std::vector<sf::Vector2f> out;
            for (const auto& p : m_points) {
                if (p.x <= 0.5f) {
                    out.push_back(p);
                    if (p.x < -0.5f) out.push_back({ -p.x, p.y });
                }
            }
            if (out.size() >= MIN_HULL_POINTS) {
                m_points = detail::convexHull(out);
                if (pointCount() > MAX_HULL_POINTS) m_points.resize(MAX_HULL_POINTS);
                // Re-hulling reorders vertices, so stored indices mean nothing now.
                m_mountedGuns.clear();
                m_mountedEngines.clear();
                m_spinal = -1;
            }
            normalise();
        }

        // ---- Mounts ---------------------------------------------------------

        const std::vector<MountSlot>& gunSlots() const { return m_gunSlots; }
        const std::vector<MountSlot>& engineSlots() const { return m_engineSlots; }
        const std::vector<int>& mountedGuns() const { return m_mountedGuns; }
        const std::vector<int>& mountedEngines() const { return m_mountedEngines; }

        bool isGunMounted(int v) const { return contains(m_mountedGuns, v); }
        bool isEngineMounted(int e) const { return contains(m_mountedEngines, e); }

        int reactorFree() const { return m_stats.reactorUnits - m_stats.reactorUsed; }

        bool mountGun(int vertexIndex) {
            const MountSlot* s = findSlot(m_gunSlots, vertexIndex);
            if (!s) { m_reject = "NO SUCH VERTEX"; return false; }
            if (!s->valid) { m_reject = s->reason; return false; }
            if (isGunMounted(vertexIndex)) { m_reject = "ALREADY ARMED"; return false; }
            if (static_cast<int>(m_mountedGuns.size()) >= spec().maxGuns) { m_reject = "CLASS WEAPON LIMIT"; return false; }
            if (reactorFree() < m_tuning.gunUpkeepUnits) { m_reject = "REACTOR AT CAPACITY"; return false; }
            if (tooCloseToGun(s->position, vertexIndex)) { m_reject = "NO BARREL CLEARANCE"; return false; }

            m_mountedGuns.push_back(vertexIndex);
            rebuildStats();
            return true;
        }

        bool mountEngine(int edgeIndex) {
            const MountSlot* s = findSlot(m_engineSlots, edgeIndex);
            if (!s) { m_reject = "NO SUCH EDGE"; return false; }
            if (!s->valid) { m_reject = s->reason; return false; }
            if (isEngineMounted(edgeIndex)) { m_reject = "ALREADY FITTED"; return false; }
            if (static_cast<int>(m_mountedEngines.size()) >= spec().maxEngines) { m_reject = "CLASS DRIVE LIMIT"; return false; }
            if (reactorFree() < m_tuning.engineUpkeepUnits) { m_reject = "REACTOR AT CAPACITY"; return false; }

            m_mountedEngines.push_back(edgeIndex);
            rebuildStats();
            return true;
        }

        bool unmountGun(int vertexIndex) {
            if (!erase(m_mountedGuns, vertexIndex)) return false;
            if (m_spinal == vertexIndex) m_spinal = -1;
            rebuildStats();
            return true;
        }

        bool unmountEngine(int edgeIndex) {
            if (!erase(m_mountedEngines, edgeIndex)) return false;
            rebuildStats();
            return true;
        }

        void unmountAll() {
            m_mountedGuns.clear();
            m_mountedEngines.clear();
            m_spinal = -1;
            m_riftSharedPref = false;
            rebuildStats();
        }

        /**
         * @brief Assign the Rift to a mounted gun. Assigning the current spinal
         *        mount again toggles SHARED / DEDICATED.
         */
        bool assignRift(int vertexIndex) {
            if (!isGunMounted(vertexIndex)) { m_reject = "MOUNT A GUN HERE FIRST"; return false; }
            if (m_stats.spinalVertex == vertexIndex) {
                if (m_mountedGuns.size() <= 1) { m_reject = "LONE GUN IS ALWAYS SHARED"; return false; }
                m_riftSharedPref = !m_riftSharedPref;
            }
            else {
                m_spinal = vertexIndex;
            }
            rebuildStats();
            return true;
        }

        /**
         * @brief Budget-aware balanced loadout.
         *
         * Mounts in symmetric GROUPS (an on-axis single, or a mirrored pair) so
         * the result never pulls to one side: best gun first (becomes the Rift),
         * then the best drive, then alternates guns and drives while at least one
         * reactor unit stays free for regeneration.
         */
        void autoMount() {
            unmountAll();

            auto gunGroups = buildGroups(m_gunSlots, true);
            auto engGroups = buildGroups(m_engineSlots, false);

            // Guns: prefer on-axis, then most forward. Engines: most forward thrust.
            std::sort(gunGroups.begin(), gunGroups.end(), [&](const Group& a, const Group& b) {
                const float ax = std::fabs(m_points[a.idx[0]].x), bx = std::fabs(m_points[b.idx[0]].x);
                if (std::fabs(ax - bx) > 0.5f) return ax < bx;
                return m_points[a.idx[0]].y < m_points[b.idx[0]].y;
                });
            std::sort(engGroups.begin(), engGroups.end(), [&](const Group& a, const Group& b) {
                return engineGroupScore(a) > engineGroupScore(b);
                });

            const auto tryGroup = [&](const Group& g, bool gun, int reserve) {
                const int cost = g.count * (gun ? m_tuning.gunUpkeepUnits : m_tuning.engineUpkeepUnits);
                const int cap = gun ? spec().maxGuns : spec().maxEngines;
                const int have = static_cast<int>(gun ? m_mountedGuns.size() : m_mountedEngines.size());
                if (have + g.count > cap || reactorFree() - cost < reserve) return false;
                for (int k = 0; k < g.count; ++k) {
                    if (gun) { if (!mountGun(g.idx[k])) { for (int j = 0; j < k; ++j) unmountGun(g.idx[j]);    return false; } }
                    else { if (!mountEngine(g.idx[k])) { for (int j = 0; j < k; ++j) unmountEngine(g.idx[j]); return false; } }
                }
                return true;
                };

            std::size_t gi = 0, ei = 0;
            while (gi < gunGroups.size() && !tryGroup(gunGroups[gi], true, 0)) ++gi;  // the Rift gun
            ++gi;
            while (ei < engGroups.size() && !tryGroup(engGroups[ei], false, 0)) ++ei; // first drive
            ++ei;

            bool progress = true;
            while (progress) {
                progress = false;
                for (; gi < gunGroups.size(); ++gi) if (tryGroup(gunGroups[gi], true, 1)) { ++gi; progress = true; break; }
                for (; ei < engGroups.size(); ++ei) if (tryGroup(engGroups[ei], false, 1)) { ++ei; progress = true; break; }
            }
            rebuildStats();
        }

        // ---- Decorative model ------------------------------------------------

        /// The model outline. Follows the hitbox until the player edits it.
        const std::vector<sf::Vector2f>& decor() const { return m_decor; }
        bool decorAuthored() const { return m_decorAuthored; }
        int  decorPointCount() const { return static_cast<int>(m_decor.size()); }

        /// Throw the model away and follow the hitbox again.
        void resetDecor() {
            m_decorAuthored = false;
            m_decor = m_points;
            m_decorCheck = checkDecor(m_decor);
        }

        const DecorCheck& decorCheck() const { return m_decorCheck; }

        /// What the game draws: the model if it is legal, otherwise the hitbox.
        const std::vector<sf::Vector2f>& renderOutline() const {
            return (m_decorAuthored && m_decorCheck.ok) ? m_decor : m_points;
        }

        /// Hitbox scaled about its centroid. Model points must stay inside.
        std::vector<sf::Vector2f> envelope() const {
            std::vector<sf::Vector2f> e = m_points;
            const sf::Vector2f c = detail::centroid(m_points);
            const float k = m_tuning.decorEnvelopeScale;
            for (auto& p : e) p = { c.x + (p.x - c.x) * k, c.y + (p.y - c.y) * k };
            return e;
        }

        std::vector<sf::Vector2f> renderTriangles() const {
            return detail::triangulate(renderOutline());
        }

        int decorMirrorPartner(int i) const {
            if (i < 0 || i >= decorPointCount()) return -1;
            if (std::fabs(m_decor[i].x) < 1.0f) return -1;
            const sf::Vector2f m{ -m_decor[i].x, m_decor[i].y };
            int best = -1; float bestD = 1e9f;
            for (int j = 0; j < decorPointCount(); ++j) {
                if (j == i) continue;
                const float d = detail::len({ m_decor[j].x - m.x, m_decor[j].y - m.y });
                if (d < bestD) { bestD = d; best = j; }
            }
            return (bestD < 3.f) ? best : -1;
        }

        bool moveDecorPoint(int i, sf::Vector2f pos) {
            if (i < 0 || i >= decorPointCount()) return false;
            std::vector<sf::Vector2f> next = m_decor;
            const int partner = m_symmetric ? decorMirrorPartner(i) : -1;
            next[i] = pos;
            if (m_symmetric) {
                if (partner >= 0) next[partner] = { -pos.x, pos.y };
                else if (std::fabs(m_decor[i].x) < 1.0f) next[i].x = 0.f;   // on-axis stays on-axis
            }
            return commitDecor(next);
        }

        /// Insert on the nearest edge. In mirror mode the mirrored point is added too.
        bool addDecorPoint(sf::Vector2f pos) {
            const bool pair = m_symmetric && std::fabs(pos.x) >= 1.0f;
            if (decorPointCount() + (pair ? 2 : 1) > MAX_DECOR_POINTS) {
                m_reject = "MODEL FULL - 64 POINT LIMIT"; return false;
            }
            if (m_symmetric && !pair) pos.x = 0.f;
            std::vector<sf::Vector2f> next = m_decor;
            insertOnNearestEdge(next, pos);
            if (pair) insertOnNearestEdge(next, { -pos.x, pos.y });
            return commitDecor(next);
        }

        bool removeDecorPoint(int i) {
            if (i < 0 || i >= decorPointCount()) return false;
            const int partner = m_symmetric ? decorMirrorPartner(i) : -1;
            if (decorPointCount() - (partner >= 0 ? 2 : 1) < MIN_HULL_POINTS) {
                m_reject = "MINIMUM 3 POINTS"; return false;
            }
            std::vector<sf::Vector2f> next = m_decor;
            if (partner >= 0) {
                next.erase(next.begin() + std::max(i, partner));
                next.erase(next.begin() + std::min(i, partner));
            }
            else next.erase(next.begin() + i);
            return commitDecor(next);
        }

        /// Replace the whole model (presets, tools). Same rules as an edit.
        bool setDecor(std::vector<sf::Vector2f> pts) {
            if (pts.size() < MIN_HULL_POINTS || pts.size() > MAX_DECOR_POINTS) return false;
            const DecorCheck c = checkDecor(pts);
            if (!c.ok) { m_reject = c.reason; return false; }
            m_decor = std::move(pts);
            m_decorAuthored = true;
            m_decorCheck = c;
            return true;
        }

        /// Where a gun's bolts leave: the most forward model point on the gun's
        /// line (a spike), else just ahead of the mount.
        sf::Vector2f muzzleFor(int vertexIndex) const {
            if (vertexIndex < 0 || vertexIndex >= pointCount()) return { 0.f, 0.f };
            const sf::Vector2f g = m_points[vertexIndex];
            sf::Vector2f best{ g.x, g.y - 2.f };
            for (const auto& q : renderOutline()) {
                if (std::fabs(q.x - g.x) <= m_tuning.muzzleSearchPx && q.y < best.y) best = { g.x, q.y };
            }
            return best;
        }

        DecorCheck checkDecor(const std::vector<sf::Vector2f>& d) const {
            DecorCheck c;
            if (d.size() < MIN_HULL_POINTS || m_points.size() < MIN_HULL_POINTS) {
                c.ok = false; c.simple = false; c.violations = 100000; c.reason = "MODEL TOO SMALL"; return c;
            }

            if (!detail::isSimplePolygon(d)) { c.simple = false; c.violations += 10000; }

            // 1. Covers the hitbox.
            for (const auto& h : m_points)
                if (!detail::pointInPolygon(h, d)) { c.covers = false; c.violations += 1000; }
            for (const auto& q : d)
                if (detail::strictlyInsideConvex(q, m_points)) { c.covers = false; c.violations += 1000; }
            const std::size_t hn = m_points.size(), dn = d.size();
            for (std::size_t i = 0; i < dn; ++i)
                for (std::size_t j = 0; j < hn; ++j)
                    if (detail::segmentsCrossProperly(d[i], d[(i + 1) % dn], m_points[j], m_points[(j + 1) % hn])) {
                        c.covers = false; c.violations += 1000;
                    }

            // 2. Envelope.
            const std::vector<sf::Vector2f> env = envelope();
            for (const auto& q : d)
                if (!detail::pointInPolygon(q, env, 0.75f)) { c.inEnvelope = false; c.violations += 1000; }

            // 3. Area.
            const float ha = std::max(1.f, std::fabs(detail::signedArea(m_points)));
            c.areaRatio = std::fabs(detail::signedArea(d)) / ha;
            if (c.areaRatio > m_tuning.decorMaxAreaRatio + 1e-3f) {
                c.areaOk = false;
                c.violations += 1 + static_cast<int>((c.areaRatio - m_tuning.decorMaxAreaRatio) * 1000.f);
            }

            c.ok = c.simple && c.covers && c.inEnvelope && c.areaOk;
            c.reason = !c.simple ? "MODEL CROSSES ITSELF"
                : !c.covers ? "MODEL MUST COVER THE HULL"
                : !c.inEnvelope ? "TOO FAR FROM THE HULL"
                : !c.areaOk ? "MODEL OVER 150% OF HULL AREA" : "";
            return c;
        }

        // ---- Output ---------------------------------------------------------

    private:
        /// Accept a model edit if it is legal, or -- when the model is already
        /// broken by a hitbox edit -- if it makes things no worse. "No worse",
        /// not "better": a real fix often needs a sideways step first, and a
        /// strict rule leaves the player stuck with nothing but RESET.
        bool commitDecor(const std::vector<sf::Vector2f>& next) {
            const DecorCheck c = checkDecor(next);
            const bool accept = c.ok || (!m_decorCheck.ok && c.simple && c.violations <= m_decorCheck.violations);
            if (!accept) { m_reject = c.reason; return false; }
            m_decor = next;
            m_decorAuthored = true;
            m_decorCheck = c;
            return true;
        }

        static void insertOnNearestEdge(std::vector<sf::Vector2f>& poly, sf::Vector2f pos) {
            const int n = static_cast<int>(poly.size());
            int best = 0; float bestD = 1e9f;
            for (int i = 0; i < n; ++i) {
                const float d = detail::segDist(pos, poly[i], poly[(i + 1) % n]);
                if (d < bestD) { bestD = d; best = i; }
            }
            poly.insert(poly.begin() + best + 1, pos);
        }

        float rawAccelRatio() const {
            const ShipStats& ref = reference();
            return (ref.accel > 1e-5f) ? m_stats.accel / ref.accel : 0.f;
        }

    public:

        const ShipStats& stats() const { return m_stats; }

        /// Forward acceleration vs the reference build, AFTER compression.
        /// What the player actually flies; the refit readout shows this.
        float mobilityRatio() const {
            const float raw = rawAccelRatio();
            return (raw > 1e-5f) ? std::pow(raw, m_tuning.mobilityExponent) : 0.f;
        }
        /// Multiply a raw accel ratio (strafe, reverse) by this to compress it the same way.
        float mobilityFactor() const {
            const float raw = rawAccelRatio();
            return (raw > 1e-5f) ? std::pow(raw, m_tuning.mobilityExponent - 1.f) : 1.f;
        }
        float agilityRatio() const {
            const ShipStats& ref = reference();
            return std::clamp(std::sqrt(ref.inertiaKgM2 / std::max(1e-5f, m_stats.inertiaKgM2)),
                m_tuning.agilityMin, m_tuning.agilityMax);
        }
        const ShipTuning& tuning() const { return m_tuning; }
        void setTuning(const ShipTuning& t) { m_tuning = t; rebuild(); }
        const std::vector<sf::Vector2f>& outline() const { return m_points; }

        ValidationResult validate() const {
            ValidationResult r;
            const HullClassSpec& cs = spec();
            const ShipStats& ref = reference();

            r.tooFewPoints = pointCount() < MIN_HULL_POINTS;
            r.tooManyPoints = pointCount() > MAX_HULL_POINTS;
            for (const auto& p : m_points) if (!inFrame(p, cs)) r.outOfFrame = true;
            r.tooSmall = m_stats.areaPx2 < std::max(cs.minAreaPx2, m_tuning.minAreaPx2);
            r.tooLarge = m_stats.areaPx2 > cs.maxAreaPx2;
            r.wasConcave = m_discardedPoints > 0;
            r.noEngine = m_mountedEngines.empty();
            r.noGun = m_mountedGuns.empty();
            r.reactorOverload = m_stats.reactorUsed > m_stats.reactorUnits;
            r.underpowered = !r.noEngine && ref.accel > 1e-4f
                && (m_stats.accel / ref.accel) < cs.minAccelRatio;

            r.ok = !r.tooFewPoints && !r.tooManyPoints && !r.outOfFrame && !r.tooSmall
                && !r.tooLarge && !r.noEngine && !r.noGun && !r.reactorOverload && !r.underpowered;

            if (r.tooFewPoints)         r.message = "Hull needs at least 3 points.";
            else if (r.tooManyPoints)   r.message = "Hull exceeds 8 points.";
            else if (r.outOfFrame)      r.message = std::string("Hull breaks the ") + cs.name + " frame.";
            else if (r.tooSmall)        r.message = std::string("Too light for ") + cs.name + " class.";
            else if (r.tooLarge)        r.message = std::string("Too heavy for ") + cs.name + " class.";
            else if (r.noEngine)        r.message = "No drive mounted.";
            else if (r.noGun)           r.message = "No weapon mounted.";
            else if (r.reactorOverload) r.message = "Reactor overloaded.";
            else if (r.underpowered)    r.message = "Underpowered - add or widen drives.";
            else                        r.message = "Airworthy.";
            return r;
        }

        /**
         * @brief Build the runtime kit.
         * @param enginePowerN The Lua engine_power the pre-refit ship used. The
         *        reference build is calibrated to accelerate exactly as that did.
         */
        KitProfile kit(float enginePowerN) const {
            KitProfile k;
            const ShipStats& ref = reference();
            const ShipStats& s = m_stats;
            if (m_points.size() < MIN_HULL_POINTS || s.massKg <= 1e-4f) return k;

            const float ppm = std::max(1.f, m_tuning.pixelsPerMetre);

            // raw thrust -> newtons, such that the reference build accelerates at
            // engine_power / hullMass: what the old hull-only body did.
            const float calib = (ref.forwardThrust > 1e-4f && ref.hullMassKg > 1e-4f)
                ? enginePowerN * (ref.massKg / ref.hullMassKg) / ref.forwardThrust
                * mobilityFactor() : 0.f;

            k.valid = true;
            k.hullClass = m_class;
            k.massKg = s.massKg;
            k.inertiaKgM2 = s.inertiaKgM2;
            k.centreOfMassPx = s.centreOfMass;

            // ---- Engines ----
            const float stack = engineStackFactor();
            for (int idx : m_mountedEngines) {
                const MountSlot* e = findSlot(m_engineSlots, idx);
                if (!e || k.engineCount >= MAX_ENGINE_MOUNTS) continue;
                const float f = rawEngineThrust(*e) * stack * calib;
                k.enginePosPx[k.engineCount] = e->position;
                k.engineDir[k.engineCount] = { -e->outward.x, -e->outward.y };
                k.engineForceN[k.engineCount] = f;

                // Forward component only (see InputSystem): a pair fired for a
                // strafe would otherwise yaw every symmetric ship.
                const float rx = (e->position.x - s.centreOfMass.x) / ppm;
                k.yawTorqueFwdNm += -rx * f * e->outward.y;
                ++k.engineCount;
            }
            k.forwardForceN = s.forwardThrust * calib;
            k.rcsForceN = s.rcsThrust * calib;

            k.agilityScale = agilityRatio();
            k.regenScale = (ref.regenFraction > 1e-4f) ? s.regenFraction / ref.regenFraction : 1.f;

            // ---- Guns ----
            for (int v : m_mountedGuns) {
                if (k.gunCount >= MAX_GUN_MOUNTS) break;
                if (v == s.spinalVertex) k.spinalSlot = k.gunCount;
                k.gunMuzzlePx[k.gunCount] = muzzleFor(v);
                k.gunPosPx[k.gunCount++] = m_points[v];
            }
            k.riftShared = s.riftShared;

            for (int g = 0; g < k.gunCount; ++g) {
                if (!k.riftShared && g == k.spinalSlot && k.gunCount > 1) continue;
                k.primarySlots[k.primaryCount++] = g;
            }
            std::sort(k.primarySlots, k.primarySlots + k.primaryCount,
                [&](int a, int b) { return k.gunPosPx[a].x < k.gunPosPx[b].x; });

            const float np = static_cast<float>(std::max(1, k.primaryCount));
            k.fireIntervalScale = 1.f / std::sqrt(np);
            // More barrels fire more often, so each shot costs less: energy per
            // SECOND of plasma is the same on every ship. Heat is the limiter.
            k.shotEnergyScale = k.fireIntervalScale;
            k.shotHeatScale = 1.f + m_tuning.plasmaHeatPerExtraGun * (np - 1.f);

            const float eRatio = s.energyMax / std::max(1.f, ref.energyMax);
            k.riftCostScale = eRatio;
            k.riftPower = std::clamp(std::sqrt(eRatio), m_tuning.riftPowerMin, m_tuning.riftPowerMax);
            return k;
        }

        std::string describe() const {
            char buf[2400];
            const ValidationResult v = validate();
            const ShipStats& s = m_stats;
            const ShipStats& ref = reference();
            const auto ratio = [](float a, float b) { return (b > 1e-6f) ? a / b : 0.f; };
            std::snprintf(buf, sizeof(buf),
                "  %s   points %d   area %.0f px2 (band %.0f-%.0f)\n"
                "  mass %.2f kg (hull %.2f)   inertia %.3f   hp %.0f   energy %.0f\n"
                "  reactor %d/%d   regen x%.2f\n"
                "  accel x%.2f   strafe x%.2f   reverse x%.2f   agility x%.2f\n"
                "  guns %d/%d (slots %d)  engines %d/%d  rift@%d %s  plasma guns %d\n"
                "  CoM (%.1f, %.1f)   lateral offset %.2f px   discarded %d\n"
                "  %s\n",
                spec().name, pointCount(), s.areaPx2, spec().minAreaPx2, spec().maxAreaPx2,
                s.massKg, s.hullMassKg, s.inertiaKgM2, s.hpMax, s.energyMax,
                s.reactorUsed, s.reactorUnits, ratio(s.regenFraction, ref.regenFraction),
                mobilityRatio(), ratio(s.strafeAccel, ref.accel) * mobilityFactor(),
                ratio(s.reverseAccel, ref.accel) * mobilityFactor(), agilityRatio(),
                s.gunCount, spec().maxGuns, s.gunSlots, s.engineCount, spec().maxEngines,
                s.spinalVertex, s.riftShared ? "SHARED" : "DEDICATED", s.primaryCount,
                s.centreOfMass.x, s.centreOfMass.y, s.lateralOffsetPx, m_discardedPoints,
                v.message.c_str());
            return std::string(buf);
        }

    private:
        // ========================================================================
        // HELPERS
        // ========================================================================

        static bool contains(const std::vector<int>& v, int x) {
            return std::find(v.begin(), v.end(), x) != v.end();
        }
        static bool erase(std::vector<int>& v, int x) {
            const auto it = std::find(v.begin(), v.end(), x);
            if (it == v.end()) return false;
            v.erase(it);
            return true;
        }
        static const MountSlot* findSlot(const std::vector<MountSlot>& slots, int idx) {
            for (const auto& s : slots) if (s.index == idx) return &s;
            return nullptr;
        }

        static bool inFrame(sf::Vector2f p, const HullClassSpec& s) {
            return std::fabs(p.x) <= s.halfWidthPx + 0.01f
                && p.y >= s.foreYPx - 0.01f && p.y <= s.aftYPx + 0.01f;
        }

        const char* frameReason() const {
            switch (m_class) {
            case HullClass::Light:  return "OUTSIDE LIGHT FRAME";
            case HullClass::Medium: return "OUTSIDE MEDIUM FRAME";
            default:                return "OUTSIDE HEAVY FRAME";
            }
        }
        const char* tonnageReason(float area) const {
            return (area > spec().maxAreaPx2) ? "EXCEEDS CLASS TONNAGE" : "BELOW CLASS TONNAGE";
        }

        /// In band, or moving toward it. Never lets an edit make tonnage worse.
        bool areaAcceptable(float oldA, float newA) const {
            const float lo = std::max(spec().minAreaPx2, m_tuning.minAreaPx2);
            const float hi = spec().maxAreaPx2;
            if (newA >= lo && newA <= hi) return true;
            const auto dist = [&](float a) { return (a < lo) ? lo - a : (a > hi ? a - hi : 0.f); };
            return dist(newA) < dist(oldA);
        }

        bool tooCloseToGun(sf::Vector2f pos, int self) const {
            for (int g : m_mountedGuns) {
                if (g == self || g < 0 || g >= pointCount()) continue;
                const sf::Vector2f& q = m_points[g];
                if (detail::len({ q.x - pos.x, q.y - pos.y }) < m_tuning.minGunSpacingPx) return true;
            }
            return false;
        }

        float rawEngineThrust(const MountSlot& e) const {
            return m_tuning.thrustPerEngine + e.edgeLengthPx * m_tuning.thrustPerEdgePx;
        }
        float engineStackFactor() const {
            const int n = static_cast<int>(m_mountedEngines.size());
            return (n > 0) ? std::pow(static_cast<float>(n), m_tuning.thrustStackExponent - 1.f) : 0.f;
        }

        // ---- Symmetric mount groups for autoMount ----
        struct Group { int idx[2] = { -1, -1 }; int count = 0; };

        std::vector<Group> buildGroups(const std::vector<MountSlot>& slots, bool vertices) const {
            std::vector<Group> out;
            std::vector<int> used;
            for (const auto& s : slots) {
                if (!s.valid || contains(used, s.index)) continue;
                Group g;
                g.idx[0] = s.index; g.count = 1;
                used.push_back(s.index);
                if (std::fabs(s.position.x) > 1.5f) {
                    for (const auto& o : slots) {
                        if (!o.valid || contains(used, o.index)) continue;
                        if (std::fabs(o.position.x + s.position.x) < 2.f
                            && std::fabs(o.position.y - s.position.y) < 2.f) {
                            g.idx[1] = o.index; g.count = 2;
                            used.push_back(o.index);
                            break;
                        }
                    }
                }
                (void)vertices;
                out.push_back(g);
            }
            return out;
        }

        float engineGroupScore(const Group& g) const {
            float f = 0.f;
            for (int k = 0; k < g.count; ++k)
                if (const MountSlot* e = findSlot(m_engineSlots, g.idx[k]))
                    f += rawEngineThrust(*e) * e->outward.y;
            return f;
        }

        // ---- Index bookkeeping ----
        // Mounts are stored by vertex/edge index. Inserting or erasing a point
        // shifts every later index, which used to silently move mounts to other
        // corners. These keep mounts attached to the geometry they were put on.

        void remapAfterInsert(int at) {
            // New vertex at `at` splits edge (at - 1): that engine loses its surface.
            const int split = at - 1;
            for (auto& g : m_mountedGuns) if (g >= at) ++g;
            if (m_spinal >= at) ++m_spinal;
            m_mountedEngines.erase(std::remove(m_mountedEngines.begin(), m_mountedEngines.end(), split),
                m_mountedEngines.end());
            for (auto& e : m_mountedEngines) if (e > split) ++e;
        }

        void remapAfterErase(int at) {
            const int n = pointCount() + 1;   // count before the erase
            erase(m_mountedGuns, at);
            if (m_spinal == at) m_spinal = -1;
            for (auto& g : m_mountedGuns) if (g > at) --g;
            if (m_spinal > at) --m_spinal;

            // Edges (at - 1) and (at) merge into one: both mounts lose their surface.
            const int before = (at - 1 + n) % n;
            erase(m_mountedEngines, before);
            erase(m_mountedEngines, at);
            for (auto& e : m_mountedEngines) if (e > at) --e;
            // Erasing vertex 0 wraps edge n-1 onto the new last edge.
            if (at == 0) for (auto& e : m_mountedEngines) if (e >= n - 1) e = n - 2;
        }

        void trimMountsToClass() {
            while (static_cast<int>(m_mountedGuns.size()) > spec().maxGuns) m_mountedGuns.pop_back();
            while (static_cast<int>(m_mountedEngines.size()) > spec().maxEngines) m_mountedEngines.pop_back();
            rebuildStats();
            while (m_stats.reactorUsed > m_stats.reactorUnits && !m_mountedEngines.empty()
                && (m_mountedEngines.size() > 1 || m_mountedGuns.size() <= 1)) {
                m_mountedEngines.pop_back(); rebuildStats();
            }
            while (m_stats.reactorUsed > m_stats.reactorUnits && m_mountedGuns.size() > 1) {
                m_mountedGuns.pop_back(); rebuildStats();
            }
        }

        // ========================================================================
        // REBUILD
        // ========================================================================

        void rebuild() {
            rebuildSlots();
            pruneMounts();
            rebuildStats();
            // An untouched model follows the hitbox; an authored one is re-judged
            // against the new hitbox and flagged if the edit broke it.
            if (!m_decorAuthored) m_decor = m_points;
            m_decorCheck = checkDecor(m_decor);
        }

        /// Canonical convex ordering. CONSTRUCTION ONLY -- re-hulling on every
        /// edit reorders the ring and editor handles teleport.
        void normalise() {
            const int before = pointCount();
            m_points = detail::convexHull(m_points);
            if (pointCount() > MAX_HULL_POINTS) m_points.resize(MAX_HULL_POINTS);
            m_discardedPoints = std::max(0, before - pointCount());
            rebuild();
        }

        bool isConvex() const {
            const int n = pointCount();
            if (n < 3) return false;
            int sign = 0;
            for (int i = 0; i < n; ++i) {
                const sf::Vector2f& a = m_points[i];
                const sf::Vector2f& b = m_points[(i + 1) % n];
                const sf::Vector2f& c = m_points[(i + 2) % n];
                const float z = detail::cross({ b.x - a.x, b.y - a.y }, { c.x - b.x, c.y - b.y });
                if (std::fabs(z) < 1e-3f) continue;
                const int s = (z > 0.f) ? 1 : -1;
                if (sign == 0) sign = s;
                else if (s != sign) return false;
            }
            return true;
        }

        sf::Vector2f edgeOutward(int i, sf::Vector2f c) const {
            const int n = pointCount();
            const sf::Vector2f& a = m_points[i];
            const sf::Vector2f& b = m_points[(i + 1) % n];
            const sf::Vector2f e{ b.x - a.x, b.y - a.y };
            sf::Vector2f nrm = detail::norm({ e.y, -e.x });
            const sf::Vector2f mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
            if (detail::dot(nrm, { mid.x - c.x, mid.y - c.y }) < 0.f) nrm = { -nrm.x, -nrm.y };
            return nrm;
        }

        void rebuildSlots() {
            m_gunSlots.clear();
            m_engineSlots.clear();

            const int n = pointCount();
            if (n < MIN_HULL_POINTS) return;

            const sf::Vector2f c = detail::centroid(m_points);
            const sf::Vector2f fwd{ FORWARD_X, FORWARD_Y };

            for (int i = 0; i < n; ++i) {
                const sf::Vector2f& v = m_points[i];
                const sf::Vector2f& prev = m_points[(i - 1 + n) % n];
                const sf::Vector2f& next = m_points[(i + 1) % n];

                MountSlot s;
                s.kind = MountKind::Gun;
                s.index = i;
                s.position = v;
                s.outward = detail::norm({ v.x - c.x, v.y - c.y });
                s.interiorAngleDeg = detail::interiorAngleDeg(prev, v, next);

                if (detail::directionBlocked(prev, v, next, fwd)) {
                    s.valid = false; s.reason = "FIRES THROUGH OWN HULL";
                }
                else if (s.interiorAngleDeg > m_tuning.maxGunInteriorAngleDeg) {
                    s.valid = false; s.reason = "VERTEX TOO BLUNT";
                }
                else {
                    s.valid = true; s.reason = "";
                }
                m_gunSlots.push_back(s);
            }

            for (int i = 0; i < n; ++i) {
                const sf::Vector2f& a = m_points[i];
                const sf::Vector2f& b = m_points[(i + 1) % n];

                MountSlot s;
                s.kind = MountKind::Engine;
                s.index = i;
                s.position = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
                s.outward = edgeOutward(i, c);
                s.edgeLengthPx = detail::len({ b.x - a.x, b.y - a.y });

                if (s.edgeLengthPx < m_tuning.minEdgePx) {
                    s.valid = false; s.reason = "EDGE TOO SHORT";
                }
                else if (s.outward.y < m_tuning.engineRearMinNormalY) {
                    s.valid = false; s.reason = "DOES NOT FACE AFT";
                }
                else {
                    s.valid = true; s.reason = "";
                }
                m_engineSlots.push_back(s);
            }
        }

        /// Drop mounts whose geometry no longer qualifies, including guns dragged
        /// into each other's barrels (the later mount loses).
        void pruneMounts() {
            m_mountedGuns.erase(std::remove_if(m_mountedGuns.begin(), m_mountedGuns.end(),
                [&](int i) { const MountSlot* s = findSlot(m_gunSlots, i); return !s || !s->valid; }),
                m_mountedGuns.end());
            m_mountedEngines.erase(std::remove_if(m_mountedEngines.begin(), m_mountedEngines.end(),
                [&](int i) { const MountSlot* s = findSlot(m_engineSlots, i); return !s || !s->valid; }),
                m_mountedEngines.end());

            std::vector<int> kept;
            for (int g : m_mountedGuns) {
                bool clear = true;
                for (int k : kept) {
                    const sf::Vector2f& a = m_points[g];
                    const sf::Vector2f& b = m_points[k];
                    if (detail::len({ a.x - b.x, a.y - b.y }) < m_tuning.minGunSpacingPx) { clear = false; break; }
                }
                if (clear) kept.push_back(g);
            }
            m_mountedGuns = kept;
            if (!contains(m_mountedGuns, m_spinal)) m_spinal = -1;
        }

        int reactorUnitsForArea(float area) const {
            const HullClassSpec& cs = spec();
            const float span = std::max(1.f, cs.maxAreaPx2 - cs.minAreaPx2);
            const float t = std::clamp((area - cs.minAreaPx2) / span, 0.f, 1.f);
            const float units = static_cast<float>(cs.reactorMinUnits)
                + static_cast<float>(cs.reactorMaxUnits - cs.reactorMinUnits) * t;
            return static_cast<int>(std::floor(units + 1e-3f)) + m_tuning.reactorBonusUnits;
        }

        void rebuildStats() {
            ShipStats s;
            s.hullClass = m_class;
            const int n = pointCount();
            if (n < MIN_HULL_POINTS) { m_stats = s; return; }

            const HullClassSpec& cs = spec();
            const float ppm = std::max(1.f, m_tuning.pixelsPerMetre);

            // ---- Hull ----
            s.areaPx2 = std::fabs(detail::signedArea(m_points));
            s.hullCentroid = detail::centroid(m_points);
            for (const auto& p : m_points)
                s.radiusPx = std::max(s.radiusPx,
                    detail::len({ p.x - s.hullCentroid.x, p.y - s.hullCentroid.y }));

            std::vector<sf::Vector2f> pm(m_points.size());
            for (std::size_t i = 0; i < m_points.size(); ++i)
                pm[i] = { m_points[i].x / ppm, m_points[i].y / ppm };

            s.hullMassKg = (s.areaPx2 / (ppm * ppm)) * m_tuning.density;
            const sf::Vector2f cM{ s.hullCentroid.x / ppm, s.hullCentroid.y / ppm };
            const float hullIc = m_tuning.density * detail::polygonInertiaAboutOrigin(pm)
                - s.hullMassKg * detail::dot(cM, cM);

            // ---- Pools ----
            const float aRatio = s.areaPx2 / std::max(1.f, m_tuning.referenceAreaPx2);
            s.hpMax = m_tuning.hpAtReference * std::pow(aRatio, m_tuning.hpAreaExponent);
            s.energyMax = m_tuning.energyAtReference * std::pow(aRatio, m_tuning.energyAreaExponent);

            // ---- Point masses: every mount, at its position ----
            struct PM { sf::Vector2f p; float m; };
            std::vector<PM> masses;
            for (int v : m_mountedGuns) masses.push_back({ m_points[v], m_tuning.gunMassKg });
            for (int e : m_mountedEngines)
                if (const MountSlot* es = findSlot(m_engineSlots, e))
                    masses.push_back({ es->position,
                        m_tuning.engineMassKg + es->edgeLengthPx * m_tuning.engineMassPerEdgePx });

            float M = s.hullMassKg;
            sf::Vector2f mc{ s.hullCentroid.x * s.hullMassKg, s.hullCentroid.y * s.hullMassKg };
            for (const auto& q : masses) { M += q.m; mc.x += q.p.x * q.m; mc.y += q.p.y * q.m; }
            s.massKg = M;
            s.centreOfMass = { mc.x / M, mc.y / M };

            const auto distM2 = [&](sf::Vector2f p) {
                const float dx = (p.x - s.centreOfMass.x) / ppm, dy = (p.y - s.centreOfMass.y) / ppm;
                return dx * dx + dy * dy;
                };
            float I = std::max(1e-5f, hullIc) + s.hullMassKg * distM2(s.hullCentroid);
            for (const auto& q : masses) I += q.m * distM2(q.p);
            s.inertiaKgM2 = I;
            s.agility = 1.f / I;

            // ---- Reactor ----
            s.gunCount = static_cast<int>(m_mountedGuns.size());
            s.engineCount = static_cast<int>(m_mountedEngines.size());
            s.reactorUnits = reactorUnitsForArea(s.areaPx2);
            s.reactorUsed = s.gunCount * m_tuning.gunUpkeepUnits + s.engineCount * m_tuning.engineUpkeepUnits;
            const int freeUnits = std::max(0, s.reactorUnits - s.reactorUsed);
            s.regenFraction = m_tuning.regenFloorFraction + (1.f - m_tuning.regenFloorFraction)
                * static_cast<float>(freeUnits) / static_cast<float>(std::max(1, s.reactorUnits));

            // ---- Thrust ----
            const float stack = engineStackFactor();
            float left = 0.f, right = 0.f, tpW = 0.f;
            sf::Vector2f tp{ 0.f, 0.f };
            for (int idx : m_mountedEngines) {
                const MountSlot* e = findSlot(m_engineSlots, idx);
                if (!e) continue;
                const float t = rawEngineThrust(*e) * stack;
                const float fwd = t * e->outward.y;
                s.forwardThrust += fwd;
                left += t * std::max(0.f, e->outward.x);    // right-side nozzle pushes left
                right += t * std::max(0.f, -e->outward.x);
                tp.x += e->position.x * fwd; tp.y += e->position.y * fwd; tpW += fwd;
            }
            if (tpW > 1e-4f) s.thrustPoint = { tp.x / tpW, tp.y / tpW };
            s.rcsThrust = cs.rcsFraction * s.forwardThrust;
            s.strafeThrust = std::max(s.rcsThrust, std::min(left, right));
            s.reverseThrust = s.rcsThrust;

            s.accel = s.forwardThrust / M;
            s.strafeAccel = s.strafeThrust / M;
            s.reverseAccel = s.reverseThrust / M;
            s.topSpeed = s.forwardThrust / (M * std::max(0.01f, m_tuning.linearDamping));
            if (tpW > 1e-4f) s.lateralOffsetPx = s.thrustPoint.x - s.centreOfMass.x;

            // ---- Weapons ----
            for (const auto& g : m_gunSlots) if (g.valid) ++s.gunSlots;
            resolveSpinal(s);

            m_stats = s;
        }

        /// Pick the Rift mount: the player's choice if still mounted, otherwise
        /// the gun nearest the centreline, ties to the most forward.
        void resolveSpinal(ShipStats& s) {
            if (m_mountedGuns.empty()) { m_spinal = -1; s.spinalVertex = -1; s.primaryCount = 0; return; }
            if (!contains(m_mountedGuns, m_spinal)) {
                int best = m_mountedGuns.front();
                for (int g : m_mountedGuns) {
                    const float bx = std::fabs(m_points[best].x), gx = std::fabs(m_points[g].x);
                    if (gx < bx - 0.5f || (std::fabs(gx - bx) <= 0.5f && m_points[g].y < m_points[best].y))
                        best = g;
                }
                m_spinal = best;
            }
            s.spinalVertex = m_spinal;
            s.riftShared = (m_mountedGuns.size() == 1) || m_riftSharedPref;
            s.primaryCount = s.riftShared ? s.gunCount : s.gunCount - 1;
        }

        std::vector<sf::Vector2f> m_points;
        std::vector<MountSlot>    m_gunSlots;
        std::vector<MountSlot>    m_engineSlots;
        std::vector<int>          m_mountedGuns;
        std::vector<int>          m_mountedEngines;

        HullClass  m_class = HullClass::Medium;
        ShipTuning m_tuning;
        ShipStats  m_stats;
        bool m_symmetric = true;
        int  m_discardedPoints = 0;
        int  m_spinal = -1;            ///< Player's Rift choice; -1 = auto
        std::vector<sf::Vector2f> m_decor;
        bool       m_decorAuthored = false;
        DecorCheck m_decorCheck;
        bool m_riftSharedPref = false; ///< Player asked for SHARED with 2+ guns
        const char* m_reject = "";
    };

} // namespace ship