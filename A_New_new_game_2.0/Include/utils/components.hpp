/**
 * @file components.hpp
 * @brief ECS Component definitions for the Modular Space Engine
 *
 * This file contains all Plain Old Data (POD) structures that represent
 * the data attached to game entities. Components are stored in parallel
 * vectors inside EntityManager for cache-friendly iteration.
 *
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <box2d/box2d.h>

 /**
  * @struct TransformComponent
  * @brief Position, rotation, and movement data for an entity
  *
  * Also contains player-specific mechanics: dash cooldown and energy drive system.
  * For non-player entities, dash/energy fields remain unused (default values).
  */


  /**
   * @struct PhysicsShapeData
   * @brief Stores collision shape data for debug rendering
   *
   * This is used ONLY for debug visualisation – Box2D handles the actual physics.
   * We store the shape data at creation time so we can draw it accurately.
   */
struct PhysicsShapeData {
    enum class Type { None, Circle, Polygon } type = Type::None;

    // For Circle
    float radius = 0.f;

    // For Polygon
    std::vector<sf::Vector2f> vertices;   // Local space vertices (pixels)

    // For both
    sf::Vector2f offset;  // Offset from body position (usually (0,0))
};

/**
 * @enum DashAnim
 * @brief Which procedural dash animation the player ship is currently playing
 */
enum class DashAnim : uint8_t {
    None = 0,
    BankLeft,        ///< Side dash left  — tail swings out (drift)
    BankRight,       ///< Side dash right — tail swings out (drift)
    SpinBack,        ///< Back dash — full counter-clockwise spin
    WiggleForward    ///< Forward dash — damped tail wiggle
};

struct TransformComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    sf::Vector2f position;              ///< World position in pixels
    sf::Vector2f velocity;              ///< Current movement speed (pixels/sec)
    sf::Vector2f acceleration;          ///< Current acceleration (pixels/sec²)
    float rotation = 0.f;               ///< Rotation angle in degrees

    // ===== Visual-only animation offsets (never affect physics) =====
    float        visualOffsetAngle = 0.f;          ///< Extra degrees on top of `rotation`
    sf::Vector2f visualPivot = { 0.f, 0.f }; ///< LOCAL point the offset rotates around
    sf::Vector2f visualScale = { 1.f, 1.f }; ///< Local squash/stretch
};


struct PlayerComponent {
    uint32_t entityId = 0;

    // ===== Dash Mechanics =====
    float dashCooldown;                 ///< Time remaining until dash can be used again
    float dashMaxCooldown;              ///< Total cooldown duration after a dash

    // ===== Dash Animation (driven by ShipAnimSystem) =====
    DashAnim dashAnim = DashAnim::None;
    float    dashAnimTimer = 0.f;   ///< Counts down
    float    dashAnimDuration = 0.f;   ///< Total, for normalising progress

    // ===== Energy Drive System (Turbo/Overheat) =====
    float energyDrive = 100.f;          ///< Current energy level (0-100)
    float maxEnergyDrive = 100.f;       ///< Maximum energy capacity
    float overheatTimer = 0.f;          ///< >0 = engine overheated, prevents energy regen
    bool isTurbo = false;               ///< Whether turbo boost is currently active

    // ===== PARRY MECHANIC =====
    bool isParrying = false;           // Currently in parry animation
    float parryTimer = 0.f;            // Active parry window (collision deflection)
    float parryAnimTimer = 0.f;        // Visual spin duration (can be longer)
    float parryCooldown = 0.f;         // Time until parry can be used again
    float parryMaxCooldown = 2.0f;     // Cooldown between parries
    float parrySpinAngle = 0.f;        // Current spin angle (for animation)
    float parryStartRotation = 0.f;    // Starting angle when parry began
    float parryTargetRotation = 0.f;   // Mouse angle at parry start
    float parryTotalDelta = 0.f;       // Total rotation to apply (720° + angle to target)

    // Shooting
    float shootTimer = 0.f;   // moved from WeaponSystem static


    // ===== RIFT SHOT =====
    bool riftCharging = false;          // Currently holding charge
    float riftChargeTimer = 0.f;        // Counts up while charging
    uint32_t riftBoltEntityId = 0;      // EntityId of live bolt (0 = none)
    bool riftBoltInFlight = false;      // True while bolt exists

    // ===== PARRY WHIFF =====
    bool parryHitSomething = false;     // Set true in DamageSystem on any parry contact
    bool parryWhiffRecovery = false;    // True during recovery window
    float parryWhiffTimer = 0.f;        // Counts down recovery

    // ===== STAGGER / KNOCKDOWN =====
    bool  isStaggered = false;  ///< Tumbling OR recovering
    float staggerTimer = 0.f;    ///< Tumble phase remaining (input fully blocked)
    float staggerDuration = 0.f;    ///< Total tumble, for normalising
    float staggerRecoverTimer = 0.f;    ///< Recovery remaining (aim only)
    float staggerRecoverDuration = 0.f;
    float staggerSpinSpeed = 0.f;    ///< deg/sec, signed, decays over the tumble

    // ===== PARRY SUCCESS FEEDBACK =====
    float parryFlashTimer = 0.f;           ///< Hull washes white while > 0


    // ===== WEAPON HEAT (separate from the engine's overheatTimer) =====
    float weaponHeat = 0.f;     ///< 0..maxWeaponHeat
    float maxWeaponHeat = 100.f;
    float heatCoolDelay = 0.f;     ///< Grace period before cooling starts
    bool  weaponOverheated = false;  ///< True = firing locked until vented
};



/**
 * @struct BulletComponent
 * @brief Projectile-specific data
 *
 * Bullets are short-lived entities that fade out and auto-destroy.
 */
struct BulletComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    float lifetime = 2.0f;              ///< Seconds until auto-destruction
    bool markedForDestroy = false;      ///< Flag for immediate destruction (hit something)
    bool isActive = false;              ///< Whether bullet is currently in flight
    bool isEnemyBullet = false;         ///< fired by an enemy ship
    uint32_t ownerEntityId = 0;         ///< entityId of the ship that fired this (avoids self-hit)

    bool isRiftBolt = false;            // This is a Rift Shot projectile
    bool isReflected = false;      ///< True if this bullet came from a parry reflection
    float damageMultiplier = 1.0f; ///< Damage scaling (e.g., 2.0 for reflected shots)

    // ===== NEW: per-projectile combat stats =====
    float damage = 25.f;   ///< Absolute damage on hit
    float knockback = 0.f;    ///< Impulse applied to the target (pixels/sec)
    float stunOnHit = 0.f;    ///< Seconds of stun applied to enemies

    // ===== NEW: homing =====
    uint32_t homingTargetEntityId = 0;   ///< 0 = flies straight
    float    homingTurnRate = 0.f; ///< Degrees/sec of steering authority
};

/**
 * @struct RenderComponent
 * @brief Visual representation of an entity
 *
 * Uses SFML ConvexShape for flexible polygon rendering.
 * Supports custom shapes defined in Lua scripts.
 */
struct RenderComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    sf::ConvexShape shape;              ///< Drawable shape (color, outline, vertices)
};

/**
 * @struct PhysicsComponent
 * @brief Box2D physics body reference
 *
 * Links an entity to its Box2D physics body for position/velocity simulation.
 */
struct PhysicsComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    b2BodyId bodyId;                    ///< Box2D body identifier (opaque handle)
};

/**
 * @struct Particle
 * @brief Visual effect particle (explosions, impacts, debris)
 *
 * Particles are short-lived, non-collidable visual elements.
 * They fade out over time and auto-remove when lifetime reaches zero.
 */
struct Particle {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    sf::Vector2f position;              ///< Current particle position
    sf::Vector2f velocity;              ///< Movement speed (pixels/sec)
    sf::Color color;                    ///< Current color (alpha fades over time)
    float lifetime;                     ///< Remaining time before removal
    float maxLifetime;                  ///< Initial lifetime (used for fade calculation)
    float size;                         ///< Particle dimensions (square, width = size)
};

/**
 * @struct DebrisChunk
 * @brief Decorative, non-physical rock shard from a fracture
 *
 * Deliberately NOT an entity: no Box2D body, no ECS slot, no collision. These
 * exist purely to sell the break. Making every fragment physical triples the
 * collision load and hands the player a dozen new hazards as a reward for
 * killing something.
 */
struct DebrisChunk {
    sf::Vector2f position;
    sf::Vector2f velocity;
    float rotation = 0.f;
    float angularVelocity = 0.f;
    sf::Color color;
    float lifetime = 1.f;
    float maxLifetime = 1.f;
    sf::Vector2f points[5];   ///< Local-space polygon
    int pointCount = 0;
};

/**
 * @struct ScreenFlash
 * @brief Full-screen colour wash (parry connect, heavy impact)
 */
struct ScreenFlash {
    float timer = 0.f;
    float maxTimer = 0.f;
    float peakAlpha = 120.f;
    sf::Color color = sf::Color::White;
};

/**
 * @struct ShockRing
 * @brief Expanding ring outline in world space
 */
struct ShockRing {
    sf::Vector2f position;
    float startRadius = 20.f;
    float maxRadius = 250.f;
    float timer = 0.f;
    float maxTimer = 0.4f;
    float thickness = 6.f;
    float peakAlpha = 255.f;
    sf::Color color = sf::Color::White;
};


/**
 * @struct HealthComponent
 * @brief Hit points and invulnerability state
 *
 * Entities with HP <= 0 are marked for destruction.
 * Invul timers provide brief immunity after taking damage.
 */
struct HealthComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    float maxHp = 100.f;                ///< Maximum health capacity
    float currentHp = 100.f;            ///< Current health (0 = dead)

    // ===== Invulnerability Frames (i-frames) =====
    float invulTimer = 0.f;             ///> Full invincibility timer (>0 = cannot take damage)
    float cheapInvulTimer = 0.f;        ///> Partial invincibility for minor collisions

    // ===== EXPLOSIVE PROPERTIES (for magma asteroids) =====
    bool isExplosive = false;           // Does this entity explode on death?
    float explosionRadius = 150.0f;     // Area of effect (pixels)
    float explosionDamage = 30.0f;      // Damage to entities in radius

    // ===== PARRY HOMING PROPERTIES =====
    bool isHoming = false;          // Is this asteroid a homing missile?
    uint32_t homingTargetEntityId = 0;   // Store entity ID

    bool wasParryLaunched = false;   // Parry: one-time straight-line launch, NO tracking
    bool isKineticWeapon = false;   ///< Launched as a weapon (parry or rift hijack).
    ///< Distinct from isHoming: a rock can steer
    ///< without being a weapon, and vice versa.

// ===== STUN PROPERTIES =====
    float stunTimer = 0.f;          // >0 = enemy is stunned (can't move)

    float   visualRadius = 0.f;   ///< Rendered radius in pixels, post-variance
    uint8_t asteroidTier = 0;     ///< 0=SMALL 1=MEDIUM 2=LARGE 3=MAGMATIC
};

/**
 * @struct Star
 * @brief Background star for parallax scrolling effect
 *
 * Stars move slower than the player (parallax factor < 1.0)
 * to create depth illusion. They wrap around screen edges.
 */
struct Star {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    sf::Vector2f position;              ///< Screen position (pixels)
    float parallaxFactor;               ///< Movement multiplier (smaller = slower/farther)
    float size;                         ///< Star diameter in pixels
    sf::Color color;                    ///< Star color with alpha for twinkling
};

/**
 * @enum EnemyState
 * @brief Top-level AI behaviour state
 */
enum class EnemyState {
    PATROL,     ///< Wandering, unaware. Slow, wide lazy turns.
    ALERT,      ///< Suspicious. Saw something, hasn't confirmed it's the player.
    COMBAT      ///< Confirmed hostile. Actively fighting.
};

/**
 * @enum Maneuver
 * @brief Discrete combat movement intent
 *
 * WHY DISCRETE INSTEAD OF CONTINUOUS:
 *   The old AI recomputed "chase if far, orbit if near" every 0.1s, which is a
 *   control loop that converges — every enemy settles onto the same clean
 *   circle at the same radius, which is what reads as being magnetised to the
 *   player. Committing to one manoeuvre for 0.6–1.8s produces deliberate,
 *   readable movement with visible direction changes instead.
 */
enum class Maneuver {
    APPROACH,    ///< Close the distance
    STRAFE,      ///< Hold range, slide sideways
    FALLBACK,    ///< Back off, usually after taking a hit
    ATTACK_RUN,  ///< Commit to a fast pass straight at the player
    REPOSITION   ///< Break off to a random offset point
};

/**
 * @enum AlertIcon
 * @brief Which state-change indicator to pop above an enemy
 */
enum class AlertIcon : uint8_t {
    None = 0,
    Suspicion,   ///< Yellow — entered ALERT
    Spotted,     ///< Red — entered COMBAT
    Lost         ///< Grey — lost the player, dropping back
};

/**
 * @struct EnemyComponent
 * @brief Enemy presentation and impact state
 *
 * Read by RenderSystem, written by DamageSystem and AISystem.
 */
struct EnemyComponent {
    uint32_t entityId = 0;

    enum State { IDLE, CHASE, AVOID } state = IDLE;  ///< Legacy; use EnemyState
    float detectionRadius = 600.f;

    // ===== Weapon =====
    float fireTimer = 0.f;
    float fireRate = 1.5f;
    float attackRange = 500.f;

    // ===== Shot telegraph =====
    // The enemy winds up visibly BEFORE firing. This is the single biggest
    // fairness improvement available: an unannounced shot from off-screen is
    // unreactable, but a 0.3s wind-up turns every shot into a dodge prompt.
    float telegraphTimer = 0.f;      ///< Counts DOWN to the shot
    float telegraphDuration = 0.f;
    bool  telegraphActive = false;
    sf::Vector2f telegraphDir;       ///< Aim locked at wind-up start, NOT at fire
    float asteroidShotTimer = 0.f;   ///< Separate cooldown for clearing rocks
    int   timesHit = 0;              ///< Lifetime hit count (drives 2nd-hit aggro)

    // ===== State indicator ("!" / "?") =====
    AlertIcon alertIcon = AlertIcon::None;
    float alertIconTimer = 0.f;
    float alertIconDuration = 0.f;

    // ===== Stagger / knockdown (mirrors the player's) =====
    float staggerTimer = 0.f;
    float staggerDuration = 0.f;
    float staggerRecoverTimer = 0.f;
    float staggerRecoverDuration = 0.f;
    float staggerSpinSpeed = 0.f;

    // ===== Bullet Storm =====
    bool  stormActive = false;
    float stormTimer = 0.f;          ///< Spin-and-fire phase
    float stormDuration = 0.f;
    float stormRecoverTimer = 0.f;   ///< Dizzy, vulnerable, no shooting
    float stormFireTimer = 0.f;
    float stormSpin = 0.f;           ///< deg/sec, ramps up then down

    // ===== Visual state feedback =====
    float hitFlashTimer = 0.f;       ///< White flash when damaged
    float dodgeFlashTimer = 0.f;     ///< Brief streak when a dodge burst fires
    EnemyState visualState = EnemyState::PATROL;  ///< Mirror of AI state, for the renderer

};


/**
 * @struct ExplosionComponent
 * @brief Temporary component for active explosions
 *
 * Used for magmatic asteroid explosions and Rift Bolt detonations.
 * Applied to a dummy entity that handles the AoE logic.
 */
struct ExplosionComponent {
    uint32_t entityId = 0;
    sf::Vector2f position;
    float mainRadius;      // Large: destroys bullets, cheap damage
    float coreRadius;      // Small: homing conversion / high damage
    float mainDamage;
    float coreDamage;
    bool isRiftExplosion = false;   // True for Rift Bolt, false for Magmatic
    float lifetime = 0.2f;          // Single-frame explosion
};


struct DebugAoE {
    sf::Vector2f position;
    float radius;
    sf::Color color;
    float lifetime;     ///< Time in seconds before disappearing
    float maxLifetime;  ///< Initial duration for fade-out calculations
};

/**
 * @struct AIState
 * @brief Persistent AI decision-making memory (private to AISystem)
 */
struct AIState {
    uint32_t entityId = 0;
    EnemyState currentState = EnemyState::PATROL;

    // ===== Vision & memory =====
    sf::Vector2f lastKnownPlayerPos;
    sf::Vector2f lastKnownPlayerVel;   ///< Lets ALERT search AHEAD, not just at the spot
    float searchTimer = 0.f;
    float suspicion = 0.f;             ///< 0..1, builds while the player is in the cone
    float timeSinceSeen = 999.f;
    bool  hasSeenPlayer = false;
    float lookAroundAngle = 0.f;       ///< Scan sweep offset used in ALERT
    float lookAroundTimer = 0.f;

    // ===== Patrol =====
    sf::Vector2f patrolTarget;
    float patrolWaitTimer = 0.f;

    // ===== Decision cadence =====
    float reactionTimer = 0.f;
    float reactionDelay = 0.2f;
    sf::Vector2f smoothedDesiredVel;

    // ===== Combat manoeuvring =====
    Maneuver maneuver = Maneuver::STRAFE;
    float maneuverTimer = 0.f;
    float strafeDir = 1.f;             ///< +1 / -1, flips between manoeuvres
    float preferredRange = 300.f;      ///< Personality: this pirate's fighting distance
    float aggression = 0.5f;           ///< Personality: 0 = timid, 1 = reckless
    float jitterPhase = 0.f;           ///< Per-enemy offset so wander noise decorrelates

    // ===== Evasive burst (the enemy's "dash") =====
    float dodgeCooldown = 0.f;
    float dodgeBurstTimer = 0.f;
    sf::Vector2f dodgeBurstDir;

    // ===== Threat reaction (human factor) =====
    sf::Vector2f pendingDodgeDir;
    float dodgeCommitTimer = 0.f;
    float threatReactionDelay = 0.f;
    bool  threatNoticed = false;
    uint32_t trackedThreatId = 0;

    sf::Vector2f bulletDodgeDir;
    float bulletDodgeTimer = 0.f;
    float bulletReactionDelay = 0.f;

    // ===== Bullet Storm gating =====
    float stormCooldown = 0.f;
    float stormUrge = 0.f;             ///< Builds under asteroid pressure

    bool  initialised = false;         ///< Personality rolled on first update


    // ===== Storm scan cadence =====
    float stormScanTimer = 0.f;      ///< Throttles the O(n) asteroid count

    // ===== Proactive chaotic dodging =====
    float chaosDodgeTimer = 0.f;     ///< Counts down to the next juke attempt
    float panicLevel = 0.f;          ///< 0..1, rises with nearby threats

    // ===== Dodge envelope =====
    float dodgeBurstDuration = 0.f;   ///< Total, for normalising the animation
    float dodgeBurstSide = 0.f;   ///< -1..1, which way to bank (locked at start)

    // ===== Flinch (noticed too late to evade) =====
    float flinchTimer = 0.f;
    float flinchDuration = 0.f;
    sf::Vector2f flinchDir;

    // ===== Punish gating =====
    float dodgePunishCooldown = 0.f;  ///< Stops every bullet chaining staggers
    float threatSeenTimer = 0.f;    ///< >0 = a real threat is inbound; suppresses idle jukes
};