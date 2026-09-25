-- ============================================================================
-- asteroids.lua
-- Asteroid configuration for the Modular Space Engine
-- 
-- Defines three asteroid sizes (SMALL, MEDIUM, LARGE) with progressive stats.
-- Larger asteroids are slower, tougher, and worth more points.
-- Spawn settings control generation frequency and limits.
-- 
-- @author Oleg Ivakhiv
-- @version 1.1
-- ============================================================================

-- ============================================================================
-- ASTEROID TYPE DEFINITIONS
-- ============================================================================

asteroid_types = {
    -- ========================================================================
    -- SMALL ASTEROID
    -- ========================================================================
    -- Fragments from destroyed medium asteroids.
    -- Fast, fragile, low point value.
    -- ========================================================================
    SMALL = {
        color = { r = 100, g = 100, b = 110 },     -- Light gray-blue tint
        base_size = 0.3,                           -- Radius in meters (9 pixels at 30px/m)
        density = 2.0,                             -- Low density = lightweight
        hp = 10,                                   -- One shot kill from player weapons
        score_reward = 10,                         -- Minimal points
        speed_range = { 7.0, 9.0 },                -- Fast movement (meters/sec)
        ase_size = 0.30,
        size_variance_min = 0.80,   -- 0.24m ( 7px)
        size_variance_max = 1.40,   -- 0.42m (13px)
        hp_follows_size = 1.0,
        jaggedness = 0.30,          -- compact; chips shouldn't look torn
        -- Cut interior detail into the silhouette: facets fanned from an
        -- off-centre hub, plus craters. Rolled per rock, so no two match.
        detail = "rock", rock_craters = 1, rock_facets = true,
        tier = 0,

        -- Normal asteroid (no explosion)
        explosive = false
    },

    -- ========================================================================
    -- MEDIUM ASTEROID
    -- ========================================================================
    -- Standard asteroid, splits into 2 SMALL on destruction.
    -- Balanced speed and durability.
    -- ========================================================================
    MEDIUM = {
        color = { r = 60, g = 55, b = 50 },        -- Dark brown/gray
        base_size = 0.7,                           -- Radius in meters (21 pixels)
        density = 5.5,                             -- Medium density
        hp = 35,                                   -- Requires 2-3 hits
        score_reward = 50,                         -- Medium reward
        speed_range = { 6.0, 7.0 },                -- Moderate speed
        base_size = 0.70,
        size_variance_min = 0.70,   -- 0.49m (15px)  <- overlaps big SMALLs
        size_variance_max = 1.30,   -- 0.91m (27px)
        hp_follows_size = 1.0,
        jaggedness = 0.42,
        -- Cut interior detail into the silhouette: facets fanned from an
        -- off-centre hub, plus craters. Rolled per rock, so no two match.
        detail = "rock", rock_craters = 2, rock_facets = true,
        tier = 1,

        -- Normal asteroid (no explosion)
        explosive = false
    },

    -- ========================================================================
    -- LARGE ASTEROID
    -- ========================================================================
    -- Rare spawn, splits into 3 MEDIUM on destruction.
    -- Slow but very durable, high point value.
    -- ========================================================================
    LARGE = {
        color = { r = 60, g = 55, b = 50 },        -- Dark brown/gray (same as medium)
        base_size = 1.5,                           -- Radius in meters (45 pixels)
        density = 10.0,                            -- High density = heavy
        hp = 120,                                  -- Requires many hits
        score_reward = 200,                        -- High reward for risk
        speed_range = { 5.0, 6.0 },                -- Slow lumbering movement
        base_size = 1.50,
        size_variance_min = 0.65,   -- 0.98m (29px)  <- overlaps big MEDIUMs
        size_variance_max = 1.45,   -- 2.18m (65px)  <- genuine monsters
        hp_follows_size = 1.0,
        jaggedness = 0.50,          -- deep craters
        -- Cut interior detail into the silhouette: facets fanned from an
        -- off-centre hub, plus craters. Rolled per rock, so no two match.
        detail = "rock", rock_craters = 4, rock_facets = true,
        tier = 2,

        -- Normal asteroid (no explosion)
        explosive = false
    },

    -- ========================================================================
    -- MAGMATIC ASTEROID (VOLCANIC/BOMB TYPE)
    -- ========================================================================
    -- Glowing orange/red asteroid that explodes on destruction.
    -- High risk/reward: deals area damage to everything nearby,
    -- including player and other asteroids!
    -- Strategy: Keep your distance or use as a weapon against enemies.
    -- ========================================================================
    MAGMATIC = {
        color = { r = 255, g = 80, b = 40 },        -- Bright lava orange/red
        base_size = 0.9,                            -- Between medium and large (27 pixels)
        density = 6.0,                              -- Quite heavy
        hp = 50,                                    -- Medium durability (2-3 hits)
        score_reward = 75,                          -- Good reward for risk
         base_size = 0.90,
        size_variance_min = 0.80,
        size_variance_max = 1.25,
        hp_follows_size = 1.0,
        jaggedness = 0.45,
        tier = 3,

        -- Movement
        speed_range = { 4.0, 5.0 },                 -- Moderate speed
        
        -- ===== EXPLOSIVE PROPERTIES =====
        core_size = 0.0,                            -- burning rock, not a reactor
        explosive = true,                           -- Triggers explosion on death
        explosion_radius = 150.0,                   -- Damage radius in pixels
        explosion_damage = 120.0,                    -- Damage to entities in radius
        
        -- Visual effects
        particle_count = 80,                        -- Extra particles for explosion
        glow_intensity = 0.8                        -- Pulsing glow effect
    },

    -- ========================================================================
    -- ========================================================================
    -- RAKSHARI SALVAGE FIELD
    -- ========================================================================
    -- JUNK is the field (75%); a dead SHIP is an event inside it (25%).
    --
    -- SIZE BANDS are pinned to the rocks in open space, so the salvage field
    -- has the same rhythm of small/medium/large the player already reads:
    --     small   = SMALL  (base 0.32)
    --     medium  = MEDIUM (base 0.70, variance 0.70-1.30)  <- identical
    --     large   = LARGE  (base 1.50, variance 0.65-1.45)  <- identical
    --
    -- COLOUR carries size. Two families, each running DARK when big to LIGHT
    -- when small, so a glance at hue tells you roughly what you are about to
    -- hit. `color_alt` is rolled per object at 50%, which is what puts rusted
    -- iron and bare steel in the same field instead of one material
    -- everywhere:
    --     rust   dark  74,50,36   ->  mid 102,72,54  ->  light 138,100,74
    --     steel  dark  76,78,84   ->  mid 112,114,120 -> light 156,158,164
    --
    -- SHAPE is what separates the junk types from each other, not size. The
    -- authored pieces are deliberately WARPED -- bent the wrong way, spine
    -- wandering, faces disagreeing, arcs that were never round. A clean arc
    -- looks manufactured; salvage has been through something.
    --
    -- `metallic = true` switches the impact palette: plasma on hull plate
    -- throws the same yellow sparks and red burst as a hit on an enemy ship,
    -- because it is the same event. Stone keeps dull grey chips, and that
    -- contrast is the fastest way to tell salvage from rock mid-fight.
    -- ========================================================================

    -- ---------------- THREE SIZES, ONE SHAPE LANGUAGE ----------------
    -- Every pile is a CLUMP: the silhouette is the union of a few offset
    -- discs, so it has bumps where a lump sticks out and gaps where two of
    -- them meet. That seam is the whole read -- a single jittered circle is
    -- what stone looks like, however rough you make it.
    --
    -- Lobe count rises with size, so the shape says small / medium / large
    -- before the colour does: a chunk is two lumps stuck together, a large
    -- pile is five. Colour then confirms it -- light when small, dark when
    -- big, in both rust and steel.
    --
    -- The bar and arc shapes that used to live here are gone. At this size
    -- they read as horns rather than as salvage, and having six junk types
    -- made small and medium impossible to tell apart.

    -- Two lumps welded together. Kept simple: at 10px across, detail is noise.
    SCRAP_CHUNK = {
        color     = { r = 168, g = 128, b = 98 },
        color_alt = { r = 184, g = 186, b = 192 },
        base_size = 0.32, density = 2.2, hp = 12, score_reward = 12,
        speed_range = { 6.5, 8.5 },
        size_variance_min = 0.80, size_variance_max = 1.18, hp_follows_size = 1.0,
        detail = "cluster",
        cluster_chunks = 2, cluster_spread = 0.46,
        cluster_gaps = 1, cluster_struts = 1,
        metallic = true, child_inherit_color = true,
        tier = 0, explosive = false
    },

    -- Medium pile -- same size band as a MEDIUM asteroid, on purpose.
    SCRAP = {
        color     = { r = 104, g = 74, b = 56 },
        color_alt = { r = 112, g = 114, b = 120 },
        base_size = 0.70, density = 4.0, hp = 32, score_reward = 55,
        speed_range = { 5.5, 7.0 },
        size_variance_min = 0.70, size_variance_max = 1.30, hp_follows_size = 1.0,
        detail = "cluster",
        cluster_chunks = 4, cluster_spread = 0.52,
        cluster_gaps = 2, cluster_struts = 3,
        metallic = true, child_inherit_color = true,
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    -- A mountain of strapped salvage -- same band as a LARGE asteroid.
    -- Five lobes: the most knotted thing in the field.
    SCRAP_LARGE = {
        color     = { r = 58, g = 40, b = 28 },
        color_alt = { r = 60, g = 62, b = 68 },
        base_size = 1.50, density = 5.0, hp = 115, score_reward = 190,
        speed_range = { 4.2, 5.6 },
        size_variance_min = 0.65, size_variance_max = 1.45, hp_follows_size = 1.0,
        detail = "cluster",
        cluster_chunks = 6, cluster_spread = 0.58,
        cluster_gaps = 3, cluster_struts = 4,
        metallic = true, child_inherit_color = true,
        tier = 2, child_type = "SCRAP", explosive = false
    },

    -- ========================================================================
    -- SHIP WRECKS
    -- ========================================================================
    -- Every wreck rolls a DAMAGE TIER at spawn (see utils/WreckDetail.hpp):
    --
    --   0  TURNED OFF   pristine, powered down, plating intact, no pitting.
    --                   Reads as a live ship running dark: the ambush hull.
    --   1  LIGHT        one torn edge, one breach, plating mostly attached
    --   2  HEAVY        torn sections, missing and peeled plates
    --   3  DESTROYED    deep bites, most plating gone, five breaches
    --
    --   wreck_damage = { w0, w1, w2, w3 }   relative weights per tier
    --
    -- The tier bites the silhouette BEFORE physics is built, so a torn wreck
    -- collides as torn and fractures along its torn outline.
    --
    -- wreck_of = "BARGE" reads hull, plates, thrusters, turrets, scars and
    -- scar_width from the LIVE archetype in enemy.lua -- the wreck cannot
    -- drift out of sync with the ship. Any of those set here overrides the
    -- archetype's, field by field (e.g. nozzles the live ship draws as flame
    -- emitters rather than hardware).
    --
    -- `color` is the COLD hull colour. The tier ramp darkens it; it is not
    -- also darkened by size like a rock is.
    --
    -- QA: asteroid_visuals.wreck_force_tier (below) forces every new wreck
    -- to one tier. Set, F5, look, set back to -1.
    -- ========================================================================

    -- ---------------- WRECKS: small hulls ----------------
    -- HP of a MEDIUM rock, and tier 0: they break into nothing at all.

    -- Authored hull (player stock pattern), already torn -- so never pristine.
    WRECK_LIGHT = {
        color = { r = 78, g = 80, b = 85 },
        hull_points = {
            { 0, -48 }, { 3, -32 }, { 8, -12 }, { 15, 12 }, { 12.2, 14.7 }, { 7.3, 16.5 },
            { 9, 30 }, { 0, 28 }, { -9, 30 }, { -12, 24 }, { -20, 21 }, { -15, 12 },
            { -8, -12 }, { -3, -32 },
        },
        wreck_damage = { 0, 3, 4, 2 },
        base_size = 1.60, density = 3.0, hp = 35, score_reward = 70,
        speed_range = { 5.0, 6.5 },
        size_variance_min = 0.92, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 0, explosive = false
    },

    WRECK_RAIDER = {
        color = { r = 74, g = 60, b = 56 },
        wreck_of = "RAIDER",
        wreck_damage = { 1, 3, 4, 2 },
        -- The live Raider has no nozzle hardware (its flame comes from the
        -- default emitter); the dead one shows three cold ones.
        thrusters = { { -7, 13 }, { 7, 13 }, { 0, 17 } },
        base_size = 0.97, density = 3.2, hp = 35, score_reward = 60,
        speed_range = { 5.2, 6.8 },
        size_variance_min = 0.92, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 0, explosive = false
    },

    -- ---------------- WRECKS: medium hulls ----------------
    -- +50% HP, and they shed 2 small chunks.

    WRECK_BERSERKER = {
        color = { r = 76, g = 58, b = 54 },
        wreck_of = "BERSERKER",
        wreck_damage = { 1, 3, 4, 2 },
        base_size = 1.73, density = 4.2, hp = 52, score_reward = 120,
        speed_range = { 4.4, 5.8 },
        size_variance_min = 0.92, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    WRECK_MANIAC = {
        color = { r = 80, g = 64, b = 50 },
        wreck_of = "MANIAC",
        wreck_damage = { 1, 3, 4, 2 },
        base_size = 1.32, density = 4.2, hp = 52, score_reward = 110,
        speed_range = { 4.6, 6.0 },
        size_variance_min = 0.92, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    -- Authored hull, already torn -- never pristine.
    WRECK_MEDIUM = {
        color = { r = 75, g = 75, b = 79 },
        hull_points = {
            { 0, -42 }, { 5, -26 }, { 20, 2 }, { 28, -3 }, { 31, 10 }, { 36, 20 },
            { 24, 24 }, { 20, 34 }, { 8, 30 }, { 0, 32 }, { -8, 30 }, { -13.6, 26.1 },
            { -13, 17.4 }, { -24.4, 16.6 }, { -31, 10 }, { -28, -3 }, { -20, 2 }, { -5, -26 },
        },
        wreck_damage = { 0, 3, 4, 2 },
        base_size = 1.40, density = 4.2, hp = 52, score_reward = 125,
        speed_range = { 4.4, 5.6 },
        size_variance_min = 0.92, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    -- ---------------- WRECKS: heavy hulls ----------------
    -- Oversized past their live analogues on purpose -- these are the two
    -- objects in the field that should make you change course. Tier 2 gives
    -- 3 fragments; child_alt at 100% turns exactly one of them into a live
    -- core, so a heavy wreck always reads as "2 piles and a bomb".

    -- Authored hull, already torn -- never pristine.
    WRECK_HEAVY = {
        color = { r = 66, g = 67, b = 70 },
        hull_points = {
            { 0, -56 }, { 10, -30 }, { 24, -8 }, { 33, 2 }, { 38, -4 }, { 43, 12 },
            { 50, 22 }, { 40, 30 }, { 32, 46 }, { 14, 44 }, { 0, 48 }, { -14, 44 },
            { -32, 46 }, { -28.3, 24.5 }, { -30.5, 17.7 }, { -26.2, 11.6 }, { -26.9, 0.4 },
            { -33, 2 }, { -24, -8 }, { -10, -30 },
        },
        wreck_damage = { 0, 3, 4, 2 },
        base_size = 2.25, density = 6.0, hp = 165, score_reward = 280,
        speed_range = { 3.6, 4.8 },
        size_variance_min = 0.94, size_variance_max = 1.06, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 2, child_type = "SCRAP",
        child_alt = "REACTOR", child_alt_chance = 100,
        explosive = false
    },

    WRECK_BARGE = {
        color = { r = 78, g = 62, b = 56 },
        wreck_of = "BARGE",
        wreck_damage = { 1, 3, 4, 2 },
        -- Engine block nozzles; the live Barge only emits flame there.
        thrusters = { { 7, 68 }, { -7, 68 } },
        base_size = 2.75, density = 7.0, hp = 210, score_reward = 340,
        speed_range = { 3.2, 4.2 },
        size_variance_min = 0.94, size_variance_max = 1.06, hp_follows_size = 1.0,
        metallic = true, child_inherit_color = true,
        tier = 2, child_type = "SCRAP",
        child_alt = "REACTOR", child_alt_chance = 100,
        explosive = false
    },

    -- ---------------- UNSTABLE REACTOR CORE ----------------
    -- Not a rock and not a ship: a containment vessel that failed. An OCTAGON,
    -- small, with a lit pulsing centre -- the one object out here that is
    -- obviously manufactured and obviously about to go off.
    --
    -- It does NOT spawn on its own. The only way one exists is that you broke
    -- a heavy hull open, which makes every core on screen something you caused.
    REACTOR = {
        color = { r = 96, g = 52, b = 34 },
        hull_points = {
            { 18.48, 7.65 }, { 7.65, 18.48 }, { -7.65, 18.48 }, { -18.48, 7.65 },
            { -18.48, -7.65 }, { -7.65, -18.48 }, { 7.65, -18.48 }, { 18.48, -7.65 },
        },
        base_size = 0.55, density = 6.5, hp = 48, score_reward = 120,
        speed_range = { 3.5, 4.6 },
        size_variance_min = 0.94, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true,
        tier = 3,
        core_size = 0.46,
        explosive = true,
        explosion_radius = 150.0,
        explosion_damage = 120.0,
        particle_count = 90,
        glow_intensity = 1.0
    }
}



asteroid_visuals = {
    -- ===== MAGMATIC =====
    magma_detail = 0,             -- 0 = fill + pulsing outline only (most minimal)
                                  -- 1 = short cracks from the rim  <- recommended
                                  -- 2 = full cracks, previous look
    magma_core_size      = 0.0,   -- GLOBAL default: off. A lit core is a
                                  -- Rakshari reactor's signature, not a
                                  -- property of burning rock -- so it is set
                                  -- per type via `core_size` instead.
    magma_vein_depth_min = 0.34,  -- crack length at detail 1
    magma_vein_depth     = 0.32,  -- crack length at detail 2
    magma_wave_speed     = 2.6,
    magma_wave_sharpness = 1.8,
    facets = 0,

    -- ===== NORMAL ASTEROIDS =====
    facets = 0,                   -- 1 = flat interior lines, 0 = plain rock.
                                  -- Default off; the minimal look is better.

    -- ===== SHIP WRECKS =====
    wreck_force_tier  = -1,       -- QA: 0..3 forces every NEW wreck to that
                                  -- tier (0 = powered-down ambush hull).
                                  -- -1 = roll from each type's wreck_damage.
    wreck_rim_alpha   = 175,      -- starlight on edges facing the light (0-255)
    wreck_rim_ambient = 22,       -- floor on the dark side, so a wreck never
                                  -- vanishes against the background
    wreck_rim_width   = 1.6,      -- px
}


-- ============================================================================
-- SPAWN CONFIGURATION
-- ============================================================================

spawn_settings = {
    interval = 0.5,          -- Seconds between spawn attempts (2 per second max)
    despawn_radius = 100.0,  -- Distance from player in meters to despawn (3000 pixels)
    max_count = 60,          -- Maximum concurrent asteroids in world
    spawn_radius = 2500,     -- Distance from player in pixels to spawn new asteroids
    
    -- ===== SPAWN PROBABILITIES (0.0 to 1.0) =====
    magmatic_chance = 0.15   -- 15% chance to spawn as magmatic instead of normal
}