/**
 * components.hpp
 *
 * All the data structures ("components") attached to game entities.
 * These are plain data only — no logic. They live in parallel vectors
 * inside EntityManager, one vector per component type, so that all the
 * data for entity index N is spread across transforms[N], renders[N],
 * physics[N], etc.
 *
 * Author: Oleg Ivakhiv
 * Version: 1.1
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <box2d/box2d.h>

 // ============================================================================
 // COLLISION SHAPE DATA (for debug drawing only)
 // ============================================================================

 // Stores what a Box2D collision shape looks like, purely so DebugSystem can
 // draw it. Box2D itself doesn't need this — it's a copy kept on the side.
struct PhysicsShapeData {
    enum class Type { None, Circle, Polygon } type = Type::None;

    // Used when type == Circle
    float radius = 0.f;

    // Used when type == Polygon. Vertices are in local space, in pixels.
    std::vector<sf::Vector2f> vertices;

    // Offset from the body's origin (almost always {0,0})
    sf::Vector2f offset;
};

// Which scripted dash animation the player is currently playing.
enum class DashAnim : uint8_t {
    None = 0,
    BankLeft,        // Side dash left  — tail swings out
    BankRight,       // Side dash right — tail swings out
    SpinBack,        // Backward dash — full spin
    WiggleForward    // Forward dash — tail wiggle
};

// ============================================================================
// TRANSFORM — position, movement, rotation
// ============================================================================

struct TransformComponent {
    uint32_t entityId = 0;              // Persistent ID (survives vector reshuffling)
    sf::Vector2f position;              // World position, pixels
    sf::Vector2f velocity;              // Pixels/sec
    sf::Vector2f acceleration;          // Pixels/sec^2
    float rotation = 0.f;               // Degrees

    // ---- Visual-only offsets ----
    // These NEVER affect physics or gameplay. They exist so animation systems
    // (dash banking, hit shudder, idle bob, etc.) can distort how a ship LOOKS
    // without touching its actual transform.
    float        visualOffsetAngle = 0.f;   // Extra rotation on top of `rotation`
    sf::Vector2f visualPivot = { 0.f, 0.f }; // Local point that offset rotates around
    sf::Vector2f visualScale = { 1.f, 1.f }; // Local squash/stretch
};

// ============================================================================
// PLAYER — everything specific to the player ship
// ============================================================================

struct PlayerComponent {
    uint32_t entityId = 0;

    // ---- Dash ----
    float dashCooldown;                 // Time left until dash is available again
    float dashMaxCooldown;              // Total cooldown after a dash

    // ---- Dash animation (driven by ShipAnimSystem) ----
    DashAnim dashAnim = DashAnim::None;
    float    dashAnimTimer = 0.f;       // Counts down
    float    dashAnimDuration = 0.f;    // Total duration, used to normalise progress

    // ---- Energy / turbo ----
    float energyDrive = 100.f;          // Current energy (0-100)
    float maxEnergyDrive = 100.f;
    float overheatTimer = 0.f;          // >0 = engine overheated, energy can't regen
    bool isTurbo = false;

    // ---- Parry ----
    bool isParrying = false;            // Currently mid-parry animation
    float parryTimer = 0.f;             // Active parry window (can actually deflect)
    float parryAnimTimer = 0.f;         // Visual spin duration (longer than the window)
    float parryCooldown = 0.f;
    float parryMaxCooldown = 2.0f;
    float parrySpinAngle = 0.f;
    float parryStartRotation = 0.f;
    float parryTargetRotation = 0.f;    // Mouse angle when the parry started
    float parryTotalDelta = 0.f;        // Total rotation to play out (720° + aim angle)

    // ---- Shooting ----
    float shootTimer = 0.f;             // Cooldown before next shot

    // ---- Rift Shot (charged shot) ----
    bool riftCharging = false;
    float riftChargeTimer = 0.f;
    uint32_t riftBoltEntityId = 0;      // EntityId of the live bolt (0 = none in flight)
    bool riftBoltInFlight = false;

    // ---- Parry whiff (parried and hit nothing) ----
    bool parryHitSomething = false;     // Set by DamageSystem if the parry connected
    bool parryWhiffRecovery = false;    // True during the recovery window
    float parryWhiffTimer = 0.f;

    // ---- Stagger / knockdown ----
    bool  isStaggered = false;          // True during EITHER tumble or recovery
    float staggerTimer = 0.f;           // Tumble phase remaining — no control at all
    float staggerDuration = 0.f;
    float staggerRecoverTimer = 0.f;    // Recovery remaining — aim only
    float staggerRecoverDuration = 0.f;
    float staggerSpinSpeed = 0.f;       // deg/sec, signed, decays over the tumble

    // ---- Parry success feedback ----
    float parryFlashTimer = 0.f;        // Hull flashes white while > 0

    // ---- Weapon heat (separate system from engine overheat) ----
    float weaponHeat = 0.f;
    float maxWeaponHeat = 100.f;
    float heatCoolDelay = 0.f;          // Grace period before it starts cooling
    bool  weaponOverheated = false;     // True = firing locked until it vents

    // ---- Hull-derived (written by EntityFactory from ShipDesign) ----
    float        enginePower = 150.f;   ///< InputSystem should read THIS, not Lua
    sf::Vector2f gunMounts[4];          ///< Local pixels, where shots originate
    int          gunMountCount = 0;

    // ---- Overheat vent QTE ----
    bool  qteActive = false;
    float qtePos = 0.f;            // 0..1 marker position along the bar
    float qteDir = 1.f;
    float qteSpeed = 1.30f;        // Sweeps per second
    float qteGoodCenter = 0.5f;    // Re-randomised every attempt
    float qteGoodHalf = 0.105f;    // Blue zone half-width
    float qtePerfectHalf = 0.028f; // Yellow zone half-width
    float qteTimeout = 0.f;
    int   qteSweeps = 0;
    int   qteResult = 0;           // 0 none, 1 perfect, 2 good, 3 miss
    float qteResultFlash = 0.f;    // Drives the HUD verdict readout
    int   qteStreak = 0;           // Consecutive perfects -> faster marker

    // ---- Overdrive (the perfect-vent reward) ----
    float overdriveTimer = 0.f;    // Heat cannot rise while > 0

    // ---- Perfect parry ----
    float perfectParryFlash = 0.f;
    int   perfectParryChain = 0;   // Consecutive perfects, for feedback scaling

};

// ============================================================================
// BULLET — any projectile (player, enemy, rift bolt, reflected shot)
// ============================================================================

struct BulletComponent {
    uint32_t entityId = 0;
    float lifetime = 2.0f;              // Seconds until it auto-destroys
    bool markedForDestroy = false;      // Set true the instant it hits something
    bool isActive = false;
    bool isEnemyBullet = false;
    uint32_t ownerEntityId = 0;         // Who fired it (so it can't hit its owner)

    bool isRiftBolt = false;
    bool isReflected = false;           // True if this came from a parry reflection
    float damageMultiplier = 1.0f;      // e.g. 2.0 for reflected shots

    // ---- Per-projectile stats ----
    // Each bullet carries its own numbers, set at spawn time from Lua.
    // DamageSystem just reads these instead of hardcoding "bullet damage".
    float damage = 25.f;
    float knockback = 0.f;
    float stunOnHit = 0.f;

    // ---- Homing (used by reflected shots) ----
    uint32_t homingTargetEntityId = 0;  // 0 = flies straight, no target
    float    homingTurnRate = 0.f;      // Degrees/sec of steering
};

// ============================================================================
// RENDER — what an entity looks like
// ============================================================================

struct RenderComponent {
    uint32_t entityId = 0;
    sf::ConvexShape shape;              // SFML shape: fill, outline, vertices
};

// ============================================================================
// PHYSICS — link to the Box2D body
// ============================================================================

struct PhysicsComponent {
    uint32_t entityId = 0;
    b2BodyId bodyId;                    // Opaque Box2D handle
};

// ============================================================================
// VISUAL EFFECTS
// ============================================================================

// A single particle (explosion spark, debris, impact flash, etc).
// Non-colliding, short-lived, purely visual.
struct Particle {
    uint32_t entityId = 0;
    sf::Vector2f position;
    sf::Vector2f velocity;
    sf::Color color;                    // Alpha fades out as lifetime runs down
    float lifetime;                     // Time left
    float maxLifetime;                  // Starting value, used to compute fade
    float size;                         // Width/height (square)
};

// A decorative rock shard from an asteroid breaking apart.
// Deliberately NOT a full entity — no Box2D body, no collision, no ECS slot.
// Making every shard physical would triple the collision load for no gameplay
// benefit; these exist purely to sell the visual of the rock cracking.
struct DebrisChunk {
    sf::Vector2f position;
    sf::Vector2f velocity;
    float rotation = 0.f;
    float angularVelocity = 0.f;
    sf::Color color;
    float lifetime = 1.f;
    float maxLifetime = 1.f;
    sf::Vector2f points[5];             // Local-space polygon (up to 5 points)
    int pointCount = 0;
};

// Full-screen color wash — used for parry connects, heavy impacts.
struct ScreenFlash {
    float timer = 0.f;
    float maxTimer = 0.f;
    float peakAlpha = 120.f;
    sf::Color color = sf::Color::White;
};

// An expanding ring outline in world space (explosions, parry hits, etc).
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

// ============================================================================
// HEALTH — hit points, invulnerability, and death-related flags
// ============================================================================

struct HealthComponent {
    uint32_t entityId = 0;
    float maxHp = 100.f;
    float currentHp = 100.f;            // <= 0 means dead

    // ---- Invulnerability frames ----
    float invulTimer = 0.f;             // Full i-frames: no damage at all
    float cheapInvulTimer = 0.f;        // Partial i-frames: blocks minor collision spam

    // ---- Explosive asteroids (magmatic) ----
    bool isExplosive = false;
    float explosionRadius = 150.0f;
    float explosionDamage = 30.0f;

    // ---- Parry / homing state (asteroids only) ----
    bool isHoming = false;              // True if this rock is actively steering
    uint32_t homingTargetEntityId = 0;

    bool wasParryLaunched = false;      // Parry-launched: goes straight, no tracking
    bool isKineticWeapon = false;       // Launched as an actual weapon (parry or rift
    // hijack). Distinct from isHoming — a rock can
    // steer without being a weapon, and vice versa.

// ---- Stun ----
    float stunTimer = 0.f;              // >0 = can't move

    // ---- Size info (cached at spawn, used by fracture / damage scaling) ----
    float   visualRadius = 0.f;         // Rendered radius in pixels, post-variance
    uint8_t asteroidTier = 0;           // 0=SMALL 1=MEDIUM 2=LARGE 3=MAGMATIC
};

// ============================================================================
// STAR — background parallax star
// ============================================================================

struct Star {
    uint32_t entityId = 0;
    sf::Vector2f position;              // Screen-space pixels
    float parallaxFactor;               // Lower = farther away = moves slower
    float size;
    sf::Color color;
};

// ============================================================================
// ENEMY AI — presentation/state data shared with RenderSystem & DamageSystem
// ============================================================================
// (Decision-making itself lives in AIState below, which is private to
// AISystem. This struct only holds what OTHER systems need to read or write.)

// Top-level behaviour state.
enum class EnemyState {
    PATROL,     // Wandering, hasn't noticed the player
    ALERT,      // Suspicious — saw something, not confirmed yet
    COMBAT      // Confirmed hostile, actively fighting
};

// A committed combat movement choice.
//
// Movement is DISCRETE rather than recalculated every frame because a
// continuous "chase if far, orbit if near" loop always converges onto the
// same clean circle at the same radius — which reads as every enemy being
// magnetically drawn to the player. Committing to one maneuver for
// 0.6-1.8 seconds produces movement with visible intent instead.
enum class Maneuver {
    APPROACH,    // Close the distance
    STRAFE,      // Hold range, slide sideways
    FALLBACK,    // Back off (usually after being hit)
    ATTACK_RUN,  // Fast committed pass straight at the player
    REPOSITION   // Break off toward a random offset point
};

// Which icon pops above an enemy's head on a state change.
enum class AlertIcon : uint8_t {
    None = 0,
    Suspicion,   // Yellow — entered ALERT
    Spotted,     // Red — entered COMBAT
    Lost         // Grey — lost track of the player
};

// A committed line charge. Distinct from a Maneuver because it overrides the
// entire movement/damage pipeline rather than being one option the maneuver
// picker can choose.
enum class RamState : uint8_t {
    None = 0,
    Windup,     // Glow builds, hull aligns. The read.
    Charge,     // Invulnerable, unparryable, clears asteroids.
    Recover     // Vulnerable. Longer than the charge, on purpose.
};

struct EnemyComponent {
    uint32_t entityId = 0;

    // ---- Identity ----
    // Index into EnemyRegistry::all(), assigned at creation and never changed.
    // Everything that needs to know WHAT this unit is -- AI behaviour, colour,
    // hull, spawn accounting -- resolves through this. 0xFF means unassigned,
    // which should never survive createEnemy.
    uint8_t archetype = 0xFF;

    enum State { IDLE, CHASE, AVOID } state = IDLE;  // Legacy, unused — see EnemyState
    float detectionRadius = 600.f;

    // ---- Weapon ----
    float fireTimer = 0.f;
    float fireRate = 1.5f;
    float attackRange = 500.f;

    // ---- Shot telegraph ----
    // The enemy visibly winds up BEFORE firing. This is the single biggest
    // fairness improvement available: an unannounced shot from off-screen
    // can't be reacted to, but a 0.3s wind-up turns every shot into something
    // dodgeable.
    float telegraphTimer = 0.f;         // Counts DOWN to the shot
    float telegraphDuration = 0.f;
    bool  telegraphActive = false;
    sf::Vector2f telegraphDir;          // Aim is locked at wind-up start, NOT at fire
    float asteroidShotTimer = 0.f;      // Separate cooldown for shooting rocks out of the way
    int   timesHit = 0;                 // Lifetime hit count (drives instant-aggro on 2nd hit)

    // ---- State indicator ("!" / "?") ----
    AlertIcon alertIcon = AlertIcon::None;
    float alertIconTimer = 0.f;
    float alertIconDuration = 0.f;

    // ---- Stagger / knockdown (mirrors the player's version) ----
    float staggerTimer = 0.f;
    float staggerDuration = 0.f;
    float staggerRecoverTimer = 0.f;
    float staggerRecoverDuration = 0.f;
    float staggerSpinSpeed = 0.f;

    // ---- Bullet Storm (panic spin-and-spray attack) ----
    bool  stormActive = false;
    float stormTimer = 0.f;             // Spin-and-fire phase
    float stormDuration = 0.f;
    float stormRecoverTimer = 0.f;      // Dizzy afterward, vulnerable, can't shoot
    float stormFireTimer = 0.f;
    float stormSpin = 0.f;              // deg/sec, ramps up then down

    // ---- Turret (independent of hull facing) ----
    // World-space, NOT relative to the hull. A turret that stored a local
    // angle would swing whenever the ship turned, which is exactly the
    // coupling this whole feature exists to remove.
    float   turretAngle = 0.f;
    float   turretCooldown = 0.f;
    float   turretTelegraphTimer = 0.f;
    float   turretTelegraphDuration = 0.f;
    bool    turretTelegraphActive = false;
    uint8_t turretMode = 0;              // 0 = aimed, 1 = burst
    int     turretBurstLeft = 0;
    float   turretBurstTimer = 0.f;
    float   turretBurstBaseAngle = 0.f;  // Fan centre, locked at wind-up end
    float   turretMuzzleFlash = 0.f;

    // ---- Ram charge ----
    RamState     ramState = RamState::None;
    float        ramTimer = 0.f;
    float        ramDuration = 0.f;
    float        ramCooldown = 0.f;
    sf::Vector2f ramDir;
    float        ramGlow = 0.f;          // 0..1, drives the wind-up tell

    // ---- Ram trail ----
    static constexpr int RAM_TRAIL_MAX = 16;
    sf::Vector2f ramTrail[RAM_TRAIL_MAX];
    int   ramTrailCount = 0;      // Valid samples, newest at index 0
    float ramTrailTimer = 0.f;    // Sample cadence
    float ramTrailFade = 0.f;    // 1 while charging, decays to 0 after

    // ---- Visual feedback ----
    float hitFlashTimer = 0.f;          // White flash when damaged
    float dodgeFlashTimer = 0.f;        // Brief streak when a dodge burst fires
    EnemyState visualState = EnemyState::PATROL;  // Mirrors AI state, for RenderSystem
};

// ============================================================================
// EXPLOSION — temporary AoE damage event
// ============================================================================

// Applied to a dummy entity that just carries this data for one frame while
// DamageSystem applies the area-of-effect damage. Used for magmatic asteroid
// deaths and Rift Bolt detonations.
struct ExplosionComponent {
    uint32_t entityId = 0;
    sf::Vector2f position;
    float mainRadius;               // Large radius: destroys bullets, light damage
    float coreRadius;                // Small radius: high damage / homing conversion
    float mainDamage;
    float coreDamage;
    bool isRiftExplosion = false;    // True = Rift Bolt, false = Magmatic asteroid
    float lifetime = 0.2f;           // Lives for a single frame
};

// Debug-only area-of-effect marker (drawn as a fading ring by DebugSystem).
struct DebugAoE {
    sf::Vector2f position;
    float radius;
    sf::Color color;
    float lifetime;
    float maxLifetime;
};

// ============================================================================
// AI STATE — private decision-making memory for AISystem
// ============================================================================
// This is the "brain" data: what the AI remembers and is currently deciding.
// It's kept separate from EnemyComponent (the "body" data other systems read)
// so AISystem can freely restructure its own internals without touching
// rendering or damage code.

struct AIState {
    uint32_t entityId = 0;
    EnemyState currentState = EnemyState::PATROL;

    // ---- Vision & memory ----
    sf::Vector2f lastKnownPlayerPos;
    sf::Vector2f lastKnownPlayerVel;    // Lets ALERT search AHEAD, not just the last spot
    float searchTimer = 0.f;
    float suspicion = 0.f;              // 0..1, builds while the player is visible
    float timeSinceSeen = 999.f;
    bool  hasSeenPlayer = false;
    float lookAroundAngle = 0.f;        // Scan sweep offset, used in ALERT
    float lookAroundTimer = 0.f;

    // ---- Patrol ----
    sf::Vector2f patrolTarget;
    float patrolWaitTimer = 0.f;

    // ---- Decision cadence ----
    float reactionTimer = 0.f;          // AI doesn't re-decide every single frame
    float reactionDelay = 0.2f;
    sf::Vector2f smoothedDesiredVel;

    // ---- Combat maneuvering ----
    Maneuver maneuver = Maneuver::STRAFE;
    float maneuverTimer = 0.f;
    float strafeDir = 1.f;              // +1 / -1, flips between maneuvers
    float preferredRange = 300.f;       // Personality: this enemy's preferred fighting distance
    float aggression = 0.5f;            // Personality: 0 = timid, 1 = reckless
    float jitterPhase = 0.f;            // Per-enemy offset so wander noise doesn't sync up

    // ---- Evasive burst ("dash") ----
    float dodgeCooldown = 0.f;
    float dodgeBurstTimer = 0.f;
    sf::Vector2f dodgeBurstDir;

    // ---- Reactive threat dodging ----
    sf::Vector2f pendingDodgeDir;
    float dodgeCommitTimer = 0.f;
    float threatReactionDelay = 0.f;    // Simulated human reaction time
    bool  threatNoticed = false;
    uint32_t trackedThreatId = 0;

    sf::Vector2f bulletDodgeDir;
    float bulletDodgeTimer = 0.f;
    float bulletReactionDelay = 0.f;

    // ---- Bullet Storm gating ----
    float stormCooldown = 0.f;
    float stormUrge = 0.f;              // Builds up under combat/asteroid pressure

    bool  initialised = false;          // Personality gets rolled once, on first update

    // ---- Storm scan throttling ----
    float stormScanTimer = 0.f;         // Limits how often the O(n) asteroid scan runs

    // ---- Unprompted evasive jukes ----
    float chaosDodgeTimer = 0.f;        // Counts down to the next random juke
    float panicLevel = 0.f;             // 0..1, rises when nearby threats stack up

    // ---- Dodge burst envelope (for the animation) ----
    float dodgeBurstDuration = 0.f;
    float dodgeBurstSide = 0.f;         // -1..1, which way to bank (locked at burst start)

    // ---- Flinch: noticed a threat too late to actually evade ----
    float flinchTimer = 0.f;
    float flinchDuration = 0.f;
    sf::Vector2f flinchDir;

    // ---- Punish gating ----
    float dodgePunishCooldown = 0.f;    // Stops one bullet chaining into repeat staggers
    float threatSeenTimer = 0.f;        // >0 = a real threat is inbound, suppresses idle jukes
};