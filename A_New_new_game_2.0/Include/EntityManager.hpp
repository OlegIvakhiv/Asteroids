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
#include "components.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <random>

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
    CATEGORY_ENEMY_BULLET = 0x0010    // enemy bullets — separate so they don't hit each other
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

    // ===== Visual Effects =====
    std::vector<Particle> particles;              ///< Explosion/debris particles
    std::vector<Star> stars;                      ///< Parallax background stars

    // ===== Scoring System =====
    std::vector<int> scoreRewards;                ///< Points awarded when entity dies
    int totalScore = 0;                           ///< Cumulative player score

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
    void initBackground(sf::Vector2u winSize, int count = 400) {
        stars.clear();
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> distX(0.f, (float)winSize.x);
        std::uniform_real_distribution<float> distY(0.f, (float)winSize.y);
        std::uniform_real_distribution<float> distSpeed(0.05f, 0.5f);
        std::uniform_int_distribution<int> distAlpha(100, 255);

        for (int i = 0; i < count; ++i) {
            Star s;
            s.parallaxFactor = distSpeed(rng);
            s.position = { distX(rng), distY(rng) };
            s.size = s.parallaxFactor * 4.0f;

            // Tint stars slightly purple/magenta
            int g = 255 - (rand() % 50);
            int a = distAlpha(rng);

            // Farthest stars are smaller and dimmer
            if (s.parallaxFactor < 0.2f) {
                a /= 2;
                s.size = 1.0f;
            }

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


};