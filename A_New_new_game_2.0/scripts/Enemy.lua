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
-- @version 3.4 -- squad AI: attack turns (1-2), stalking, Maniac moods, pack sight, chase
-- 3.3 -- squad AI: roles, threat budget, engagement ring, flanking
-- 3.2 -- playtest: charged bursts (3 rapid), Barge slug / 6-round
--                barrage / pellet shotgun, committed execution, lancer beam
-- 3.1 -- Barge (+35%, turret eye, shotgun, summon), Maniac (curved
--                carpet, mine decay, mine toss), Wardog chaff, burst windups
--                (Berserker, Bloodseeker melee), death-chaos 10s + melee
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
    score_reward         = 500,     -- no longer a score: kept as a size tag, see EntityManager
    -- SCRAP: the currency. Rolled in [min, max] on a kill and thrown out as
    -- cubes the player's magnet collects. Priced by how hard the unit is to
    -- kill, how dangerous it is and how rarely it shows up. Despawned units
    -- (flew out of range) drop nothing -- only kills pay.
    scrap_drop           = { 3, 5 },
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
    burst_windup         = 0.0,      -- >0: CHARGED BURST instead -- charge this
                                     -- long (tracking, ship still flies), then
                                     -- burst_count rounds burst_interval apart,
                                     -- committed. telegraph_time is unused.
    burst_interval       = 0.07,
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
    -- COMBAT is lost only after this long unseen AND this far away. Pack
    -- sight (squad_ai) keeps the clock at zero while any packmate sees you.
    combat_lose_time     = 6.0,      -- WAS the 3.2 code default
    combat_lose_distance = 1700.0,   -- WAS the 950 code default

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
    turret_size           = 10.0,    -- TurretModel unit, px: housing ~1.7x this
                                     -- across, barrels 2.25x long. Live and
                                     -- dormant/dead guns both use it.
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

    -- Turret eye + search sweep (0 range = no eye). Shotgun and summon are
    -- off unless an archetype turns them on; see BARGE.
    turret_vision_range    = 0.0,
    shotgun_enabled        = false,
    summon_enabled         = false,

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
    -- Laid by the mine run and the mine toss. The old passive trail (dropped
    -- behind him while moving, a cluster after a volley) is behind
    -- mine_trail_enabled and off by default. Arm, then a proximity trigger starts a FUSE the player can still walk
    -- out of: the hazard is the route it denies, not the damage. Destructible
    -- and detonated by gunfire, which makes clearing a lane a real option --
    -- and a mine shot next to a Rakshari still goes off on THEM.
    mine_enabled          = false,
    mine_trail_enabled    = false,   -- the passive trail; mine_interval below
    mine_interval         = 2.6,     -- Between trail drops, plus up to 0.9s jitter
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
    mine_lifetime         = 22.0,    -- Decay: an UNLIT mine fizzles (no blast)
                                     -- after this, freeing its slot. A lit
                                     -- one always finishes its fuse.

    -- ===== MINE RUN SHAPE / MINE TOSS (Maniac) =====
    mine_run_straight_weight = 1.0,  -- per-run roll: straight / arc / S
    mine_run_arc_weight   = 0.0,
    mine_run_s_weight     = 0.0,
    mine_run_curve        = 95.0,    -- deg/s of bend on an arc or S
    toss_enabled          = false,   -- spin + ring of lit mines; see MANIAC

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
    death_style           = "standard",   -- "visceral": tight blast, pieces thrown harder
    death_shards          = 7,            -- old triangle shards, only when wreckage = false
    death_trauma          = 0.38,         -- visceral only

    -- ===== WRECKAGE (decorative -- nothing here collides) =====
    -- On death the hull is cut into pieces of itself and the plates tear off
    -- whole. Pieces hold for wreck_life, then fade over wreck_fade. Thrown
    -- harder on a visceral death (x1.6) and harder still when blown up by a
    -- Maniac charge (x2.2).
    wreckage              = true,
    wreck_cuts            = 0,                -- straight cuts; 0 = by size (2 / 3 / 4)
    wreck_life            = { 2.0, 3.0 },     -- s at full opacity, rolled per piece
    wreck_fade            = 0.8,              -- s to fade out after that
    wreck_speed           = { 40.0, 160.0 },  -- px/s outward, hull pieces
    wreck_plate_speed     = { 80.0, 240.0 },  -- px/s outward, armour plates
    wreck_spin            = 230.0,            -- deg/s max tumble
    wreck_inherit         = 0.6,              -- share of the ship's velocity kept
    wreck_drag            = 0.45,             -- 1/s; higher = settles sooner
    wreck_char            = 0.45,             -- hull colour x this = burnt fill
    wreck_cool_time       = 1.1,              -- s for a fracture edge to go cold
    wreck_plates          = true,

    -- ===== SPAWN DIRECTOR =====
    faction               = "RAKSHARI",
    spawn_weight          = 0.0,
    max_active            = 4,
    threat_cost           = 3,
    summon_only           = false,

    -- ===== AMBUSH (spawned TURNED OFF) =====
    -- A director spawn can arrive powered down: drifting like a wreck, drawn
    -- as this ship's own undamaged wreck (tier 0), no AI, no engines, no
    -- vision cone. Plain wrecks never look like that, so a clean dark hull
    -- is always one of these -- the only tell, and a fair one.
    --
    -- It wakes, straight into COMBAT, on either:
    --   * the player within ambush_wake_range of its HULL EDGE -- any
    --     direction, 360 degrees
    --   * any damage the PLAYER deals it, from any range (bullets, rift,
    --     ram, parried rounds/rocks, blasts the player caused). Stray rocks
    --     and other pirates' fire do NOT wake it.
    --
    -- Waking is a short reboot (ambush_wake_time): the hull flickers from
    -- cold to live and swings toward you, and cannot fire or lunge yet.
    -- Set it to 0 for a zero-warning wake.
    ambush_chance         = 0.0,     -- share of this unit's director spawns
    ambush_wake_range     = 220.0,   -- px beyond the hull edge
    ambush_wake_time      = 0.45,    -- s of reboot before it may attack
    ambush_spawn_distance = 1250.0,  -- px, placed AHEAD of the player's heading
    ambush_drift_speed    = { 0.6, 1.8 },   -- m/s while dormant
    ambush_spin           = 0.35,    -- rad/s max tumble while dormant
    -- ambush_cold_color = { r=, g=, b= }   -- optional; default is the colour
    --                                         of this unit's wreck type

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
-- overlay(base, overrides) -- a full copy of an ARCHETYPE plus overrides
--
-- For units that fight in more than one mode (the Bloodseeker's `duel_range`).
-- Same rule as derive(): a plain, complete table, no metatables -- so the
-- engine reads every key the mode needs directly, never through a fallback
-- chain sol2 could silently default on. The sub-table being built is skipped
-- so a mode never contains a copy of itself.
-- ============================================================================

local function overlay(base, overrides)
    local out = {}
    for k, v in pairs(base) do
        if k ~= "duel_range" then out[k] = copyValue(v) end
    end
    for k, v in pairs(overrides) do out[k] = copyValue(v) end
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

    -- Plate COUNT is the tier read: fodder gets one strip, the Barge gets
    -- eight. Authored starboard-only and mirrored at load unless `mirror =
    -- false` (a plate that already crosses the centreline would double up --
    -- which is exactly how the Barge ended up half-plated).
    -- `shade` multiplies the LIVE hull fill, so panels flash and ramp with it
    -- instead of sitting inert through hit flash and frenzy.
    plates = {
        { points = { { 0, -14 }, { 1.9, -4.3 }, { 0, 5 }, { -1.9, -4.3 } },
          shade = 1.45, mirror = false, accent = true },     -- one welded strip
    },
    turrets = {},

    scale                = 1.50,   -- WAS 1.15 (0.85 before that). +30% in the
                                   -- size playtest: still the smallest hull in
                                   -- the roster, now plainly a ship rather
                                   -- than a speck next to the player.
    density              = 1.54,   -- WAS 2.6. Area grew 1.69x with the scale;
                                   -- density drops by the same factor so the
                                   -- mass, and with it every handling number,
                                   -- is exactly what it was.
    hp                   = 35.0,   -- WAS 60. One splash hit, or two plasma bolts.
    bullet_damage        = 5.0,    -- WAS 9 (25 default before that). Chaff:
                                   -- the pressure is the numbers, not a round.
    bullet_poise_mult    = 0.1,    -- next to no poise wear: a swarm softens you,
                                   -- it cannot set up a tumble on its own
    score_reward         = 45,
    scrap_drop           = { 0, 1 },     -- summoned chaff: next to nothing, or the summoner is a farm
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

    -- ===== SQUAD =====
    squad_role           = "harasser",   -- free: the swarm never waits

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

    plates = {
        { points = { { 0, -6 }, { 6, -18 }, { 9, -8 }, { 0, 12 } },
          shade = 1.40, accent = true },                     -- prow cheek
        { points = { { 14, -2 }, { 22, 6 }, { 20, 12 }, { 12, 8 } },
          shade = 0.55 },                                    -- wing patch
    },
    turrets = {},

    -- +30% (playtest): it read smaller than a medium player ship. Density
    -- falls by the area ratio (1.69x) so the MASS -- and with it acceleration,
    -- knockback and every tuned handling number -- is exactly what it was.
    scale        = 1.30,
    density      = 2.37,

    -- ===== PACK =====
    -- Lowest rank a Bloodseeker will execute. Wardogs, Maniacs and Barges
    -- have no rank: he never executes them.
    execute_rank = 1,

    spawn_weight = 150.0,
    scrap_drop   = { 3, 5 },             -- common line unit: a little, often
    max_active   = 6,
    threat_cost  = 3,

    -- ===== SQUAD: fire support =====
    squad_role       = "support",        -- free, never waits
    squad_min_range  = 360.0,            -- covering fire stands off
    ambush_chance = 0.15,

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

    -- Layered belts rather than one slab: a heavy is armoured in BANDS, and
    -- the count is what sells it as the anchor of the roster.
    plates = {
        { points = { { -7, -58 }, { 7, -58 }, { 10, -10 }, { 9, 30 }, { 5, 56 }, { -5, 56 }, { -9, 30 }, { -10, -10 } },
          shade = 1.30, mirror = false, accent = true },     -- dorsal spine, full width
        { points = { { 12, -44 }, { 22, -31 }, { 21, -14 }, { 12, -16 } },
          shade = 0.55 },                                    -- forward belt
        { points = { { 13, -6 }, { 29, -4 }, { 28, 14 }, { 13, 12 } },
          shade = 1.22, accent = true },                     -- mid flank band
        { points = { { 11, 24 }, { 22.6, 25.7 }, { 20, 48 }, { 10, 44 } },
          shade = 0.55 },                                    -- aft skirt
        { points = { { 0, -72 }, { 7, -62 }, { 0, -48 }, { -7, -62 } },
          shade = 1.60, mirror = false, accent = true },     -- prow wedge
    },
    turrets = { { x = 0, y = -14 } },

    scale                = 1.15,   -- WAS 0.85, which put him (r 66) level with
                                   -- the Bloodseeker (r 63). Now r ~90: ~40%
                                   -- past the elite, plainly the capital ship.

    -- ===== MASS AND TOUGHNESS =====
    density              = 8.74,   -- WAS 16 at scale 0.85. Area grew 1.83x
                                   -- with the scale; density falls by the same
                                   -- factor so the MASS -- knockback, ram, every
                                   -- handling number -- is exactly what it was.
    angulardrag_factor   = 6.0,
    hp                   = 2600.0,
    score_reward         = 3200,
    scrap_drop           = { 30, 40 },   -- 2600 HP of salvage: the jackpot
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
    turret_size            = 17.0,   -- WAS 13, with the hull. TurretModel draws
                                     -- the live AND the dormant/dead gun at this
                                     -- size: nothing shrinks when it dies.
    turret_lead_target     = true,

    -- ===== TURRET EYE =====
    -- Out of combat the gun searches (a slow sweep about the heading; on
    -- alert a tighter one about where you were last seen) and SEES along
    -- its barrels. Either eye spotting you puts the whole ship in the fight.
    turret_vision_range    = 780.0,
    turret_vision_fov      = 50.0,   -- narrow: a searchlight, not a radar
    turret_scan_arc        = 150.0,
    turret_scan_rate       = 0.12,   -- sweeps per second (~8s a full pass)
    turret_scan_alert_arc  = 0.55,

    -- ===== SHOTGUN BLAST =====
    -- The answer to parking under the guns. Inside 260px, on its own
    -- cooldown: the gun charges (wedge, tracking fast), LOCKS for the last
    -- 0.28s, then 11 ordinary rounds leave at once, spread across the wedge
    -- and living exactly its range. Point-blank most of them hit you, the
    -- far edge one or two. Parry reflects them; dodge i-frames pass.
    shotgun_enabled        = true,
    shotgun_trigger_range  = 260.0,
    shotgun_trigger_arc    = 35.0,   -- gun must already be roughly on you
    shotgun_windup         = 0.85,
    shotgun_lock           = 0.28,
    shotgun_track_mult     = 1.8,
    shotgun_range          = 320.0,
    shotgun_half_angle     = 30.0,
    shotgun_pellets        = 11,
    shotgun_pellet_speed   = 950.0,
    shotgun_pellet_damage  = 7.0,
    shotgun_pellet_iframes = 0.02,   -- so several pellets of one blast can land
    shotgun_cooldown       = 7.0,
    shotgun_recover        = 0.9,    -- main gun quiet after it

    turret_aimed_telegraph = 0.45,
    turret_aimed_cooldown  = 1.4,   -- WAS 2.2
    turret_aimed_spread    = 2.5,
    -- The aimed shot is one HEAVY SLUG out of both barrels: big, pale, a hot
    -- trace behind it, faster and harder than a burst round.
    turret_slug_speed_mult = 1.6,   -- x bullet_speed: ~1060 px/s
    turret_slug_damage     = 40.0,
    turret_slug_iframes    = 0.8,

    turret_burst_count     = 6,     -- WAS 4 (playtest: +2)
    turret_burst_interval  = 0.085,
    turret_burst_spread    = 22.0,  -- WAS 16: six rounds need the wider fan
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
    ram_clear_reach        = 105.0,  -- WAS the 78 default; the hull grew 35%

    -- ===== SUMMON: WARDOGS =====
    -- The hull CHARGES (slows, swells across the beam, sponson hatches glow
    -- and vent) and SPITS two Wardogs out of both sides. They arrive already
    -- fighting and stay on his leash. The turret holds fire through the
    -- charge -- that is the tell, and the window. A stagger/stun mid-charge
    -- breaks it: nothing comes out.
    summon_enabled         = true,
    summon_key             = "WARDOG",
    summon_count           = 2,
    summon_max_alive       = 4,      -- his own summons; the 2 he spawns with don't count
    summon_first_delay     = 5.0,
    summon_cooldown        = 14.0,
    summon_fail_cooldown   = 5.0,
    summon_windup          = 1.1,
    summon_recover         = 0.6,
    summon_spit_speed      = 520.0,
    summon_max_range       = 1100.0,
    summon_hatch           = { x = 33, y = 4 },   -- authored units, starboard; mirrored

    -- ===== ESCORT =====
    -- He spawns with two Wardogs (director, same caps and budget as any
    -- escort) and every Wardog near him flies with him: on patrol around
    -- him, in a fight never more than escort_leash away. Nobody else
    -- follows a Barge. No aura: escort_radius alone makes him a leader.
    escort                 = { "WARDOG", "WARDOG" },
    escort_radius          = 900.0,
    escort_leash           = 600.0,
    escort_takes           = { "WARDOG" },

    -- ===== SQUAD =====
    squad_role             = "elite",   -- free; support-ish in practice (turret, wardogs)

    -- ===== SPAWN =====
    spawn_weight         = 120.0,
    max_active           = 2,
    threat_cost          = 6,
    ambush_chance        = 0.20,   -- a "dead" capital ship whose turret swings
                                   -- round is the ambush worth the most

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

    plates = {
        { points = { { 0, -30 }, { 3, -18 }, { 7.1, -22.4 }, { 10, -12 } },
          shade = 1.50, accent = true },                     -- mandible root
        { points = { { 0, -10 }, { 12, -2 }, { 7.7, 11.3 }, { 0, 20 } },
          shade = 0.55 },                                    -- body patch
        { points = { { 0, -18 }, { 3, -8 }, { 0, 14 }, { -3, -8 } },
          shade = 1.28, mirror = false },                    -- keel strip
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
    scrap_drop           = { 14, 18 },   -- the most dangerous thing in a fight pays best per kill
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
    combat_lose_distance = 1800.0,   -- WAS 1300 (playtest: packs de-aggroed)

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
    -- CHARGED BURST: a 0.45s charge (hull glows, aim line grows, sparks pull
    -- in to the nose; he keeps flying), then THREE rapid rounds, committed --
    -- crossing the hold-fire band no longer cuts the volley off.
    burst_count          = 3,      -- WAS 5 (playtest: "3 rapid shots")
    burst_interval       = 0.07,   -- between the volley's rounds
    burst_pause          = 1.5,    -- WAS 1.05
    burst_windup         = 0.45,

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

    -- ===== PACK =====
    -- Receives the Bloodseeker's aura at full value (roster default), and is
    -- the second rank he will execute -- only when no wounded Raider is near.
    execute_rank         = 2,

    -- ===== SPAWN =====
    spawn_weight         = 70.0,
    max_active           = 3,
    threat_cost          = 4,      -- Three of them fill the 12 budget

    squad_role           = "assault",   -- takes attack turns (squad_ai)
    ambush_chance        = 0.30,   -- melee: waking at arm's length is the point

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

    plates = {
        { points = { { 1.9, -10.9 }, { 12, -24 }, { 13, -12 }, { 6, -4 } },
          shade = 1.45, accent = true },                     -- open-front prong
        { points = { { 10, 2 }, { 22, 10 }, { 16, 18 }, { 8, 12 } },
          shade = 0.55 },                                    -- outrigger patch
        { points = { { 0, -6 }, { 3, 2 }, { 0, 18 }, { -3, 2 } },
          shade = 1.28, mirror = false },                    -- keel strip
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
    scrap_drop           = { 12, 16 },   -- rare and dangerous: worth chasing down
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
    combat_lose_time     = 6.0,
    combat_lose_distance = 1800.0,   -- WAS 5s / 1400

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
    rocket_fast_speed_mult = 1.5,  -- WAS 2.2: near impossible to parry or dodge
    micro_recover        = 0.85,

    -- ===== MINES (primary) =====
    -- No passive trail any more (mine_trail_enabled defaults false): every
    -- mine he lays now comes from a move you can see -- the carpet or the
    -- toss. Mines DECAY: an unlit one fizzles after mine_lifetime, no blast,
    -- and its slot under mine_max_active is free again.
    mine_enabled         = true,
    mine_max_active      = 8,      -- The run alone lays ~7
    mine_lifetime        = 9.0,    -- WAS 22. The last 2.5s it visibly dims.
    mine_trigger_radius  = 95.0,
    mine_blast_radius    = 130.0,
    mine_blast_damage    = 40.0,

    mine_run_enabled     = true,
    mine_run_cooldown    = 6.5,
    mine_run_speed       = 780.0,
    mine_run_time        = 1.25,     -- ~975px of run...
    mine_run_gap         = 140.0,    -- ...at one blast radius apart = ~7 mines
                                     -- spanning a wall, not a pile
    -- Shape, rolled per run: a straight wall, an ARC that bends round the
    -- player's side (95 deg/s -> ~120 deg on a ~470px radius), or an S.
    mine_run_straight_weight = 0.34,
    mine_run_arc_weight      = 0.40,
    mine_run_s_weight        = 0.26,
    mine_run_curve           = 95.0,

    -- ===== MINE TOSS (get-off-me) =====
    -- Within 320px: brakes and shakes (0.5s, sparks all round), SPINS once
    -- throwing 6 mines outward -- armed AND lit, 1.4s fuse -- into a ring
    -- ~235px out, then a 0.7s dizzy recover. Leave on the windup, or get
    -- right on top of him: the ring's centre is the gap. Toss mines do not
    -- count against mine_max_active.
    toss_enabled         = true,
    toss_trigger_range   = 320.0,
    toss_windup          = 0.5,
    toss_spin_time       = 0.5,
    toss_count           = 6,
    toss_speed           = 300.0,
    toss_fuse            = 1.4,
    toss_recover         = 0.7,
    toss_cooldown        = 8.0,

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

    -- ===== PACK =====
    -- Too unstable to coordinate with, even for other Rakshari: no aura, no
    -- execution buff, never goes feral when a Bloodseeker dies, never
    -- executed. Also escorts nothing.
    pack_immune          = true,
    squad_role           = "wild",   -- mood-driven: assault / support / berserk (squad_ai)

    -- ===== SPAWN =====
    spawn_weight         = 50.0,
    max_active           = 2,
    threat_cost          = 4,
    ambush_chance        = 0.20,

    -- Standard Rakshari red. He is identified by SHAPE -- split jaws, outward
    -- flanks, three uneven engines -- and by the frenzy shift when it matters,
    -- not by being a different colour at rest.
    color = { r = 200, g = 70, b = 55 },
}


-- ----------------------------------------------------------------------------
-- BLOODSEEKER -- elite duelist. Upgraded Berserker that fights in two modes.
--
-- MELEE (this table) is the Berserker kit, better: spray while closing and
-- circling, the cyan BASH you parry, the amber RAM CHAIN you dodge.
-- RANGE (`duel_range` below) keeps its distance and fights with the gun,
-- the lancer and the cone -- and still BASHES if you walk into him. It never
-- rams.
--
-- Telling the modes apart (three channels, any one is enough):
--   LAMP   the diamond in his chest: ORANGE quick pulse = MELEE,
--          BLUE-CYAN slow pulse = RANGE. Hard fast blink = perfect-dodging
--          right now. Dim and still = stunned or tumbling (open).
--   FLAME  MELEE long hot aft flame; RANGE guttered.
--   RETROS RANGE throws bone-white plumes off the prow as he holds back.
--
-- SWITCHING (playtest pass): a roll every duel_roll_interval seconds, 50%
-- and +10% per miss, reset on a switch. Nothing else switches him.
--
-- THE TWO SWITCHES (AISystem 2.4, notes 22-26):
--   MELEE -> RANGE  DISENGAGE. Backpedals nose-on, gun silent, and perfectly
--                   dodges plasma and Rift bolts (each leaves a bone ghost).
--                   Parry, ramming, the shoulder bash, kinetic rocks and every
--                   blast still land. Lasts 1.5..3s. Punish: close the gap.
--   RANGE -> MELEE  BLOOD DIVE. Prow flare + narrowing hull (the read), then a
--                   steered rush. No contact damage, no i-frames: sidestep it
--                   and he overshoots, or shoot him on the way in. If it
--                   reaches bash range it becomes a NORMAL bash with the full
--                   cyan windup -- a transition never shortens the parry read.
--
-- RANGE kit (Phase 2): LANCER (one fast round at your intercept, lock beat
-- before it fires) and SUPPRESSION CONE (rooted barrage into a locked wedge,
-- then a rooted recovery). See duel_range.
--
-- MELEE tricks (Phase 3): the FEINT (bash windup that breaks off into a
-- backstep spray) and the RAM CANCEL (a charge that stops and becomes a
-- rush into a full bash).
--
-- POST-STUN EVADE: any stun from a parry or a parried round ends in 3s of
-- perfect dodging, in either mode.
--
-- PACK (this pass): the 10% AURA, the EXECUTION and the DEATH-CHAOS, plus
-- escort spawns and idle escort flying. See the PACK blocks below.
--
-- Hull (Oleg's concept, model pass): Berserker family -- the same mandibles
-- -- with a long nose and a deep maw between nose and mandibles, forward
-- horns off the wing shoulders, swept wings and a three-engine block. The
-- concept's plates are kept, pulled inside the hull where they poked out
-- (the loader skips any that do): the horns are part of the hull outline
-- now, the maw tooth sits inside the beak, and the engine block and lower
-- jaw stop short of the tail notch.
-- Elite read: GOLD EDGES on the armour plates and a gold spine (plate_edge,
-- RenderSystem 1.12). The fill stays the faction red, so he is plainly
-- Rakshari -- just the one whose plating someone bothered to trim.
--
-- Mass: ~20kg from the physics hull, same as the first model. Density is
-- low for the size so the wider hull flies the same. Hitbox ratio ~1.15 at
-- 0.86: the maw and the horn notches fill in by convexity.
-- ----------------------------------------------------------------------------
local GOLD = { r = 214, g = 172, b = 92 }

enemy_archetypes.BLOODSEEKER = derive {
    display = "Bloodseeker",
    faction = "RAKSHARI",

    hull = {
        {   0, -50 },                                     -- nose
        {  10, -25 },                                     -- maw (neck notch)
        {  22, -48 },                                     -- mandible tip
        {  16, -14 },                                     -- horn root
        {  24, -28 },                                     -- shoulder horn
        {  20, -10 },                                     -- horn back on the wing
        {  42,  12 },                                     -- wing tip
        {  28,  26 }, {  18,  12 },                       -- wing trailing edge, return
        {  12,  30 },                                     -- engine block side
        {   0,  24 },                                     -- tail notch
        { -12,  30 }, { -18,  12 }, { -28,  26 },
        { -42,  12 }, { -20, -10 }, { -24, -28 },
        { -16, -14 }, { -22, -48 }, { -10, -25 },
    },

    -- Every point checked inside the hull at load (EnemyRegistry warns and
    -- skips any that are not). `accent = true` plates get the gold edge.
    plates = {
        { points = { { 0, -44 }, { 6, -31 }, { 9, -24 }, { 12, -20 }, { 10, -10 }, { 0, 4 } },
          shade = 0.62, accent = true },                     -- maw spine (the beak)
        { points = { { 12, -25.5 }, { 20.3, -42.5 }, { 16.6, -19.5 } },
          shade = 1.35, accent = true },                     -- mandible blades
        { points = { { 14, -12 }, { 38, 10 }, { 26, 22 }, { 12, 6 } },
          shade = 0.50, accent = true },                     -- wing armour
        { points = { { 17.2, -15.0 }, { 22.8, -24.8 }, { 20.0, -12.2 } },
          shade = 1.40, accent = true },                     -- horn plates
        { points = { { 7, 12 }, { 14, 20 }, { 11, 27 }, { 6, 24.5 }, { 1, 19 } },
          shade = 0.55, accent = true },                     -- engine housing
        { points = { { 0, 7 }, { 7, 16 }, { 0, 21 }, { -7, 16 } },
          shade = 1.25, mirror = false, accent = true },     -- lower jaw
        -- Gold spine, the concept's dashed line, as four studs.
        { points = { { 0, -44 }, { 0.8, -41.5 }, { 0, -39 }, { -0.8, -41.5 } },
          shade = 1.0, mirror = false, tint = GOLD, tint_mix = 0.85 },
        { points = { { 0, -37 }, { 0.8, -34.5 }, { 0, -32 }, { -0.8, -34.5 } },
          shade = 1.0, mirror = false, tint = GOLD, tint_mix = 0.85 },
        { points = { { 0, -30 }, { 0.8, -27.5 }, { 0, -25 }, { -0.8, -27.5 } },
          shade = 1.0, mirror = false, tint = GOLD, tint_mix = 0.85 },
        { points = { { 0, -23 }, { 0.8, -20.5 }, { 0, -18 }, { -0.8, -20.5 } },
          shade = 1.0, mirror = false, tint = GOLD, tint_mix = 0.85 },
    },
    turrets = {},

    -- Elite trim: hard gold edges on the accent plates (width in px).
    plate_edge           = GOLD,
    plate_edge_width     = 1.2,

    -- MODE LAMP: the diamond in his chest, between the beak and the jaw.
    -- ORANGE = MELEE, BLUE-CYAN = RANGE (bluer than the bash crescent's mint,
    -- so the two never read as one colour). Drawn over everything.
    mode_lamp            = { { 0, -11 }, { 4.5, -3 }, { 0, 5 }, { -4.5, -3 } },
    mode_lamp_melee      = { r = 255, g = 140, b = 40 },
    mode_lamp_range      = { r = 70,  g = 200, b = 255 },

    -- Lancer geometry (authored units, x scale): the charge runs from the
    -- side spikes (mandible tips, mirrored) into the central nose spike,
    -- and the beam leaves from there.
    lancer_maw           = { x = 0,  y = -50 },
    lancer_spike         = { x = 22, y = -48 },

    -- Kill tallies on the starboard wing armour: decorated, not battered.
    scars = {
        { { 22, -1 }, { 23.5, 3 } },                        -- four strokes...
        { { 24.5, 0.5 }, { 26, 4.5 } },
        { { 27, 2 }, { 28.5, 6 } },
        { { 29.5, 3.5 }, { 31, 7.5 } },
        { { 21.5, 3.5 }, { 32, 4 } },                       -- ...and the strike
    },
    scar_width = 1.4,

    -- Three engines, as in the concept: two in the block, one in the notch.
    thrusters = { { 8, 24 }, { -8, 24 }, { 0, 19.5 } },

    scale                = 1.20,   -- radius ~63 vs the Berserker's ~52
    hitbox_scale         = 0.86,

    -- ===== MASS AND TOUGHNESS =====
    density              = 3.4,    -- ~20kg physics mass, same as the old hull
    hp                   = 1150.0, -- playtest: 680 was glassy for this hull
    score_reward         = 1600,
    scrap_drop           = { 26, 34 },   -- elite: second only to the Barge
    stagger_resist       = 0.45,
    -- Playtest: the parry still stops him, but not for long, and he comes out
    -- of it dodging (post_stun_evade_time below).
    stun_resist          = 0.50,   -- parry stun 1.5s -> 0.75s
    stun_resist_ranged   = 0.50,   -- reflected-round stun 0.9s -> 0.45s.
                                   -- Its own key: every other unit takes a
                                   -- reflected round's stun unresisted, and
                                   -- that stays true unless they set this.

    -- ===== POST-STUN EVADE WINDOW =====
    -- Whenever a PARRY or a PARRIED ROUND stuns him, he comes out of the stun
    -- with this long of perfect dodging: plasma and Rift bolts pass through,
    -- he sidesteps them visibly, the mode sigil flickers. In either mode.
    -- Parries, reflected rounds, contact, kinetic rocks and blasts still land.
    post_stun_evade_time = 3.0,

    -- ===== MOVEMENT (MELEE) =====
    engine_power         = 1000.0,
    max_speed            = 33.0,   -- ~10% over the Berserker
    rotation_speed       = 7.5,
    lineardrag_factor    = 0.85,
    angulardrag_factor   = 3.0,
    maneuver_profile     = "melee",
    preferred_range      = 80.0,
    melee_circle_range   = 340.0,
    melee_circle_inward  = 0.25,
    melee_cutoff_speed   = 90.0,
    personality_variance = false,
    fixed_aggression     = 0.95,

    -- ===== PERCEPTION =====
    suspicion_rate       = 1.8,
    suspicion_combat     = 0.35,
    combat_lose_time     = 7.0,
    combat_lose_distance = 2000.0,   -- WAS 6.5s / 1400

    -- ===== SPRAY WHILE CLOSING (Berserker logic, a touch hotter) =====
    fire_rate            = 0.22,
    telegraph_time       = 0.0,
    aim_spread           = 10.0,
    attack_range         = 700.0,  -- playtest: he never fired in the old band
    bullet_speed         = 660.0,
    bullet_lifetime      = 1.0,
    bullet_damage        = 6.0,
    bullet_iframes       = 0.15,
    burst_count          = 3,      -- WAS 5: charged burst, see BERSERKER
    burst_interval       = 0.07,
    burst_pause          = 1.4,    -- WAS 0.95
    burst_windup         = 0.45,   -- the Berserker's charge tell (melee only)
    -- The gun goes quiet just outside bash reach, not at the orbit edge: he
    -- sprays while circling too, and the bash zone stays a single read.
    -- (Was 340 = the orbit radius; he crossed 340..540 in a fraction of a
    -- second and almost never fired -- "he doesn't shoot at all".)
    hold_fire_range      = 230.0,  -- > bash_trigger_range (215)
    melee_shot_clear     = 0.55,

    storm_enabled        = false,
    chaos_dodge_chance   = 0.15,
    bullet_dodge_chance  = 0.30,

    -- ===== BASH =====
    -- Windup kept at the Berserker's 0.40 minus a hair. "Better at
    -- everything" does NOT extend to a shorter parry read: speed and reach
    -- go up, the tell barely moves.
    bash_enabled         = true,
    bash_trigger_range   = 215.0,
    bash_windup          = 0.37,
    bash_lunge_speed     = 1200.0,
    bash_lunge_time      = 0.19,
    bash_reach           = 108.0,
    bash_damage          = 36.0,
    bash_knockback       = 980.0,
    bash_cooldown        = 1.0,
    bash_hit_cooldown    = 2.2,

    -- ===== RAM CHAIN =====
    ram_enabled          = true,
    ram_windup           = 0.56,
    ram_chain_windup     = 0.34,
    ram_windup_turn      = 13.0,
    ram_charge_speed     = 1350.0,
    ram_charge_duration  = 0.42,
    ram_recover          = 1.1,    -- still a real punish window
    ram_cooldown         = 5.0,
    ram_damage           = 30.0,
    ram_iframes          = 0.9,
    ram_knockback        = 1050.0,
    ram_near_trigger     = 0.0,
    ram_far_trigger      = 240.0,
    ram_max_trigger      = 740.0,
    ram_trigger_chance   = 0.55,
    ram_reroll_delay     = 0.8,
    ram_surprise_bonus   = 1.3,
    ram_chain_min        = 2,
    ram_chain_max        = 3,
    ram_clear_reach      = 58.0,
    hit_confirm_cooldown = 1.8,

    -- ===== TRICK 1: THE FEINT (MELEE) =====
    -- A bash windup that stops halfway: the cyan crescent vanishes on the
    -- frame, the hull stops dead and the retros flare (the half-beat tell),
    -- then he backs off firing a short spray. Parry early and you whiff --
    -- the spray arrives during the whiff. Wait for the lunge and you just
    -- step out of 3 rounds. A bash that a dive or a ram cancel PROMISED never
    -- feints, and two feints in a row are half as likely.
    feint_enabled        = true,
    feint_chance         = 0.30,
    feint_break          = 0.5,    -- fraction of bash_windup where it breaks
    feint_hesitate       = 0.16,   -- s: dead stop, retro flare
    feint_fallback_time  = 0.60,
    feint_fallback_speed = 520.0,
    feint_spray_count    = 5,      -- playtest: 3 was not a threat
    feint_spray_interval = 0.09,   -- 5 rounds inside the 0.6s fallback
    feint_spray_damage   = 6.0,
    feint_spray_spread   = 6.0,    -- deg either side

    -- ===== TRICK 2: RAM CANCEL INTO RUSH-BASH (MELEE) =====
    -- Any link of the ram chain can stop: the amber lane and the white-hot
    -- hull drop on the frame (he is no longer invulnerable), a short brake
    -- flare, then the same steered rush as the Blood Dive -- ending in a full
    -- cyan bash. The skill check: you dodged the ram, your dodge is cooling
    -- down, so PARRY. Shooting him during the rush also works.
    --   planned  rolled once per link, fires partway into the charge
    --   reactive rolled once per link, the moment the player dodges
    ram_cancel_enabled   = true,
    ram_cancel_chance    = 0.20,
    ram_cancel_point     = 0.30,   -- fraction of the charge before a planned cancel
    ram_cancel_on_dodge  = 0.65,
    ram_cancel_brake     = 0.14,   -- s of brake flare before the rush

    -- ===== PACK: THE AURA =====
    -- +aura_bonus move speed and attack cadence to every Rakshari within
    -- aura_radius (not Maniacs, not himself). Cadence only: it never shortens
    -- a telegraph, a windup or a recovery -- buffed units attack more OFTEN,
    -- never less readably. Buffed allies show a gold pip and a hotter flame.
    aura_radius          = 520.0,
    aura_bonus           = 0.10,

    -- ===== PACK: EXECUTION =====
    -- Every exec_check s while free, with exec_chance: if a ranked ally
    -- (execute_rank: Raider 1, Berserker 2 -- never Wardogs, Maniacs or
    -- Barges) is under exec_hp_fraction within exec_range, he turns his back
    -- on you and flies at it. Lowest rank first, then lowest HP. A gold
    -- windup at the target (not cyan: it is not aimed at you), then the
    -- kill. The executed ally drops NO scrap. Allies within exec_radius get
    -- +exec_bonus for exec_time (replaces the aura) and every recovery and
    -- cooldown they are sitting in ends on the spot.
    -- Counterplay: finish the wounded ally first, or punish his turned back.
    exec_enabled         = true,
    exec_first_delay     = 6.0,    -- s after he spawns before the first try
    exec_check           = 1.0,
    exec_chance          = 0.5,
    exec_range           = 550.0,
    exec_hp_fraction     = 0.30,
    exec_speed           = 950.0,
    exec_turn            = 9.0,
    exec_reach           = 105.0,  -- centre to centre, to start the windup
    exec_windup          = 0.32,
    exec_max_time        = 12.0,   -- WAS 2.5. Runaway guard only: once he picks a
                                   -- victim he is COMMITTED -- only its death or
                                   -- a stagger/stun/parry on him ends it
    exec_cooldown        = 18.0,
    exec_fail_cooldown   = 4.0,
    exec_radius          = 520.0,
    exec_bonus           = 0.20,
    exec_time            = 5.0,

    -- ===== PACK: DEATH-CHAOS =====
    -- When he dies, every Rakshari within death_chaos_radius (not Maniacs)
    -- goes FERAL for death_chaos_time: it fights whichever ship is nearest --
    -- you OR each other -- re-picking every death_chaos_retarget s. Feral
    -- fire, bashes and rams hurt enemy hulls (x death_chaos_damage); a
    -- feral ram hurts ANY hull it meets on the way. Feral
    -- units move and fire death_chaos_frenzy faster. Red brackets frame the
    -- screen while any are feral; each one carries a red slash mark.
    death_chaos_radius   = 800.0,
    death_chaos_time     = 10.0,
    death_chaos_retarget = 1.0,
    death_chaos_damage   = 1.5,
    death_chaos_frenzy   = 0.25,

    -- ===== PACK: ESCORT =====
    -- The director brings these in with him, as many as the faction's
    -- threat budget and caps allow (an aura with nobody to buff is just an
    -- expensive Berserker). While idle, allies near him fly loose escort
    -- around him rather than wandering off.
    escort               = { "RAIDER", "RAIDER", "BERSERKER" },
    escort_radius        = 1000.0,

    -- ===== DUEL: MODES =====
    -- Playtest: switching is a dice roll, not a reaction to damage (the HP
    -- trigger made him jump modes whenever you landed hits, which read as
    -- him being impossible to pin down). Every duel_roll_interval seconds
    -- he rolls duel_switch_chance; a miss adds duel_switch_step to the next
    -- roll, a switch resets it. 50 -> 60 -> 70 ... so a mode lasts 5s at
    -- the least and very rarely past 20s. Rolls only happen while he is free
    -- -- never mid-bash, mid-ram, mid-lancer or mid-cone.
    duel_enabled           = true,
    duel_roll_interval     = 5.0,
    duel_switch_chance     = 0.5,
    duel_switch_step       = 0.1,

    -- ===== BLOOD DIVE (RANGE -> MELEE) =====
    -- Read in MELEE, because the dive is the first thing MELEE does.
    dive_windup          = 0.38,   -- the read: brake, swing on, prow flare
    dive_windup_turn     = 12.0,
    dive_speed           = 1350.0,
    dive_turn            = 2.4,    -- rad/s: bends toward you, a late sidestep wins
    dive_max_time        = 0.85,   -- ~1150px of rush
    dive_recover         = 0.6,    -- overshoot / out of rush: the punish window

    -- ===== DISENGAGE (MELEE -> RANGE) =====
    -- Read from duel_range at runtime; set here so overlay() copies them and
    -- DamageSystem (which reads this table) sees the same kick.
    disengage_min_time      = 1.5,   -- playtest: it ended almost at once
    disengage_max_time      = 3.0,   -- hard cap: never an invulnerability phase
    disengage_speed         = 640.0,
    disengage_weave_hz      = 2.2,
    disengage_exit_fraction = 0.9,   -- of RANGE's preferred_range
    disengage_dodge_horizon = 0.30,  -- s ahead it looks for incoming rounds
    disengage_dodge_kick    = 460.0, -- px/s sideways per sidestep
    disengage_dodge_gap     = 0.12,  -- s between visible sidesteps

    -- ===== LOOK =====
    -- Hotter and redder than the Berserker's, as in the concept: three
    -- nozzles, so a little less per nozzle to keep the plume the same size.
    thruster_rate        = 1.2,
    thruster_speed       = 170.0,
    thruster_size        = 3.0,
    thruster_life        = 0.15,
    thruster_color       = { r = 255, g = 150, b = 80, a = 235 },
    thruster_glow        = 1.15,

    death_style          = "visceral",
    death_shards         = 8,
    death_trauma         = 0.45,

    -- ===== SPAWN =====
    -- One at a time, and rare. No ambush: his whole kit assumes a fight that
    -- is already running, and a powered-down elite would waste the entrance.
    spawn_weight         = 25.0,
    max_active           = 1,
    squad_role           = "elite",  -- free, no flank slot
    threat_cost          = 6,
    ambush_chance        = 0.0,

    color = { r = 200, g = 70, b = 55 },
}

-- RANGE mode: the same unit with different numbers. Everything not listed is
-- copied from MELEE above, hull and all.
enemy_archetypes.BLOODSEEKER.duel_range = overlay(enemy_archetypes.BLOODSEEKER, {
    maneuver_profile     = "kite",   -- holds the band, never charges inside it
    preferred_range      = 480.0,
    max_speed            = 30.0,
    fixed_aggression     = 0.6,

    -- The filler gun, disciplined: every round telegraphed, short bursts with
    -- a long breather. It is the background between the two attacks below,
    -- not the threat -- so it is quieter than in Phase 1.
    fire_rate            = 0.30,
    telegraph_time       = 0.24,
    aim_spread           = 7.0,
    attack_range         = 680.0,
    bullet_speed         = 700.0,
    bullet_lifetime      = 1.3,
    bullet_damage        = 8.0,
    bullet_iframes       = 0.35,
    burst_count          = 3,
    burst_pause          = 1.9,
    burst_windup         = 0.0,    -- every round already telegraphed here
    -- Gun quiet inside bash reach, and no bash over his own rounds: RANGE has
    -- a bash now (playtest), so the same one-read-at-a-time rules apply.
    hold_fire_range      = 230.0,
    melee_shot_clear     = 0.45,

    -- ===== LANCER: a beam of light down the lane you WILL be in =====
    -- Charge: both side spikes (the mandible tips, lancer_spike) pour into
    --         the central one (lancer_maw): lines and motes across the maw, a
    --         swelling white core, and the thin red lane solving where your
    --         drift puts you at the fire moment.
    -- Lock:   the aim freezes, the lane goes bone-white, the core gets a red
    --         rim. Holding course from here is what gets you hit.
    -- Fire:   HITSCAN -- light, no travel. Pierces everything on the line:
    --         rocks and wrecks are gone, ships of max HP <= lancer_kill_hp are
    --         destroyed (his own pack too), bigger ones take
    --         lancer_pierce_damage. You: lancer_damage and a tumble.
    -- Parry:  the beam stops on your shield and REFLECTS like light off a
    --         convex mirror -- dead centre straight back at him, an edge hit
    --         glances off. The reflection is yours (yellow) and pierces the
    --         same way, with weapon.lancer_reflect_damage on the big ones.
    lancer_enabled       = true,
    lancer_cooldown      = { 6.0, 8.0 },   -- WAS 3.2-4.6: it is devastating now
    lancer_min_range     = 200.0,
    lancer_max_range     = 900.0,  -- trigger range
    lancer_range         = 1300.0, -- beam length
    lancer_width         = 14.0,
    lancer_charge        = 0.9,    -- WAS 0.55: the spike-to-maw build is the tell
    lancer_lock          = 0.28,   -- WAS 0.18: no travel time, so a longer honest beat
    lancer_recover       = 0.45,
    lancer_lead          = 1.0,    -- 0 = aims at you, 1 = your full drift to the fire moment
    lancer_damage        = 45.0,
    lancer_iframes       = 0.6,
    lancer_knockback     = 1000.0,
    lancer_pierce_damage = 600.0,
    lancer_kill_hp       = 500.0,  -- Wardog, Raider, Berserker, Maniac die outright
    lancer_object_damage = 9999.0,

    -- ===== SUPPRESSION CONE: rooted barrage into a locked wedge =====
    -- Windup draws the wedge and swings him onto you, then LOCKS it. While it
    -- fires he cannot move, turn or dodge, and he stays rooted through the
    -- recovery: ~4s of a stationary target for anyone who steps out of the
    -- wedge first. Width at 480px is ~390px -- one dash clears it.
    cone_enabled         = true,
    cone_cooldown        = { 9.0, 12.0 },
    cone_first_delay     = { 1.5, 3.0 },  -- playtest: it never showed up
    cone_chance          = 0.7,    -- per check when ready...
    cone_reroll          = 1.0,    -- ...and this long before checking again
    cone_min_range       = 180.0,
    cone_max_range       = 620.0,
    cone_windup          = 0.75,
    cone_windup_turn     = 6.0,
    cone_fire_time       = 2.2,
    cone_recover         = 1.1,
    cone_half_angle      = 22.0,   -- deg each side of the centre line
    cone_interval        = 0.05,   -- s between rounds (~44 per barrage)
    cone_speed           = 640.0,
    cone_damage          = 6.0,
    cone_iframes         = 0.10,
    cone_lifetime        = 1.15,   -- ~740px: the wedge is drawn to this

    -- Playtest: RANGE keeps the BASH, so walking into him is not free --
    -- closing the gap is still the answer, but you have to read the cyan
    -- crescent on the way in. No ram and no feint: those are MELEE's.
    bash_enabled         = true,
    ram_enabled          = false,
    feint_enabled        = false,

    -- Ordinary, fallible dodging between switches. The perfect dodge is the
    -- Disengage's alone.
    chaos_dodge_chance   = 0.35,
    bullet_dodge_chance  = 0.55,
    dodge_cooldown       = 1.0,

})


-- ============================================================================
-- ARCHETYPE ORDER -- APPEND ONLY
-- ============================================================================

archetype_order = { "WARDOG", "RAIDER", "BARGE", "BERSERKER", "MANIAC", "BLOODSEEKER" }


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
-- SQUAD AI (AISystem notes 42-47)
--
-- Above every unit's own kit: decides WHEN a heavy hitter may attack, where
-- everyone stands, and keeps the pack on you. No attack is changed.
--
--   Roles (per archetype, squad_role):
--     assault   Berserker  -- takes TURNS: at most 1-2 hold one at a time
--                             (re-rolled every hand-off: two_turn_chance).
--                             A turn is turn_min..turn_max s of its full kit,
--                             never cut mid-attack; then turn_rest. Waiting
--                             ones STALK at stalk_ring -- circling, dodging,
--                             never parked. Alone, it always has the turn.
--     harasser  Wardog     -- free: the swarm never waits
--     support   Raider     -- free; stands off at squad_min_range
--     elite     Bloodseeker, Barge -- free, no flank slot
--     wild      Maniac     -- rolls a MOOD every mood_min..mood_max s:
--                             assault (takes turns, presses in),
--                             support (hangs back on rockets and mines),
--                             berserk (tears into the middle, never waits)
--
--   flank_*            non-elites fan round you from the pack's side
--   breather_time      you tumble: no new turn, no new heavy attack
--   share_sight_radius one packmate seeing you keeps everyone's memory fresh
--   call_radius        ...and pulls searching (ALERT) units in
--   chase_*            fallen behind in a fight: up to chase_mult speed
--
-- QA: enabled = false, F5 -> the old every-man-for-himself fight. The dev
-- overlay labels each unit TURN / STALKING / FREE (+ the Maniac's mood).
-- ============================================================================
squad_ai = {
    enabled            = true,
    squad_radius       = 1800.0,
    two_turn_chance    = 0.6,    -- else 1 at a time
    turn_min           = 2.5,
    turn_max           = 4.0,
    turn_rest          = 1.0,
    stalk_ring         = 380.0,  -- just outside a Berserker's bash/ram reach
    stalk_push         = 6.0,
    flank_spacing      = 55.0,   -- deg between neighbours
    flank_max_arc      = 240.0,  -- deg, whole fan
    flank_strength     = 4.0,
    breather_time      = 1.3,
    retry_delay        = 0.25,
    share_sight_radius = 2400.0,
    call_radius        = 900.0,
    chase_distance     = 750.0,  -- or 1.6x its preferred range, whichever is further
    chase_mult         = 1.5,
    mood_min           = 3.5,
    mood_max           = 8.0,
    mood_assault       = 0.40,   -- mood weights
    mood_support       = 0.35,
    mood_berserk       = 0.25,
    mood_range_assault = 240.0,  -- his preferred range in each mood
    mood_range_support = 540.0,
    mood_range_berserk = 120.0,
}


-- ============================================================================
-- BACK-COMPATIBILITY
--
-- PhysicsSystem (despawn_radius) and RenderSystem's vision cone still read the
-- old global `enemy_config`. Pointing it at the Raider keeps both correct while
-- they are migrated. Same table, not a copy.
-- ============================================================================

enemy_config = enemy_archetypes.RAIDER