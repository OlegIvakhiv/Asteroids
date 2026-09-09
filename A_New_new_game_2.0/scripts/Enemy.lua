-- ============================================================================
-- enemy.lua
-- Enemy archetypes and faction spawn tables for Void Hunter
--
-- STRUCTURE
-- ---------
--   enemy_defaults    shared stats every unit inherits
--   enemy_archetypes  one table per unit type, overriding what it needs
--   archetype_order   THE authoritative id order -- APPEND ONLY
--   factions          which units a faction fields, and how often
--   active_factions   which factions are currently spawning
--
-- ADDING A NEW UNIT
-- -----------------
--   1. Add a table to enemy_archetypes
--   2. Append its key to the END of archetype_order
--   3. F5
--
-- WHY archetype_order EXISTS
-- --------------------------
-- Live enemies store a numeric archetype id, not a name. That id is the
-- position in archetype_order. Lua table iteration is hash order and would
-- reshuffle between runs, silently turning a live Raider into a Barge. APPEND
-- to this list; never insert in the middle or reorder it mid-run.
--
-- INHERITANCE IS AN EXPLICIT COPY, NOT A METATABLE
-- ------------------------------------------------
-- derive() physically copies the default fields into each archetype table.
-- Metatable __index would also work, but sol2's get_or() reading through an
-- __index chain silently returns a default when something is subtly wrong.
-- Plain data after load means what you read here is what the engine sees.
--
-- @author Oleg Ivakhiv
-- @version 2.1
-- ============================================================================


-- ============================================================================
-- SHARED DEFAULTS
-- ============================================================================

enemy_defaults = {

    -- ===== PHYSICS =====
    density              = 4.0,
    lineardrag_factor    = 1.0,
    angulardrag_factor   = 2.0,
    scale                = 1.0,      -- Uniform multiplier on the hull points
    hitbox_scale         = 1.0,      -- Collision hull only

    -- ===== MOVEMENT =====
    engine_power         = 200.0,
    max_speed            = 20.0,
    rotation_speed       = 4.0,

    -- ===== STEERING CONTROLLER =====
    -- AISystem steers with a P-controller that applies a MASS-INDEPENDENT
    -- force. That was fine while every enemy weighed the same. It is not fine
    -- now: the Barge masses ~93kg against the Raider's ~7kg, so the identical
    -- force produced 13x less acceleration and the ship crawled at 11 px/s
    -- while still correctly turning broadside -- which read as "spinning in
    -- place".
    --
    -- With steer_mass_compensate = true the controller gain and force budget
    -- both scale by (this ship's real mass / steer_reference_mass), turning it
    -- into an ACCELERATION controller. Heavy ships then accelerate like light
    -- ones, and mass only affects what happens when something hits them --
    -- which is what mass should mean.
    --
    -- Left OFF by default so the Raider and Wardog paths are byte-identical.
    steer_mass_compensate = false,
    steer_reference_mass  = 7.5,     -- kg, ~= the Raider. The mass the hardcoded
                                     -- gain of 50 was originally tuned against.

    -- Used only when personality_variance is false. 0 = timid, 1 = reckless.
    fixed_aggression      = 0.8,

    -- Hull facing. "target" points the nose at the player in COMBAT.
    -- "velocity" points the nose along the direction of travel and NEVER at
    -- the player -- naval broadside behaviour. See the Barge.
    facing_mode          = "target",

    -- ===== COMBAT =====
    hp                   = 250.0,
    score_reward         = 500,
    bullet_speed         = 550.0,
    bullet_lifetime      = 2.0,
    bullet_damage        = 25.0,     -- Per projectile. Was BulletComponent's
                                     -- hardcoded default; now per unit.
    fire_rate            = 1.8,
    attack_range         = 480.0,
    aim_spread           = 18.0,
    telegraph_time       = 0.30,

    -- ===== TOUGHNESS =====
    -- 0.0 = staggers exactly as before. 1.0 = cannot be staggered at all.
    -- Scales tumble duration, spin, and knockback speed together, so a heavy
    -- ship is not merely harder to knock down, it is barely moved when it is.
    stagger_resist       = 0.0,
    -- Same idea for the parry stun. Kept separate because "can't be knocked
    -- around" and "can't be shut down" are different design statements.
    stun_resist          = 0.0,

    -- ===== VISION =====
    vision_range         = 620.0,
    vision_fov           = 110.0,
    vision_fov_alert_mult = 1.45,

    -- ===== WORLD =====
    despawn_radius       = 250.0,

    -- ===== KINETIC WEAPONS (parry-launched and rift-hijacked rocks) =====
    kinetic_base_damage  = 170.0,
    kinetic_tier_small   = 0.38,
    kinetic_tier_medium  = 0.78,
    kinetic_tier_large   = 1.50,
    kinetic_tier_magma   = 2.00,
    kinetic_knockback    = 950.0,

    -- ===== DODGE TIMING =====
    dodge_speed          = 620.0,
    dodge_duration       = 0.42,
    dodge_cooldown       = 1.1,
    dodge_manoeuvre_time = 0.25,
    dodge_bank_angle     = 34.0,

    -- ===== THREAT NOTICE RANGES =====
    bullet_notice_range  = 520.0,
    homing_notice_range  = 450.0,
    homing_dodge_penalty = 2.5,

    -- ===== REACTION FLOORS =====
    bullet_reaction_min  = 0.28,
    threat_reaction_min  = 0.35,
    bullet_dodge_chance  = 0.45,
    avoid_force          = 380.0,

    -- ===== PROACTIVE JUKES =====
    chaos_dodge_interval = 5.5,
    chaos_dodge_chance   = 0.40,

    -- ===== BULLET STORM =====
    storm_enabled         = true,
    storm_base_rate       = 0.25,
    storm_rock_rate       = 0.22,
    storm_rock_cap        = 1.1,
    storm_asteroid_radius = 340.0,
    storm_urge_threshold  = 2.2,
    storm_duration        = 1.7,
    storm_spin_speed      = 760.0,
    storm_fire_interval   = 0.13,
    storm_recover_time    = 1.6,
    storm_cooldown        = 12.0,
    storm_brake_rate      = 7.0,

    -- ===== TURRETS =====
    -- Off by default. A unit with `turrets` mount points and turret_enabled
    -- gets an independently-aiming weapon that does NOT care where the hull
    -- is pointing.
    turret_enabled        = false,
    turret_traverse       = 90.0,    -- Degrees/sec. Full 360 available, but the
                                     -- traverse rate is the counterplay: cut
                                     -- across the swing and it has to catch up.
    turret_range          = 800.0,
    turret_size           = 10.0,    -- Draw radius
    turret_lead_target    = true,    -- Predictive aim: solve for where the
                                     -- player WILL be, not where they are

    -- Aimed shot: single, telegraphed, leads the target.
    turret_aimed_telegraph = 0.42,
    turret_aimed_cooldown  = 2.4,
    turret_aimed_spread    = 3.0,

    -- Burst: 3-4 rounds fanned to cover a region rather than hit a point.
    -- Not aimed AT the player -- aimed at where the player can GO.
    turret_burst_count     = 4,
    turret_burst_interval  = 0.09,
    turret_burst_spread    = 15.0,   -- Degrees of total fan
    turret_burst_cooldown  = 3.6,
    turret_burst_telegraph = 0.30,

    -- How often it picks burst over an aimed shot (0..1).
    turret_burst_bias      = 0.45,

    -- ===== RAM CHARGE =====
    ram_enabled           = false,
    ram_windup            = 0.85,    -- Glow builds; this is the read
    ram_charge_speed      = 1150.0,
    ram_charge_duration   = 1.25,
    ram_recover           = 1.9,     -- The punish window. Longer than the
                                     -- charge, on purpose.
    ram_cooldown          = 15.0,
    ram_damage            = 95.0,
    ram_far_trigger       = 700.0,   -- Player further than this -> close the gap
    ram_near_trigger      = 210.0,   -- Player closer than this -> shove them off
    ram_surprise_bonus    = 2.0,     -- Trigger chance multiplier while the
                                     -- player has not confirmed contact yet

    -- ===== PERSONALITY ROLL =====
    personality_variance  = true,

    -- ===== SPAWN DIRECTOR =====
    faction               = "RAKSHARI",
    spawn_weight          = 0.0,
    max_active            = 4,
    threat_cost           = 3,
    summon_only           = false,

    -- ===== COLOUR =====
    -- One palette for the whole faction. Hue carries STATE (patrol / alert /
    -- combat); silhouette carries IDENTITY. Giving each unit its own base
    -- colour made those two channels compete -- a red Raider and an orange
    -- Barge in ALERT both read amber anyway, so the per-unit tint bought
    -- nothing and muddied the state read.
    color = { r = 200, g = 70, b = 55 },
}


-- ============================================================================
-- derive(overrides) -- shallow copy of enemy_defaults + this unit's overrides
-- ============================================================================

local function copyValue(v)
    if type(v) ~= "table" then return v end
    local t = {}
    for k, sub in pairs(v) do t[k] = copyValue(sub) end
    return t
end

local function derive(overrides)
    local out = {}
    for k, v in pairs(enemy_defaults) do out[k] = copyValue(v) end
    for k, v in pairs(overrides)      do out[k] = copyValue(v) end
    return out
end


-- ============================================================================
-- ARCHETYPES
--
-- `hull` is the DETAILED silhouette and may be concave. The engine derives the
-- Box2D collision hull from it automatically: convex hull, then reduced to
-- Box2D's hard 8-point ceiling. You never author the collision shape.
--
-- `turrets` are local-space mount points. With turret_enabled, the first mount
-- becomes a live, independently-aiming weapon.
-- ============================================================================

enemy_archetypes = {}

-- ----------------------------------------------------------------------------
-- WARDOG -- attrition tax, swarm unit
--
-- Rebalanced: less HP, much less damage, slightly larger hull. At 60 HP and
-- the 25 damage default, a pack of ten was an execution rather than a tax;
-- and at scale 0.85 they were small enough to be genuinely hard to click,
-- which is the wrong kind of difficulty for chaff.
--
-- Everything expensive stays switched off. That is performance as much as
-- design: the storm asteroid scan is O(n) per enemy and a swarm running it
-- would go quadratic.
--
-- summon_only = true. Seeing Wardogs must always mean something bigger called
-- them in, so the director will never roll them.
-- ----------------------------------------------------------------------------
enemy_archetypes.WARDOG = derive {
    display = "Wardog",
    faction = "RAKSHARI",

    hull = {
        {  0, -18 }, {  4, -10 }, {  2,  -5 }, {  9,   8 }, { 12,  18 },
        {  0,  12 },
        { -12, 18 }, { -9,   8 }, { -2,  -5 }, { -4, -10 },
    },
    turrets = {},

    scale                = 1.15,   -- WAS 0.85. Still the smallest hull in the
                                   -- roster, but now big enough to aim at
                                   -- inside a moving swarm.
    density              = 2.6,
    hp                   = 35.0,   -- WAS 60. One splash hit, or two plasma bolts.
    bullet_damage        = 9.0,    -- WAS the 25 default. Ten firing at once is
                                   -- real pressure; one is a nuisance. That gap
                                   -- is the whole unit.
    score_reward         = 45,
    engine_power         = 260.0,
    max_speed            = 26.0,
    rotation_speed       = 6.0,

    fire_rate            = 0.55,
    telegraph_time       = 0.0,    -- Volume is the threat, not any single shot
    aim_spread           = 26.0,
    attack_range         = 380.0,
    bullet_speed         = 480.0,

    storm_enabled        = false,
    personality_variance = false,  -- They must NOT read as individuals
    chaos_dodge_chance   = 0.0,
    bullet_dodge_chance  = 0.15,

    spawn_weight         = 0.0,
    summon_only          = true,
    max_active           = 14,
    threat_cost          = 1,

    -- Faction palette, same as every Rakshari unit. See the note in defaults.
    color = { r = 200, g = 70, b = 55 },
}

-- ----------------------------------------------------------------------------
-- RAIDER -- the baseline. Every other unit is defined relative to this one.
-- Unchanged from the tuned values it has always had.
-- ----------------------------------------------------------------------------
enemy_archetypes.RAIDER = derive {
    display = "Raider",
    faction = "RAKSHARI",

    hull = {
        {  0, -10 }, {  8, -25 }, { 12, -10 }, { 25,   5 }, { 25,  15 },
        { 15,  10 }, {  0,  20 },
        { -15, 10 }, { -25, 15 }, { -25,  5 }, { -12, -10 }, { -8, -25 },
    },
    turrets = {},

    spawn_weight = 75.0,
    max_active   = 6,
    threat_cost  = 3,

    color = { r = 200, g = 70, b = 55 },
}

-- ----------------------------------------------------------------------------
-- BARGE -- heavy cruiser. Sustained-pressure anchor.
--
-- Three things make this a capital ship rather than a fat Raider:
--
--  1. THE TURRET DOES THE FIGHTING. facing_mode = "velocity" means the hull
--     never turns to face you. It holds a wide orbit like a warship at sea,
--     showing its broadside, while a 360-degree turret tracks independently.
--     You cannot read its intent from where the nose points, because the nose
--     is irrelevant. The turret's traverse rate IS the counterplay: cut across
--     the swing and it has to catch up.
--
--  2. IT DOES NOT GET KNOCKED AROUND. stagger_resist 0.92, stun_resist 0.88.
--     A melee parry used to stunlock it and throw it across the arena, which
--     read as "large Raider" and made the tank identity a lie. A parry still
--     hurts and still interrupts -- it just no longer ragdolls a cruiser.
--
--  3. THE RAM IS UNSTOPPABLE ON PURPOSE. During the charge it takes zero
--     damage, cannot be parried, and clears every asteroid it passes through.
--     That is a hard rule, not a tuning number: the only correct answer is to
--     not be there. Everything else in the roster teaches "read the tell and
--     counter it"; this one teaches "read the tell and leave." The recovery
--     afterwards is longer than the charge -- that is where you get paid.
--
-- Hull: long, broad, flat-decked, sponson bulges amidships, engine block aft.
-- It should read as a gun platform before it fires.
-- ----------------------------------------------------------------------------
enemy_archetypes.BARGE = derive {
    display = "Barge",
    faction = "RAKSHARI",

    hull = {
        {   0, -78 },                                   -- bow
        {   9, -66 }, {  13, -46 },                     -- forward hull taper
        {  26, -34 }, {  24, -16 },                     -- forward gun deck shoulder
        {  34,  -8 }, {  33,  16 },                     -- sponson bulge (starboard)
        {  22,  22 },                                   -- waist
        {  27,  40 }, {  20,  52 },                     -- aft sponson
        {  24,  68 }, {  10,  62 }, {   8,  76 },       -- engine block + nozzle
        {   0,  66 },                                   -- centre exhaust notch
        {  -8,  76 }, { -10,  62 }, { -24,  68 },       -- engine block (port)
        { -20,  52 }, { -27,  40 },
        { -22,  22 },
        { -33,  16 }, { -34,  -8 },
        { -24, -16 }, { -26, -34 },
        { -13, -46 }, {  -9, -66 },
    },

    -- One mount, slightly forward of amidships, so the sweep covers both
    -- broadsides cleanly and the ship reads as "main battery + hull".
    turrets = { { x = 0, y = -14 } },

    scale                = 0.85,   -- ~130 units long against the Raider's 45.
                                   -- Roughly 2.9x: the "unmovable object" read
                                   -- without eating the whole screen.

    -- ===== MASS AND TOUGHNESS =====
    density              = 16.0,   -- WAS 9. Kinetic knockback should barely
                                   -- register on this.
    angulardrag_factor   = 6.0,
    hp                   = 2600.0,
    score_reward         = 3200,
    stagger_resist       = 0.92,
    stun_resist          = 0.88,

    -- ===== MOVEMENT =====
    engine_power         = 620.0,
    max_speed            = 15.0,   -- WAS 6.5. STRAFE multiplies this by ~8.5,
                                   -- so 6.5 asked for 61 px/s and the loop
                                   -- delivered 11. 15 asks for 125 and lands
                                   -- ~106 -- clearly slower than a Raider's
                                   -- ~150, which is the point, but a ship
                                   -- under way rather than a turret on a rock.
    lineardrag_factor    = 1.2,    -- WAS 2.4, which fought the engine harder
                                   -- than the engine could push.
    rotation_speed       = 1.5,    -- WAS 0.7. Still ponderous (~0.7s to settle
                                   -- onto a new heading) but the hull now
                                   -- tracks its own course instead of lagging
                                   -- a second behind it.
    facing_mode          = "velocity",   -- Broadside. See note 1 above.

    steer_mass_compensate = true,  -- REQUIRED on this hull. See defaults.
    fixed_aggression      = 0.45,  -- Calm. A cruiser holds its line; it does
                                   -- not lunge.

    -- ===== HULL GUNS: OFF =====
    -- The turret is the weapon. Leaving the hull gun on would fire shots from
    -- a barrel that is not pointing anywhere, which is exactly the confusion
    -- the turret exists to remove.
    fire_rate            = 999.0,
    attack_range         = 0.0,
    storm_enabled        = false,
    chaos_dodge_chance   = 0.0,
    bullet_dodge_chance  = 0.0,
    dodge_cooldown       = 999.0,
    personality_variance = false,

    -- ===== TURRET =====
    turret_enabled         = true,
    turret_traverse        = 78.0,   -- ~4.6s for a full 360. Slow enough that
                                     -- crossing its arc is real counterplay.
    turret_range           = 860.0,  -- Outranges everything else in the faction
    turret_size            = 13.0,
    turret_lead_target     = true,

    turret_aimed_telegraph = 0.45,
    turret_aimed_cooldown  = 1.4,   -- WAS 2.2
    turret_aimed_spread    = 2.5,

    turret_burst_count     = 4,
    turret_burst_interval  = 0.085,
    turret_burst_spread    = 16.0,
    turret_burst_cooldown  = 2.1,   -- WAS 3.4
    turret_burst_telegraph = 0.30,
    turret_burst_bias      = 0.45,

    bullet_speed           = 660.0,
    bullet_damage          = 26.0,
    bullet_lifetime        = 2.4,

    -- ===== RAM =====
    ram_enabled            = true,
    ram_windup             = 0.85,
    ram_charge_speed       = 1150.0,
    ram_charge_duration    = 1.3,
    ram_recover            = 2.0,
    ram_cooldown           = 15.0,
    ram_damage             = 110.0,
    ram_far_trigger        = 720.0,
    ram_near_trigger       = 230.0,
    ram_surprise_bonus     = 2.2,

    -- ===== SPAWN =====
    spawn_weight         = 99.0,
    max_active           = 2,
    threat_cost          = 6,

    color = { r = 200, g = 70, b = 55 },
}


-- ============================================================================
-- ARCHETYPE ORDER -- APPEND ONLY
-- ============================================================================

archetype_order = { "WARDOG", "RAIDER", "BARGE" }


-- ============================================================================
-- FACTIONS
--
-- The director picks an active faction, then rolls a unit by spawn_weight,
-- subject to three gates: per-archetype max_active, per-faction max_active, and
-- the threat budget.
--
-- THREAT BUDGET: every live unit costs threat_cost against max_threat. With
-- max_threat = 12 and costs of 1/3/6, the field can be four Raiders, or two
-- Barges, or a Barge plus two Raiders -- but never two Barges AND a Raider
-- squad. A pure count cap cannot express that.
-- ============================================================================

factions = {

    RAKSHARI = {
        display        = "Rakshari Marauders",
        spawn_interval = 4.0,
        max_active     = 8,
        max_threat     = 12,
    },

    PHANTOM_LEGION = {
        display        = "Phantom Legion",
        spawn_interval = 6.0,
        max_active     = 5,
        max_threat     = 9,
    },
}

active_factions = { "RAKSHARI" }


-- ============================================================================
-- BACK-COMPATIBILITY
--
-- PhysicsSystem (despawn_radius) and RenderSystem's vision cone still read the
-- old global `enemy_config`. Pointing it at the Raider keeps both correct while
-- they are migrated. Same table, not a copy.
-- ============================================================================

enemy_config = enemy_archetypes.RAIDER