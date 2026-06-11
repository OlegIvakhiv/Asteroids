-- ============================================================================
-- player.lua
-- Player ship configuration for the Modular Space Engine
-- 
-- Defines all player-specific stats including physics properties,
-- movement mechanics (normal, sprint/turbo, dash), ship shape,
-- colors, weapons, and key bindings.
-- 
-- @author Oleg Ivakhiv
-- @version 1.1
-- ============================================================================

-- ============================================================================
-- PHYSICS PROPERTIES (Box2D)
-- ============================================================================

angulardrag_factor = 1.0     -- Angular damping (slows rotation over time)
lineardrag_factor = 1.0      -- Linear damping (air resistance in space)
density = 3.0                -- Mass per area (affects collision response)

-- ============================================================================
-- MOVEMENT MECHANICS
-- ============================================================================

-- Normal movement
engine_power = 150.0         -- Base thrust force (pixels/sec²)
rotation_speed = 4.0         -- Turn rate (degrees per second)

-- Turbo / Sprint (energy-based speed boost)
sprint_power_multiplier = 2.5    -- Speed multiplier when sprinting (150 → 375 thrust)
sprint_drain_speed = 40.0        -- Energy drain rate (units per second)
sprint_regen_speed = 20.0        -- Energy regen rate when not sprinting
penalty_energy = 3.0             -- Overheat penalty duration (seconds when energy hits 0)

-- Dash mechanic (instant velocity burst)
dash_velocity = 40.0         -- Instant velocity boost (pixels/sec)
dash_energy_cost = 30.0      -- Energy consumed per dash
dash_max_cooldown = 1.0      -- Cooldown time between dashes (seconds)

-- ============================================================================
-- SHAPE DEFINITION (12-point polygon)
-- ============================================================================
-- Creates a sleek, aggressive fighter ship shape with:
-- - Sharp pointed nose
-- - Swept-back wings with sharp tips
-- - Engine stabilizers at the rear
-- - Central exhaust cutout
-- ============================================================================

ship_shape = {
    { x = 0,   y = -30 },   -- 1.  Sharp nose tip
    { x = 7,   y = -10 },   -- 2.  Right nose shoulder
    { x = 12,  y = -5 },    -- 3.  Front wing base
    { x = 28,  y = 15 },    -- 4.  Main wing tip (very sharp)
    { x = 15,  y = 15 },    -- 5.  Inner wing crease
    { x = 18,  y = 28 },    -- 6.  Right engine stabilizer
    { x = 0,   y = 20 },    -- 7.  Center exhaust notch
    { x = -18, y = 28 },    -- 8.  Left engine stabilizer
    { x = -15, y = 15 },    -- 9.  Inner left wing crease
    { x = -28, y = 15 },    -- 10. Left wing tip
    { x = -12, y = -5 },    -- 11. Left wing base
    { x = -7,  y = -10 }    -- 12. Left nose shoulder
}

-- ============================================================================
-- PARRY MECHANIC
-- ============================================================================

parry_window = 0.3             -- Active parry frames (invincibility, deflection)
parry_anim_duration = 0.6      -- Visual spin duration (longer for smooth feel)
parry_stun_duration = 1.5   -- How long enemies stay stunned
parry_cooldown = 2.0           -- Cooldown before next parry
parry_reflect_damage = 50   -- Damage reflected back to enemies
homing_missile_speed = 800  -- Speed of parried asteroid (pixels/sec)
homing_turn_rate = 3.0      -- How aggressively homing missiles turn

-- ============================================================================
-- COLORS
-- ============================================================================

color = { r = 50, g = 150, b = 255, a = 255 }           -- Dark rusty red / maroon
outline_color = { r = 0, g = 200, b = 255, a = 255 } -- Neon cyan (glowing outline)
dash_flash_color = { r = 100, g = 255, b = 255, a = 200 } -- Bright cyan flash during dash

-- ============================================================================
-- WEAPON SYSTEM
-- ============================================================================

bullet_speed = 800.0         -- Projectile velocity (pixels/sec)
bullet_lifetime = 1.5        -- Seconds before bullet auto-destructs
fire_rate = 0.2
bullet_color = { r = 0, g = 255, b = 200, a = 255 } -- Turquoise neon

-- Rift Shot
rift_charge_time    = 0.6     -- seconds to hold before firing
rift_bullet_speed   = 1100.0  -- faster than normal (800)
rift_damage         = 45.0    -- 3x normal bullet (15)
rift_hitbox_radius  = 0.35    -- larger than normal bullet (0.1)

-- Air Burst (detonate in open space)
rift_burst_radius   = 220.0
rift_burst_damage   = 18.0

-- Parry whiff
parry_whiff_duration = 0.5

-- ============================================================================
-- HEALTH & INVULNERABILITY
-- ============================================================================

max_hp = 100                 -- Maximum health points

-- Invulnerability frames (i-frames) after taking damage
invul_time = 0.4             -- Full invincibility duration (seconds)
cheap_invul_time = 0.1       -- Partial invincibility for minor collisions

-- ============================================================================
-- KEY BINDINGS
-- ============================================================================
-- Maps input names to actual keyboard/mouse buttons.
-- These strings are looked up by InputRegistry and converted to SFML keys.
-- ============================================================================

key_bindings = {
    up     = "W",           -- Forward thrust
    down   = "S",           -- Reverse thrust
    left   = "A",           -- Strafe left
    right  = "D",           -- Strafe right
    dash   = "Space",       -- Dash / dodge roll (instant velocity burst)
    fire   = "MouseLeft",   -- Primary weapon (shoot bullets)
    sprint = "LShift",      -- Turbo / energy boost (consumes energy)
    parry = "R",            -- Parry button!
    rift_detonate_key   = "MouseRight"
}