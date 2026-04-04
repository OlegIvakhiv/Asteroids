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
    CATEGORY_ENEMY = 0x0008    ///< Enemy ships
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

            // Swap optional components (only if they exist for both indices)
            if (index < healths.size() && lastIdx < healths.size()) {
                std::swap(healths[index], healths[lastIdx]);
            }
            if (index < bullets.size() && lastIdx < bullets.size()) {
                std::swap(bullets[index], bullets[lastIdx]);
            }
            if (index < enemies.size() && lastIdx < enemies.size()) {
                std::swap(enemies[index], enemies[lastIdx]);
            }

            // Update ID map for the entity that just moved into the deleted slot
            uint32_t swappedId = transforms[index].entityId;
            entityIdMap[swappedId] = index;
        }

        // 5. Remove the last element from all component vectors
        transforms.pop_back();
        renders.pop_back();
        physics.pop_back();
        scoreRewards.pop_back();

        if (!healths.empty()) healths.pop_back();
        if (!bullets.empty()) bullets.pop_back();
        if (!enemies.empty()) enemies.pop_back();

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

    // ===== Entity Creation Methods =====

    /**
     * @brief Create the player ship entity
     * @param pos Initial world position in pixels
     * @param lua Lua state containing player configuration
     * @param worldId Box2D world identifier
     * @return Persistent entity ID (not index!)
     *
     * Player is dynamic body with circle collision shape.
     * Shape vertices are loaded from Lua (or defaults to triangle).
     */
    uint32_t createPlayer(sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = nextEntityId++;

        // Transform component
        transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f, 0.f, 0.f });
        bullets.push_back({ entityId });
        scoreRewards.push_back({});
        enemies.push_back({});
        healths.push_back({ entityId, 100.f, 100.f, 0.f, 0.f });

        // Box2D physics body
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.userData = (void*)(uintptr_t)BodyType::Player;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearDamping = lua["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = lua["angulardgrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // Collision shape (circle)
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_PLAYER;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;
        shapeDef.density = lua["density"].get_or(0.5f);

        b2Circle circle = { {0.0f, 0.0f}, 0.8f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        physics.push_back({ entityId, bid });

        // Render shape (from Lua or default triangle)
        RenderComponent rc;
        sol::table shapeTable = lua["ship_shape"];
        if (shapeTable.valid()) {
            rc.shape.setPointCount(shapeTable.size());
            for (size_t i = 1; i <= shapeTable.size(); ++i) {
                sol::table point = shapeTable[i];
                rc.shape.setPoint(i - 1, sf::Vector2f(point["x"].get<float>(), point["y"].get<float>()));
            }
        }
        else {
            // Default triangular ship
            rc.shape.setPointCount(3);
            rc.shape.setPoint(0, { 0, -15 });
            rc.shape.setPoint(1, { 10, 10 });
            rc.shape.setPoint(2, { -10, 10 });
        }

        // Colors from Lua
        sol::table luaColor = lua["color"];
        rc.shape.setFillColor(sf::Color(
            luaColor["r"].get_or(40),
            luaColor["g"].get_or(100),
            luaColor["b"].get_or(255),
            luaColor["a"].get_or(255)
        ));

        sol::table luaOutline = lua["outline_color"];
        rc.shape.setOutlineColor(sf::Color(
            luaOutline["r"].get_or(255),
            luaOutline["g"].get_or(255),
            luaOutline["b"].get_or(255)
        ));
        rc.shape.setOutlineThickness(2.5f);

        renders.push_back(rc);
        entityIdMap[entityId] = transforms.size() - 1;

        return entityId;
    }

    /**
     * @brief Create an asteroid entity
     * @param pos Initial position in pixels
     * @param vel Initial velocity in pixels/sec
     * @param baseSize Radius in pixels (pre-scale)
     * @param config Lua table with asteroid properties (hp, density, score, etc.)
     * @param worldId Box2D world identifier
     * @return Persistent entity ID
     *
     * Asteroids use procedural 8-point irregular polygons for organic look.
     * Random spin velocity adds variety to movement.
     */
    uint32_t createAsteroid(sf::Vector2f pos, sf::Vector2f vel, float baseSize, sol::table config, b2WorldId worldId) {
        uint32_t entityId = nextEntityId++;
        transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f, 0.f, 0.f });
        enemies.push_back({});

        // Box2D physics body
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.userData = (void*)(uintptr_t)BodyType::Asteroid;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { vel.x, vel.y };
        bodyDef.linearDamping = 0.0f;
        bodyDef.angularDamping = 0.05f;

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // Generate irregular polygon (8 points with random radius variation)
        RenderComponent rc;
        std::vector<b2Vec2> physicsPoints;
        int numPoints = 8;
        rc.shape.setPointCount(numPoints);
        float pixelRadius = baseSize * SCALE;

        for (int i = 0; i < numPoints; ++i) {
            float angle = (i / (float)numPoints) * 2.f * 3.14159f;
            float noise = (rand() % 100) / 100.f;
            float dist = pixelRadius * (0.85f + noise * 0.4f);

            float px = std::cos(angle) * dist;
            float py = std::sin(angle) * dist;
            rc.shape.setPoint(i, { px, py });
            physicsPoints.push_back({ px / SCALE, py / SCALE });
        }

        // Box2D collision shape (convex hull from generated points)
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ASTEROID;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(1.0f);
        shapeDef.material.friction = 0.1f;
        shapeDef.material.restitution = 0.8f;

        b2Hull hull = b2ComputeHull(physicsPoints.data(), (int)physicsPoints.size());
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        physics.push_back({ entityId, bid });

        // Stats from Lua config
        float hpValue = config["hp"].get_or(20.0f);
        bool isExplosive = config["explosive"].get_or(false);
        float explosionRadius = config["explosion_radius"].get_or(150.0f);
        float explosionDamage = config["explosion_damage"].get_or(30.0f);

        healths.push_back({
            entityId,
            hpValue,
            hpValue,
            0.f,
            0.f,
            isExplosive,
            explosionRadius,
            explosionDamage
            });

        bullets.push_back({ entityId });
        scoreRewards.push_back(config["score_reward"].get_or(10));

        // Asteroid color (grayscale with slight variation)
        int gray = 40 + (rand() % 30);
        rc.shape.setFillColor(sf::Color(gray, gray, gray + (rand() % 5)));
        rc.shape.setOutlineColor(sf::Color(gray + 40, gray + 40, gray + 45));
        rc.shape.setOutlineThickness(2.0f);

        // Random rotation speed
        float randomSpin = ((rand() % 200) - 100.f) / 50.f;
        b2Body_SetAngularVelocity(bid, randomSpin);

        renders.push_back(rc);
        entityIdMap[entityId] = transforms.size() - 1;

        return entityId;
    }

    /**
     * @brief Create a bullet projectile
     * @param pos Spawn position in pixels (tip of player ship)
     * @param velocity Direction and speed in pixels/sec
     * @param angle Rotation angle of bullet (degrees)
     * @param lua Lua state with bullet configuration
     * @param worldId Box2D world identifier
     * @return Persistent entity ID
     *
     * Bullets are fast, short-lived, and use "isBullet" flag in Box2D
     * for continuous collision detection (prevents tunneling through asteroids).
     */
    uint32_t createBullet(sf::Vector2f pos, sf::Vector2f velocity, float angle, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = nextEntityId++;

        // Configuration from Lua
        float speed = lua["bullet_speed"].get_or(800.0f);
        float lifetime = lua["bullet_lifetime"].get_or(1.5f);
        sol::table col = lua["bullet_color"];

        // Recalculate velocity (ensures consistent direction)
        float rad = (angle - 90.f) * 3.14159f / 180.f;
        sf::Vector2f newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };

        transforms.push_back({ entityId, pos, newVelocity, {0.f, 0.f}, angle, 0.f, 0.f });

        // Box2D physics (fast moving bullet)
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.userData = (void*)(uintptr_t)BodyType::Bullet;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { newVelocity.x / SCALE, newVelocity.y / SCALE };
        bodyDef.rotation = b2MakeRot(angle * 3.14159f / 180.f);
        bodyDef.isBullet = true;  // Enable CCD to prevent tunneling

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_BULLET;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;

        b2Circle circle = { {0.0f, 0.0f}, 0.1f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        physics.push_back({ entityId, bid });
        bullets.push_back({ entityId, lifetime, false, true });
        healths.push_back({ entityId });
        scoreRewards.push_back({});
        enemies.push_back({});

        // Diamond-shaped bullet visual
        RenderComponent rc;
        rc.shape.setPointCount(4);
        rc.shape.setPoint(0, { 0, -10 });
        rc.shape.setPoint(1, { 1.5f, 0 });
        rc.shape.setPoint(2, { 0, 10 });
        rc.shape.setPoint(3, { -1.5f, 0 });

        rc.shape.setFillColor(sf::Color::White);
        rc.shape.setOutlineThickness(1.5f);
        rc.shape.setOutlineColor(sf::Color(
            col["r"].get_or(0),
            col["g"].get_or(255),
            col["b"].get_or(255),
            180
        ));

        renders.push_back(rc);
        entityIdMap[entityId] = transforms.size() - 1;

        return entityId;
    }

    /**
     * @brief Create an enemy ship entity
     * @param pos Initial position in pixels
     * @param lua Lua state with enemy configuration
     * @param worldId Box2D world identifier
     * @return Persistent entity ID
     *
     * Enemies have complex polygon shape (12 points) resembling a pirate ship.
     * AI behavior is handled separately in AISystem.
     */
    uint32_t createEnemy(sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        sol::table config = lua["enemy_config"];
        uint32_t entityId = nextEntityId++;

        TransformComponent tf;
        tf.entityId = entityId;
        tf.position = pos;
        transforms.push_back(tf);

        // Pirate ship shape (12 points)
        RenderComponent rc;
        rc.shape.setPointCount(12);
        rc.shape.setPoint(0, { 0, -10 });
        rc.shape.setPoint(1, { 8, -25 });
        rc.shape.setPoint(2, { 12, -10 });
        rc.shape.setPoint(3, { 25, 5 });
        rc.shape.setPoint(4, { 25, 15 });
        rc.shape.setPoint(5, { 15, 10 });
        rc.shape.setPoint(6, { 0, 20 });
        rc.shape.setPoint(7, { -15, 10 });
        rc.shape.setPoint(8, { -25, 15 });
        rc.shape.setPoint(9, { -25, 5 });
        rc.shape.setPoint(10, { -12, -10 });
        rc.shape.setPoint(11, { -8, -25 });

        rc.shape.setFillColor(sf::Color(config["color"]["r"], config["color"]["g"], config["color"]["b"]));
        rc.shape.setOutlineThickness(1.5f);
        rc.shape.setOutlineColor(sf::Color(255, 255, 255, 150));

        // Box2D physics body
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.userData = (void*)(uintptr_t)BodyType::Enemy;
        bodyDef.linearDamping = config["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = config["angulardgrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // Simplified collision hull (6 points, not full visual shape)
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ENEMY;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_PLAYER | CATEGORY_BULLET | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(3.0f);
        shapeDef.material.restitution = 0.4f;

        b2Vec2 physicsPoints[6] = {
            {0.0f, -25.0f / SCALE},
            {25.0f / SCALE, 5.0f / SCALE},
            {25.0f / SCALE, 15.0f / SCALE},
            {0.0f, 20.0f / SCALE},
            {-25.0f / SCALE, 15.0f / SCALE},
            {-25.0f / SCALE, 5.0f / SCALE}
        };
        b2Hull hull = b2ComputeHull(physicsPoints, 6);
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        physics.push_back({ entityId, bid });
        healths.push_back({ entityId, config["hp"].get_or(50.f), config["hp"].get_or(50.f) });
        bullets.push_back({ entityId });
        enemies.push_back({});
        scoreRewards.push_back(config["score_reward"].get_or(100));

        renders.push_back(rc);
        entityIdMap[entityId] = transforms.size() - 1;

        return entityId;
    }
};