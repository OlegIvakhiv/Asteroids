-- ============================================================================
-- enemy.lua
-- Enemy ship configuration for the Modular Space Engine
-- 
-- Defines stats for AI-controlled pirate ships including physics properties,
-- movement capabilities, combat durability, and visual appearance.
-- 
-- @author Oleg Ivakhiv
-- @version 1.1
-- ============================================================================

-- ============================================================================
-- ENEMY CONFIGURATION
-- ============================================================================

enemy_config = {
    -- ========================================================================
    -- PHYSICS PROPERTIES
    -- ========================================================================
    
    density = 4.0,              -- Mass per area (higher = heavier, harder to push)
    lineardrag_factor = 1.0,    -- Linear damping (air resistance in space)
    angulardrag_factor = 2.0,   -- Angular damping (slows rotation over time)
    
    -- ========================================================================
    -- MOVEMENT CAPABILITIES
    -- ========================================================================
    
    engine_power = 200.0,          -- Thrust force (higher = faster acceleration)
    rotation_speed = 4.0,          -- Turn rate in degrees per second
    homing_notice_range = 450.0,   -- Distance to notice a homing missile
    bullet_dodge_chance = 0.60,    -- Base probability of picking a good dodge direction
 
    -- ========================================================================
    -- COMBAT STATS
    -- ========================================================================
    
    hp = 250.0,                 -- Hit points (requires multiple hits to destroy)
    score_reward = 500,         -- Points awarded when destroyed
    bullet_speed    = 550.0,    -- slower than player (800), readable in flight
    bullet_lifetime = 2.0,      -- seconds
    fire_rate       = 1.8,      -- seconds between shots (moderate)
    attack_range    = 480.0,    -- pixels, must be in range to fire
    aim_spread      = 18.0,     -- degrees of random inaccuracy (human factor)

    -- ========================================================================
    -- VISUAL APPEARANCE
    -- ========================================================================

    color = { r = 255, g = 50, b = 50 },    -- Bright red (threatening/pirate theme)
    
    -- ========================================================================
    -- WORLD MANAGEMENT
    -- ========================================================================
    
    despawn_radius = 250.0      -- Distance in meters from player to despawn
                                -- (250m = 7500 pixels, larger than asteroid despawn)
}