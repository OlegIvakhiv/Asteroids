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


struct TransformComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    sf::Vector2f position;              ///< World position in pixels
    sf::Vector2f velocity;              ///< Current movement speed (pixels/sec)
    sf::Vector2f acceleration;          ///< Current acceleration (pixels/sec²)
    float rotation = 0.f;               ///< Rotation angle in degrees
};


struct PlayerComponent {
    uint32_t entityId = 0;

    // ===== Dash Mechanics =====
    float dashCooldown;                 ///< Time remaining until dash can be used again
    float dashMaxCooldown;              ///< Total cooldown duration after a dash

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

    // ===== STUN PROPERTIES =====
    float stunTimer = 0.f;          // >0 = enemy is stunned (can't move)
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
 * @brief AI behavior states for enemy ships
 */
enum class EnemyState {
    PATROL,     ///< Wandering randomly, not aware of player
    ALERT,      ///< Searching for player after losing line of sight
    COMBAT      ///< Actively chasing and attacking player
};

/**
 * @struct EnemyComponent
 * @brief Enemy-specific combat data (currently partially implemented)
 *
 * Reserved for future enemy weapon systems and behavior tuning.
 */
struct EnemyComponent {
    uint32_t entityId = 0;              ///< Unique persistent identifier

    enum State { IDLE, CHASE, AVOID } state = IDLE;  ///< Legacy state (use EnemyState instead)
    float detectionRadius = 600.f;      ///< Distance to start tracking player

    // ===== Weapon System (reserved) =====
    float fireTimer = 0.f;              ///< Cooldown remaining until next shot
    float fireRate = 1.5f;              ///< Shots per second
    float attackRange = 500.f;          ///< Distance at which enemy can fire
};

/**
 * @struct AIState
 * @brief Persistent AI memory and decision-making data
 *
 * Stored in a separate unordered_map keyed by entityId because:
 * 1. Not all entities have AI (only enemies)
 * 2. AI needs to persist across entity swaps/deletions
 * 3. Avoids bloating component vectors with unused AI data
 */
struct AIState {
    uint32_t entityId = 0;              ///< Unique persistent identifier
    EnemyState currentState = EnemyState::PATROL;  ///< Current behavioral state

    // ===== Tracking Memory =====
    sf::Vector2f lastKnownPlayerPos;    ///< Last seen player position (used in ALERT state)
    float searchTimer = 0.f;            ///< Time remaining to search before returning to PATROL

    // ===== Patrol Behavior =====
    sf::Vector2f patrolTarget;          ///< Current destination point while patrolling
    float patrolWaitTimer = 0.f;        ///< Time to wait at patrol target before moving

    // ===== Reaction System =====
    float reactionTimer = 0.f;          ///< Cooldown between AI decision updates
    float reactionDelay = 0.2f;         ///< Artificial reaction time (makes AI feel more human)
    sf::Vector2f smoothedDesiredVel;    ///< Filtered velocity target (prevents jitter)

    // ===== PIRATE HUMAN FACTOR =====

    // Threat awareness: the pirate "notices" threats with a delay
    // and commits to a dodge direction even if it becomes wrong
    sf::Vector2f pendingDodgeDir;           ///< Dodge direction decided when threat was noticed
    float dodgeCommitTimer = 0.f;           ///< How long to keep dodging in committed direction
    float threatReactionDelay = 0.f;        ///< Countdown before pirate starts reacting to a threat
    bool threatNoticed = false;             ///< Has the pirate noticed the current threat yet?
    uint32_t trackedThreatId = 0;           ///< entityId of the threat being tracked (0 = none)

    // Bullet dodge: separate lightweight system
    sf::Vector2f bulletDodgeDir;            ///< Current bullet dodge direction
    float bulletDodgeTimer = 0.f;           ///< How long to keep dodging bullets
    float bulletReactionDelay = 0.f;        ///< Countdown before reacting to incoming bullet
};