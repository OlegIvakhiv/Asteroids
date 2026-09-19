/**
 * @file EntityManager.hpp
 * @brief Entity-Component-System (ECS) core manager
 *
 * Manages all game entities using a parallel array (struct-of-arrays) architecture.
 * Entities are stored in component vectors with consistent indices, enabling
 * cache-friendly iteration. Features swap-and-pop deletion for O(1) removal
 * and persistent entity IDs for stable references across frames.
 *
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#pragma once

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <unordered_map>
#include <vector>
#include <memory>
#include "utils/components.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <random>
#include <algorithm> 
#include <cmath>

 /**
  * @brief Physics scale factor: Box2D meters to pixels conversion
  *
  * Box2D uses meters (real-world scale), SFML uses pixels.
  * 30 pixels = 1 meter. Adjust for gameplay feel.
  */
const float SCALE = 30.f;

/**
 * @brief Box2D collision category bitmasks
 *
 * Used for collision filtering. Each body type belongs to a category
 * and defines which categories it can collide with via maskBits.
 */
enum CollisionCategory {
    CATEGORY_PLAYER = 0x0001,   ///< Player ship
    CATEGORY_ASTEROID = 0x0002,   ///< Asteroid obstacles
    CATEGORY_BULLET = 0x0004,   ///< Player projectiles
    CATEGORY_ENEMY = 0x0008,    ///< Enemy ships
    CATEGORY_ENEMY_BULLET = 0x0010,   // enemy bullets — separate so they don't hit each other

    /// Rockets and mines. Ordinary enemy rounds pass through other enemies on
    /// purpose (a squad would otherwise shred itself in a crossfire), but the
    /// Maniac's ordnance is supposed to be a hazard to EVERYONE -- that is the
    /// entire reason to redirect it. Giving it its own bit turns friendly fire
    /// on for these two without changing how any other projectile behaves.
    CATEGORY_ORDNANCE = 0x0020
};

/**
 * @brief Runtime type identification for physics bodies
 *
 * Stored in Box2D's userData pointer to identify entity types
 * during collision callbacks and system updates.
 */
enum class BodyType {
    Player,     ///< Player-controlled ship
    Asteroid,   ///< Destructible rock
    Bullet,     ///< Projectile
    Enemy       ///< AI-controlled enemy
};

struct BodyUserData {
    BodyType type;
    uint32_t entityId;
};

/**
 * @brief Temporary entity data structure (legacy, kept for compatibility)
 */
struct EntityData {
    BodyType type;
    size_t id;
};

/**
 * @class EntityManager
 * @brief Central ECS manager handling entity creation, destruction, and component storage
 *
 * Implements a struct-of-arrays (SoA) pattern for optimal cache performance.
 * All component vectors are kept in sync by index. Entity IDs provide stable
 * references that survive vector reordering due to swap-and-pop deletion.
 */
class EntityManager {
public:

    /**
 * @brief Reserve memory for all component vectors to prevent reallocation
 * @param capacity Number of entities to reserve space for
 *
 * Call this once after construction, before any entity creation.
 * Prevents vector reallocation during gameplay, which avoids dangling references.
 */
 /**
  * @brief Reserve storage for all component arrays
  *
  * IMPORTANT: this reserve is LOAD-BEARING, not an optimisation.
  *
  * Several systems hold references into these vectors across calls that can
  * create entities -- e.g. AISystem holds `auto& tf = transforms[i]` and
  * then fires a shot, which push_backs a bullet. If a push_back reallocates,
  * every such reference dangles and the next write corrupts the heap.
  *
  * Keeping capacity comfortably above the live entity count means no
  * reallocation ever happens in practice. pop_back never shrinks capacity,
  * so once reserved it stays reserved.
  *
  * The real fix is to re-index instead of holding references (see the
  * re-acquire comments in AISystem and WeaponSystem). Until that is done
  * everywhere, do NOT lower this number.
  */
    void reserveAll(size_t capacity) {
        transforms.reserve(capacity);
        renders.reserve(capacity);
        physics.reserve(capacity);
        healths.reserve(capacity);
        bullets.reserve(capacity);
        enemies.reserve(capacity);
        players.reserve(capacity);
        physicsShapes.reserve(capacity);
        scoreRewards.reserve(capacity);
        particles.reserve(capacity * 2);    // particles can be numerous
        stars.reserve(capacity / 2);        // stars are static
        screenFlashes.reserve(8);
        shockRings.reserve(32);
        // A single fracture emits one shard per polygon vertex (6-8), and a
        // magma detonation adds 14. A rift burst clearing a cluster can put
        // 100+ in flight at once, each living up to ~1.35s.
        debris.reserve(512);
    }


    /**
     * @brief Wipe all entities and transient state for a fresh game
     *
     * IMPORTANT: this does NOT touch Box2D bodies. Callers must destroy the
     * Box2D world (b2DestroyWorld) and physics vector's bodies BEFORE calling
     * this, or recreate the world entirely -- otherwise this leaves orphaned
     * BodyUserData allocations and dangling b2BodyId handles. The intended
     * flow (see SystemManager::restart()) is:
     *   1. b2DestroyWorld(old world)   // frees all bodies + their userData...
     *      -- actually NO: Box2D does NOT call delete on your userData.
     *         You must free BodyUserData yourself first (see below).
     *   2. em.reset()
     *   3. b2CreateWorld(...)          // new world
     *   4. re-create player via EntityFactory
     */
    void reset() {
        // Free the heap-allocated BodyUserData for every live body before
        // the world (and the bodies with it) goes away underneath us.
        for (auto& p : physics) {
            if (b2Body_IsValid(p.bodyId)) {
                delete (BodyUserData*)b2Body_GetUserData(p.bodyId);
            }
        }

        transforms.clear();
        renders.clear();
        physics.clear();
        healths.clear();
        bullets.clear();
        enemies.clear();
        players.clear();
        physicsShapes.clear();
        scoreRewards.clear();
        debugAoEs.clear();
        screenFlashes.clear();
        shockRings.clear();
        particles.clear();
        debris.clear();
        // NOTE: `stars` is deliberately NOT cleared -- the starfield is
        // cosmetic background, not game state, and initBackground() is
        // expensive-ish (regenerates 800 stars with an RNG loop).

        entityIdMap.clear();
        nextEntityId = 1;
        totalScore = 0;

        timeScale = 1.f;
        hitstopFreeze = 0.f;
        hitstopSlomo = 0.f;
        hitstopSlomoMax = 0.f;

        cameraTrauma = 0.f;
        cameraZoomKick = 0.f;

        reserveAll(8192); // vectors were cleared, not shrunk-and-freed, but
        // re-asserting capacity costs nothing and guards
        // against a future change to clear()'s semantics
    }



    // ===== Entity ID Management =====
    std::unordered_map<uint32_t, size_t> entityIdMap;  ///< Maps persistent entity ID → current vector index
    uint32_t nextEntityId = 1;                         ///< Auto-incrementing ID generator (0 is invalid)

    // ===== Component Arrays (Parallel Vectors) =====
    std::vector<TransformComponent> transforms;   ///< Position, rotation, movement
    std::vector<RenderComponent> renders;         ///< Visual shape data
    std::vector<PhysicsComponent> physics;        ///< Box2D body references
    std::vector<HealthComponent> healths;         ///< Hit points and invincibility
    std::vector<BulletComponent> bullets;         ///< Projectile lifetime data
    std::vector<EnemyComponent> enemies;          ///< Enemy-specific AI data (reserved)
    std::vector<PlayerComponent> players;         ///< Player-specific stats (only index 0 is valid)

    std::vector<PhysicsShapeData> physicsShapes;  ///< Collision shape data for debug rendering

    std::vector<DebugAoE> debugAoEs;

    std::vector<ScreenFlash> screenFlashes;
    std::vector<ShockRing>   shockRings;

    // ===== Visual Effects =====
    std::vector<Particle> particles;              ///< Explosion/debris particles
    std::vector<Star> stars;                      ///< Parallax background stars

    // ===== Scoring System =====
    std::vector<int> scoreRewards;                ///< Points awarded when entity dies
    int totalScore = 0;                           ///< Cumulative player score

    // ===== TIME CONTROL (hitstop / slow-motion) =====
    float timeScale = 1.f;    ///< Read-only for the curious; set by advanceTime()
    float hitstopFreeze = 0.f;    ///< Hard-freeze remaining (REAL seconds)
    float hitstopSlomo = 0.f;    ///< Slow-motion ramp remaining (REAL seconds)
    float hitstopSlomoMax = 0.f;
    float hitstopMinScale = 0.25f;  ///< Time scale at the START of the slow-mo ramp

    // ===== Camera Feedback (written by any system, consumed by CameraSystem) =====
    float cameraTrauma = 0.f;   ///< 0..1, decays every frame. Shake = trauma²
    float cameraZoomKick = 0.f;   ///< One-shot zoom impulse (negative = punch in)

    // ---- Stagger rules. DamageSystem refreshes these from Lua (visuals.*) ----
    float staggerGrace = 0.8f;          ///< Tumble-immunity after control returns
    float staggerTumbleIframes = 1.0f;  ///< Fraction of the tumble spent invulnerable
    float staggerImmuneShove = 0.35f;   ///< Knockback kept when a hit cannot tumble
    float poisePerKnockback = 0.1f;     ///< Poise damage = hit knockback x this
    float poiseAbsorbShove = 0.30f;     ///< Knockback kept when poise absorbs a hit

    /**
     * @brief Push screen shake from anywhere without coupling to CameraSystem
     * @param amount 0..1. Additive, clamped. ~0.15 = tap, ~0.6 = heavy hit
     */
    void addTrauma(float amount) {
        cameraTrauma = std::min(1.0f, cameraTrauma + amount);
    }

    sf::Vector2f starFieldSize;   ///< Wrap bounds for the parallax starfield

    void addDebugAoE(sf::Vector2f pos, float radius, sf::Color color, float duration = 0.3f) {
        debugAoEs.push_back({ pos, radius, color, duration, duration });
    }

    /**
     * @brief Get the current vector index for an entity ID
     * @param entityId Persistent entity identifier
     * @return Vector index, or (size_t)-1 if entity doesn't exist
     */
    size_t getEntityIndex(uint32_t entityId) const {
        auto it = entityIdMap.find(entityId);
        if (it == entityIdMap.end()) return (size_t)-1;
        return it->second;
    }

    /**
     * @brief Check if an entity currently exists
     * @param entityId Persistent entity identifier
     * @return true if entity is alive and tracked
     */
    bool entityExists(uint32_t entityId) const {
        return entityIdMap.find(entityId) != entityIdMap.end();
    }


    /**
     * @brief Request a hitstop. Takes the MAX of any request already running,
     *        so two impacts in the same frame never stack into a lockup.
     */
    void requestHitstop(float freezeSec, float slomoSec, float minScale = 0.25f) {
        hitstopFreeze = std::max(hitstopFreeze, freezeSec);
        if (slomoSec > hitstopSlomo) { hitstopSlomo = slomoSec; hitstopSlomoMax = slomoSec; }
        hitstopMinScale = minScale;
    }

    /**
     * @brief Advance the time controller and return the scaled delta
     * @param realDt Unscaled frame time
     * @return dt the LOGIC systems should run on
     *
     * Note the freeze uses 0.02 rather than 0.0 — a literal zero dt makes any
     * `x / dt` in a system (the sway spring, for one) undefined.
     */
    float advanceTime(float realDt) {
        if (hitstopFreeze > 0.f) {
            hitstopFreeze -= realDt;
            timeScale = 0.02f;
        }
        else if (hitstopSlomo > 0.f) {
            hitstopSlomo -= realDt;
            const float u = 1.f - (hitstopSlomo / std::max(0.0001f, hitstopSlomoMax)); // 0..1
            timeScale = hitstopMinScale + (1.f - hitstopMinScale) * (u * u);
        }
        else {
            timeScale = 1.f;
        }
        return realDt * timeScale;
    }


    /**
     * @brief Destroy an entity by its vector index
     *
     * Uses swap-and-pop technique for O(1) deletion:
     * - Swaps target with last element in each component vector
     * - Updates entityIdMap for the swapped entity
     * - Pops the last element (now the deleted entity)
     *
     * @param index Current position in component vectors
     */
    void destroyEntity(size_t index) {
        if (index >= transforms.size()) return;

        // 1. Get the persistent ID of entity being destroyed
        uint32_t entityId = transforms[index].entityId;

        // 1.2 Delete BodyUserData
        delete (BodyUserData*)b2Body_GetUserData(physics[index].bodyId);

        // 2. Clean up Box2D physics body
        b2DestroyBody(physics[index].bodyId);

        // 3. Get the last valid index
        size_t lastIdx = transforms.size() - 1;

        // 4. If not deleting the last element, swap with last element
        if (index != lastIdx) {
            // Swap all core components
            std::swap(transforms[index], transforms[lastIdx]);
            std::swap(renders[index], renders[lastIdx]);
            std::swap(physics[index], physics[lastIdx]);
            std::swap(scoreRewards[index], scoreRewards[lastIdx]);
            std::swap(healths[index], healths[lastIdx]);
            std::swap(bullets[index], bullets[lastIdx]);
            std::swap(enemies[index], enemies[lastIdx]);
            std::swap(players[index], players[lastIdx]);
            std::swap(physicsShapes[index], physicsShapes[lastIdx]);

            // Update ID map for the entity that just moved into the deleted slot
            uint32_t swappedId = transforms[index].entityId;
            entityIdMap[swappedId] = index;
        }

        // 5. Remove the last element from all component vectors
        transforms.pop_back();
        renders.pop_back();
        physics.pop_back();
        scoreRewards.pop_back();
        healths.pop_back();
        bullets.pop_back();
        enemies.pop_back();
        players.pop_back();
        physicsShapes.pop_back();

        // 6. Remove the destroyed entity from ID map
        entityIdMap.erase(entityId);
    }

    /**
     * @brief Destroy an entity by its persistent ID
     * @param entityId Persistent entity identifier
     */
    void destroyEntityById(uint32_t entityId) {
        auto it = entityIdMap.find(entityId);
        if (it != entityIdMap.end()) {
            destroyEntity(it->second);
        }
    }

    /**
     * @brief Generate parallax starfield background
     * @param winSize Screen dimensions in pixels
     * @param count Number of stars to generate (default: 400)
     *
     * Stars have varying parallax factors (0.05-0.5) creating depth illusion.
     * Farther stars (lower parallax) are smaller and darker.
     */
    void initBackground(sf::Vector2u winSize, int count = 400, float margin = 1.5f) {
        stars.clear();
        starFieldSize = { winSize.x * margin, winSize.y * margin };

        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> distX(0.f, starFieldSize.x);
        std::uniform_real_distribution<float> distY(0.f, starFieldSize.y);
        std::uniform_real_distribution<float> distSpeed(0.05f, 0.5f);
        std::uniform_int_distribution<int> distAlpha(100, 255);

        for (int i = 0; i < count; ++i) {
            Star s;
            s.parallaxFactor = distSpeed(rng);
            s.position = { distX(rng), distY(rng) };
            s.size = s.parallaxFactor * 4.0f;

            int g = 255 - (rand() % 50);
            int a = distAlpha(rng);
            if (s.parallaxFactor < 0.2f) { a /= 2; s.size = 1.0f; }

            s.color = sf::Color(255, g, 255, a);
            stars.push_back(s);
        }
    }

    /**
     * @brief Create explosion particle effect
     * @param pos Center position of explosion
     * @param color Base color of particles
     * @param count Number of particles to spawn
     * @param baseSize Base particle size (randomized ±50%)
     *
     * Particles radiate outward with random velocities and fade over time.
     */
    void spawnExplosion(sf::Vector2f pos, sf::Color color, int count, float baseSize) {
        for (int i = 0; i < count; ++i) {
            float angle = (rand() % 360) * 3.14159f / 180.f;
            float speed = (rand() % 100) / 10.f + 2.f;
            float life = 0.5f + (rand() % 50) / 100.f;
            float pSize = baseSize * (0.5f + (rand() % 100) / 100.f);
            uint32_t entityId = nextEntityId++;




            particles.push_back({
                entityId,
                pos,
                { std::cos(angle) * speed * 20.f, std::sin(angle) * speed * 20.f },
                color,
                life,
                life,
                pSize
                });
        }



    }

    /**
     * @brief Create impact spark effect (bullet hits)
     * @param pos Impact position
     * @param color Spark color
     * @param bulletVelocity Direction/speed of incoming bullet
     *
     * Sparks spray outward, mostly in the opposite direction of bullet travel,
     * with some random spread for visual variety.
     */
    void spawnImpact(sf::Vector2f pos, sf::Color color, sf::Vector2f bulletVelocity) {
        int count = 5 + (rand() % 4);
        uint32_t entityId = nextEntityId++;

        // Direction opposite to bullet travel
        sf::Vector2f reverseDir = -bulletVelocity;
        float bulletSpeed = std::sqrt(reverseDir.x * reverseDir.x + reverseDir.y * reverseDir.y);
        if (bulletSpeed > 0) reverseDir /= bulletSpeed;

        for (int i = 0; i < count; ++i) {
            // Random spread around reverse direction
            float spread = 1.2f;
            sf::Vector2f dir = reverseDir + sf::Vector2f(
                ((rand() % 100) / 50.f - 1.f) * spread,
                ((rand() % 100) / 50.f - 1.f) * spread
            );

            float speed = (rand() % 80) / 10.f + 5.f;
            float life = 0.3f + (rand() % 30) / 100.f;
            float pSize = 3.0f + (rand() % 30) / 10.f;

            // Brighten the spark color
            sf::Color sparkColor = color;
            sparkColor.r = std::min(255, sparkColor.r + 50);
            sparkColor.g = std::min(255, sparkColor.g + 50);
            sparkColor.b = std::min(255, sparkColor.b + 50);

            particles.push_back({
                entityId,
                pos,
                dir * speed * 25.f,
                sparkColor,
                life,
                life,
                pSize
                });
        }
    }


    void spawnShockwave(sf::Vector2f pos, float radius, sf::Color color) {
        int numParticles = 360 / 15;  // 24 particles

        for (int angle = 0; angle < 360; angle += 15) {
            float rad = angle * 3.14159f / 180.f;
            sf::Vector2f dir(std::cos(rad), std::sin(rad));

            sf::Vector2f particlePos = pos + sf::Vector2f(dir.x * radius, dir.y * radius);
            sf::Vector2f particleVel = sf::Vector2f(dir.x * 400, dir.y * 400);

            uint32_t entityId = nextEntityId++;

            particles.push_back({
                entityId,
                particlePos,
                particleVel,
                color,
                0.3f,   // lifetime
                0.3f,   // maxLifetime
                4.0f    // size
                });
        }
    }


    /**
     * @brief Start a mine's countdown.
     *
     * Lives here rather than in a system because both WeaponSystem (the zone
     * trigger) and DamageSystem (every damaging trigger) need it, and a mine
     * that lit differently depending on which path found it would be a bug
     * waiting to happen. Idempotent: a mine already burning ignores further
     * triggers, so a burst of gunfire cannot shorten a fuse the player is
     * already running from.
     */
    void lightMineFuse(BulletComponent& mine, sf::Vector2f pos) {
        if (!mine.isMine || !mine.mineArmed || mine.mineFuse > 0.f) return;
        mine.mineFuse = mine.mineFuseTime;
        spawnShockRing(pos, 6.f, mine.mineTrigger, 0.30f,
            sf::Color(255, 70, 40), 3.f, 235.f);
    }

    void spawnScreenFlash(sf::Color color, float duration, float peakAlpha = 120.f) {
        screenFlashes.push_back({ duration, duration, peakAlpha, color });
    }

    void spawnShockRing(sf::Vector2f pos, float startRadius, float maxRadius,
        float duration, sf::Color color,
        float thickness = 6.f, float peakAlpha = 255.f) {
        shockRings.push_back({ pos, startRadius, maxRadius, duration, duration,
                               thickness, peakAlpha, color });
    }

    /**
     * @brief Tick the screen FX. MUST be called with REAL dt, not scaled dt —
     *        the flash has to keep animating while the world is frozen.
     */
    void updateFx(float realDt) {
        for (size_t i = screenFlashes.size(); i-- > 0; ) {
            screenFlashes[i].timer -= realDt;
            if (screenFlashes[i].timer <= 0.f) {
                screenFlashes[i] = screenFlashes.back();
                screenFlashes.pop_back();
            }
        }
        for (size_t i = shockRings.size(); i-- > 0; ) {
            shockRings[i].timer -= realDt;
            if (shockRings[i].timer <= 0.f) {
                shockRings[i] = shockRings.back();
                shockRings.pop_back();
            }
        }
    }

    /**
 * @brief Knock the player out of control
 * @param playerIdx      Current vector index of the player
 * @param knockDir       Direction to be thrown (does not need normalising)
 * @param knockSpeed     Pixels/sec of knockback
 * @param tumbleDuration Seconds of no control at all
 * @param recoverDuration Seconds of aim-only recovery afterwards
 * @param spinSpeed      Peak tumble rate in deg/sec (sign is randomised)
 */

 // ========================================================================
 // PLAYER DAMAGE -- the ONE funnel for hull damage
 // ========================================================================

 /**
  * @brief Deal damage to the player through class armour.
  *
  * Every hull-damage site goes through here: bullets, contact, blasts,
  * bash, ram. Writing `currentHp -= x` directly skips the gunship's
  * damage reduction -- exactly how "armour works on bullets but not on
  * rockets" happens. Returns the damage actually taken.
  */
    float damagePlayer(size_t playerIdx, float amount) {
        if (playerIdx >= healths.size() || playerIdx >= players.size() || amount <= 0.f) return 0.f;
        const auto& ps = players[playerIdx];
        const float taken = amount * ps.damageTakenScale * (ps.hyperarmor ? ps.hyperarmorDamageScale : 1.f);
        healths[playerIdx].currentHp -= taken;
        return taken;
    }

    /**
     * @brief Wear poise down without ever breaking it.
     *
     * For chip damage (enemy bullets): it softens the ship up so the next
     * real blow breaks through, but a stream of small shots alone can never
     * tumble anyone. Floors at 1 poise, costs nothing under hyperarmor.
     */
    void chipPoise(size_t playerIdx, float amount) {
        if (playerIdx >= players.size() || amount <= 0.f) return;
        auto& ps = players[playerIdx];
        if (ps.poiseMax <= 0.f || ps.hyperarmor || ps.staggerImmuneTimer > 0.f) return;
        if (ps.poise > 1.f) ps.poise = std::max(1.f, ps.poise - amount);
        ps.poiseRegenTimer = ps.poiseDelay;
        ps.poiseHitFlash = std::max(ps.poiseHitFlash, 0.12f);
    }

    /// @param poiseDamage explicit poise cost of this hit; negative = derive from knockback
    void staggerPlayer(size_t playerIdx, sf::Vector2f knockDir, float knockSpeed,
        float tumbleDuration, float recoverDuration, float spinSpeed, float poiseDamage = -1.f) {
        if (playerIdx >= players.size()) return;
        auto& ps = players[playerIdx];

        // Already tumbling? Don't restack — that's how you get a 4-second lockout
        // from a single asteroid cluster.
        if (ps.staggerTimer > 0.f) return;

        // Poise damage reads the hit's RAW knockback: how hard the blow was,
        // not how far this particular hull gets pushed by it.
        if (poiseDamage < 0.f) poiseDamage = knockSpeed * poisePerKnockback;
        knockSpeed *= ps.knockbackScale;

        // ---- Anti-stunlock ----
        // Two Berserkers used to chain bash -> tumble -> bash forever: each
        // respected its OWN cooldown, but not each other's. After a tumble the
        // player is immune to a NEW tumble until control has been back for a
        // moment. The hit still lands (damage is the caller's), it just shoves
        // instead of knocking the ship out of control.
        if (ps.staggerImmuneTimer > 0.f) {
            float l = std::sqrt(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
            if (l > 0.001f) {
                const float shove = knockSpeed * staggerImmuneShove / l;
                b2Body_SetLinearVelocity(physics[playerIdx].bodyId,
                    { knockDir.x * shove / SCALE, knockDir.y * shove / SCALE });
            }
            addTrauma(0.35f);
            return;
        }
        // ---- Poise: mass absorbs the blow ----
        // Checked AFTER the anti-stunlock gate, so hits during the grace period
        // cost nothing. Hyperarmor (heavy dodge burst) absorbs without draining.
        ps.poiseRegenTimer = ps.poiseDelay;
        if (ps.poiseMax > 0.f && (ps.hyperarmor || ps.poise - poiseDamage > 0.f)) {
            if (!ps.hyperarmor) ps.poise -= poiseDamage;
            ps.poiseHitFlash = 0.25f;

            float l = std::sqrt(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
            if (l > 0.001f) {
                // ADD, don't set: a heavy that absorbs a hit keeps its heading.
                const float shove = knockSpeed * poiseAbsorbShove / l;
                const b2Vec2 v = b2Body_GetLinearVelocity(physics[playerIdx].bodyId);
                b2Body_SetLinearVelocity(physics[playerIdx].bodyId,
                    { v.x + knockDir.x * shove / SCALE, v.y + knockDir.y * shove / SCALE });
            }
            addTrauma(0.30f);
            spawnShockRing(transforms[playerIdx].position, 14.f, 70.f, 0.22f,
                sf::Color(255, 200, 80), 4.f, 230.f);
            return;
        }

        // ---- Poise broke (or there was none): a real tumble ----
        if (ps.poiseMax > 0.f) {
            ps.poise = ps.poiseMax;          // refills: the break IS the punishment
            ps.poiseBreakFlash = 0.6f;
        }
        tumbleDuration *= ps.tumbleScale;
        recoverDuration *= ps.tumbleScale;

        ps.staggerImmuneTimer = tumbleDuration + recoverDuration + staggerGrace;

        // Can't be hit while spinning helplessly -- the tumble is the punishment.
        if (playerIdx < healths.size())
            healths[playerIdx].invulTimer = std::max(healths[playerIdx].invulTimer,
                tumbleDuration * staggerTumbleIframes);

        ps.isStaggered = true;
        ps.staggerDuration = tumbleDuration;
        ps.staggerTimer = tumbleDuration;
        ps.staggerRecoverDuration = recoverDuration;
        ps.staggerRecoverTimer = 0.f;
        ps.staggerSpinSpeed = ((rand() % 2) ? 1.f : -1.f) * spinSpeed;

        // Cancel every competing state.
        ps.dashAnim = DashAnim::None; ps.dashAnimTimer = 0.f;
        if (ps.dashTimer > 0.f) b2Body_SetBullet(physics[playerIdx].bodyId, false);  // CCD was dodge-only
        ps.dashTimer = 0.f;   // a running dodge would otherwise overwrite the knockback
        ps.dashDriftTimer = 0.f;
        ps.isParrying = false; ps.parryTimer = 0.f; ps.parryAnimTimer = 0.f;
        ps.riftCharging = false;
        ps.isTurbo = false;

        float len = std::sqrt(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
        if (len > 0.001f) {
            knockDir /= len;
            // SET rather than impulse: a stumble should override your momentum,
            // not be quietly cancelled by it if you were charging in head-on.
            b2Body_SetLinearVelocity(physics[playerIdx].bodyId,
                { knockDir.x * knockSpeed / SCALE, knockDir.y * knockSpeed / SCALE });
        }

        addTrauma(0.85f);
        cameraZoomKick = -0.10f;
        requestHitstop(0.05f, 0.22f, 0.30f);
        spawnScreenFlash(sf::Color(255, 120, 80), 0.22f, 90.f);
        spawnShockRing(transforms[playerIdx].position, 20.f, 210.f, 0.40f,
            sf::Color(255, 140, 60), 6.f, 220.f);
        spawnExplosion(transforms[playerIdx].position, sf::Color(255, 160, 80), 24, 4.f);
    }

    /**
     * @brief The parry connected. Time-stop, flash, rings, shake.
     */
     /// @param accent the player's painted parry colour. EntityManager has no
     ///        idea which entity is the player, so the colour is passed in
     ///        rather than looked up -- the default is the original teal.
    void triggerParrySuccess(sf::Vector2f pos, float freeze, float slomo,
        float minScale, float trauma, float flashAlpha,
        sf::Color accent = sf::Color(0, 255, 220)) {
        requestHitstop(freeze, slomo, minScale);
        addTrauma(trauma);
        cameraZoomKick = -0.13f;   // punch IN — pulls the eye to the contact

        // Pale version of the accent for the flash and sparks; the small inner
        // ring stays white, because that white is what sells the impact.
        const sf::Color pale(
            static_cast<std::uint8_t>(accent.r + (255 - accent.r) * 0.72f),
            static_cast<std::uint8_t>(accent.g + (255 - accent.g) * 0.72f),
            static_cast<std::uint8_t>(accent.b + (255 - accent.b) * 0.72f));

        // Flash lasts the whole time-stop so the freeze reads as intentional.
        spawnScreenFlash(pale, freeze + slomo, flashAlpha);

        spawnShockRing(pos, 25.f, 300.f, 0.50f, accent, 7.f, 255.f);
        spawnShockRing(pos, 10.f, 140.f, 0.28f, sf::Color(255, 255, 255), 4.f, 255.f);
        spawnExplosion(pos, pale, 26, 3.5f);
        spawnShockwave(pos, 45.f, accent);
    }


    // ========================================================================
    // DEBRIS (decorative, non-physical rock shards)
    // ========================================================================
    std::vector<DebrisChunk> debris;

    /**
     * @brief Break an asteroid apart into visible shards
     * @param idx        Index of the dying asteroid
     * @param impactDir  Direction the killing blow came FROM (need not be unit)
     * @param physicalChildren How many real Box2D children to spawn
     * @param ef         Factory, for creating those children
     * @param lua        Lua state, for the child type config
     * @param worldId    Box2D world
     *
     * ==========================================================================
     * HOW THE FRACTURE WORKS
     * ==========================================================================
     * The rock's own outline is already stored in physicsShapes[idx].vertices.
     * We fan-triangulate it from the centroid into N wedges — one per edge —
     * and each wedge becomes a shard. So the pieces are literally the polygon
     * that just died, cut up. That's why it reads as "this rock cracked" rather
     * than "some generic debris appeared".
     *
     * Each shard is then thrown outward along its own centroid direction, with
     * a bias along the impact vector, so the break has a DIRECTION. A purely
     * radial burst looks like a firework; a directional one looks like
     * something hit it.
     *
     * Only a few shards become real physical children. The rest are decorative
     * (DebrisSystem). Making them all physical would triple the collision load
     * and hand the player a dozen new hazards as a reward for killing something.
     * ==========================================================================
     */
    void fractureAsteroid(size_t idx, sf::Vector2f impactDir, int physicalChildren,
        class EntityFactory* ef, sol::state* lua, b2WorldId worldId);

    /**
     * @brief Spawn a decorative shard
     * @param pts Local-space polygon points (max 5)
     */
    void spawnDebris(sf::Vector2f pos, sf::Vector2f vel, float angularVel,
        const sf::Vector2f* pts, int count,
        sf::Color color, float lifetime)
    {
        if (count < 3) return;
        DebrisChunk d;
        d.position = pos;
        d.velocity = vel;
        d.angularVelocity = angularVel;
        d.rotation = static_cast<float>(rand() % 360);
        d.color = color;
        d.lifetime = lifetime;
        d.maxLifetime = lifetime;
        d.pointCount = std::min(count, 5);
        for (int i = 0; i < d.pointCount; ++i) d.points[i] = pts[i];
        debris.push_back(d);
    }

    /**
     * @brief Layered magma detonation
     * @param pos    Epicentre
     * @param radius Damage radius (the visuals are keyed to this so the effect
     *               always shows the player exactly how far it reached)
     * @param empowered True if the rock was parry-launched (bigger, cyan-shot)
     *
     * ==========================================================================
     * The old version was three spawnExplosion() calls plus a ring of impacts —
     * loud, but structureless. Real explosions read in LAYERS with different
     * timings, and the eye picks that up even when it can't articulate it:
     *
     *   t=0.00  white flash core          — the detonation itself
     *   t=0.00  fast shock ring           — the pressure wave, expands ahead
     *   t=0.00  fireball, slow + fat      — the burn
     *   t=0.00  ember shower, long-lived  — thrown material, outlasts the flame
     *   t=0.00  molten shards (debris)    — actual chunks of the rock
     *   t=0.00  smoke, slowest, darkest   — what's left behind
     *
     * Crucially the DAMAGE RADIUS ring is drawn at full opacity: an AoE the
     * player can't measure is an AoE the player will resent.
     * ==========================================================================
     */
     /// @param accent used when `empowered` -- a rock the player turned into
     ///        their own weapon detonates in the player's homing colour.
    void spawnMagmaExplosion(sf::Vector2f pos, float radius, bool empowered,
        sf::Color accent = sf::Color(0, 230, 190)) {
        const auto shade = [&](float k) {
            return sf::Color(static_cast<std::uint8_t>(std::clamp(accent.r * k, 0.f, 255.f)),
                static_cast<std::uint8_t>(std::clamp(accent.g * k, 0.f, 255.f)),
                static_cast<std::uint8_t>(std::clamp(accent.b * k, 0.f, 255.f)));
            };
        const sf::Color hot = empowered ? shade(1.45f) : sf::Color(255, 190, 90);
        const sf::Color mid = empowered ? accent : sf::Color(255, 110, 30);
        const sf::Color deep = empowered ? shade(0.62f) : sf::Color(150, 40, 10);

        // ---- 1. Core flash: brief, white, small ----
        spawnShockRing(pos, 4.f, radius * 0.42f, 0.16f, sf::Color::White, 9.f, 255.f);

        // ---- 2. Pressure wave: reaches the exact damage radius ----
        spawnShockRing(pos, 10.f, radius, 0.34f, hot, 7.f, 255.f);
        spawnShockRing(pos, 10.f, radius * 1.28f, 0.55f, mid, 3.5f, 190.f);

        // ---- 3. Fireball: fat, slow, opaque ----
        for (int i = 0; i < 40; ++i) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = radius * (0.9f + (rand() % 60) / 100.f);
            const float lf = 0.30f + (rand() % 30) / 100.f;
            particles.push_back({
                nextEntityId++,
                pos + sf::Vector2f(std::cos(a), std::sin(a)) * (radius * 0.12f),
                sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                sf::Color(hot.r, hot.g, hot.b, 235),
                lf, lf,
                7.f + rand() % 6
                });
        }

        // ---- 4. Embers: fast, small, long-lived. These sell the SCALE,
        //         because they're still drifting after the flame is gone. ----
        for (int i = 0; i < 55; ++i) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = radius * (1.3f + (rand() % 140) / 100.f);
            const float lf = 0.55f + (rand() % 70) / 100.f;
            particles.push_back({
                nextEntityId++,
                pos,
                sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                sf::Color(255, static_cast<uint8_t>(120 + rand() % 100), 40, 230),
                lf, lf,
                1.5f + rand() % 3
                });
        }

        // ---- 5. Molten shards: angular pieces, glowing ----
        for (int i = 0; i < 14; ++i) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sz = 4.f + rand() % 7;
            const sf::Vector2f pts[4] = {
                { -sz,        -sz * 0.6f },
                {  sz * 0.8f, -sz },
                {  sz,         sz * 0.7f },
                { -sz * 0.7f,  sz * 0.9f }
            };
            spawnDebris(
                pos + sf::Vector2f(std::cos(a), std::sin(a)) * (radius * 0.15f),
                sf::Vector2f(std::cos(a), std::sin(a)) * (radius * (1.0f + (rand() % 100) / 100.f)),
                ((rand() % 2) ? 1.f : -1.f) * (120.f + rand() % 260),
                pts, 4,
                sf::Color(mid.r, mid.g, mid.b, 245),
                0.9f + (rand() % 60) / 100.f);
        }

        // ---- 6. Smoke: slowest, darkest, outlives everything ----
        for (int i = 0; i < 22; ++i) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = radius * (0.30f + (rand() % 40) / 100.f);
            const float lf = 0.9f + (rand() % 80) / 100.f;
            particles.push_back({
                nextEntityId++,
                pos + sf::Vector2f(std::cos(a), std::sin(a)) * (radius * 0.25f),
                sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                sf::Color(deep.r, deep.g, deep.b, 170),
                lf, lf,
                9.f + rand() % 8
                });
        }

        spawnScreenFlash(sf::Color(255, 170, 90), 0.22f, empowered ? 80.f : 65.f);
        addTrauma(0.55f);
        requestHitstop(0.03f, 0.14f, 0.40f);
    }


};