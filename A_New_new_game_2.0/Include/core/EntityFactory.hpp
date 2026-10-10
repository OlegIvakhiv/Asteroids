/**
 * @file EntityFactory.hpp
 * @brief Factory for creating game entities
 *
 * Responsible for creating player, asteroid, bullet, and enemy entities.
 * Uses EntityManager for storage and Box2D/Lua for configuration.
 *
 * CHANGED in 1.2 -- ship wrecks: `wreck_of` reads the live archetype,
 * damage tiers (WreckDetail.hpp), wreckTierOverride on createAsteroid.
 *
 * @author Oleg Ivakhiv
 * @version 1.2
 */

#pragma once

#include "core/EnemyArchetypes.hpp"
#include "utils/FieldObjectModels.hpp"   // lab models: rock, scrap template, unstable core
#include "utils/WreckDetail.hpp"
#include "EntityManager.hpp"
#include "utils/ShipDesign.hpp"
#include <sol/sol.hpp>
#include <optional>

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
 * MASS: Box2D would derive mass from the polygon alone, which ignores the
 * mounts. After the shape is created the body's mass data is overwritten
 * with the design's total (hull + guns + drives, at their real positions),
 * so a wide-mounted ship genuinely turns and shoves like one.
 *
 * KIT: everything the starting gear reads from the hull goes onto
 * PlayerComponent::kit. Thrust is calibrated against Lua engine_power so the
 * reference build (stock hull, auto-mounted) moves exactly as before.
 */
    uint32_t createPlayerFromDesign(EntityManager& em, sf::Vector2f pos,
        sol::state& lua, b2WorldId worldId,
        const ship::ShipDesign& design,
        const ship::Livery& livery = ship::Livery{}) {
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
        pc.kit = design.kit(lua["engine_power"].get_or(150.f));
        pc.enginePower = pc.kit.forwardForceN;

        pc.gunMountCount = 0;
        for (int g = 0; g < pc.kit.gunCount && g < 4; ++g)
            pc.gunMounts[pc.gunMountCount++] = pc.kit.gunPosPx[g];

        // The model only ships if the player authored a legal one; otherwise
        // the renderer draws the hitbox exactly as before.
        // Paint travels with the ship; the renderer reads it every frame.
        pc.livery = livery;

        if (design.decorAuthored() && design.decorCheck().ok) {
            pc.modelOutline = design.renderOutline();
            pc.modelTris = design.renderTriangles();
        }

        // Never leave the ship unable to shoot, whatever the editor produced.
        if (pc.gunMountCount == 0) {
            pc.gunMounts[0] = { 0.f, -30.f };
            pc.gunMountCount = 1;
            pc.kit.gunCount = 1;
            pc.kit.gunPosPx[0] = { 0.f, -30.f };
            pc.kit.spinalSlot = 0;
            pc.kit.riftShared = true;
            pc.kit.primarySlots[0] = 0;
            pc.kit.primaryCount = 1;
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
        // ORDNANCE belongs here too -- this is the path the refit ship uses,
        // so leaving it out meant rockets and mines ignored the actual player.
        shapeDef.filter.maskBits = CATEGORY_ASTEROID | CATEGORY_ENEMY
            | CATEGORY_ENEMY_BULLET | CATEGORY_BULLET | CATEGORY_ORDNANCE;
        shapeDef.enableContactEvents = true;
        shapeDef.density = design.tuning().density;

        b2CreatePolygonShape(bid, &shapeDef, &polygon);

        // Hull + mounts. MUST come after the last shape: adding a shape makes
        // Box2D recompute mass from shapes and silently discard this.
        // Body-level rotational inertia is about the centre of mass.
        if (pc.kit.valid) {
            b2MassData md;
            md.mass = pc.kit.massKg;
            md.center = { pc.kit.centreOfMassPx.x / SCALE, pc.kit.centreOfMassPx.y / SCALE };
            md.rotationalInertia = pc.kit.inertiaKgM2;
            b2Body_SetMassData(bid, md);
        }

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
     * @param wreckTierOverride -1 = roll from `wreck_damage`; 1 = light,
     *        2 = heavy. Tier 0 (turned off) is refused here: it belongs to
     *        dormant enemies, never to a plain wreck. Ignored by anything
     *        that is not a wreck.
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
        float sizeRollOverride = -1.f, int wreckTierOverride = -1)
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

        // Wreck state. Declared up here because the silhouette code below
        // jumps to `hull_done`, and a goto may not cross an initialisation.
        bool isWreck = false;
        float wreckHp = -1.f;              // >0: the dead ship's own HP
        wreckdetail::Source wreckSrc;
        wreckdetail::Model  wreckModel;

        // Lab model state (FieldObjectModels.hpp), for the same reason.
        fieldmodel::Model fieldModel;
        RenderComponent::FieldStyle fieldStyle = RenderComponent::FieldStyle::None;

        const float pixelRadius = finalSize * SCALE;
        int numPoints = 6;
        if (pixelRadius > 18.f) numPoints = 7;
        if (pixelRadius > 32.f) numPoints = 8;

        rc.shape.setPointCount(numPoints);

        // Jaggedness also scales: big rocks get deeper craters, small chips
        // stay compact so they don't look like torn paper.
        const float jag = config["jaggedness"].get_or(0.40f);

        // ====================================================================
        // SILHOUETTE CLASS -- what separates a rock from a wreck
        //
        // The generator above makes one shape: a jittered circle. At gameplay
        // distance jaggedness and colour barely register, so every type read
        // as "rock" no matter how it was tuned. Silhouette is what the eye
        // actually sorts on, and silhouette is ASPECT RATIO and REGULARITY.
        //
        //   elongation   1.0 = round rock. 2.5 = a long ship section. The
        //                stretch axis is random per entity, so an elongated
        //                type still fills the screen with varied angles.
        //   angle_jitter 1.0 = organic (the original). 0.0 = evenly spaced
        //                vertices, which reads as manufactured plate rather
        //                than broken stone -- the difference between a rock
        //                and a hull panel.
        //
        // Both default to the old behaviour, so every existing asteroid type
        // is untouched by this.
        //
        // Note the stretch happens BEFORE the hull is built, so the physics
        // shape is elongated too: a long wreck collides like a long wreck.
        // ====================================================================
        // ====================================================================
        // AUTHORED HULL -- a wreck instead of a rock
        //
        // `hull_points` swaps the procedural jitter below for a real ship
        // silhouette, scaled so its longest radius matches the rolled size.
        // That is what lets a dead Raider drift through the field looking
        // like a dead Raider rather than like a lumpy stone.
        //
        // `wreck_of = "BARGE"` goes one better: the hull, plates, nozzles,
        // turret mounts, scars AND HP are read from the LIVE archetype in
        // enemy.lua, so the wreck can never drift out of sync with the ship
        // it used to be. An explicit `hull_points` (or `plates`, `thrusters`,
        // `turrets`, `scars`) on the asteroid type still wins, per field.
        //
        // `wreck_of_class = "LIGHT"` is the same idea for the player's three
        // stock hulls: outline, engine positions and HP come from
        // ship::ShipDesign::preset(), the hull the refit bay starts you on.
        //
        // HP. A wreck has the HP of the ship it was -- a dead Barge soaks
        // what a live Barge soaks. Not scaled by the size roll either: the
        // roll is +-8% of drift variety, not a different ship.
        //
        // Three things have to be kept apart here:
        //   VISUAL   the full concave outline, triangulated into rc.tris
        //   OUTLINE  rc.shape, which SFML needs convex -- so it gets the hull
        //   PHYSICS  the same convex hull, decimated to Box2D's 8-point cap
        // Ships already make exactly this split (visual / physics); this is
        // the same trick one layer down.
        //
        // WRECK TIERS. A wreck type (`wreck_of` or `wreck_damage` set) rolls
        // a damage tier and is BITTEN before any of that split happens, so
        // the torn silhouette is what gets drawn, what the physics hull is
        // built from, and what the fracture path later cuts into wedges.
        // See WreckDetail.hpp.
        // ====================================================================
        {
            sol::state_view sv(config.lua_state());

            sol::optional<sol::table> arch;
            const std::string wreckOf = config["wreck_of"].get_or<std::string>("");
            if (!wreckOf.empty()) {
                sol::optional<sol::table> ea = sv["enemy_archetypes"];
                if (ea) arch = (*ea)[wreckOf].get<sol::optional<sol::table>>();
                if (!arch)
                    std::cerr << "[EntityFactory] wreck_of = \"" << wreckOf
                    << "\" -- no such enemy archetype. Using hull_points if set, else a plain rock.\n";
            }
            const std::optional<ship::ShipDesign> stock =
                stockDesignFor(config["wreck_of_class"].get_or<std::string>(""));

            sol::optional<sol::table> wreckDamage = config["wreck_damage"];
            isWreck = arch.has_value() || stock.has_value() || wreckDamage.has_value();

            if (arch)       wreckHp = (*arch)["hp"].get_or(250.f);
            else if (stock) wreckHp = stock->stats().hpMax;

            std::vector<sf::Vector2f> pts;
            if (sol::optional<sol::table> hp = config["hull_points"]) pts = readLuaPoints(*hp);
            else if (arch) {
                if (sol::optional<sol::table> ah = (*arch)["hull"]) pts = readLuaPoints(*ah);
            }
            else if (stock) pts = stock->renderOutline();

            if (pts.size() >= 3) {
                float maxR = 0.f;
                for (const auto& q : pts) maxR = std::max(maxR, std::sqrt(q.x * q.x + q.y * q.y));

                if (pts.size() >= 3 && maxR > 0.01f) {
                    const float k = pixelRadius / maxR;
                    for (auto& q : pts) { q.x *= k; q.y *= k; }

                    if (enemyarch::geom::signedArea(pts) < 0.f)
                        std::reverse(pts.begin(), pts.end());

                    if (isWreck) {
                        wreckSrc = readWreckSource(config, arch, pts, k);
                        if (stock && wreckSrc.thrusters.empty())
                            wreckSrc.thrusters = stockNozzles(*stock, k);
                        const int tier = rollWreckTier(sv, wreckDamage, wreckTierOverride);
                        const uint32_t seed = (static_cast<uint32_t>(rand()) * 2654435761u)
                            ^ (entityId * 40503u) ^ 0x9E3779B9u;
                        wreckModel = wreckdetail::build(wreckSrc, tier, seed);
                        if (wreckModel.hull.size() >= 3) pts = wreckModel.hull;
                    }

                    rc.tris = enemyarch::geom::triangulate(pts);

                    std::vector<sf::Vector2f> convex = enemyarch::geom::decimateConvex(
                        enemyarch::geom::convexHull(pts), 8);

                    // The SHAPE keeps the real silhouette, not the convex
                    // hull. It previously held the hull, whose outline traced
                    // a blunt shell around the ship and read as a debug
                    // hitbox rather than as a wreck. Only PHYSICS needs the
                    // convex version.
                    rc.shape.setPointCount(pts.size());
                    for (size_t i = 0; i < pts.size(); ++i)
                        rc.shape.setPoint(static_cast<unsigned>(i), pts[i]);
                    for (const auto& c : convex)
                        physicsPoints.push_back({ c.x / SCALE, c.y / SCALE });
                    goto hull_done;   // skip the procedural silhouette below
                }
            }
            isWreck = false;   // opted in, but nothing to build it from
            wreckHp = -1.f;
        }

        // ====================================================================
        // LAB MODELS -- `detail = "rock" | "cluster" | "unstable_core"`
        //
        // The designed field objects from the "Cold Field Objects" page. Each
        // one builds its OWN silhouette, so it has to happen here, before the
        // physics body is made: the collider is the convex hull of what you
        // actually see.
        //
        // (The previous cluster path rebuilt the silhouette AFTER the Box2D
        // shape had already been created from the jittered circle, so scrap
        // piles collided -- and fractured -- as round rocks. Building first
        // fixes that for free.)
        //
        // A type with no `detail`, like MAGMATIC, never enters this block and
        // keeps the generator below exactly as it was.
        // ====================================================================
        {
            const std::string detail = config["detail"].get_or<std::string>("");
            const uint32_t fseed = (static_cast<uint32_t>(rand()) * 2654435761u)
                ^ (entityId * 2246822519u) ^ 0x85EBCA6Bu;

            if (detail == "rock") {
                fieldmodel::RockParams rp;
                rp.designR = config["model_r"].get_or(46.f);
                rp.verts = config["rock_verts"].get_or(12);
                rp.crevices = config["rock_crevices"].get_or(5);
                rp.facets = config["rock_facets"].get_or(9);
                rp.fractures = config["rock_fractures"].get_or(6);
                rp.oreChance = config["rock_ore_chance"].get_or(0.45f);
                fieldModel = fieldmodel::buildAsteroid(pixelRadius, rp, fseed);
                fieldStyle = RenderComponent::FieldStyle::Rock;
            }
            else if (detail == "cluster") {
                const std::string key = config["scrap_template"].get_or<std::string>("junk_m");
                const fieldmodel::Template tpl = fieldmodel::readTemplate(
                    sol::state_view(config.lua_state()), key);
                fieldModel = fieldmodel::buildScrap(tpl, pixelRadius, fseed,
                    (rand() % 100000) / 100.f);
                fieldStyle = RenderComponent::FieldStyle::Scrap;
            }
            else if (detail == "unstable_core") {
                fieldModel = fieldmodel::buildUnstableCore(pixelRadius, fseed);
                fieldStyle = RenderComponent::FieldStyle::UnstableCore;
            }

            if (fieldStyle != RenderComponent::FieldStyle::None) {
                if (fieldModel.ok()) {
                    rc.shape.setPointCount(fieldModel.outline.size());
                    for (size_t i = 0; i < fieldModel.outline.size(); ++i)
                        rc.shape.setPoint(static_cast<unsigned>(i), fieldModel.outline[i]);
                    for (const auto& c : fieldModel.physics)
                        physicsPoints.push_back({ c.x / SCALE, c.y / SCALE });
                    goto hull_done;
                }
                std::cerr << "[EntityFactory] detail = \"" << detail
                    << "\" built nothing usable. Falling back to a plain rock.\n";
                fieldStyle = RenderComponent::FieldStyle::None;
            }
        }

        // ====================================================================
        // CLUMP -- a pile of junk that got stuck together
        //
        // The ordinary generator jitters ONE circle, which always reads as a
        // rock no matter how the roughness is tuned: one closed convex-ish
        // blob is what stone looks like. A salvage pile is not one object, it
        // is several fused, and the giveaway is the SEAM -- the concave valley
        // where two lumps meet.
        //
        // So the silhouette is the union of a few offset discs. Sampling the
        // outer envelope of that union gives bumps where a disc sticks out and
        // gaps where two of them meet, which is exactly the lumpy read. More
        // lobes on the bigger types, so size shows in the shape as well as in
        // the colour.
        //
        // Visual keeps the full concave outline via rc.tris; PHYSICS takes the
        // convex hull, capped at Box2D's 8 points -- the same split ship-hull
        // wrecks already use, so a clump costs nothing new.
        // ====================================================================
        {
            int lobes = config["clump_lobes"].get_or(0);
            if (lobes >= 2) {
                const auto frand = []() { return (rand() % 1000) / 1000.f; };

                lobes = std::clamp(lobes + (rand() % 3 - 1), 2, 6);
                const float spread = config["clump_spread"].get_or(0.66f);
                const float lobeVar = config["clump_lobe_var"].get_or(0.35f);
                // Lobe radius and core radius are explicit, because they ARE
                // the seam depth: lobes wide enough to swallow the valleys
                // give a rock again, and a fat core fills them from inside.
                const float lobeR = config["clump_lobe_r"].get_or(0.46f);
                const float coreR = config["clump_core"].get_or(0.36f);
                const float rough = config["clump_roughness"].get_or(0.10f);

                struct Lobe { float x, y, r; };
                std::vector<Lobe> L;
                L.reserve(static_cast<size_t>(lobes) + 1);
                for (int i = 0; i < lobes; ++i) {
                    const float a = (i / static_cast<float>(lobes)) * 6.28318f
                        + (frand() - 0.5f) * (5.2f / lobes);
                    const float d = pixelRadius * spread * (0.70f + frand() * 0.6f);
                    const float rr = pixelRadius * lobeR
                        * (1.f + (frand() * 2.f - 1.f) * lobeVar);
                    L.push_back({ std::cos(a) * d, std::sin(a) * d, std::max(3.f, rr) });
                }
                // A core lobe, so the middle is never hollow and the union
                // stays a single connected shape.
                L.push_back({ 0.f, 0.f, pixelRadius * coreR });

                // 20 samples: enough for a seam to be visible as a notch
                // rather than as a single flat cut.
                const int N = 20;
                std::vector<sf::Vector2f> pts;
                pts.reserve(N);
                float maxR = 0.001f;
                for (int k = 0; k < N; ++k) {
                    const float th = (k / static_cast<float>(N)) * 6.28318f;
                    const float cx = std::cos(th), cy = std::sin(th);

                    // Distance from the origin to the far side of each disc
                    // along this ray; the envelope is the largest of them.
                    float best = 0.f;
                    for (const auto& lo : L) {
                        const float proj = lo.x * cx + lo.y * cy;
                        const float perp2 = (lo.x * lo.x + lo.y * lo.y) - proj * proj;
                        const float disc = lo.r * lo.r - perp2;
                        if (disc <= 0.f) continue;          // ray misses this lobe
                        best = std::max(best, proj + std::sqrt(disc));
                    }
                    if (best < pixelRadius * 0.18f) best = pixelRadius * 0.18f;
                    best *= 1.f + (frand() * 2.f - 1.f) * rough;
                    maxR = std::max(maxR, best);
                    pts.push_back({ cx * best, cy * best });
                }

                // Normalise so the rolled size still means what it says --
                // the union can overshoot pixelRadius, and the size bands are
                // what tell small salvage from medium.
                const float k = pixelRadius / maxR;
                for (auto& q : pts) { q.x *= k; q.y *= k; }

                if (enemyarch::geom::signedArea(pts) < 0.f)
                    std::reverse(pts.begin(), pts.end());
                rc.tris = enemyarch::geom::triangulate(pts);

                rc.shape.setPointCount(pts.size());
                for (size_t i = 0; i < pts.size(); ++i)
                    rc.shape.setPoint(static_cast<unsigned>(i), pts[i]);

                for (const auto& c : enemyarch::geom::decimateConvex(
                    enemyarch::geom::convexHull(pts), 8))
                    physicsPoints.push_back({ c.x / SCALE, c.y / SCALE });

                goto hull_done;
            }
        }

        {
            const float elong = std::max(0.2f, config["elongation"].get_or(1.0f));
            const float jitterScale = std::clamp(config["angle_jitter"].get_or(1.0f), 0.f, 2.f);
            const float stretchAngle = ((rand() % 360) / 180.f) * 3.14159f;
            const float sCos = std::cos(stretchAngle), sSin = std::sin(stretchAngle);

            // Area is held roughly constant while stretching, so an elongated type
            // does not also become a heavier type by accident -- density and mass
            // come from the hull Box2D computes from these points.
            const float ex = std::sqrt(elong);
            const float ey = 1.f / ex;

            for (int i = 0; i < numPoints; ++i) {
                // Jitter the ANGLE as well as the radius. Evenly-spaced angles
                // give a regular polygon no matter how much you vary the radius,
                // which is why the old rocks still read as circles.
                const float baseAngle = (i / (float)numPoints) * 2.f * 3.14159f;
                const float angleJit = ((rand() % 100) / 100.f - 0.5f)
                    * (1.2f / numPoints) * jitterScale;
                const float angle = baseAngle + angleJit;

                const float noise = (rand() % 100) / 100.f;
                const float dist = pixelRadius * (1.f - jag * 0.5f + noise * jag);

                // Stretch along a random axis: rotate in, scale, rotate back.
                const float ux = std::cos(angle) * dist;
                const float uy = std::sin(angle) * dist;
                const float lx = (ux * sCos + uy * sSin) * ex;
                const float ly = (-ux * sSin + uy * sCos) * ey;
                const float px = lx * sCos - ly * sSin;
                const float py = lx * sSin + ly * sCos;

                rc.shape.setPoint(i, { px, py });
                physicsPoints.push_back({ px / SCALE, py / SCALE });
            }
        }
    hull_done:

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
        // A wreck with a ship behind it takes that ship's HP, flat. Everything
        // else keeps the old rule.
        const float baseHp = (wreckHp > 0.f) ? wreckHp : config["hp"].get_or(20.0f);
        const float hpFromSize = (wreckHp > 0.f) ? 0.f : config["hp_follows_size"].get_or(1.0f);

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

        // Optional: what this rock breaks into. Truncated rather than
        // rejected if someone writes a 20-character type name -- the lookup
        // then fails loudly in FractureImpl instead of corrupting memory here.
        {
            em.healths.back().childInheritColor = config["child_inherit_color"].get_or(false);
            em.healths.back().metallic = config["metallic"].get_or(false);

            // Scrap on destruction: `scrap_drop = { min, max }`, or a single
            // number for a fixed amount. Unset = nothing -- plain rock is
            // rock. Salvage types set it.
            {
                int lo = 0, hi = 0;
                sol::object sd = config["scrap_drop"];
                if (sd.is<sol::table>()) {
                    sol::table t = sd.as<sol::table>();
                    lo = t[1].get_or(0);
                    hi = t[2].get_or(lo);
                }
                else if (sd.is<double>()) {
                    lo = hi = static_cast<int>(sd.as<double>());
                }
                lo = std::clamp(lo, 0, 9999);
                hi = std::clamp(hi, lo, 9999);
                em.healths.back().scrapMin = static_cast<uint16_t>(lo);
                em.healths.back().scrapMax = static_cast<uint16_t>(hi);
            }

            // Per-type core, falling back to the global visual so nothing
            // that relied on `magma_core_size` changes behaviour.
            {
                // createAsteroid only receives the config table, so the
                // global fallback is reached through that table's own state.
                sol::state_view sv(config.lua_state());
                sol::optional<sol::table> av = sv["asteroid_visuals"];
                const float globalCore = av ? (*av)["magma_core_size"].get_or(0.f) : 0.f;
                em.healths.back().magmaCore = std::max(0.f,
                    config["core_size"].get_or(globalCore));
            }

            const std::string ca = config["child_alt"].get_or<std::string>("");
            auto& adst = em.healths.back().childAlt;
            const size_t an = std::min(ca.size(), sizeof(adst) - 1);
            std::memcpy(adst, ca.data(), an);
            adst[an] = '\0';
            em.healths.back().childAltPct = static_cast<uint8_t>(
                std::clamp(config["child_alt_chance"].get_or(0.f), 0.f, 100.f));
            em.healths.back().burstChildren = static_cast<uint8_t>(
                std::clamp(config["burst_children"].get_or(0), 0, 8));

            const std::string ct = config["child_type"].get_or<std::string>("");
            auto& dst = em.healths.back().childType;
            const size_t n = std::min(ct.size(), sizeof(dst) - 1);
            std::memcpy(dst, ct.data(), n);
            dst[n] = '\0';

            // CODEX: which bestiary entry this object opens when seen/killed.
            const std::string ck = config["codex"].get_or<std::string>("");
            auto& kdst = em.healths.back().codexKey;
            const size_t kn = std::min(ck.size(), sizeof(kdst) - 1);
            std::memcpy(kdst, ck.data(), kn);
            kdst[kn] = '\0';
        }

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
        int cr = col["r"].get_or(60);
        int cg = col["g"].get_or(55);
        int cb = col["b"].get_or(50);

        // Bigger rocks read slightly darker and denser; chips catch more light.
        // Not wrecks: their `color` IS the cold hull colour from the design
        // lab, and the tier ramp in WreckDetail already darkens it -- lifting
        // it again would bury a Barge in the background.
        const float lift = isWreck ? 1.f
            : (1.15f - 0.35f * std::clamp(pixelRadius / 50.f, 0.f, 1.f));
        const int jitter = (rand() % 22) - 8;

        auto ch = [&](int base) {
            return static_cast<uint8_t>(std::clamp(
                static_cast<int>(base * lift) + jitter, 0, 255));
            };

        // Optional second palette, picked per rock. One salvage field should
        // hold both rusted iron and bare steel; without this every piece of a
        // given type came out the same hue and the field read as one material.
        {
            sol::optional<sol::table> alt = config["color_alt"];
            if (alt && (rand() % 100) < 50) {
                cr = (*alt)["r"].get_or(static_cast<int>(cr));
                cg = (*alt)["g"].get_or(static_cast<int>(cg));
                cb = (*alt)["b"].get_or(static_cast<int>(cb));
            }
        }

        rc.shape.setFillColor(sf::Color(ch(cr), ch(cg), ch(cb)));
        rc.shape.setOutlineColor(sf::Color(
            std::min(255, ch(cr) + 45),
            std::min(255, ch(cg) + 45),
            std::min(255, ch(cb) + 50)));
        rc.shape.setOutlineThickness(pixelRadius > 25.f ? 2.5f : 1.8f);

        // ====================================================================
        // LAB MODEL -- install what was built before the physics body
        //
        // The model's triangles are already painter-ordered and coloured, so
        // they go straight onto the existing detail path (world transform and
        // hit flash for free). The per-frame layer -- ore flicker, rivet
        // shimmer, glints, the core's pulse -- is keyed off fieldStyle.
        //
        // `shape` keeps the silhouette and takes the model's DEBRIS colour as
        // its fill: that is what the fracture path cuts shards from. Its own
        // stroke is off; the model draws its edge itself.
        // ====================================================================
        if (fieldStyle != RenderComponent::FieldStyle::None) {
            rc.detailTris = std::move(fieldModel.tris);
            rc.detailLines.clear();
            rc.tris.clear();
            rc.shape.setFillColor(fieldModel.debris);
            rc.shape.setOutlineThickness(0.f);
            rc.fieldStyle = fieldStyle;
            rc.fieldFx.clear();
            for (const auto& f : fieldModel.fx)
                rc.fieldFx.push_back({ f.p, f.r, f.phase });
            rc.glintSpots = std::move(fieldModel.glintSpots);
            rc.fieldGlowR = fieldModel.glowRadius;
            rc.fieldPhase = fieldModel.phase;
            rc.glintTimer = 1.5f + (rand() % 300) / 100.f;   // the lab's first-glint delay
        }

        // ====================================================================
        // WRECK DETAIL -- armour, soot, breaches, pits, scars, peeled plates,
        // dead nozzles and turrets, and the outline. One baked triangle list
        // on the existing detail path, so the world transform and the hit
        // flash come for free. Flat, like everything else in the field: no
        // gradients, no lighting (see WreckDetail.hpp).
        //
        // The outline is the SAME colour and width as every rock's stroke,
        // just baked -- `shape`'s own stroke is switched off because SFML
        // mitres the torn corners into spikes.
        //
        // `shape` keeps the cold colour as its fill: that is what the
        // fracture path reads back, so scrap from a rust Barge is rust.
        // ====================================================================
        if (isWreck) {
            rc.detailTris = wreckdetail::bake(wreckSrc, wreckModel, rc.shape.getFillColor(),
                rc.shape.getOutlineColor(), rc.shape.getOutlineThickness());
            rc.detailLines.clear();
            rc.shape.setOutlineThickness(0.f);
            rc.wreckTier = static_cast<int8_t>(wreckModel.tier);
        }

        // Hull-shaped objects DO get a stroke. They lost it when the outline
        // was still being traced around the convex HULL, which drew a blunt
        // shell that read as a debug collider. The shape now holds the true
        // silhouette, so the stroke follows the real edge -- and without it a
        // dark wreck on a near-black background has nothing to catch the eye.
        // `hull_outline = false` opts a type out.
        if (!rc.tris.empty() && !config["hull_outline"].get_or(true))
            rc.shape.setOutlineThickness(0.f);

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
 *
 * @param dormant  Spawn TURNED OFF -- the ambush. The unit drifts like a
 *        wreck, is drawn as its own tier-0 hull (same recipe, colour and
 *        hardware as that ship's wreck), and has no AI until AISystem wakes
 *        it. Physics and hitbox are the live ship's, unchanged: a shot that
 *        would hit the ship hits the ambusher.
 */
    uint32_t createEnemy(EntityManager& em, sf::Vector2f pos, sol::state& lua,
        b2WorldId worldId,
        const enemyarch::EnemyRegistry& registry,
        uint8_t archetypeId,
        bool dormant = false)
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

        // ---- Ambush: the powered-down disguise ----
        //
        // Built by the SAME calls a wreck of this ship is built with --
        // readWreckSource over the matching wreck type, then build() at tier
        // 0 and bake() -- so the disguise cannot drift away from the real
        // thing. The only way to tell it apart is that it is undamaged:
        // wrecks never roll tier 0, so a clean dark hull is always this.
        float spawnRotDeg = 0.f;
        b2Vec2 driftVel{ 0.f, 0.f };
        float driftSpin = 0.f;
        if (dormant) {
            sol::state_view sv(lua);
            const sol::optional<sol::table> wtype = wreckTypeFor(sv, def.key);
            const float unit = config["scale"].get_or(1.0f);
            const wreckdetail::Source src = readWreckSource(
                wtype ? *wtype : config, sol::optional<sol::table>(config), def.visual, unit);
            const uint32_t seed = (static_cast<uint32_t>(rand()) * 2654435761u) ^ (entityId * 40503u);
            const wreckdetail::Model model = wreckdetail::build(src, 0, seed);

            const sf::Color cold = coldColorFor(config, wtype, def.color);
            const sf::Color outline(
                static_cast<uint8_t>(std::min(255, cold.r + 45)),
                static_cast<uint8_t>(std::min(255, cold.g + 45)),
                static_cast<uint8_t>(std::min(255, cold.b + 50)));
            rc.detailTris = wreckdetail::bake(src, model, cold, outline,
                def.radius > 25.f ? 2.5f : 1.8f);
            rc.wreckTier = 0;

            // Adrift like the wrecks around it: random heading, slow drift,
            // slow tumble. A ship holding perfectly still in a field where
            // everything else moves is its own tell.
            const auto frand = []() { return (rand() % 1000) / 1000.f; };
            spawnRotDeg = frand() * 360.f;
            float vmin = 0.6f, vmax = 1.8f;
            if (sol::optional<sol::table> ds = config["ambush_drift_speed"]) {
                vmin = (*ds)[1].get_or(vmin);
                vmax = (*ds)[2].get_or(vmax);
            }
            const float sp = vmin + (vmax - vmin) * frand();
            const float da = frand() * 6.2831853f;
            driftVel = { std::cos(da) * sp, std::sin(da) * sp };
            driftSpin = (frand() * 2.f - 1.f) * config["ambush_spin"].get_or(0.35f);
        }

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

        // A dormant hull is an object: no drag, so it keeps drifting the way
        // a wreck does. AISystem puts the configured drag back on waking.
        if (dormant) {
            bodyDef.linearDamping = 0.f;
            bodyDef.angularDamping = 0.05f;
            bodyDef.rotation = b2MakeRot(spawnRotDeg * 3.14159265f / 180.f);
            bodyDef.linearVelocity = driftVel;
            bodyDef.angularVelocity = driftSpin;
            em.transforms.back().rotation = spawnRotDeg;
        }

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
        ec.dormant = dormant;

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
        bc.poiseMult = cfg["bullet_poise_mult"].get_or(1.f);      // Wardog chaff: 0.1
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
        const sol::table& cfg, float speedMult = 1.f)
    {
        uint32_t entityId = em.nextEntityId++;

        const float speed = cfg["rocket_speed"].get_or(430.f) * speedMult;
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
        bc.wildDrag = cfg["parry_rocket_drag"].get_or(0.75f);
        bc.wildStallSpeed = cfg["parry_rocket_stall"].get_or(170.f);
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
        // Live rockets sit in the ORANGE/RED band -- the enemy-threat colour.
        // A parried one turns YELLOW, matching a parried bullet, so "this is
        // mine now" is the same visual promise everywhere in the game.
        rc.shape.setFillColor(sf::Color(225, 95, 35));
        rc.shape.setOutlineThickness(2.f);
        rc.shape.setOutlineColor(sf::Color(255, 60, 20, 235));

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

        // No health, on purpose. A mine is not a thing you whittle down: any
        // damaging contact lights the fuse, full stop. Giving it HP meant
        // player rounds visibly bounced off it while it sat there unharmed,
        // which told the player the wrong thing about what shooting it does.
        em.healths.push_back({ entityId });

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
        rc.shape.setFillColor(sf::Color(64, 58, 56));
        rc.shape.setOutlineThickness(2.4f);
        rc.shape.setOutlineColor(sf::Color(150, 150, 140, 220));

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

private:
    // ========================================================================
    // WRECK HELPERS
    // ========================================================================

    /// `{ {x,y}, ... }` or `{ {x=..,y=..}, ... }` -- archetype turrets use the
    /// second form, everything else the first.
    static std::vector<sf::Vector2f> readLuaPoints(const sol::table& arr) {
        std::vector<sf::Vector2f> out;
        for (size_t i = 1; i <= arr.size(); ++i) {
            sol::optional<sol::table> v = arr[i];
            if (!v) continue;
            const float x = (*v)["x"].valid() ? (*v)["x"].get_or(0.f) : (*v)[1].get_or(0.f);
            const float y = (*v)["y"].valid() ? (*v)["y"].get_or(0.f) : (*v)[2].get_or(0.f);
            out.push_back({ x, y });
        }
        return out;
    }

    /// Everything authored about the dead ship, scaled by `k` into the same
    /// pixel space as the hull. Plates mirror exactly as the live archetype
    /// does: authored starboard-only unless `mirror = false`.
    static wreckdetail::Source readWreckSource(const sol::table& config,
        const sol::optional<sol::table>& arch, const std::vector<sf::Vector2f>& hullPx, float k)
    {
        // Per field: the asteroid type first, then the live archetype.
        const auto field = [&](const char* key) -> sol::optional<sol::table> {
            sol::optional<sol::table> own = config[key];
            if (own) return own;
            if (arch) return (*arch)[key].get<sol::optional<sol::table>>();
            return sol::nullopt;
            };

        wreckdetail::Source src;
        src.hull = hullPx;
        src.unit = k;

        if (auto plates = field("plates")) {
            for (size_t i = 1; i <= plates->size(); ++i) {
                sol::optional<sol::table> e = (*plates)[i];
                if (!e) continue;
                sol::optional<sol::table> pts = (*e)["points"];
                if (!pts) continue;
                wreckdetail::Plate pl;
                pl.shade = (*e)["shade"].get_or(1.35f);
                pl.accent = (*e)["accent"].get_or(false);
                pl.pts = readLuaPoints(*pts);
                if (pl.pts.size() < 3) continue;
                for (auto& v : pl.pts) { v.x *= k; v.y *= k; }
                src.plates.push_back(pl);
                if ((*e)["mirror"].get_or(true)) {
                    for (auto& v : pl.pts) v.x = -v.x;
                    src.plates.push_back(std::move(pl));
                }
            }
        }
        // No thrusters from here. An archetype's `thrusters` are flame
        // emitters, not hardware -- the live hull draws nothing there -- so
        // drawing nozzle housings on its wreck or its dormant disguise gave
        // both away. Player stock hulls get theirs from stockNozzles().
        if (auto t = field("turrets")) {
            src.turrets = readLuaPoints(*t);
            for (auto& v : src.turrets) { v.x *= k; v.y *= k; }
        }
        // The live turret's size, carried to this scale: k / the archetype's
        // own scale. A dormant hull (k == scale) gets exactly turret_size.
        {
            const float archScale = arch ? (*arch)["scale"].get_or(1.f) : config["scale"].get_or(1.f);
            const float tsz = config["turret_size"].get_or(
                arch ? (*arch)["turret_size"].get_or(10.f) : 10.f);
            src.turretSize = tsz * k / std::max(0.01f, archScale);
        }
        if (auto scars = field("scars")) {
            for (size_t i = 1; i <= scars->size(); ++i) {
                sol::optional<sol::table> line = (*scars)[i];
                if (!line) continue;
                auto pts = readLuaPoints(*line);
                for (auto& v : pts) { v.x *= k; v.y *= k; }
                if (pts.size() >= 2) src.scars.push_back(std::move(pts));
            }
        }
        // A number, not a table, so it gets its own fallback chain. Reading
        // only the asteroid config would silently hand every Berserker the
        // 1.6 default instead of its authored width.
        src.scarWidth = config["scar_width"].get_or(
            arch ? (*arch)["scar_width"].get_or(1.6f) : 1.6f);
        return src;
    }

    /// `wreck_of_class = "LIGHT" | "MEDIUM" | "HEAVY"` -> that class's
    /// stock preset. Anything else -> nothing.
    static std::optional<ship::ShipDesign> stockDesignFor(const std::string& cls) {
        if (cls.empty()) return std::nullopt;
        if (cls == "LIGHT")  return ship::ShipDesign::preset(ship::HullClass::Light);
        if (cls == "MEDIUM") return ship::ShipDesign::preset(ship::HullClass::Medium);
        if (cls == "HEAVY")  return ship::ShipDesign::preset(ship::HullClass::Heavy);
        std::cerr << "[EntityFactory] wreck_of_class = \"" << cls
            << "\" -- expected LIGHT, MEDIUM or HEAVY. Ignored.\n";
        return std::nullopt;
    }

    /// Dead nozzles where the preset mounts its drives. Mounts sit on the
    /// hitbox edge; they are pulled a few units inside so the housing reads
    /// as part of the hull rather than hanging off it.
    static std::vector<sf::Vector2f> stockNozzles(const ship::ShipDesign& d, float k) {
        std::vector<sf::Vector2f> out;
        for (int idx : d.mountedEngines())
            for (const auto& slot : d.engineSlots())
                if (slot.index == idx) {
                    out.push_back({ (slot.position.x - slot.outward.x * 3.5f) * k,
                                    (slot.position.y - slot.outward.y * 3.5f) * k });
                    break;
                }
        return out;
    }

    /// Cold hull colour for a dormant unit: `ambush_cold_color` on the
    /// archetype if set, else its wreck type's `color`, else the live colour
    /// drained to a cold grey-brown.
    static sf::Color coldColorFor(const sol::table& arch, const sol::optional<sol::table>& wreckType,
        sf::Color live)
    {
        const auto read = [](const sol::table& t, sf::Color fb) {
            return sf::Color(
                static_cast<uint8_t>(std::clamp(t["r"].get_or(static_cast<float>(fb.r)), 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(t["g"].get_or(static_cast<float>(fb.g)), 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(t["b"].get_or(static_cast<float>(fb.b)), 0.f, 255.f)));
            };
        const sf::Color drained(
            static_cast<uint8_t>(live.r * 0.22f + 44.f),
            static_cast<uint8_t>(live.g * 0.22f + 42.f),
            static_cast<uint8_t>(live.b * 0.22f + 42.f));
        if (sol::optional<sol::table> c = arch["ambush_cold_color"]) return read(*c, drained);
        if (wreckType)
            if (sol::optional<sol::table> c = (*wreckType)["color"]) return read(*c, drained);
        return drained;
    }

    /// The asteroid type whose `wreck_of` names this archetype, if any.
    /// A dormant ambusher borrows its look from here -- colour, nozzle
    /// overrides -- so it is exactly what that ship's wreck would be at
    /// tier 0, and nothing about it says "different object".
    static sol::optional<sol::table> wreckTypeFor(sol::state_view sv, const std::string& archKey) {
        sol::optional<sol::table> types = sv["asteroid_types"];
        if (!types) return sol::nullopt;
        for (const auto& kv : *types) {
            if (!kv.second.is<sol::table>()) continue;
            sol::table t = kv.second.as<sol::table>();
            if (t["wreck_of"].get_or<std::string>("") == archKey) return t;
        }
        return sol::nullopt;
    }

    /**
     * @brief Which damage tier a new wreck rolls: 1 (light) or 2 (heavy).
     *
     * NEVER 0. The pristine turned-off hull is the ambusher's disguise, so a
     * plain wreck must not wear it -- otherwise a clean dark hull would mean
     * "maybe a trap" instead of "a trap". Every path here clamps to 1..2,
     * the QA switch included.
     *
     * Priority: the caller's override, then `asteroid_visuals.wreck_force_tier`
     * (set it and F5 to see every new wreck at one tier), then a weighted
     * roll over `wreck_damage = { light = w1, heavy = w2 }`.
     */
    static int rollWreckTier(sol::state_view sv, const sol::optional<sol::table>& weights,
        int overrideTier)
    {
        if (overrideTier >= 0) return std::clamp(overrideTier, 1, wreckdetail::MAX_TIER);

        sol::optional<sol::table> av = sv["asteroid_visuals"];
        const int forced = av ? (*av)["wreck_force_tier"].get_or(-1) : -1;
        if (forced >= 0) return std::clamp(forced, 1, wreckdetail::MAX_TIER);

        float light = 3.f, heavy = 4.f;
        if (weights) {
            light = std::max(0.f, (*weights)["light"].get_or(light));
            heavy = std::max(0.f, (*weights)["heavy"].get_or(heavy));
        }
        if (light + heavy <= 0.f) return 2;
        return ((rand() % 10000) / 10000.f * (light + heavy) < light) ? 1 : 2;
    }
};
