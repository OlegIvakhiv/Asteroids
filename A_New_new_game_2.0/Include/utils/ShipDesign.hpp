/**
 * @file ShipDesign.hpp
 * @brief Player-authored hull: geometry, mount validity, derived stats.
 *
 * Pure data and math. No rendering, no Box2D, no Lua, no UI. This is what the
 * refit editor manipulates, what EntityFactory consumes, and what the stat
 * readout displays. It is deliberately testable in isolation -- describe()
 * prints everything to a console with no window open.
 *
 * ============================================================================
 * COORDINATE CONTRACT
 * ============================================================================
 * Local pixels, same space as RenderComponent::shape and PhysicsShapeData.
 * The ship faces -Y (the stock hull's nose vertex is at (0,-30)). Forward is
 * up on screen. Conversion to Box2D metres happens at the factory boundary by
 * dividing by SCALE, exactly as every other entity already does -- this header
 * never sees SCALE except as ShipTuning::pixelsPerMetre.
 *
 * ============================================================================
 * WHY CONVEX IS ENFORCED, NOT SUGGESTED
 * ============================================================================
 * Two problems die at once:
 *
 *   1. b2ComputeHull requires convex and caps at 8 vertices (B2_MAX_POLYGON_
 *      VERTICES). Enforcing convexity here means the player's outline IS the
 *      collision shape, with no decomposition step and no visual/physics
 *      mismatch.
 *
 *   2. It kills the degenerate build. With free-form points the optimum is an
 *      eight-pointed forward-facing star: near-zero area (so nearly massless
 *      and fast) with eight sharp vertices (so eight guns). Every spike is
 *      concave. Convexity makes that shape unrepresentable rather than
 *      merely discouraged.
 *
 * Concave detail is still available as a SEPARATE decorative point list that
 * carries no stats -- the same trick the asteroid facets already use.
 *
 * ============================================================================
 * MOUNT RULES
 * ============================================================================
 * Guns mount on VERTICES. Engines mount on EDGES. That division is not
 * decoration: a gun needs a point to sit on, an engine needs a flat surface to
 * bolt to. It means the two can never be confused and the rule explains itself
 * without a tooltip.
 *
 * A vertex accepts a gun if a forward ray from it does not pass back through
 * the hull -- you cannot shoot through your own nose. This replaces an
 * arbitrary "interior angle <= 60" threshold with a physical test, and it
 * happens to encode sharpness anyway: a blunt forward vertex has a wide
 * interior wedge that swallows the forward direction, so it fails. Interior
 * angle is still computed and exposed so the editor can show it, and
 * ShipTuning::maxGunInteriorAngleDeg can tighten things further if the
 * clearance test alone proves too generous in play.
 *
 * An edge accepts an engine if its outward normal points rearward. Thrust
 * that does not face backward is not thrust.
 *
 * ============================================================================
 * WHAT IS DERIVED RATHER THAN AUTHORED
 * ============================================================================
 * Only HP and energy are invented numbers. Everything else falls out of
 * physics that already runs:
 *
 *   mass         = area x density          (Box2D already does this)
 *   accel        = thrust / mass           (F = ma, engine_power is a force)
 *   topSpeed     = thrust / (mass x drag)  (terminal velocity under damping)
 *   yaw bias     = lateral offset of thrust from centre of mass
 *
 * So a bigger hull is slower without a single tuning value, and an
 * off-centre engine layout makes the ship pull to one side because the
 * simulation says so, not because a penalty was written. Asymmetric builds
 * referee themselves.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/System/Vector2.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ship {

    // ============================================================================
    // CONSTANTS
    // ============================================================================

    /// b2ComputeHull's limit. Not a design preference -- a hard engine cap.
    inline constexpr int MAX_HULL_POINTS = 8;
    inline constexpr int MIN_HULL_POINTS = 3;

    /// Ship-space forward. Matches the stock hull's nose at (0,-30).
    inline constexpr float FORWARD_X = 0.f;
    inline constexpr float FORWARD_Y = -1.f;

    // ============================================================================
    // TUNING
    //
    // Every balance number in one struct so the whole system can be swept without
    // touching logic. Migrate to Lua once the shape of it stops changing -- but
    // note sol2's get_or() returns silent defaults for missing keys, so do that
    // only when a bad load would be visible rather than quiet.
    // ============================================================================

    struct ShipTuning {
        // ---- Unit bridge ----
        float pixelsPerMetre = 30.f;   ///< MUST match SCALE in the physics headers.
        float density = 3.0f;   ///< player.lua density
        float linearDamping = 1.0f;   ///< player.lua lineardrag_factor

        // ---- Invented numbers (the only two) ----
        float hpPerArea = 0.0538f; ///< Calibrated so the stock hull lands on 100.
        float energyPerArea = 0.0538f;

        // ---- Thrust ----
        float thrustPerEngine = 55.f; ///< Flat per-engine contribution.
        float thrustPerEdgePx = 1.1f; ///< Wider stern mounts more motor.

        // ---- Weapons ----
        float weaponEnergyDraw = 22.f; ///< Drawn from the same pool as dash/turbo.
        int   maxGuns = 4;    ///< Hard cap so the UI never sees a silly case.
        int   maxEngines = 4;

        // ---- Mount validity ----
        /// Outward normal Y above this counts as rear-facing. ~0.35 ~= 20 degrees.
        float engineRearMinNormalY = 0.25f;
        /// Clearance alone accepts blunt vertices on wide hulls. THIS is the knob
        /// that enforces "guns go on sharp angles". 180 disables it.
        float maxGunInteriorAngleDeg = 135.f;

        // ---- Sanity floors ----
        float minAreaPx2 = 400.f;     ///< Below this the hull is not a ship.
        float minEdgePx = 6.f;       ///< Shorter edges are treated as degenerate.
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
        float areaPx2 = 0.f;
        float massKg = 0.f;
        float hpMax = 0.f;
        float energyMax = 0.f;

        float thrustN = 0.f;
        float accel = 0.f;   ///< m/s^2
        float topSpeed = 0.f;   ///< m/s, terminal velocity under linear damping
        float agility = 0.f;   ///< 1 / rotational inertia, normalised to stock = 1

        int   gunSlots = 0;     ///< Geometrically valid vertices
        int   gunsPowered = 0;     ///< What the energy pool can actually run
        int   engineCount = 0;

        sf::Vector2f centreOfMass;     ///< Local pixels
        sf::Vector2f thrustPoint;      ///< Mean of mounted engine positions
        float lateralOffsetPx = 0.f;   ///< Signed. Non-zero = ship yaws under thrust.
        float yawAccelDegPerSec2 = 0.f; ///< Angular accel from that offset

        float radiusPx = 0.f;   ///< Max vertex distance from centroid
    };

    // ============================================================================
    // VALIDATION
    // ============================================================================

    struct ValidationResult {
        bool ok = false;
        bool tooFewPoints = false;
        bool tooManyPoints = false;
        bool tooSmall = false;
        bool wasConcave = false;    ///< True if makeConvex() had to discard points
        bool noEngine = false;
        bool noGun = false;
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

        /// Shoelace. Sign depends on winding; callers want magnitude.
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

        /// Area-weighted centroid of a polygon. NOT the average of the vertices --
        /// that would be wrong for any hull that isn't regular, and the whole
        /// asymmetric-handling model depends on this being right.
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

        /// Andrew's monotone chain. Returns the hull in counter-clockwise order for a
        /// standard Y-up axis, which is clockwise on screen -- consistent either way,
        /// and every downstream test here is winding-independent by construction.
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

        /**
         * @brief Does a ray from vertex V in direction d re-enter the hull?
         *
         * The interior at V is the wedge between the two edges leaving it (guaranteed
         * < 180 degrees for a convex polygon). If d falls inside that wedge the shot
         * would travel through the ship's own body. Sign-comparison against
         * cross(a,b) makes this independent of winding order.
         */
        inline bool directionBlocked(sf::Vector2f prev, sf::Vector2f v,
            sf::Vector2f next, sf::Vector2f d) {
            const sf::Vector2f a{ prev.x - v.x, prev.y - v.y };
            const sf::Vector2f b{ next.x - v.x, next.y - v.y };

            const float cab = cross(a, b);
            if (std::fabs(cab) < 1e-4f) return true;   // collinear: treat as blocked

            const float cad = cross(a, d);
            const float cdb = cross(d, b);
            const bool s = (cab > 0.f);
            return ((cad > 0.f) == s) && ((cdb > 0.f) == s);
        }

        inline float interiorAngleDeg(sf::Vector2f prev, sf::Vector2f v, sf::Vector2f next) {
            const sf::Vector2f a = norm({ prev.x - v.x, prev.y - v.y });
            const sf::Vector2f b = norm({ next.x - v.x, next.y - v.y });
            const float c = std::clamp(dot(a, b), -1.f, 1.f);
            return std::acos(c) * 57.2957795f;
        }

    } // namespace detail

    // ============================================================================
    // SHIP DESIGN
    // ============================================================================

    class ShipDesign {
    public:
        // ---- Construction ---------------------------------------------------

        /// The stock hull, matching EntityFactory's current collision points.
        static ShipDesign stock() {
            ShipDesign d;
            d.m_points = {
                {   0.f, -30.f },   // nose
                {  28.f,  15.f },   // right wing tip
                {  18.f,  28.f },   // right stern
                { -18.f,  28.f },   // left stern
                { -28.f,  15.f }    // left wing tip
            };
            d.normalise();
            return d;
        }

        static ShipDesign fromPoints(std::vector<sf::Vector2f> pts, bool symmetric = true) {
            ShipDesign d;
            d.m_points = std::move(pts);
            d.m_symmetric = symmetric;
            d.normalise();
            return d;
        }

        // ---- Editing --------------------------------------------------------

        const std::vector<sf::Vector2f>& points() const { return m_points; }
        int pointCount() const { return static_cast<int>(m_points.size()); }

        bool symmetric() const { return m_symmetric; }

        /**
         * @brief Toggle mirror editing.
         *
         * Turning it ON snaps the hull symmetric immediately, so the player sees
         * what the mode means instead of waiting until their next drag. Turning it
         * off changes nothing -- an asymmetric ship is a legal ship. It just has
         * to live with the torque, which Box2D applies for free.
         */
        void setSymmetric(bool on) {
            m_symmetric = on;
            if (on) enforceSymmetry();
        }

        /**
         * @brief Move a point, respecting symmetry mode.
         * @return false if the move was rejected (would break convexity).
         *
         * Rejection rather than silent correction: a point that snaps somewhere
         * the player didn't put it is worse than a point that refuses to move.
         * The editor should show the attempted position in red and leave the hull
         * where it was.
         */
        bool movePoint(int i, sf::Vector2f pos) {
            if (i < 0 || i >= pointCount()) return false;

            const std::vector<sf::Vector2f> backup = m_points;
            m_points[i] = pos;

            if (m_symmetric) {
                const int p = mirrorPartner(i);
                if (p >= 0) m_points[p] = { -pos.x, pos.y };
                else        m_points[i].x = 0.f;   // on-axis points stay on-axis
            }

            if (!isConvex() || std::fabs(detail::signedArea(m_points)) < m_tuning.minAreaPx2) {
                m_points = backup;
                return false;
            }
            rebuild();
            return true;
        }

        bool addPoint(sf::Vector2f pos) {
            if (pointCount() >= MAX_HULL_POINTS) return false;
            const std::vector<sf::Vector2f> backup = m_points;

            // Splice into the edge it stretches least, preserving every other
            // point's index.
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

            if (!isConvex()) { m_points = backup; return false; }
            rebuild();
            return true;
        }

        bool removePoint(int i) {
            if (i < 0 || i >= pointCount() || pointCount() <= MIN_HULL_POINTS) return false;
            m_points.erase(m_points.begin() + i);
            rebuild();
            return true;
        }

        /// Index of the point closest to the mirror of point i, or -1 if on-axis.
        int mirrorPartner(int i) const {
            if (i < 0 || i >= pointCount()) return -1;
            const sf::Vector2f m{ -m_points[i].x, m_points[i].y };
            if (std::fabs(m_points[i].x) < 1.5f) return -1;

            int best = -1;
            float bestD = 1e9f;
            for (int j = 0; j < pointCount(); ++j) {
                if (j == i) continue;
                const float d = detail::len({ m_points[j].x - m.x, m_points[j].y - m.y });
                if (d < bestD) { bestD = d; best = j; }
            }
            // Only a partner if it is actually near the mirrored position.
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
            }
            normalise();
        }

        // ---- Mount selection ------------------------------------------------

        const std::vector<MountSlot>& gunSlots() const { return m_gunSlots; }
        const std::vector<MountSlot>& engineSlots() const { return m_engineSlots; }

        bool mountGun(int vertexIndex) {
            for (const auto& s : m_gunSlots)
                if (s.index == vertexIndex && s.valid) {
                    if (static_cast<int>(m_mountedGuns.size()) >= m_tuning.maxGuns) return false;
                    if (std::find(m_mountedGuns.begin(), m_mountedGuns.end(),
                        vertexIndex) != m_mountedGuns.end()) return false;
                    m_mountedGuns.push_back(vertexIndex);
                    rebuildStats();
                    return true;
                }
            return false;
        }

        bool mountEngine(int edgeIndex) {
            for (const auto& s : m_engineSlots)
                if (s.index == edgeIndex && s.valid) {
                    if (static_cast<int>(m_mountedEngines.size()) >= m_tuning.maxEngines) return false;
                    if (std::find(m_mountedEngines.begin(), m_mountedEngines.end(),
                        edgeIndex) != m_mountedEngines.end()) return false;
                    m_mountedEngines.push_back(edgeIndex);
                    rebuildStats();
                    return true;
                }
            return false;
        }

        void unmountAll() {
            m_mountedGuns.clear();
            m_mountedEngines.clear();
            rebuildStats();
        }

        /// Fill every valid slot, capped by tuning. Used for presets and for the
        /// editor's "auto" button so a player is never stuck with an unarmed hull.
        void autoMount() {
            unmountAll();
            for (const auto& s : m_gunSlots)
                if (s.valid && static_cast<int>(m_mountedGuns.size()) < m_tuning.maxGuns)
                    m_mountedGuns.push_back(s.index);
            for (const auto& s : m_engineSlots)
                if (s.valid && static_cast<int>(m_mountedEngines.size()) < m_tuning.maxEngines)
                    m_mountedEngines.push_back(s.index);
            rebuildStats();
        }

        const std::vector<int>& mountedGuns() const { return m_mountedGuns; }
        const std::vector<int>& mountedEngines() const { return m_mountedEngines; }

        // ---- Output ---------------------------------------------------------

        const ShipStats& stats() const { return m_stats; }
        const ShipTuning& tuning() const { return m_tuning; }
        void setTuning(const ShipTuning& t) { m_tuning = t; rebuild(); }

        ValidationResult validate() const {
            ValidationResult r;
            r.tooFewPoints = pointCount() < MIN_HULL_POINTS;
            r.tooManyPoints = pointCount() > MAX_HULL_POINTS;
            r.tooSmall = m_stats.areaPx2 < m_tuning.minAreaPx2;
            r.wasConcave = m_discardedPoints > 0;
            r.noEngine = m_mountedEngines.empty();
            r.noGun = m_mountedGuns.empty();
            r.ok = !r.tooFewPoints && !r.tooManyPoints && !r.tooSmall
                && !r.noEngine && !r.noGun;

            if (r.tooFewPoints)      r.message = "Hull needs at least 3 points.";
            else if (r.tooManyPoints)r.message = "Hull exceeds 8 points.";
            else if (r.tooSmall)     r.message = "Hull too small to fly.";
            else if (r.noEngine)     r.message = "No engine mounted.";
            else if (r.noGun)        r.message = "No weapon mounted.";
            else                     r.message = "Airworthy.";
            return r;
        }

        /// Local-space outline, ready for sf::ConvexShape and b2ComputeHull alike.
        const std::vector<sf::Vector2f>& outline() const { return m_points; }

        /// Human-readable dump. Exists so this header can be verified with no
        /// window, no Box2D and no game running.
        std::string describe() const {
            char buf[2400];
            const ValidationResult v = validate();
            std::snprintf(buf, sizeof(buf),
                "  points %d   area %.0f px2   mass %.2f kg   radius %.1f px\n"
                "  hp %.0f   energy %.0f\n"
                "  thrust %.0f N   accel %.2f m/s2   topSpeed %.2f m/s   agility %.2f\n"
                "  guns %d/%d slots (powered %d)   engines %d\n"
                "  centreOfMass (%.1f, %.1f)   thrustPoint (%.1f, %.1f)\n"
                "  lateralOffset %.2f px   yawAccel %.0f deg/s2   discarded %d\n"
                "  %s\n",
                pointCount(), m_stats.areaPx2, m_stats.massKg, m_stats.radiusPx,
                m_stats.hpMax, m_stats.energyMax,
                m_stats.thrustN, m_stats.accel, m_stats.topSpeed, m_stats.agility,
                static_cast<int>(m_mountedGuns.size()), m_stats.gunSlots,
                m_stats.gunsPowered, m_stats.engineCount,
                m_stats.centreOfMass.x, m_stats.centreOfMass.y,
                m_stats.thrustPoint.x, m_stats.thrustPoint.y,
                m_stats.lateralOffsetPx, m_stats.yawAccelDegPerSec2,
                m_discardedPoints, v.message.c_str());
            return std::string(buf);
        }

    private:
        // ========================================================================
        // REBUILD
        // ========================================================================

        /// Recompute derived data. Does NOT touch point order -- see normalise().
        void rebuild() {
            rebuildSlots();
            pruneMounts();   // drop mounts whose geometry no longer qualifies
            rebuildStats();
        }

        /**
         * @brief Establish a canonical convex ordering. CONSTRUCTION ONLY.
         *
         * Re-hulling on every edit reorders the ring, so a point's index stops
         * meaning the same corner between frames and editor handles teleport.
         * Hull once here; afterwards movePoint/addPoint preserve order and merely
         * verify convexity, rejecting edits that would break it.
         */
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
                const float z = detail::cross({ b.x - a.x, b.y - a.y },
                    { c.x - b.x, c.y - b.y });
                if (std::fabs(z) < 1e-3f) continue;
                const int s = (z > 0.f) ? 1 : -1;
                if (sign == 0) sign = s;
                else if (s != sign) return false;
            }
            return true;
        }

        /// Outward normal of edge i, resolved by pointing it away from the
        /// centroid. Winding-independent, which matters because the hull routine
        /// and the player's edit order do not agree on orientation.
        sf::Vector2f edgeOutward(int i, sf::Vector2f c) const {
            const int n = pointCount();
            const sf::Vector2f& a = m_points[i];
            const sf::Vector2f& b = m_points[(i + 1) % n];
            const sf::Vector2f e{ b.x - a.x, b.y - a.y };
            sf::Vector2f nrm = detail::norm({ e.y, -e.x });
            const sf::Vector2f mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
            if (detail::dot(nrm, { mid.x - c.x, mid.y - c.y }) < 0.f) {
                nrm = { -nrm.x, -nrm.y };
            }
            return nrm;
        }

        void rebuildSlots() {
            m_gunSlots.clear();
            m_engineSlots.clear();

            const int n = pointCount();
            if (n < MIN_HULL_POINTS) return;

            const sf::Vector2f c = detail::centroid(m_points);
            const sf::Vector2f fwd{ FORWARD_X, FORWARD_Y };

            // ---- Guns: vertices with forward clearance ----
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
                    s.valid = false;
                    s.reason = "Fires through own hull";
                }
                else if (s.interiorAngleDeg > m_tuning.maxGunInteriorAngleDeg) {
                    s.valid = false;
                    s.reason = "Vertex too blunt to mount";
                }
                else {
                    s.valid = true;
                    s.reason = "";
                }
                m_gunSlots.push_back(s);
            }

            // ---- Engines: edges whose outward normal faces rearward ----
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
                    s.valid = false;
                    s.reason = "Edge too short";
                }
                else if (s.outward.y < m_tuning.engineRearMinNormalY) {
                    s.valid = false;
                    s.reason = "Does not face aft";
                }
                else {
                    s.valid = true;
                    s.reason = "";
                }
                m_engineSlots.push_back(s);
            }
        }

        void pruneMounts() {
            const auto stillValid = [](const std::vector<MountSlot>& slots, int idx) {
                for (const auto& s : slots) if (s.index == idx) return s.valid;
                return false;
            };
            m_mountedGuns.erase(std::remove_if(m_mountedGuns.begin(), m_mountedGuns.end(),
                [&](int i) { return !stillValid(m_gunSlots, i); }), m_mountedGuns.end());
            m_mountedEngines.erase(std::remove_if(m_mountedEngines.begin(), m_mountedEngines.end(),
                [&](int i) { return !stillValid(m_engineSlots, i); }), m_mountedEngines.end());
        }

        void rebuildStats() {
            ShipStats s;
            const int n = pointCount();
            if (n < MIN_HULL_POINTS) { m_stats = s; return; }

            // ---- Geometry ----
            s.areaPx2 = std::fabs(detail::signedArea(m_points));
            s.centreOfMass = detail::centroid(m_points);

            for (const auto& p : m_points) {
                s.radiusPx = std::max(s.radiusPx,
                    detail::len({ p.x - s.centreOfMass.x, p.y - s.centreOfMass.y }));
            }

            // ---- Mass. Box2D works in metres, so area converts by scale^2. ----
            const float ppm = std::max(1.f, m_tuning.pixelsPerMetre);
            const float areaM2 = s.areaPx2 / (ppm * ppm);
            s.massKg = areaM2 * m_tuning.density;

            // ---- The two invented numbers ----
            s.hpMax = s.areaPx2 * m_tuning.hpPerArea;
            s.energyMax = s.areaPx2 * m_tuning.energyPerArea;

            // ---- Thrust from mounted engines ----
            float thrust = 0.f;
            sf::Vector2f tp{ 0.f, 0.f };
            int engines = 0;
            for (int idx : m_mountedEngines) {
                for (const auto& e : m_engineSlots) {
                    if (e.index != idx) continue;
                    thrust += m_tuning.thrustPerEngine + e.edgeLengthPx * m_tuning.thrustPerEdgePx;
                    tp.x += e.position.x;
                    tp.y += e.position.y;
                    ++engines;
                    break;
                }
            }
            s.engineCount = engines;
            s.thrustN = thrust;
            if (engines > 0) {
                tp.x /= static_cast<float>(engines);
                tp.y /= static_cast<float>(engines);
            }
            s.thrustPoint = tp;

            // ---- F = ma. No tuning value involved: a heavier hull is slower
            //      because it is heavier, and that is the whole point. ----
            if (s.massKg > 1e-4f) {
                s.accel = s.thrustN / s.massKg;
                const float damp = std::max(0.01f, m_tuning.linearDamping);
                s.topSpeed = s.thrustN / (s.massKg * damp);
            }

            // ---- Agility, normalised so the stock hull reads 1.00 ----
            //      I ~= m * r^2 / 2 for a rough plate.
            const float rM = s.radiusPx / ppm;
            const float inertia = std::max(1e-5f, s.massKg * rM * rM * 0.5f);
            s.agility = kStockInertia / inertia;

            // ---- Asymmetry. Lateral offset of thrust from centre of mass gives
            //      torque under Box2D for free -- no penalty is written anywhere.
            //      This value only PREDICTS what the simulation will do so the
            //      editor can warn about it. ----
            if (engines > 0) {
                s.lateralOffsetPx = s.thrustPoint.x - s.centreOfMass.x;
                const float armM = s.lateralOffsetPx / ppm;
                const float torque = s.thrustN * armM;
                s.yawAccelDegPerSec2 = (torque / inertia) * 57.2957795f;
            }

            // ---- Weapons: geometry offers, energy pays ----
            int slots = 0;
            for (const auto& g : m_gunSlots) if (g.valid) ++slots;
            s.gunSlots = std::min(slots, m_tuning.maxGuns);

            const int affordable = (m_tuning.weaponEnergyDraw > 0.01f)
                ? static_cast<int>(s.energyMax / m_tuning.weaponEnergyDraw) : s.gunSlots;
            s.gunsPowered = std::min({ static_cast<int>(m_mountedGuns.size()),
                                       s.gunSlots, affordable });

            m_stats = s;
        }

        // Stock hull inertia, so agility is a ratio the player can read rather
        // than an absolute nobody has intuition for. Measured once from stock().
        static constexpr float kStockInertia = 4.66f;

        std::vector<sf::Vector2f> m_points;
        std::vector<MountSlot>    m_gunSlots;
        std::vector<MountSlot>    m_engineSlots;
        std::vector<int>          m_mountedGuns;
        std::vector<int>          m_mountedEngines;

        ShipTuning m_tuning;
        ShipStats  m_stats;
        bool m_symmetric = true;
        int  m_discardedPoints = 0;
    };

} // namespace ship