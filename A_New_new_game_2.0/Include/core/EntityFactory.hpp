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

#include "core/EnemyArchetypes.hpp"
#include "EntityManager.hpp"
#include "utils/ShipDesign.hpp"
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

        // --- Box2D physics body ---
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Player, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearDamping = lua["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = lua["angulardrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // --- Create Convex Physics Hitbox (Box2D v3 Hull) ---
        // We sample key exterior points from the 12-point ship (Nose, Wings, Tail) 
        // to form a tight 6-point convex collision hull under the 8-vertex limit.
        std::vector<sf::Vector2f> debugHullVerts = {
            {   0.f, -30.f }, // Nose tip
            {  28.f,  15.f }, // Right wing tip
            {  18.f,  28.f }, // Right engine bottom
            { -18.f,  28.f }, // Left engine bottom
            { -28.f,  15.f }  // Left wing tip
        };

        b2Vec2 b2Points[5];
        for (size_t i = 0; i < debugHullVerts.size(); ++i) {
            b2Points[i] = { debugHullVerts[i].x / SCALE, debugHullVerts[i].y / SCALE };
        }

        b2Hull hull = b2ComputeHull(b2Points, static_cast<int32_t>(debugHullVerts.size()));
        b2Polygon polygon = b2MakePolygon(&hull, 0.0f);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_PLAYER;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY | CATEGORY_ENEMY_BULLET
            | CATEGORY_BULLET | CATEGORY_ORDNANCE;
        shapeDef.enableContactEvents = true;
        shapeDef.density = lua["density"].get_or(0.5f);

        b2CreatePolygonShape(bid, &shapeDef, &polygon);

        // Store shape data in ECS for DebugSystem drawing
        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Polygon;
        shapeData.vertices = debugHullVerts;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });

        // --- Render component (Full 12-point shape from Lua) ---
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
            // Default triangular ship fallback
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
 * @brief Create the player from a ShipDesign instead of hardcoded points.
 *
 * The design's outline IS the collision hull -- ShipDesign enforces
 * convexity and the 8-point cap, so it feeds b2ComputeHull with no
 * conversion and no visual/physics mismatch.
 *
 * HP, energy and engine power come from geometry. Mass is not set here at
 * all: Box2D derives it from shape area x density, which is why a bigger
 * hull is slower without a single tuning value.
 */
    uint32_t createPlayerFromDesign(EntityManager& em, sf::Vector2f pos,
        sol::state& lua, b2WorldId worldId,
        const ship::ShipDesign& design) {
        const ship::ShipStats& st = design.stats();
        const auto& outline = design.outline();

        uint32_t entityId = em.nextEntityId++;

        em.transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f });
        em.bullets.push_back({ entityId });
        em.players.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.healths.push_back({ entityId, st.hpMax, st.hpMax, 0.f, 0.f });

        // ---- Hull-derived stats onto the player component ----
        PlayerComponent& pc = em.players.back();
        pc.maxEnergyDrive = st.energyMax;
        pc.energyDrive = st.energyMax;
        pc.enginePower = st.thrustN;

        pc.gunMountCount = 0;
        for (int gi : design.mountedGuns()) {
            if (pc.gunMountCount >= 4) break;
            if (gi >= 0 && gi < static_cast<int>(outline.size()))
                pc.gunMounts[pc.gunMountCount++] = outline[gi];
        }
        // Never leave the ship unable to shoot, whatever the editor produced.
        if (pc.gunMountCount == 0) {
            pc.gunMounts[0] = { 0.f, -30.f };
            pc.gunMountCount = 1;
        }

        // ---- Box2D body ----
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Player, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearDamping = lua["lineardrag_factor"].get_or(0.5f);
        bodyDef.angularDamping = lua["angulardrag_factor"].get_or(0.5f);

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        // The design outline goes straight in -- convex and <= 8 by construction.
        b2Vec2 b2Points[ship::MAX_HULL_POINTS];
        const int pc_n = std::min(static_cast<int>(outline.size()),
            ship::MAX_HULL_POINTS);
        for (int i = 0; i < pc_n; ++i)
            b2Points[i] = { outline[i].x / SCALE, outline[i].y / SCALE };

        b2Hull hull = b2ComputeHull(b2Points, static_cast<int32_t>(pc_n));
        b2Polygon polygon = b2MakePolygon(&hull, 0.0f);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_PLAYER;
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY
            | CATEGORY_ENEMY_BULLET | CATEGORY_BULLET;
        shapeDef.enableContactEvents = true;
        shapeDef.density = design.tuning().density;

        b2CreatePolygonShape(bid, &shapeDef, &polygon);

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Polygon;
        shapeData.vertices = outline;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });

        // ---- Render: same outline, so what you built is what you fly ----
        RenderComponent rc;
        rc.shape.setPointCount(outline.size());
        for (std::size_t i = 0; i < outline.size(); ++i)
            rc.shape.setPoint(i, outline[i]);

        sol::table luaColor = lua["color"];
        rc.shape.setFillColor(sf::Color(
            luaColor["r"].get_or(40), luaColor["g"].get_or(100),
            luaColor["b"].get_or(255), luaColor["a"].get_or(255)));
        rc.shape.setOutlineThickness(2.5f);
        rc.shape.setOutlineColor(sf::Color(255, 255, 255, 150));
        rc.entityId = entityId;

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }


    /**
     * @brief Create an asteroid entity
     * @param pos Initial position in pixels
     * @param vel Initial velocity in pixels/sec
     * @param baseSize Radius in METERS (pre-variance)
     * @param config Lua table with asteroid properties
     * @param worldId Box2D world identifier
     * @param sizeRollOverride -1 = roll fresh; 0..1 = forced roll (used by
     *        fracture so a shard's size can be derived from its parent)
     * @return Persistent entity ID
     *
     * ==========================================================================
     * SIZE AND HP VARIANCE
     * ==========================================================================
     * Each asteroid rolls a size multiplier inside its type's configured range.
     * The ranges DELIBERATELY OVERLAP: a big SMALL is larger than a runt MEDIUM.
     * That's what makes a field look like real rubble instead of three repeated
     * props.
     *
     * The critical rule: HP IS DERIVED FROM THE ROLLED SIZE, not rolled
     * independently. A visually tiny rock that soaks four shots is infuriating
     * because the player has no way to read it. Tying HP to area (roll², since
     * these are 2D discs) means what you see is what you get — big rocks are
     * always tougher, and a big SMALL is genuinely tougher than a runt MEDIUM,
     * which is the whole point.
     *
     * Density is left alone: Box2D computes mass from the polygon area, so a
     * larger roll is automatically heavier and hits harder. No extra work.
     * ==========================================================================
     */
    uint32_t createAsteroid(EntityManager& em, sf::Vector2f pos, sf::Vector2f vel,
        float baseSize, sol::table config, b2WorldId worldId,
        float sizeRollOverride = -1.f)
    {
        uint32_t entityId = em.nextEntityId++;
        em.transforms.push_back({ entityId, pos, {0.f, 0.f}, {0.f, 0.f}, 0.f });
        em.enemies.push_back({});
        em.players.push_back({});

        // ====================================================================
        // SIZE ROLL
        // ====================================================================
        float sizeMin = config["size_variance_min"].get_or(0.85f);
        float sizeMax = config["size_variance_max"].get_or(1.15f);

        float roll;
        if (sizeRollOverride >= 0.f) {
            roll = sizeMin + (sizeMax - sizeMin) * std::clamp(sizeRollOverride, 0.f, 1.f);
        }
        else {
            // Two averaged rolls: a rough triangular distribution, so most
            // rocks sit near the middle and extremes stay special. A flat
            // rand() makes runts and giants equally common, which just reads
            // as noise.
            const float r1 = (rand() % 1000) / 1000.f;
            const float r2 = (rand() % 1000) / 1000.f;
            roll = sizeMin + (sizeMax - sizeMin) * ((r1 + r2) * 0.5f);
        }

        const float finalSize = baseSize * roll;

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

        // ====================================================================
        // SILHOUETTE
        //
        // Point count scales with size. An 8-point outline on a 9px rock is
        // wasted geometry that reads as a circle; a 45px rock with only 8
        // points reads as a stop sign. Capped at 8 because b2ComputeHull
        // rejects more than 8 vertices.
        // ====================================================================
        RenderComponent rc;
        std::vector<b2Vec2> physicsPoints;

        const float pixelRadius = finalSize * SCALE;
        int numPoints = 6;
        if (pixelRadius > 18.f) numPoints = 7;
        if (pixelRadius > 32.f) numPoints = 8;

        rc.shape.setPointCount(numPoints);

        // Jaggedness also scales: big rocks get deeper craters, small chips
        // stay compact so they don't look like torn paper.
        const float jag = config["jaggedness"].get_or(0.40f);

        for (int i = 0; i < numPoints; ++i) {
            // Jitter the ANGLE as well as the radius. Evenly-spaced angles
            // give a regular polygon no matter how much you vary the radius,
            // which is why the old rocks still read as circles.
            const float baseAngle = (i / (float)numPoints) * 2.f * 3.14159f;
            const float angleJit = ((rand() % 100) / 100.f - 0.5f) * (1.2f / numPoints);
            const float angle = baseAngle + angleJit;

            const float noise = (rand() % 100) / 100.f;
            const float dist = pixelRadius * (1.f - jag * 0.5f + noise * jag);

            const float px = std::cos(angle) * dist;
            const float py = std::sin(angle) * dist;
            rc.shape.setPoint(i, { px, py });
            physicsPoints.push_back({ px / SCALE, py / SCALE });
        }

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ASTEROID;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(1.0f);
        shapeDef.material.friction = 0.1f;
        shapeDef.material.restitution = 0.8f;

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Polygon;
        shapeData.vertices.reserve(physicsPoints.size());
        for (const auto& v : physicsPoints) {
            shapeData.vertices.push_back({ v.x * SCALE, v.y * SCALE });
        }
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        b2Hull hull = b2ComputeHull(physicsPoints.data(), (int)physicsPoints.size());
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        em.physics.push_back({ entityId, bid });

        // ====================================================================
        // STATS — HP DERIVED FROM THE ROLLED SIZE
        // ====================================================================
        const float baseHp = config["hp"].get_or(20.0f);
        const float hpFromSize = config["hp_follows_size"].get_or(1.0f);

        // roll^2 because HP tracks cross-sectional area, not radius. A rock
        // 20% wider has ~44% more area and should feel proportionally tougher.
        const float areaScale = roll * roll;
        const float hpValue = baseHp * (1.f - hpFromSize + hpFromSize * areaScale);

        const bool isExplosive = config["explosive"].get_or(false);
        const float explosionRadius = config["explosion_radius"].get_or(150.0f) * roll;
        const float explosionDamage = config["explosion_damage"].get_or(30.0f) * roll;

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

        // Cache size info for the fracture path, so DamageSystem never has to
        // infer an asteroid's tier from its score reward again.
        em.healths.back().visualRadius = pixelRadius;
        em.healths.back().asteroidTier = static_cast<uint8_t>(config["tier"].get_or(0));

        em.bullets.push_back({ entityId });
        em.scoreRewards.push_back(config["score_reward"].get_or(10));
        // NOTE: the second `em.players.push_back({})` that used to live here has
        // been REMOVED. It was pushing two PlayerComponents per asteroid while
        // every other array got one, so players.size() outran transforms.size()
        // and destroyEntity's swap-and-pop (which indexes off transforms.size())
        // was operating on the wrong element.

        // ====================================================================
        // COLOUR — tinted per type, with per-rock variation
        // ====================================================================
        sol::table col = config["color"];
        const int cr = col["r"].get_or(60);
        const int cg = col["g"].get_or(55);
        const int cb = col["b"].get_or(50);

        // Bigger rocks read slightly darker and denser; chips catch more light.
        const float lift = (1.15f - 0.35f * std::clamp(pixelRadius / 50.f, 0.f, 1.f));
        const int jitter = (rand() % 22) - 8;

        auto ch = [&](int base) {
            return static_cast<uint8_t>(std::clamp(
                static_cast<int>(base * lift) + jitter, 0, 255));
        };

        rc.shape.setFillColor(sf::Color(ch(cr), ch(cg), ch(cb)));
        rc.shape.setOutlineColor(sf::Color(
            std::min(255, ch(cr) + 45),
            std::min(255, ch(cg) + 45),
            std::min(255, ch(cb) + 50)));
        rc.shape.setOutlineThickness(pixelRadius > 25.f ? 2.5f : 1.8f);

        // Small rocks tumble faster — angular momentum for a given impulse
        // scales inversely with moment of inertia.
        const float spinScale = std::clamp(1.6f - roll * 0.6f, 0.5f, 2.0f);
        const float randomSpin = (((rand() % 200) - 100.f) / 50.f) * spinScale;
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
        sf::Vector2f newVelocity;
        if (velocity.x != 0.f || velocity.y != 0.f) {
            newVelocity = velocity;
        }
        else {
            float speed = lua["bullet_speed"].get_or(800.0f);
            float rad = (angle - 90.f) * 3.14159f / 180.f;
            newVelocity = { std::cos(rad) * speed, std::sin(rad) * speed };
        }
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
        // ORDNANCE so the player can shoot rockets and mines out of the air.
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY | CATEGORY_ORDNANCE;
        shapeDef.enableContactEvents = true;

        b2Circle circle = { {0.0f, 0.0f}, 0.1f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Circle;
        shapeData.radius = 0.1f * SCALE;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });
        em.bullets.push_back({ entityId, lifetime, false, true });
        em.bullets.back().damage = lua["bullet_damage"].get_or(25.f);
        em.bullets.back().knockback = lua["bullet_knockback"].get_or(40.f);
        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

        RenderComponent rc;
        rc.shape.setPointCount(6);
        rc.shape.setPoint(0, { 0.f,  -16.f });  // sharp tip
        rc.shape.setPoint(1, { 2.2f,  -6.f });
        rc.shape.setPoint(2, { 1.6f,   9.f });
        rc.shape.setPoint(3, { 0.f,   14.f });  // tapered tail
        rc.shape.setPoint(4, { -1.6f,   9.f });
        rc.shape.setPoint(5, { -2.2f,  -6.f });

        rc.shape.setFillColor(sf::Color(235, 255, 255));
        rc.shape.setOutlineThickness(2.2f);
        rc.shape.setOutlineColor(sf::Color(
            col["r"].get_or(0), col["g"].get_or(255), col["b"].get_or(255), 210));
        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }

    /**
 * @brief Create an enemy ship of a given archetype.
 *
 * Both hulls come from the registry, derived from one authored silhouette:
 * the visual polygon (any point count, may be concave) drives rendering,
 * and a convex <= 8-point reduction of it drives Box2D. Nothing here
 * hardcodes a shape any more.
 */
    uint32_t createEnemy(EntityManager& em, sf::Vector2f pos, sol::state& lua,
        b2WorldId worldId,
        const enemyarch::EnemyRegistry& registry,
        uint8_t archetypeId)
    {
        const enemyarch::ArchetypeDef* defPtr = registry.byId(archetypeId);
        if (!defPtr) {
            std::cerr << "[EntityFactory] createEnemy: bad archetype id "
                << static_cast<int>(archetypeId) << ". Nothing spawned.\n";
            return 0;
        }
        const enemyarch::ArchetypeDef& def = *defPtr;
        sol::table config = def.config;

        const uint32_t entityId = em.nextEntityId++;

        TransformComponent tf;
        tf.entityId = entityId;
        tf.position = pos;
        em.transforms.push_back(tf);

        // ---- Visual ----
        //
        // rc.shape is kept populated as a fallback for any path that still
        // draws it, but the enemy branch of RenderSystem now draws the
        // triangulated hull from the registry instead. Concave silhouettes
        // cannot go through sf::ConvexShape: SFML fans from the bounding-box
        // centre, which fills in any notch deep enough to be hidden from it.
        RenderComponent rc;
        rc.shape.setPointCount(def.visual.size());
        for (size_t k = 0; k < def.visual.size(); ++k)
            rc.shape.setPoint(k, def.visual[k]);
        rc.shape.setFillColor(def.color);
        rc.shape.setOutlineThickness(1.5f);
        rc.shape.setOutlineColor(sf::Color(255, 255, 255, 150));

        // ---- Physics body ----
        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        BodyUserData* ud = new BodyUserData{ BodyType::Enemy, entityId };
        bodyDef.userData = ud;
        bodyDef.linearDamping = config["lineardrag_factor"].get_or(1.0f);
        // NOTE: the old code read "angulardgrag_factor" -- a typo that never
        // matched anything in enemy.lua, so every pirate has silently been
        // using get_or's 0.5 default instead of the configured 2.0.
        bodyDef.angularDamping = config["angulardrag_factor"].get_or(2.0f);

        const b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ENEMY;
        // CATEGORY_ORDNANCE added so rockets and mines actually TOUCH enemies.
        // Without it a parried rocket sailed straight through the squad it was
        // aimed at: the bit has to appear on BOTH shapes' masks, and the enemy
        // side was the half that was missing.
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_PLAYER |
            CATEGORY_BULLET | CATEGORY_ENEMY | CATEGORY_ORDNANCE;
        shapeDef.enableContactEvents = true;
        shapeDef.density = config["density"].get_or(4.0f);
        shapeDef.material.restitution = 0.4f;

        // Registry guarantees <= 8 points and convexity, so this cannot fail
        // the way a hand-authored hull can.
        std::vector<b2Vec2> pp;
        pp.reserve(def.physics.size());
        for (const auto& v : def.physics)
            pp.push_back({ v.x / SCALE, v.y / SCALE });

        b2Hull hull = b2ComputeHull(pp.data(), static_cast<int32_t>(pp.size()));
        b2Polygon poly = b2MakePolygon(&hull, 0.0f);
        b2CreatePolygonShape(bid, &shapeDef, &poly);

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Polygon;
        shapeData.vertices.reserve(def.physics.size());
        for (const auto& v : def.physics)
            shapeData.vertices.push_back({ v.x, v.y });
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        // ---- Components ----
        EnemyComponent ec;
        ec.entityId = entityId;
        ec.archetype = archetypeId;
        ec.fireRate = config["fire_rate"].get_or(1.8f);
        ec.attackRange = config["attack_range"].get_or(480.f);

        const float hp = config["hp"].get_or(250.f);

        em.physics.push_back({ entityId, bid });
        em.healths.push_back({ entityId, hp, hp });
        em.bullets.push_back({ entityId });
        em.enemies.push_back(ec);
        em.scoreRewards.push_back(config["score_reward"].get_or(500));
        em.players.push_back({});
        em.renders.push_back(rc);

        em.entityIdMap[entityId] = em.transforms.size() - 1;
        return entityId;
    }



    uint32_t createEnemyBullet(EntityManager& em, sf::Vector2f pos, sf::Vector2f velocity, float angle, uint32_t ownerEntityId, sol::state& lua, b2WorldId worldId, const sol::table& cfg) {
        uint32_t entityId = em.nextEntityId++;

        // Now per-archetype: was lua["enemy_config"] before
        const float speed = cfg["bullet_speed"].get_or(550.0f);
        const float lifetime = cfg["bullet_lifetime"].get_or(2.0f);
        const float damage = cfg["bullet_damage"].get_or(25.0f);

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

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Circle;
        shapeData.radius = 0.1f * SCALE;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });

        BulletComponent bc;
        bc.entityId = entityId;
        bc.lifetime = lifetime;
        bc.isActive = true;
        bc.isEnemyBullet = true;
        bc.ownerEntityId = ownerEntityId;
        bc.damage = damage;
        bc.playerIframes = cfg["bullet_iframes"].get_or(0.8f);   // 0.8 == old hardcode
        em.bullets.push_back(bc);

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



    /**
     * @brief A Maniac skid rocket.
     *
     * Deliberately NOT a faster bullet. It tracks hard for `rocket_track_time`
     * and then the steering collapses to `rocket_skid_turn` -- it keeps flying
     * where it was pointed and drifts. That split is the whole mechanic: you
     * cannot outrun the opening turn, you CAN step out of the skid, and a
     * rocket that misses is still a live fuse in the arena rather than a spent
     * one. It carries its own blast, so the hit is an area event, not a poke.
     */
    uint32_t createEnemyRocket(EntityManager& em, sf::Vector2f pos, float angle,
        uint32_t ownerEntityId, uint32_t targetEntityId, b2WorldId worldId,
        const sol::table& cfg)
    {
        uint32_t entityId = em.nextEntityId++;

        const float speed = cfg["rocket_speed"].get_or(430.f);
        const float rad = (angle - 90.f) * 3.14159f / 180.f;
        const sf::Vector2f vel = { std::cos(rad) * speed, std::sin(rad) * speed };

        em.transforms.push_back({ entityId, pos, vel, {0.f, 0.f}, angle });

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Bullet, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { vel.x / SCALE, vel.y / SCALE };
        bodyDef.rotation = b2MakeRot(angle * 3.14159f / 180.f);
        bodyDef.isBullet = true;

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ORDNANCE;
        shapeDef.filter.maskBits = CATEGORY_PLAYER | CATEGORY_ASTEROID
            | CATEGORY_ENEMY | CATEGORY_BULLET;
        shapeDef.enableContactEvents = true;

        b2Circle circle = { {0.0f, 0.0f}, 0.28f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Circle;
        shapeData.radius = 0.28f * SCALE;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });

        BulletComponent bc;
        bc.entityId = entityId;
        bc.lifetime = cfg["rocket_fuse"].get_or(3.5f);   // fuse, not just a despawn
        bc.isActive = true;
        bc.isEnemyBullet = true;
        bc.ownerEntityId = ownerEntityId;
        bc.damage = cfg["rocket_impact_damage"].get_or(6.f);  // the blast is the threat
        bc.playerIframes = cfg["rocket_iframes"].get_or(0.5f);
        bc.isRocket = true;
        bc.trackTimer = cfg["rocket_track_time"].get_or(0.85f);
        bc.skidTurnRate = cfg["rocket_skid_turn"].get_or(35.f);
        bc.blastRadius = cfg["rocket_blast_radius"].get_or(150.f);
        bc.blastDamage = cfg["rocket_blast_damage"].get_or(34.f);
        bc.armTimer = cfg["rocket_arm_time"].get_or(0.12f);
        bc.homingTargetEntityId = targetEntityId;
        bc.homingTurnRate = cfg["rocket_track_turn"].get_or(260.f);
        em.bullets.push_back(bc);

        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

        // Stubbier and wider than a round: at a glance the player must be able
        // to tell "that one is worth parrying" from "that one is chip damage".
        RenderComponent rc;
        rc.shape.setPointCount(6);
        rc.shape.setPoint(0, { 0.f,  -13.f });
        rc.shape.setPoint(1, { 5.f,   -4.f });
        rc.shape.setPoint(2, { 4.f,    7.f });
        rc.shape.setPoint(3, { 0.f,   11.f });
        rc.shape.setPoint(4, { -4.f,   7.f });
        rc.shape.setPoint(5, { -5.f,  -4.f });
        rc.shape.setFillColor(sf::Color(255, 200, 90));
        rc.shape.setOutlineThickness(2.f);
        rc.shape.setOutlineColor(sf::Color(255, 90, 20, 230));

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }

    /**
     * @brief A floating mine.
     *
     * Drifts with whatever momentum it was dropped with, arms after
     * `mine_arm_time`, then triggers on proximity and burns a fuse the player
     * can still leave. Destructible: it carries real HP and lives in the
     * ORDNANCE category, so player fire both CAN hit it and SHOULD -- shooting
     * one detonates it early, which is a tool (clear a lane, or set off the
     * one sitting next to a Rakshari) rather than a safe deletion.
     *
     * Deliberately a bullet-type entity and not an enemy: it must not count
     * toward spawn budgets, wave-clear checks, or anything the AI reasons
     * about. It is scenery with a timer.
     */
    uint32_t createMine(EntityManager& em, sf::Vector2f pos, sf::Vector2f drift,
        uint32_t ownerEntityId, b2WorldId worldId, const sol::table& cfg)
    {
        uint32_t entityId = em.nextEntityId++;

        em.transforms.push_back({ entityId, pos, drift, {0.f, 0.f},
            static_cast<float>(rand() % 360) });

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        BodyUserData* ud = new BodyUserData{ BodyType::Bullet, entityId };
        bodyDef.userData = ud;
        bodyDef.position = { pos.x / SCALE, pos.y / SCALE };
        bodyDef.linearVelocity = { drift.x / SCALE, drift.y / SCALE };
        bodyDef.angularVelocity = ((rand() % 2) ? 1.f : -1.f) * (0.6f + (rand() % 80) / 100.f);
        bodyDef.linearDamping = 1.4f;   // settles into place instead of sailing off

        b2BodyId bid = b2CreateBody(worldId, &bodyDef);

        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.filter.categoryBits = CATEGORY_ORDNANCE;
        shapeDef.filter.maskBits = CATEGORY_PLAYER | CATEGORY_ASTEROID
            | CATEGORY_ENEMY | CATEGORY_BULLET;
        shapeDef.enableContactEvents = true;
        shapeDef.density = 0.6f;

        b2Circle circle = { {0.0f, 0.0f}, 0.40f };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Circle;
        shapeData.radius = 0.40f * SCALE;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        em.physics.push_back({ entityId, bid });

        BulletComponent bc;
        bc.entityId = entityId;
        bc.lifetime = cfg["mine_lifetime"].get_or(22.f);   // eventual cleanup
        bc.isActive = true;
        bc.isEnemyBullet = true;
        bc.ownerEntityId = ownerEntityId;
        bc.damage = 0.f;                                   // the blast is all of it
        bc.isMine = true;
        bc.armTimer = cfg["mine_arm_time"].get_or(0.5f);
        bc.mineTrigger = cfg["mine_trigger_radius"].get_or(95.f);
        bc.mineFuseTime = cfg["mine_fuse"].get_or(2.0f);
        bc.blastRadius = cfg["mine_blast_radius"].get_or(130.f);
        bc.blastDamage = cfg["mine_blast_damage"].get_or(42.f);
        em.bullets.push_back(bc);

        HealthComponent hc;
        hc.entityId = entityId;
        hc.maxHp = cfg["mine_hp"].get_or(12.f);
        hc.currentHp = hc.maxHp;
        em.healths.push_back(hc);

        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});

        // Squat hexagon with spikes -- not a ship shape, not a bullet shape.
        RenderComponent rc;
        rc.shape.setPointCount(6);
        for (int k = 0; k < 6; ++k) {
            const float a = k * 3.14159f / 3.f;
            rc.shape.setPoint(k, { std::cos(a) * 9.f, std::sin(a) * 9.f });
        }
        rc.shape.setFillColor(sf::Color(70, 60, 58));
        rc.shape.setOutlineThickness(2.2f);
        rc.shape.setOutlineColor(sf::Color(255, 120, 40, 220));

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
        // ORDNANCE so the player can shoot rockets and mines out of the air.
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY | CATEGORY_ORDNANCE;
        shapeDef.enableContactEvents = true;

        float hitboxRadius = lua["rift_hitbox_radius"].get_or(0.35f);
        b2Circle circle = { {0.0f, 0.0f}, hitboxRadius };
        b2CreateCircleShape(bid, &shapeDef, &circle);

        em.physics.push_back({ entityId, bid });
        // isRiftBolt = true (7th field)
        em.bullets.push_back({ entityId, lifetime, false, true, false, 0, true });
        em.bullets.back().damage = lua["rift_direct_damage"].get_or(130.f);
        em.bullets.back().knockback = lua["rift_direct_knockback"].get_or(750.f);
        em.bullets.back().stunOnHit = lua["rift_direct_stun"].get_or(1.2f);
        em.healths.push_back({ entityId });
        em.scoreRewards.push_back({});
        em.enemies.push_back({});
        em.players.push_back({});


        PhysicsShapeData shapeData;
        shapeData.type = PhysicsShapeData::Type::Circle;
        shapeData.radius = 0.1f * SCALE;
        shapeData.offset = { 0.f, 0.f };
        em.physicsShapes.push_back(shapeData);

        // Rift bolt visual: wider diamond, purple-white core with cyan outline
        RenderComponent rc;
        rc.shape.setPointCount(6);
        rc.shape.setPoint(0, { 0.f, -20.f });
        rc.shape.setPoint(1, { 5.f,  -6.f });
        rc.shape.setPoint(2, { 3.5f,  8.f });
        rc.shape.setPoint(3, { 0.f,  17.f });
        rc.shape.setPoint(4, { -3.5f,  8.f });
        rc.shape.setPoint(5, { -5.f,  -6.f });
        rc.shape.setFillColor(sf::Color(240, 215, 255));
        rc.shape.setOutlineThickness(3.f);
        rc.shape.setOutlineColor(sf::Color(160, 60, 255, 235));

        em.renders.push_back(rc);
        em.entityIdMap[entityId] = em.transforms.size() - 1;

        return entityId;
    }
};