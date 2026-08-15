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
    
    despawn_radius = 250.0,      -- Distance in meters from player to despawn
                                -- (250m = 7500 pixels, larger than asteroid despawn)


    -- ===== KINETIC WEAPONS (parry-launched and rift-hijacked rocks) =====
    kinetic_base_damage = 170.0,   -- before tier and speed scaling
    kinetic_tier_small  = 0.38,    -- ~65 dmg  -> ~4 hits on a 250 HP enemy
    kinetic_tier_medium = 0.78,    -- ~133 dmg -> 2 hits
    kinetic_tier_large  = 1.50,    -- ~255 dmg -> ONE SHOT
    kinetic_tier_magma  = 2.00,    -- overkill, plus the blast on top
    kinetic_knockback   = 950.0,   -- scaled by tier


    
    -- ===== DODGE TIMING =====
    dodge_speed          = 620.0,
    dodge_duration       = 0.42,
    dodge_cooldown       = 1.1,    -- WAS 2.0. The cooldown is shared between
                                   -- reactive dodges and idle jukes; at 2.0-3.2s
                                   -- the pirate had no budget left to actually
                                   -- react with. Idle jukes now have their own
                                   -- separate, much longer timer.
    dodge_manoeuvre_time = 0.25,   -- WAS 0.40, which made player bullets
                                   -- mathematically undodgeable -- pirates could
                                   -- only flinch at gunfire.
    dodge_bank_angle     = 34.0,
 
    -- ===== THREAT NOTICE RANGES =====
    bullet_notice_range  = 520.0,  -- WAS hardcoded 260. At 800px/s that gave
                                   -- 0.33s; minus a 0.28s reaction, 0.04s left.
                                   -- 520 buys 0.65s -> 0.37s after reacting,
                                   -- clearing the 0.25s manoeuvre with margin.
                                   -- Lower this to make pirates eat more shots.
    homing_notice_range  = 450.0,
 
    -- ===== HOMING PENALTY (keeps kinetic rocks lethal) =====
    homing_dodge_penalty = 2.5,    -- A parry-launched or rift-hijacked rock
                                   -- STEERS, so a sidestep that clears a dumb
                                   -- projectile does nothing. Available time is
                                   -- divided by this before the dodge check.
                                   -- This is what keeps kinetic rocks unavoidable
                                   -- while ordinary bullets stay dodgeable --
                                   -- they travel at similar speeds, so speed
                                   -- alone cannot separate them.
 
    -- ===== REACTION FLOORS =====
    bullet_reaction_min  = 0.28,
    threat_reaction_min  = 0.35,
    bullet_dodge_chance  = 0.45,   -- how often they pick a GOOD direction
    avoid_force          = 380.0,
 
    -- ===== PROACTIVE JUKES (heavily reduced) =====
    chaos_dodge_interval = 5.5,    -- WAS 1.9. At 1.9 it fired on roughly the
                                   -- same period as the dodge cooldown and
                                   -- consumed it every cycle, so pirates
                                   -- sidestepped at random instead of evading.
    chaos_dodge_chance   = 0.40,   -- and it is now SUPPRESSED ENTIRELY whenever
                                   -- a real threat is inbound.
 
    -- ===== BULLET STORM (now actually reachable) =====
    storm_base_rate       = 0.25,  -- urge/sec during ANY combat
    storm_rock_rate       = 0.22,  -- extra urge/sec per nearby asteroid
    storm_rock_cap        = 1.1,   -- cap on the rock bonus
    storm_asteroid_radius = 340.0, -- WAS 260, barely wider than the ship's own
                                   -- engagement bubble
    storm_urge_threshold  = 2.2,   -- scaled by 0.7 + aggression*0.6
 
    -- Resulting trigger times:
    --   0 rocks nearby -> 6-11s of combat
    --   2 rocks        -> 2-4s
    --   4 rocks        -> 1.4-2.4s
    -- Previously it needed 2.6-4.5s of unbroken combat with 4+ rocks inside
    -- 260px, which effectively never happened.
 
    storm_duration       = 1.7,
    storm_spin_speed     = 760.0,
    storm_fire_interval  = 0.13,
    storm_recover_time   = 1.6,    -- the punish window
    storm_cooldown       = 12.0,   -- NOTE: this now means what it says. It was
                                   -- being decremented in two places and drained
                                   -- ~2x too fast, so 12 behaved like ~6.
    storm_brake_rate     = 7.0
}