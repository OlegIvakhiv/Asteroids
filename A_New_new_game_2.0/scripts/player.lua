-- ============================================================================
-- player.lua
-- Player ship configuration for the Modular Space Engine
-- 
-- Defines all player-specific stats including physics properties,
-- movement mechanics (normal, sprint/turbo, dash), ship shape,
-- colors, weapons, and key bindings.
-- 
-- @author Oleg Ivakhiv
-- @version 1.3
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


visuals = {
    -- ===== DASH: side (drift / bank) =====
    dash_bank_angle    = 26.0,   -- peak lean in degrees
    dash_bank_duration = 0.45,
    dash_bank_pivot_y  = -22.0,  -- LOCAL pivot. Negative = toward the nose.
                                 --  -22 -> tail swings wide (drift)
                                 --    0 -> pivots about the centre (barrel roll)
                                 --  +28 -> nose swings, tail anchored
    dash_bank_sign     = 1,      -- set to -1 to reverse which way the tail throws

    -- ===== DASH: backward (spin) =====
    backdash_spin_degrees  = 360.0,   -- try 540 for a showier version
    backdash_spin_duration = 0.55,

    -- ===== DASH: forward (wiggle) =====
    forward_wiggle_angle    = 8.0,
    forward_wiggle_duration = 0.35,
    forward_wiggle_cycles   = 2.5,

    -- ===== Continuous =====
    turbo_stretch        = 1.10,
    hit_shudder_angle    = 6.0,
    hit_shudder_duration = 0.22,

    -- ===== Camera shake =====
    shake_dash        = 0.35,
    shake_parry       = 0.25,
    shake_damage_base = 0.30,
    shake_turbo_floor = 0.22,   -- sustained rumble ceiling while boosting
    shake_decay       = 1.4,    -- trauma units per second
    shake_frequency   = 22.0,
    shake_max_offset  = 26.0,   -- pixels at full trauma
    shake_max_angle   = 1.6,    -- degrees at full trauma. Above ~2.5 = nausea.

    -- ===== Camera zoom =====
    zoom_reference_speed = 900.0,
    zoom_speed_amount    = 0.20,
    zoom_turbo_amount    = 0.08,
    zoom_dash_punch      = 0.07,  -- momentary punch IN on dash
    zoom_kick_release    = 8.0,
    zoom_smoothing       = 5.0,
    zoom_max             = 1.32,

    -- ===== Camera follow =====
    camera_follow_speed     = 8.0,
    camera_lookahead_factor = 0.20,
    camera_max_lookahead    = 170.0,

    -- ===== Space dust =====
    dust_count            = 240,
    dust_fade_in_speed    = 120.0,   -- below this the dust is invisible
    dust_full_speed       = 700.0,
    dust_max_streak       = 90.0,
    dust_streak_per_speed = 0.055,
    dust_alpha            = 150.0,
    dust_region_padding   = 1.25,
    dust_color_r = 170, dust_color_g = 200, dust_color_b = 255,

    -- ===== TURN SWAY =====
    sway_max_angle = 9.0,     -- peak lag/overshoot in degrees
    sway_min_speed = 90.0,    -- deg/sec. BELOW this: no effect at all.
    sway_full_speed = 420.0,  -- deg/sec for the full effect
    sway_stiffness = 90.0,    -- spring k
    sway_damping = 11.0,      -- spring c. Critical is ~19, so this bounces once.
                              -- Raise toward 19 for a tighter, snappier settle.
    sway_sign = 1,            -- flip to -1 to reverse which way the tail lags

    -- ===== STAGGER / KNOCKDOWN =====
    stagger_speed_threshold = 20.0,  -- Box2D m/s of relative impact (x30 = px/s)
    stagger_knockback = 900.0,       -- px/sec thrown away from the impact
    stagger_tumble_duration = 1.1,   -- seconds of NO control
    stagger_recover_duration = 0.55, -- seconds of aim-only recovery
    stagger_spin_speed = 620.0,      -- deg/sec peak tumble (sign randomised)
    stagger_spin_decay = 2.6,        -- higher = spin dies off sooner
    stagger_trauma_floor = 0.18,     -- sustained rumble while tumbling
    stagger_blast_falloff = 0.45,    -- how close to an explosion staggers you (0-1)

    -- ===== POISE =====
    -- Poise soaks stagger-grade hits before they tumble you. It is depleted by
    -- knockback-weighted hits and refills after a quiet window.
    poise_per_knockback = 0.1,   -- poise damage per point of hit knockback
                                 -- (bash 95, ram 110, rock 90, blast 40-90)
    poise_absorb_shove  = 0.30,  -- share of knockback still applied when poise
                                 -- eats the hit. 0 = pure absorb, 1 = same as
                                 -- a normal shove with no tumble.
    bullet_poise_per_damage = 0.6,   -- chip only: bullets never break poise alone

    -- ===== ROCK TIERS =====
    -- Poise and knockback scale x0.5 at 12 m/s closing speed up to x1.0 at 30 m/s.
    rock_small_damage = 8,    rock_small_poise = 12,  rock_small_knockback = 260,
    rock_medium_damage = 15,  rock_medium_poise = 45, rock_medium_knockback = 600,
    rock_large_damage = 22,   rock_large_poise = 90,  rock_large_knockback = 900,
    rock_magma_damage = 18,   rock_magma_poise = 70,  rock_magma_knockback = 750,

    -- ===== PARRY SUCCESS =====
    parry_hitstop_freeze = 0.06,     -- HARD freeze. Above ~0.15 reads as a hitch.
    parry_hitstop_slomo = 0.35,      -- slow-motion ramp back to normal
    parry_hitstop_min_scale = 0.25,  -- time scale where the ramp starts
    parry_trauma = 0.75,
    parry_flash_alpha = 170.0,       -- 0-255. Above ~200 is genuinely blinding.
    shake_parry_start = 0.18,        -- small kick on the parry INPUT (not the connect)

    shake_overheat_floor = 0.16,   -- sustained rumble while the gun is locked

    rift_direct_hit_trauma = 0.40,
}

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
parry_cooldown = 1.0           -- Cooldown before next parry
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
reflect_knockback = 50
bullet_damage = 25.0
bullet_knockback = 40.0

-- Rift Shot
rift_charge_time    = 0.5     -- seconds to hold before firing
rift_bullet_speed   = 1100.0  -- faster than normal (800)
rift_damage         = 130.0    -- 3x normal bullet (15)
rift_hitbox_radius  = 0.35    -- larger than normal bullet (0.1)
rift_direct_knockback = 750.0
rift_direct_stun = 1.2

-- Air Burst (detonate in open space)
rift_burst_radius   = 150.0
rift_burst_damage   = 50.0

-- Parry whiff
parry_whiff_duration = 0.5


weapon = {
    -- ===== NORMAL FIRE (much longer before overheat) =====
    shot_energy_cost = 6.0,
    shot_heat        = 5.0,    -- was 9.0 -> 20 shots / 4.0s instead of 11 / 2.2s
    shot_recoil_impulse = 3.0,
    shot_trauma      = 0.10,

    -- ===== HEAT =====
    max_weapon_heat  = 100.0,
    heat_cool_delay  = 0.40,
    heat_cool_rate   = 34.0,   -- was 28.0
    heat_vent_rate   = 50.0,
    heat_unlock_threshold = 30.0,
    overheat_trauma  = 0.45,

    -- ===== RIFT: costs (spam is no longer free) =====
    rift_energy_cost   = 40.0,   -- was 35
    rift_heat          = 52.0,   -- was 34 -> two rifts overheat you
    rift_charge_drain  = 14.0,
    rift_charge_trauma = 0.12,   -- was 0.20

    -- ===== RIFT: shake (all reduced) =====
    rift_fire_trauma       = 0.40,  -- was 0.85
    rift_fire_zoom_kick    = 0.05,
    rift_fire_flash_alpha  = 45.0,

    rift_detonate_trauma      = 0.32,  -- was 0.95
    rift_detonate_freeze      = 0.03,
    rift_detonate_slomo       = 0.10,
    rift_detonate_flash_alpha = 55.0,

    rift_hijack_trauma = 0.28,

    rift_overload_trauma      = 0.45,  -- was 1.00
    rift_overload_freeze      = 0.05,
    rift_overload_slomo       = 0.18,
    rift_overload_flash_alpha = 80.0,

    -- ===== RIFT: damage =====
    rift_recoil_impulse      = 26.0,
    rift_burst_knockback     = 420.0,
    rift_overload_multiplier = 4.0,
    rift_overload_knockback  = 900.0,
    rift_overload_stun       = 2.5,

    -- ===== PARRY REFLECT =====
    reflect_damage    = 100.0,  -- 3-shot kill on a 250 HP enemy
    reflect_knockback = 700.0,
    reflect_stun      = 0.9,
    reflect_speed     = 1000.0,
    reflect_turn_rate = 420.0,  -- deg/sec steering authority
    reflect_lifetime  = 3.0,

    -- ===== DODGE PUNISH (was staggering on ~29% of all bullets) =====
    dodge_punish_chance     = 45.0,
    dodge_punish_min_damage = 40.0,  -- basic shots (25) never trigger it
    dodge_punish_knockback  = 320.0, -- was 700
}

-- ===== KINETIC WEAPONS (parry-launched and rift-hijacked rocks) =====
kinetic_base_damage = 170.0   -- before tier and speed scaling
kinetic_tier_small  = 0.38    -- ~65 dmg  -> ~4 hits on a 250 HP enemy
kinetic_tier_medium = 0.78    -- ~133 dmg -> 2 hits
kinetic_tier_large  = 1.50    -- ~255 dmg -> ONE SHOT
kinetic_tier_magma  = 2.00    -- overkill, plus the blast on top
kinetic_knockback   = 950.0   -- scaled by tier

-- ============================================================================
-- HEALTH & INVULNERABILITY
-- ============================================================================

max_hp = 100                 -- Maximum health points

-- Invulnerability frames (i-frames) after taking damage
invul_time = 0.4             -- Full invincibility duration (seconds)
cheap_invul_time = 0.1       -- Partial invincibility for minor collisions



-- ===== VENT QTE =====
-- Fires automatically whenever the weapon overheats. Three outcomes:
--   perfect (yellow) -> heat zeroed + overdrive
--   good    (blue)   -> heat zeroed
--   miss             -> vent normally, exactly as before
--
-- Miss is deliberately free. Stacking a penalty on top of an overheat would
-- punish one mistake twice, and the player who most needs the vent is already
-- the one under the most pressure. Upside-only is what makes reaching for it
-- feel like an opportunity instead of a bomb.
qte_timeout          = 2.4     -- WAS 3.2. Venting is frozen while this runs, so
                               -- the window is now wagered time, not free time.
qte_max_sweeps       = 3       -- WAS 4, same reason.
qte_speed            = 1.30    -- Sweeps/sec at streak 0
qte_good_half        = 0.105   -- Blue zone half-width (0..1 of the bar)
qte_perfect_half     = 0.035   -- WAS 0.028. The zone is read against a bar that
                               -- is 340px wide at most, and 0.028 gave a ~43ms
                               -- window – under 3 frames at 60fps.
qte_perfect_trauma   = 0.28

-- Consecutive perfects speed the marker up. Without this, "perfect vent grants
-- unlimited fire" is a closed loop: fire freely, overheat, hit perfect, repeat.
-- A player who can hit the check once can hit it forever, and the heat system
-- stops existing for them. The ramp lets a skilled player ride the loop for a
-- while and then closes it on its own, with no arbitrary hard cap.
qte_streak_speed_step = 0.16
qte_speed_max         = 2.35   -- ~1.8x base. Past this it stops reading as a
                               -- skill check and starts reading as a coin flip.

overdrive_duration   = 4.0     -- Seconds of free fire on a perfect vent
overdrive_exit_heat  = 0.0     -- Heat set when overdrive ends. 0 = the plain
                               -- reading of "heat doesn't rise for a few
                               -- seconds". Raise toward 45 if the streak ramp
                               -- alone does not close the loop in playtesting.


-- ===== PERFECT PARRY =====
-- A parry counts as PERFECT when it connects in the first slice of its active
-- window -- i.e. you pressed just before the hit landed rather than pressing
-- early and waiting. Perfect parries on BULLETS and SHIPS skip the recovery
-- entirely and grant brief i-frames.
--
-- Asteroids are excluded on purpose. Parrying a rock already pays out a kinetic
-- weapon, which is the largest single reward in the game; adding free recovery
-- and i-frames on top would make rock-parrying strictly better than every other
-- defensive option. Rocks are also big and slow, so the timing is not the
-- achievement there.
parry_perfect_fraction = 0.45  -- First 45% of parry_window counts as perfect
parry_perfect_cooldown = 0.12  -- Instead of parry_cooldown (1.0)
parry_perfect_iframes  = 0.22  -- THIS is what fixes "parried one bullet and the
                               -- next one hit me anyway". Cancelling recovery
                               -- lets you parry again; it does not help against
                               -- a shot already in flight when the first
                               -- connected. Only i-frames cover that.
parry_perfect_chain_falloff = 0.75  -- Each chained perfect gets this fraction
                                    -- of the i-frames, so a parry-lock cannot
                                    -- be held indefinitely.

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
    rift_detonate_key   = "MouseRight",
    vent   = "E",           -- Overheat vent QTE

    -- NOT the fire button, deliberately. Players mash fire the instant they
    -- overheat -- that is the reflex the lockout creates -- so binding the QTE
    -- to fire would auto-fail it before the bar could even be read.
}



-- Optional. Paste into player.lua. Every key has a C++ default equal to the
-- value below, so a missing table changes nothing (sol2 get_or is silent).
refit = {
    rcs_scale          = 1.0,   -- reverse/strafe floor. 1 = class default, 2 = MEDIUM reverses at full power
    yaw_drift_scale    = 0.2,   -- how hard off-axis drives pull the nose
    yaw_drift_damping  = 3.0,   -- how fast that pull settles
    yaw_drift_max      = 240,   -- deg/s cap on drift
    recoil_yaw_scale   = 0.35,  -- nose kick from off-centre guns / wing-mounted Rift
    gun_convergence    = 520,   -- px ahead where all plasma barrels meet
}


-- Playtest pass: values to change / add in player.lua.
-- Every key below also has the same default in C++, so deleting an old line
-- gives you the new value too. sol2 get_or is silent: a typo'd key = default.

-- ---- In your existing `weapon = { ... }` table ----
--   shot_energy_cost  = 3,     -- was 6.  Plasma is the spam button; heat limits it, not energy
--   rift_energy_cost  = 24,    -- was 40. ~24% of a medium pool instead of ~48% with the drain
--   rift_charge_drain = 4,     -- was 14. Charging costs time, not a second energy bill
--   rift_cooldown     = 2.0,   -- NEW.    The heavy attack's real price: slow to SHOOT

-- ---- In your existing `visuals = { ... }` table ----
--   stagger_grace          = 0.8,   -- NEW. No new tumble until 0.8s after control returns
--   stagger_tumble_iframes = 1.0,   -- NEW. Fraction of the tumble spent invulnerable
--   stagger_immune_shove   = 0.35,  -- NEW. Knockback kept when a hit can't tumble you

-- ---- Now ONLY used by the legacy (non-refit) ship ----
--   dash_velocity, dash_max_cooldown, dash_energy_cost

-- Lua beats the C++ defaults: if you keep the old table, you keep the old
-- short dodges. New keys: dash_carry, dash_drift.
--
--   dash_carry  share of the burst's average speed kept when the burst ends
--   dash_drift  seconds for that carry to bleed back to your entry speed
--               (only the component along the dodge is capped: you can steer out)
--
-- Measured from standstill at 60fps: LIGHT 335px, MEDIUM 311px, HEAVY 286px.
--
-- ---- POISE (per hull class) ----
--   poise        0 = every stagger-grade hit tumbles you
--   poise_regen  per second, after poise_delay seconds without a hit
--   knockback    multiplier on every shove
--   tumble       stagger duration multiplier when poise breaks
--   hyperarmor   1 = dodge burst absorbs hits without draining poise
--
--   damage_reduction       flat damage multiplier applied to incoming hits
--   hyperarmor_reduction   extra multiplier applied during the dodge burst
--                          (95 ram: 76 normally, 38 in the dodge)
--   shoulder_bash          enables the shoulder check
--   shoulder_damage        damage on a clean shoulder hit
--   shoulder_knock         px/sec shove on a clean shoulder hit
--   shoulder_stun          seconds of stun on a clean shoulder hit
--   shoulder_counter       x multiplier when the bash breaks a charge
--   ramming                enables ramming damage
--   ram_min_speed          minimum closing m/s to register a ram
--   ram_damage_per_speed   damage per m/s above ram_min_speed
--   ram_base_damage        flat damage added on top of the speed term
--   ram_medium_taken       share of damage the heavy takes from a medium ram
hull_classes = {
    light = {
        dash_distance = 290, dash_duration = 0.18, dash_iframes = 0.26,
        dash_recovery = 0.22, dash_cost = 12, dash_carry = 0.20, dash_drift = 0.22,
        heat_capacity = 0.75, heat_cool = 1.40, heat_vent = 1.35, qte_window = 1.45,
        parry_window = 1.15, regen = 1.20,

        -- POISE
        poise        = 30,
        poise_regen  = 30,
        poise_delay  = 1.0,
        knockback    = 1.10,
        tumble       = 1.0,
        hyperarmor   = 0,
    },
    medium = {
        dash_distance = 250, dash_duration = 0.21, dash_iframes = 0.20,
        dash_recovery = 0.40, dash_cost = 15, dash_carry = 0.28, dash_drift = 0.32,
        heat_capacity = 1.0, heat_cool = 1.0, heat_vent = 1.0, qte_window = 1.0,
        parry_window = 1.0, regen = 1.0,

        -- POISE
        poise        = 80,
        poise_regen  = 60,
        poise_delay  = 1.2,
        knockback    = 0.85,
        tumble       = 1.0,
        hyperarmor   = 0,
    },
    heavy = {
        dash_distance = 200, dash_duration = 0.27, dash_iframes = 0.12,
        dash_recovery = 0.70, dash_cost = 20, dash_carry = 0.38, dash_drift = 0.45,
        heat_capacity = 1.35, heat_cool = 0.72, heat_vent = 0.80, qte_window = 0.65,
        parry_window = 0.85, regen = 0.85,

        -- POISE
        poise        = 340,
        poise_regen  = 22,       -- slow on purpose
        poise_delay  = 4.0,      -- long quiet window before regen starts
        knockback    = 0.50,
        tumble       = 0.75,
        hyperarmor   = 1,

        -- DAMAGE MITIGATION
        damage_reduction     = 0.20,
        hyperarmor_reduction = 0.50,  -- 95 ram: 76 normally, 38 in the dodge

        -- SHOULDER BASH
        shoulder_bash     = 1,
        shoulder_damage   = 20,
        shoulder_knock    = 750,
        shoulder_stun     = 0.9,
        shoulder_counter  = 1.75,     -- x1.75 when it breaks a charge

        -- RAMMING
        ramming              = 1,
        ram_min_speed        = 12,
        ram_damage_per_speed = 3,
        ram_base_damage      = 15,
        ram_medium_taken     = 0.35,
    },
}