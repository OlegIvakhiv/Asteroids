/**
 * @file EntityFactory.hpp
 * @brief Factory for creating game entities
 *
 * Responsible for creating player, asteroid, bullet, and enemy entities.
 * Uses EntityManager for storage and Box2D/Lua for configuration.
 *
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#pragma once

#include "EntityManager.hpp"
#include <sol/sol.hpp>

class EntityFactory {
public:
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
    uint32_t createPlayer(EntityManager& em, sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = em.nextEntityId++;

        // Transform component
        em.transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f });
        em.bullets.push_back({ entityId });
        em.players.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.healths.push_back({ entityId, 100.f, 100.f, 0.f, 0.f });
        em.players.push_back({});

        // Box2D physics body
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Player, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearDamping = lua["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = lua["angulardgrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // Collision shape (circle)
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_PLAYER;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY | CATEGORY_ENEMY_BULLET | CATEGORY_BULLET;
        shapeDef.enableContactEvents = true;
        shapeDef.density = lua["density"].get_or(0.5f);

        b2Circle circle = { {0.0f, 0.0f}, 0.8f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        em.physics.push_back({ entityId, bid });

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

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

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
    uint32_t createAsteroid(EntityManager& em, sf::Vector2f pos, sf::Vector2f vel, float baseSize, sol::table config, b2WorldId worldId) {
        uint32_t entityId = em.nextEntityId++;
        em.transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f });
        em.enemies.push_back({});
        em.players.push_back({});

        // Box2D physics body
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Asteroid, entityId };
        bodyDef.userData = ud;
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

        em.physics.push_back({ entityId, bid });

        // Stats from Lua config
        float hpValue = config["hp"].get_or(20.0f);
        bool isExplosive = config["explosive"].get_or(false);
        float explosionRadius = config["explosion_radius"].get_or(150.0f);
        float explosionDamage = config["explosion_damage"].get_or(30.0f);

        em.healths.push_back({
            entityId,
            hpValue,
            hpValue,
            0.f,
            0.f,
            isExplosive,
            explosionRadius,
            explosionDamage
            });

        em.bullets.push_back({ entityId });
        em.players.push_back({});
        em.scoreRewards.push_back(config["score_reward"].get_or(10));

        // Asteroid color (grayscale with slight variation)
        int gray = 40 + (rand() % 30);
        rc.shape.setFillColor(sf::Color(gray, gray, gray + (rand() % 5)));
        rc.shape.setOutlineColor(sf::Color(gray + 40, gray + 40, gray + 45));
        rc.shape.setOutlineThickness(2.0f);

        // Random rotation speed
        float randomSpin = ((rand() % 200) - 100.f) / 50.f;
        b2Body_SetAngularVelocity(bid, randomSpin);

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

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
    uint32_t createBullet(EntityManager& em, sf::Vector2f pos, sf::Vector2f velocity, float angle, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = em.nextEntityId++;

        // Configuration from Lua
        float speed = lua["bullet_speed"].get_or(800.0f);
        float lifetime = lua["bullet_lifetime"].get_or(1.5f);
        sol::table col = lua["bullet_color"];

        // Recalculate velocity (ensures consistent direction)
        float rad = (angle - 90.f) * 3.14159f / 180.f;
        sf::Vector2f newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };

        em.transforms.push_back({ entityId, pos, newVelocity, {0.f, 0.f}, angle });

        // Box2D physics (fast moving bullet)
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Bullet, entityId };
        bodyDef.userData = ud;
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

        em.physics.push_back({ entityId, bid });
        em.bullets.push_back({ entityId, lifetime, false, true });
        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

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

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

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
    uint32_t createEnemy(EntityManager& em, sf::Vector2f pos, sol::state& lua, b2WorldId worldId) {
        sol::table config = lua["enemy_config"];
        uint32_t entityId = em.nextEntityId++;

        TransformComponent tf;
        tf.entityId = entityId;
        tf.position = pos;
        em.transforms.push_back(tf);

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
        BodyUserData* ud = new BodyUserData{ BodyType::Enemy, entityId };
        bodyDef.userData = ud;
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

        em.physics.push_back({ entityId, bid });
        em.healths.push_back({ entityId, config["hp"].get_or(50.f), config["hp"].get_or(50.f) });
        em.bullets.push_back({ entityId });
        em.enemies.push_back({});
        em.scoreRewards.push_back(config["score_reward"].get_or(100));
        em.players.push_back({});

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }




    uint32_t createEnemyBullet(EntityManager& em, sf::Vector2f pos, sf::Vector2f velocity, float angle, uint32_t ownerEntityId, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = em.nextEntityId++;

        float speed = lua["enemy_config"]["bullet_speed"].get_or(550.0f);
        float lifetime = lua["enemy_config"]["bullet_lifetime"].get_or(2.0f);

        float rad = (angle - 90.f) * 3.14159f / 180.f;
        sf::Vector2f newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };

        em.transforms.push_back({ entityId, pos, newVelocity, {0.f, 0.f}, angle });

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Bullet, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { newVelocity.x / SCALE, newVelocity.y / SCALE };
        bodyDef.rotation = b2MakeRot(angle * 3.14159f / 180.f);
        bodyDef.isBullet = true;

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ENEMY_BULLET;
        // hits player and asteroids � NOT other enemies, NOT player bullets
        shapeDef.filter.maskBits = CATEGORY_PLAYER | CATEGORY_ASTEROID | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;

        b2Circle circle = { {0.0f, 0.0f}, 0.15f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        em.physics.push_back({ entityId, bid });
        em.bullets.push_back({ entityId, lifetime, false, true, true, ownerEntityId, false });
        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

        // Visual: elongated orange diamond � clearly enemy origin
        RenderComponent rc;
        rc.shape.setPointCount(4);
        rc.shape.setPoint(0, { 0,  -12 });
        rc.shape.setPoint(1, { 2.5f, 0 });
        rc.shape.setPoint(2, { 0,   12 });
        rc.shape.setPoint(3, { -2.5f, 0 });
        rc.shape.setFillColor(sf::Color(255, 120, 0));
        rc.shape.setOutlineThickness(1.5f);
        rc.shape.setOutlineColor(sf::Color(255, 60, 0, 200));

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }



    uint32_t createRiftBolt(EntityManager& em, sf::Vector2f pos, sf::Vector2f velocity, float angle, sol::state& lua, b2WorldId worldId) {
        uint32_t entityId = em.nextEntityId++;

        float speed = lua["rift_bullet_speed"].get_or(1100.f);
        float lifetime = lua["bullet_lifetime"].get_or(1.5f);

        float rad = (angle - 90.f) * 3.14159f / 180.f;
        sf::Vector2f newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };

        em.transforms.push_back({ entityId, pos, newVelocity, {0.f, 0.f}, angle });

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Bullet, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { newVelocity.x / SCALE, newVelocity.y / SCALE };
        bodyDef.rotation = b2MakeRot(angle * 3.14159f / 180.f);
        bodyDef.isBullet = true;

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_BULLET;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY;
        shapeDef.enableContactEvents = true;

        float hitboxRadius = lua["rift_hitbox_radius"].get_or(0.35f);
        b2Circle circle = { {0.0f, 0.0f}, hitboxRadius };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        em.physics.push_back({ entityId, bid });
        // isRiftBolt = true (5th bool in BulletComponent)
        em.bullets.push_back({ entityId, lifetime, false, true, false, 0, true });
        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

        // Rift bolt visual: wider diamond, purple-white core with cyan outline
        RenderComponent rc;
        rc.shape.setPointCount(4);
        rc.shape.setPoint(0, { 0,   -14 });
        rc.shape.setPoint(1, { 4.f,   0 });
        rc.shape.setPoint(2, { 0,    14 });
        rc.shape.setPoint(3, { -4.f,   0 });
        rc.shape.setFillColor(sf::Color(200, 100, 255, 255));
        rc.shape.setOutlineThickness(2.f);
        rc.shape.setOutlineColor(sf::Color(0, 255, 255, 220));

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }
};