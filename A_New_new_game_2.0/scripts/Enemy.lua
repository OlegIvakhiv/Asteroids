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
-- @version 2.4 -- Maniac: skid rockets, suicide charge, thrown state
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

    -- "standard": the Raider's picker (strafe / reposition / fall back).
    -- "melee":    closes or lunges, never strafes, never retreats, never
    --             flinches away from a hit. See the Berserker.
    -- (Naval behaviour is still selected by facing_mode = "velocity".)
    maneuver_profile     = "standard",
    preferred_range      = 0.0,      -- >0 overrides the stand-off range derived
                                     -- from attack_range. A melee unit shoots
                                     -- from 400 but wants to live at 80.

    -- "erratic" is a third profile: short timers, frequent flips, no settled
    -- band. Being hard to LEAD is that unit's defence instead of armour.

    -- ===== MELEE PROFILE (ignored unless maneuver_profile = "melee") =====
    melee_circle_range   = 320.0,    -- Beyond this it runs straight in. Inside,
                                     -- it orbits -- still closing, but on a
                                     -- spiral instead of a line.
    melee_circle_inward  = 0.26,     -- Inward bite per unit of tangential
                                     -- travel. 0 would orbit forever; this
                                     -- guarantees the spiral reaches bash range.
    melee_cutoff_speed   = 90.0,     -- Player lateral speed above which the
                                     -- orbit direction is chosen to CUT THEM
                                     -- OFF rather than rolled at random.

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
    bullet_iframes       = 0.8,      -- Player i-frames when one of OUR rounds
                                     -- lands. 0.8 was DamageSystem's hardcode.
                                     -- A fast spray needs this short, or it
                                     -- can only ever land one round in three
                                     -- -- and each graze shields the player
                                     -- from that same unit's melee.
    fire_rate            = 1.8,
    attack_range         = 480.0,
    hold_fire_range      = 0.0,      -- >0: the gun is dead inside this radius.
                                     -- One threat at a time -- a unit that
                                     -- sprays while it closes makes the player
                                     -- dodge a bullet and parry a lunge on the
                                     -- same beat, and neither read survives it.
    burst_count          = 0,        -- >0: fire this many, then pause. Fire
    burst_pause          = 1.0,      -- with no rhythm has no gap to move into.
    melee_shot_clear     = 0.0,      -- No bash or ram until this long after the
                                     -- last round left the barrel, so rounds
                                     -- already in flight have resolved first.
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

    -- Added in 2.2. Every default reproduces the Barge's old behaviour.
    -- Integers stay integers: sol2 will not read 2.0 as an int.
    ram_max_trigger       = 1.0e9,   -- Far trigger fires only BELOW this. With
                                     -- near = 0 it becomes a mid-range band.
    ram_trigger_chance    = 0.9,     -- Roll when the range test passes...
    ram_reroll_delay      = 2.0,     -- ...and wait this long if it fails.
    ram_chain_min         = 1,       -- Charges per commit. A chain that
    ram_chain_max         = 1,       -- CONNECTS stops early (DamageSystem).
    ram_chain_windup      = 0.35,    -- Re-aim windup between links
    ram_windup_turn       = 6.0,     -- Hull turn rate during windup (1/s)
    ram_clear_reach       = 78.0,    -- Asteroid clearing radius while charging
    ram_iframes           = 1.0,     -- Player i-frames after a charge lands
    ram_knockback         = 1100.0,  -- px/s, mostly sideways out of the lane
    hit_confirm_cooldown  = 0.0,     -- After ANY melee connect (bash or ram),
                                     -- neither attack may start for this long.
                                     -- The anti-stunlock rule. See Berserker.

    -- ===== BASH (parriable melee) =====
    -- Windup -> lunge -> recover. The one attack in the roster you are
    -- MEANT to parry. The tell is drawn in the parry's own cyan, as a short
    -- crescent at the prow -- the ram's tell is amber and a long straight
    -- lane. The two must never be confusable at speed.
    bash_enabled          = false,
    bash_trigger_range    = 150.0,   -- Centre-to-centre distance to start
    bash_windup           = 0.38,    -- The read. parry_window is 0.3, so this
                                     -- leaves reaction time before it matters.
    bash_turn_rate        = 12.0,    -- Tracks through the windup, locks at lunge
    bash_coil_speed       = 70.0,    -- Eases BACKWARDS while winding up
    bash_lunge_time       = 0.16,
    bash_lunge_speed      = 950.0,   -- ~150px of lunge
    bash_reach            = 90.0,    -- Strike lands inside this...
    bash_arc_cos          = 0.30,    -- ...and inside this arc (~72 deg each side)
    bash_recoil           = 160.0,   -- Bounce off the impact
    bash_recover          = 0.35,    -- After a connect (or a parry-stun)
    bash_whiff_recover    = 0.60,    -- After a miss. Getting out of reach pays.
    bash_cooldown         = 1.1,
    bash_hit_cooldown     = 2.0,     -- After a LANDED bash. Anti-stunlock.
    bash_damage           = 30.0,
    bash_iframes          = 0.5,
    bash_knockback        = 950.0,
    bash_tell_color       = { r = 90, g = 255, b = 230 },

    -- ===== SKID ROCKETS =====
    -- Not a faster bullet: it tracks hard for rocket_track_time, then the
    -- steering collapses to rocket_skid_turn and it drifts on where it was
    -- pointed. You cannot outrun the opening turn; you CAN step out of the
    -- skid. A rocket that misses stays live on its fuse and detonates where
    -- it dies -- never a silent despawn.
    rocket_enabled        = false,
    rocket_count_min      = 1,
    rocket_count_max      = 2,
    rocket_spacing        = 0.22,    -- Between rounds of one volley
    rocket_cooldown       = 4.5,
    rocket_min_range      = 260.0,   -- Too close to arm and turn
    rocket_max_range      = 900.0,
    rocket_launch_spread  = 7.0,     -- Small jitter only. Rounds leave from the
                                     -- NOSE; a volley that fans out of the hull
                                     -- reads as a shotgun, not as aimed fire.
    rocket_fast_chance    = 0.35,    -- Chance a volley is instead ONE rocket at
    rocket_fast_speed_mult = 2.1,    -- this multiple of speed, same tracking.
                                     -- Salvo = a wall you route around; snipe =
                                     -- a shot you react to.
    parry_rocket_speed    = 1150.0,  -- After a parry. Must beat the sender's
                                     -- top speed or the reward has no target.
    parry_rocket_drag     = 0.75,    -- Bleeds off; stalls into its own blast
    parry_rocket_stall    = 170.0,
    rocket_speed          = 430.0,   -- Slow enough to read and to parry
    rocket_track_time     = 0.85,
    rocket_track_turn     = 260.0,   -- deg/s while tracking: hard to escape
    rocket_skid_turn      = 35.0,    -- deg/s after: committed, not cancelled
    rocket_fuse           = 3.5,
    rocket_arm_time       = 0.12,
    rocket_impact_damage  = 6.0,     -- The blast is the threat, not the poke
    rocket_blast_radius   = 95.0,    -- Tight. A rocket should punish standing
                                     -- in one spot, not delete the spot: at
                                     -- 150 there was often no clean ground
                                     -- left to dodge to, which reads as unfair
                                     -- rather than as pressure.
    rocket_blast_damage   = 38.0,    -- Up slightly: less area, same bite
    rocket_iframes        = 0.5,

    -- ===== FLOATING MINES =====
    -- Dropped behind him while moving, and in a cluster right after a volley.
    -- Arm, then a proximity trigger starts a FUSE the player can still walk
    -- out of: the hazard is the route it denies, not the damage. Destructible
    -- and detonated by gunfire, which makes clearing a lane a real option --
    -- and a mine shot next to a Rakshari still goes off on THEM.
    mine_enabled          = false,
    mine_interval         = 2.6,     -- Between drops, plus up to 0.9s jitter
    mine_max_active       = 4,       -- Per unit, counted live
    mine_min_speed        = 60.0,    -- No dropping while parked, or it lands
                                     -- on top of him and reads as a bug
    mine_arm_time         = 0.5,
    mine_fuse             = 2.0,     -- Once triggered. Long enough to leave.

    -- ===== MINE RUN =====
    -- A committed dash that lays the field ACROSS the player's ground instead
    -- of behind the Maniac's. Harmless to touch -- no damage, no i-frames --
    -- which is the whole separation from a Berserker charge.
    mine_run_enabled      = false,
    mine_run_windup       = 0.45,
    mine_run_time         = 1.15,    -- Long enough to actually lay a line
    mine_run_speed        = 760.0,
    mine_run_gap          = 0.0,     -- DISTANCE between drops, px. 0 = use
                                     -- mine_blast_radius, which makes the
                                     -- zones touch without overlapping. Timed
                                     -- spacing bunched the whole carpet into
                                     -- one clump covering one mine's ground.
    mine_run_recover      = 0.55,
    mine_run_cooldown     = 7.0,
    mine_run_turn         = 9.0,
    mine_run_min_range    = 240.0,
    mine_run_max_range    = 800.0,
    mine_run_lead         = 0.55,    -- How far ahead of the player the line is
                                     -- aimed. 0 chases their tail; too high
                                     -- and he runs at empty space.
    mine_trigger_radius   = 95.0,
    mine_blast_radius     = 130.0,
    mine_blast_damage     = 42.0,
    mine_hp               = 12.0,    -- ~2 player rounds
    mine_lifetime         = 22.0,    -- Eventual cleanup so a long fight does
                                     -- not leave a permanent minefield

    -- ===== MICRO-RECOVERY =====
    -- A window after a volley where no attack may start. He still moves, so
    -- it reads as reloading rather than as a stun -- and it is the only punish
    -- window this unit has, since unlike the Berserker he never commits to a
    -- long attack you can wait out.
    micro_recover         = 0.8,

    -- ===== SUICIDE CHARGE =====
    -- One-way. No cooldown, no exit, no interrupt -- not even a stun.
    suicide_enabled       = false,
    suicide_hp_fraction   = 0.3,
    suicide_ignite_time   = 0.8,     -- Colour shift + laugh. Short but LOUD:
                                     -- miss this beat and the charge is unfair.
    suicide_speed         = 700.0,
    suicide_turn_rate     = 5.0,     -- Steered, not railed: sidestepping him
                                     -- should not be the answer -- parry or
                                     -- kill him should be.
    suicide_max_time      = 9.0,     -- Safety valve: goes off rather than
                                     -- charging forever
    suicide_blast_radius  = 260.0,
    suicide_blast_damage  = 75.0,
    suicide_fuse          = 5.0,     -- Runs during the charge. He detonates
                                     -- when it is out AND you are inside the
                                     -- blast -- a countdown you can out-run,
                                     -- not a touch of death.
    suicide_detonate_fraction = 0.7, -- Of the blast radius: how close you must
                                     -- be for the fuse running out to mean you
    suicide_grace         = 1.0,     -- If you are not, he gets this long to
                                     -- close before going off anyway.

    -- ===== THROWN (parried mid-charge) =====
    thrown_fuse           = 1.5,
    thrown_speed          = 1150.0,  -- Raised automatically if the fuse and
    thrown_clearance      = 1.35,    -- radius would otherwise land him on you
    thrown_spin           = 16.0,
    thrown_blast_mult     = 1.35,    -- Parry is the highest-risk answer, so it
    thrown_damage_mult    = 1.4,     -- has to be the biggest bang

    -- ===== PERSONALITY ROLL =====
    personality_variance  = true,

    -- ===== EXHAUST =====
    -- Numeric defaults match the old hardcoded exhaust exactly. There is
    -- deliberately NO thruster_color here: its absence is what keeps the old
    -- red-orange jitter, and derive() would otherwise copy it onto every unit.
    -- `thrusters = { {x,y}, ... }` sets nozzles; unset = one at (0, 22).
    thruster_rate         = 0.5,     -- Per nozzle per frame. >1 = several.
    thruster_speed        = 80.0,
    thruster_size         = 2.0,
    thruster_life         = 0.10,
    thruster_glow         = 0.0,     -- Hull flame length. 0 = none (old look).
                                     -- >0 also makes the exhaust react to
                                     -- charge / lunge / coil.

    -- ===== DEATH =====
    death_style           = "standard",   -- "visceral": hull splits into shards
    death_shards          = 7,
    death_trauma          = 0.38,         -- visceral only

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

    spawn_weight = 150.0,
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
    ram_damage             = 40.0,   -- WAS 110, but that number was never read:
                                     -- DamageSystem applied a flat 15 on any
                                     -- contact. Wired for real in 1.4, 110
                                     -- would one-shot a stock hull (~100 HP).
                                     -- 40 + the stagger is a real punishment
                                     -- without being a coin-flip death.
    ram_far_trigger        = 720.0,
    ram_near_trigger       = 230.0,
    ram_surprise_bonus     = 2.2,

    -- ===== SPAWN =====
    spawn_weight         = 120.0,
    max_active           = 2,
    threat_cost          = 6,

    color = { r = 200, g = 70, b = 55 },
}


-- ----------------------------------------------------------------------------
-- BERSERKER -- close-range pressure check. Punishes kiting everything.
--
-- The whole unit is one question asked at speed: "which tool?"
--
--   BASH    point-blank, CYAN crescent at the prow, hull coils back.
--           PARRY IT. Parried, it is stunned and thrown -- the reward.
--   CHARGE  mid-range, AMBER glow + long lane line, hull goes white-hot.
--           DO NOT PARRY IT. It is the Barge's ram contract: untouchable
--           while charging, and a parry whiffs AND you still eat the hit.
--           Chains 2-3 times, re-aiming each link. A link that CONNECTS
--           ends the chain -- then a 1.2s recover. That is the punish window.
--
-- Between attacks it sprays: cheap, fast, short i-frames. It punishes
-- standing off and breaking line of sight late, not standing close.
--
-- hit_confirm_cooldown 1.8 + bash_hit_cooldown 2.2: once it lands anything,
-- it cannot start another attack until the player has had real control back.
-- Without that, bash -> tumble -> bash is a loop with no input that answers it.
--
-- Hitbox: the mandible gap is filled in by convexity, so the raw ratio is
-- ~1.58. hitbox_scale 0.9 brings it to ~1.28. Shots "between the horns"
-- still land -- that reads as hitting its face, which is fine for a unit you
-- are shooting head-on as it comes.
-- ----------------------------------------------------------------------------
enemy_archetypes.BERSERKER = derive {
    display = "Berserker",
    faction = "RAKSHARI",

    hull = {
        {   0, -35 }, {   5, -20 }, {  18, -40 },           -- nose, starboard mandible
        {  16, -10 }, {  28,   0 }, {  18,  10 },           -- wing
        {  12,   5 }, {   0,  25 },                         -- engine notch, tail
        { -12,   5 }, { -18,  10 }, { -28,   0 },           -- port wing
        { -16, -10 }, { -18, -40 }, {  -5, -20 },           -- port mandible
    },
    turrets = {},

    -- Scars. Polylines in hull space, every point verified inside the
    -- silhouette with >=1.2px clearance. Badge of status per the roster.
    scars = {
        { {   9, -8 }, {  15, -3 }, {  22, 1 } },           -- long gouge, starboard wing
        { {  -5, -13 }, { -10, -6 } },                      -- claw rake, three lines
        { {  -3,  -9 }, {  -8, -2 } },
        { {  -1,  -5 }, {  -6,  2 } },
        { { 0.3, -26 }, { -0.8, -22.5 }, { 0.6, -19 } },    -- cracked nose
        { { -15, -3 }, { -22, 1.5 } },                      -- port wing slash
    },
    scar_width = 1.6,

    -- Twin engines. Sunk ~2.5px INSIDE the hull, with ~5px of plating aft of
    -- each before the silhouette opens: the flame root is hidden and the
    -- flame emerges through the tail notch rather than starting in space.
    thrusters = { { 9, 5 }, { -9, 5 } },

    scale                = 1.18,   -- +18%. At 1.0 it read as a Raider from
                                   -- across the arena; radius 44 -> 52 against
                                   -- the Raider's 29, so the silhouette is
                                   -- unmistakable before the tells start.
    hitbox_scale         = 0.9,

    -- ===== MASS AND TOUGHNESS =====
    -- ~9kg against the Raider's ~5 and the Barge's ~93: the midpoint.
    -- Area grew ~39% with the scale, so mass goes 7.2kg -> 12.6kg at this
    -- density. Engine power below is raised to pay for it: the steering
    -- controller is mass-dependent, so a bigger hull on the old 420 would
    -- have accelerated ~40% worse -- the opposite of what this pass wants.
    density              = 5.0,
    hp                   = 440.0,  -- Bigger target, slightly more to chew
    score_reward         = 900,
    stagger_resist       = 0.25,   -- Harder to knock around than a Raider...
    stun_resist          = 0.10,   -- ...but a parried bash still SHUTS IT DOWN.
                                   -- Keep this low: the stun is the reward.

    -- ===== MOVEMENT =====
    engine_power         = 700.0,  -- Pays for the mass AND the speed bump
    max_speed            = 30.0,   -- ATTACK_RUN ~650 px/s, CIRCLE ~500.
                                   -- A walking player cannot open the gap;
                                   -- sprinting still can, which is the answer.
    rotation_speed        = 7.0,
    lineardrag_factor     = 0.85,  -- Less drag fighting the chase
    angulardrag_factor    = 3.0,
    maneuver_profile      = "melee",
    preferred_range       = 80.0,

    -- ===== WOLF CIRCLE =====
    melee_circle_range   = 330.0,  -- Same radius as hold_fire_range on purpose:
                                   -- the gun goes quiet at the exact moment
                                   -- the orbit starts, so "it stopped shooting
                                   -- and started circling" is one event.
    melee_circle_inward  = 0.24,
    melee_cutoff_speed   = 90.0,
    personality_variance = false,  -- Raiders alone get the personality roll
    fixed_aggression     = 0.95,

    -- ===== PERCEPTION: commits fast, gives up late =====
    suspicion_rate       = 1.8,
    suspicion_combat     = 0.35,
    combat_lose_time     = 6.0,
    combat_lose_distance = 1300.0,

    -- ===== SPRAY (hull gun) =====
    fire_rate            = 0.24,
    telegraph_time       = 0.0,    -- Minimal telegraph: volume is the threat
    aim_spread           = 11.0,
    attack_range         = 520.0,  -- Sprays across the 330..520 band while it
                                   -- closes, and nowhere else.
    bullet_speed         = 620.0,
    bullet_lifetime      = 1.0,    -- ~620px
    bullet_damage        = 5.0,
    bullet_iframes       = 0.15,

    -- Five rounds (~1.2s), then a real breather with a visible sway. This is
    -- the "recovery after a series" -- continuous fire has no rhythm to learn.
    burst_count          = 5,
    burst_pause          = 1.05,

    -- Gun off inside the orbit, and no melee commit until 0.55s after the last
    -- round -- long enough for a shot fired at the hold-fire boundary (330px
    -- at 620px/s = 0.53s) to have landed or missed before the lunge starts.
    hold_fire_range      = 330.0,
    melee_shot_clear     = 0.55,

    storm_enabled        = false,  -- The storm is a Raider/Maniac move
    chaos_dodge_chance   = 0.12,   -- No retreat instinct...
    bullet_dodge_chance  = 0.25,   -- ...and it would rather tank it

    -- ===== BASH =====
    -- Triggers further out than the default 150 so the windup starts BEFORE
    -- the player is already inside the hull, and the lunge is lengthened to
    -- match (1150 * 0.19 = ~220px of travel) or it would whiff every time.
    bash_enabled         = true,
    bash_trigger_range   = 210.0,
    bash_windup          = 0.40,
    bash_lunge_speed     = 1150.0,
    bash_lunge_time      = 0.19,
    bash_reach           = 105.0,
    bash_damage          = 32.0,
    bash_knockback       = 950.0,
    bash_cooldown        = 1.1,
    bash_hit_cooldown    = 2.2,

    -- ===== RAM CHAIN =====
    ram_enabled          = true,
    ram_windup           = 0.60,   -- Opener: moderate
    ram_chain_windup     = 0.36,   -- Each re-aim: short, still announced
    ram_windup_turn      = 12.0,
    ram_charge_speed     = 1250.0,
    ram_charge_duration  = 0.42,   -- ~525px per link
    ram_recover          = 1.2,    -- The punish window after the chain
    ram_cooldown         = 5.5,
    ram_damage           = 26.0,
    ram_iframes          = 0.9,
    ram_knockback        = 1000.0,
    ram_near_trigger     = 0.0,    -- Close range belongs to the bash
    ram_far_trigger      = 240.0,  -- Charges across the 240..720 band
    ram_max_trigger      = 720.0,
    ram_trigger_chance   = 0.55,   -- ...or keeps closing to bash instead
    ram_reroll_delay     = 0.8,
    ram_surprise_bonus   = 1.3,
    ram_chain_min        = 2,
    ram_chain_max        = 3,
    ram_clear_reach      = 55.0,
    hit_confirm_cooldown = 1.8,

    -- ===== LOOK =====
    -- Brighter and hotter than a Raider: its threat reads through motion.
    thruster_rate        = 1.4,
    thruster_speed       = 150.0,
    thruster_size        = 3.0,
    thruster_life        = 0.14,
    thruster_color       = { r = 255, g = 190, b = 110, a = 230 },
    thruster_glow        = 1.0,

    death_style          = "visceral",
    death_shards         = 7,
    death_trauma         = 0.38,

    -- Bloodseeker aura: receives it at full value (roster default). Nothing
    -- to wire until the Bloodseeker exists.

    -- ===== SPAWN =====
    spawn_weight         = 70.0,
    max_active           = 3,
    threat_cost          = 4,      -- Three of them fill the 12 budget

    color = { r = 200, g = 70, b = 55 },
}


-- ----------------------------------------------------------------------------
-- MANIAC -- mobile hazard generator.
--
-- Where the Berserker asks "which defensive tool?", the Maniac asks "where is
-- it safe to stand, and is it worth killing him now?".
--
--   SCRAPFIRE  noise and chip. Explicitly NOT the threat; it exists to make
--              standing still unpleasant while the real kit cycles.
--   ROCKETS    1-2 skid rockets. Track hard, then drift. PARRIABLE -- and a
--              parried one is not deleted, it goes wild on its remaining fuse
--              and hurts whoever it reaches. Friendly fire is on.
--   BASH       borrowed from the Berserker at lower value. A panic tool, not
--              an identity; short trigger range and a long cooldown.
--   SUICIDE    below 35% HP he ignites, drops the ranged kit and comes at you.
--              Parry it and he becomes a spinning bomb thrown along the parry.
--              Kill him first and the blast is roughly half -- so "shoot him
--              down early" stays the safe play and therefore a real decision.
--
-- No stagger or stun resistance on purpose: he is a hazard dispenser, not a
-- brawler, and interrupting him should feel like the reward for closing.
--
-- Hitbox: the split jaws and the flank cavities are filled in by convexity,
-- so the raw ratio is ~1.59. hitbox_scale 0.88 brings it to ~1.23.
-- ----------------------------------------------------------------------------
enemy_archetypes.MANIAC = derive {
    display = "Maniac",
    faction = "RAKSHARI",

    hull = {
        {   0, -22 }, {   3,  -8 },                      -- central drill spire
        {  12, -28 }, {  16, -12 }, {   8,  -2 },         -- right jaw + cavity
        {  26,   8 }, {  20,  22 }, {  10,  15 },         -- right outward flank
        {   0,  24 },                                     -- rear centre
        { -10,  15 }, { -20,  22 }, { -26,   8 },         -- left outward flank
        {  -8,  -2 }, { -16, -12 }, { -12, -28 },         -- left jaw + cavity
        {  -3,  -8 },
    },
    turrets = {},

    -- Asymmetric on purpose. The hull itself is mirrored, so the "someone kept
    -- bolting things on until it stopped being sane" read has to come from the
    -- plating and the engine count -- five weld lines, weighted to starboard.
    scars = {
        { {  6,  2 }, { 14,  6 }, { 19, 12 } },
        { {  4, -4 }, {  9,  1 } },
        { { -14, 10 }, { -19, 15 } },
        { { -6,  0 }, { -11,  6 }, { -9, 12 } },
        { {  1,  6 }, {  3, 12 } },
    },
    scar_width = 1.9,

    -- THREE engines, and the third is off-centre. An odd, unbalanced count
    -- reads as overloaded at a glance, and the flare never looks symmetric.
    thrusters = { { 8, 10 }, { -8, 10 }, { 16, 14 } },

    -- Area 1294 vs the Berserker's 1625, so it needs a larger scale to land at
    -- the same mass: 1.30 gives ~12.2kg against the Berserker's ~12.6. Radius
    -- 39.6 -- between the Raider's 29 and the Berserker's 52, and stubbier
    -- than either, so all three read apart at distance.
    scale                = 1.30,
    hitbox_scale         = 0.88,

    density              = 5.0,
    hp                   = 300.0,  -- Squishier than a Berserker: killing him
                                   -- early is supposed to be viable
    score_reward         = 850,
    stagger_resist       = 0.0,
    stun_resist          = 0.0,

    -- ===== MOVEMENT =====
    engine_power         = 560.0,
    max_speed            = 26.0,
    rotation_speed       = 7.0,
    angulardrag_factor   = 3.0,
    maneuver_profile     = "erratic",
    preferred_range      = 420.0,  -- Wants to live inside rocket range
    personality_variance = false,
    fixed_aggression     = 0.85,

    -- ===== PERCEPTION =====
    suspicion_rate       = 1.5,
    suspicion_combat     = 0.4,
    combat_lose_time     = 5.0,
    combat_lose_distance = 1400.0,

    -- ===== SCRAPFIRE (backup only) =====
    -- Telegraphed like a Raider's, but slower and far less accurate: you see
    -- the charge, then 3 wild rounds. It exists to stop the player parking at
    -- mid range between volleys, not to kill anyone.
    fire_rate            = 0.42,
    telegraph_time       = 0.55,
    aim_spread           = 22.0,
    attack_range         = 560.0,
    bullet_speed         = 480.0,
    bullet_lifetime      = 1.3,
    bullet_damage        = 5.0,
    bullet_iframes       = 0.15,
    burst_count          = 3,
    burst_pause          = 1.6,

    storm_enabled        = false,
    chaos_dodge_chance   = 0.30,   -- Twitchy, and it shows
    bullet_dodge_chance  = 0.45,

    parry_rocket_speed   = 1150.0, -- Fast enough to run him down with his own
    parry_rocket_drag    = 0.75,   -- rocket before it stalls and goes off
    parry_rocket_stall   = 170.0,

    -- ===== ROCKETS (primary) =====
    rocket_enabled       = true,
    rocket_count_min     = 2,
    rocket_count_max     = 3,
    rocket_spacing       = 0.20,
    rocket_cooldown      = 3.6,
    rocket_min_range     = 260.0,
    rocket_max_range     = 950.0,
    rocket_fast_chance   = 0.35,
    rocket_fast_speed_mult = 2.2,
    micro_recover        = 0.85,

    -- ===== MINES (primary) =====
    mine_enabled         = true,
    mine_interval        = 3.2,      -- Trail drops are the slow drip; the run
                                     -- below is where the field comes from
    mine_max_active      = 8,      -- The run alone lays ~7
    mine_trigger_radius  = 95.0,
    mine_blast_radius    = 130.0,
    mine_blast_damage    = 40.0,

    mine_run_enabled     = true,
    mine_run_cooldown    = 6.5,
    mine_run_speed       = 780.0,
    mine_run_time        = 1.25,     -- ~975px of run...
    mine_run_gap         = 140.0,    -- ...at one blast radius apart = ~7 mines
                                     -- spanning a wall, not a pile

    -- ===== BASH (secondary panic tool) =====
    bash_enabled         = true,
    bash_trigger_range   = 120.0,
    bash_windup          = 0.34,
    bash_lunge_speed     = 900.0,
    bash_lunge_time      = 0.15,
    bash_reach           = 85.0,
    bash_damage          = 22.0,
    bash_knockback       = 1150.0,  -- High knockback, low damage: it is for
                                    -- making space, not for killing
    bash_cooldown        = 2.6,
    bash_hit_cooldown    = 3.0,
    hold_fire_range      = 150.0,
    melee_shot_clear     = 0.35,

    ram_enabled          = false,

    -- ===== SUICIDE =====
    suicide_enabled      = true,
    suicide_hp_fraction  = 0.35,
    suicide_speed        = 720.0,
    suicide_blast_radius = 270.0,
    suicide_blast_damage = 78.0,
    suicide_fuse         = 5.0,
    suicide_grace        = 1.0,
    thrown_fuse          = 1.5,
    thrown_blast_mult    = 1.4,
    thrown_damage_mult   = 1.45,

    -- ===== LOOK =====
    thruster_rate        = 1.1,
    thruster_speed       = 130.0,
    thruster_size        = 3.0,
    thruster_life        = 0.13,
    thruster_color       = { r = 255, g = 175, b = 95, a = 225 },
    thruster_glow        = 0.85,

    death_style          = "visceral",
    death_shards         = 8,
    death_trauma         = 0.34,

    -- ===== SPAWN =====
    spawn_weight         = 50.0,
    max_active           = 2,
    threat_cost          = 4,

    -- Standard Rakshari red. He is identified by SHAPE -- split jaws, outward
    -- flanks, three uneven engines -- and by the frenzy shift when it matters,
    -- not by being a different colour at rest.
    color = { r = 200, g = 70, b = 55 },
}


-- ============================================================================
-- ARCHETYPE ORDER -- APPEND ONLY
-- ============================================================================

archetype_order = { "WARDOG", "RAIDER", "BARGE", "BERSERKER", "MANIAC" }


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