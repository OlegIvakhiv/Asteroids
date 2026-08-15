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
        explosive = true,                           -- Triggers explosion on death
        explosion_radius = 150.0,                   -- Damage radius in pixels
        explosion_damage = 120.0,                    -- Damage to entities in radius
        
        -- Visual effects
        particle_count = 80,                        -- Extra particles for explosion
        glow_intensity = 0.8                        -- Pulsing glow effect
    }
}



asteroid_visuals = {
    -- ===== MAGMATIC =====
    magma_detail = 0,             -- 0 = fill + pulsing outline only (most minimal)
                                  -- 1 = short cracks from the rim  <- recommended
                                  -- 2 = full cracks, previous look
    magma_core_size      = 0.0,   -- off by default now
    magma_vein_depth_min = 0.34,  -- crack length at detail 1
    magma_vein_depth     = 0.32,  -- crack length at detail 2
    magma_wave_speed     = 2.6,
    magma_wave_sharpness = 1.8,
    facets = 0,

    -- ===== NORMAL ASTEROIDS =====
    facets = 0,                   -- 1 = flat interior lines, 0 = plain rock.
                                  -- Default off; the minimal look is better.
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