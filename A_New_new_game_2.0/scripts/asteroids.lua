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
        -- Lab model (FieldObjectModels.hpp): hull, inner shell, facet
        -- chords, crevices, fracture lines, ore flecks. No craters.
        -- model_r is the radius the lab drew this size at; line weights
        -- scale from it. Fill/edge colours are the lab palette, not `color`.
        detail = "rock", model_r = 22, rock_verts = 9,
        rock_crevices = 3, rock_facets = 5, rock_fractures = 4, rock_ore_chance = 0.35,
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
        -- Lab model (FieldObjectModels.hpp): hull, inner shell, facet
        -- chords, crevices, fracture lines, ore flecks. No craters.
        -- model_r is the radius the lab drew this size at; line weights
        -- scale from it. Fill/edge colours are the lab palette, not `color`.
        detail = "rock", model_r = 46, rock_verts = 12,
        rock_crevices = 5, rock_facets = 9, rock_fractures = 6, rock_ore_chance = 0.45,
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
        -- Lab model (FieldObjectModels.hpp): hull, inner shell, facet
        -- chords, crevices, fracture lines, ore flecks. No craters.
        -- model_r is the radius the lab drew this size at; line weights
        -- scale from it. Fill/edge colours are the lab palette, not `color`.
        detail = "rock", model_r = 84, rock_verts = 16,
        rock_crevices = 7, rock_facets = 13, rock_fractures = 9, rock_ore_chance = 0.55,
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

    -- ---------------- AUTHORED TEMPLATES (lab: "Cold Field Objects") ------
    -- The procedural clump generator is retired. Each size is ONE designed
    -- pile (scrap_templates, bottom of this file); every spawn re-rolls the
    -- lab's per-object traits -- warm/cool tint, age, rivet density, strut
    -- brightness, a slight pile rotation and ~2.5% vertex jitter -- so the
    -- field reads as the same designed object, never as an identical stamp.
    -- Colour comes from the template palette. `color` below is no longer
    -- drawn; it stays as the colour of the plain-rock fallback if a template
    -- ever fails to load.
    --
    -- ---------------- (history) THREE SIZES, ONE SHAPE LANGUAGE ------------
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
        base_size = 0.32, density = 2.2, hp = 12, score_reward = 12,
        speed_range = { 6.5, 8.5 },
        size_variance_min = 0.80, size_variance_max = 1.18, hp_follows_size = 1.0,
        detail = "cluster", scrap_template = "junk_s",
        metallic = true, child_inherit_color = true,
        tier = 0, explosive = false
    },

    -- Medium pile -- same size band as a MEDIUM asteroid, on purpose.
    SCRAP = {
        color     = { r = 104, g = 74, b = 56 },
        base_size = 0.70, density = 4.0, hp = 32, score_reward = 55,
        speed_range = { 5.5, 7.0 },
        size_variance_min = 0.70, size_variance_max = 1.30, hp_follows_size = 1.0,
        detail = "cluster", scrap_template = "junk_m",
        metallic = true, child_inherit_color = true,
        scrap_drop = { 0, 1 },
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    -- A mountain of strapped salvage -- same band as a LARGE asteroid.
    -- Five lobes: the most knotted thing in the field.
    SCRAP_LARGE = {
        color     = { r = 58, g = 40, b = 28 },
        base_size = 1.50, density = 5.0, hp = 115, score_reward = 190,
        speed_range = { 4.2, 5.6 },
        size_variance_min = 0.65, size_variance_max = 1.45, hp_follows_size = 1.0,
        detail = "cluster", scrap_template = "junk_l",
        metallic = true, child_inherit_color = true,
        scrap_drop = { 1, 2 },
        tier = 2, child_type = "SCRAP", explosive = false
    },

    -- ========================================================================
    -- SHIP WRECKS
    -- ========================================================================
    -- Every wreck rolls a DAMAGE TIER at spawn (see utils/WreckDetail.hpp):
    --
    --   1  LIGHT   one torn edge, one breach, plating mostly attached
    --   2  HEAVY   torn sections, up to three breaches, missing and peeled
    --              plates
    --
    --   wreck_damage = { light = w1, heavy = w2 }   relative weights
    --
    -- Tier 0 (TURNED OFF: pristine, powered down) exists, but a wreck never
    -- rolls it. It is the disguise of a dormant enemy (enemy.lua,
    -- ambush_chance), so a clean dark hull in the field ALWAYS means a live
    -- ship waiting. There is no tier 3: a gutted hull stopped reading as the
    -- ship it was.
    --
    -- The tier bites the silhouette BEFORE physics is built, so a torn wreck
    -- collides as torn and fractures along its torn outline.
    --
    -- WHERE A WRECK COMES FROM -- one of:
    --   wreck_of = "BARGE"         hull, plates, thrusters, turrets, scars and
    --                              HP from the live archetype in enemy.lua
    --   wreck_of_class = "LIGHT"   outline, drive positions and HP from the
    --                              player's stock hull of that class (the
    --                              refit bay preset)
    -- Plates / thrusters / turrets / scars / hull_points set here override the
    -- source, field by field.
    --
    -- HP is the ship's own, flat -- a dead Barge soaks what a live one does.
    -- There is deliberately no `hp` on these types; it would be ignored.
    --
    -- `color` is the COLD hull colour. The tier ramp darkens it; it is not
    -- also darkened by size like a rock is. A dormant enemy borrows the colour
    -- of its wreck type, so the two always match.
    --
    -- QA: asteroid_visuals.wreck_force_tier (below) forces every new wreck
    -- to one tier. Set, F5, look, set back to -1.
    -- ========================================================================

    -- ---------------- WRECKS: player stock hulls ----------------

    WRECK_LIGHT = {
        color = { r = 78, g = 80, b = 85 },
        wreck_of_class = "LIGHT",
        wreck_damage = { light = 3, heavy = 4 },
        base_size = 1.60, density = 3.0, score_reward = 70,
        speed_range = { 5.0, 6.5 },
        size_variance_min = 0.92, size_variance_max = 1.08,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 1, 2 },
        tier = 0, explosive = false
    },

    WRECK_MEDIUM = {
        color = { r = 75, g = 75, b = 79 },
        wreck_of_class = "MEDIUM",
        wreck_damage = { light = 3, heavy = 4 },
        base_size = 1.40, density = 4.2, score_reward = 125,
        speed_range = { 4.4, 5.6 },
        size_variance_min = 0.92, size_variance_max = 1.08,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 2, 3 },
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    -- Oversized past the live hull on purpose, like the Barge below: one of
    -- the two objects in the field that should make you change course.
    -- Tier 2 gives 3 fragments; child_alt at 100% turns exactly one of them
    -- into a live core, so a heavy wreck always reads as "2 piles and a bomb".
    WRECK_HEAVY = {
        color = { r = 66, g = 67, b = 70 },
        wreck_of_class = "HEAVY",
        wreck_damage = { light = 3, heavy = 4 },
        base_size = 2.25, density = 6.0, score_reward = 280,
        speed_range = { 3.6, 4.8 },
        size_variance_min = 0.94, size_variance_max = 1.06,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 3, 5 },
        tier = 2, child_type = "SCRAP",
        child_alt = "REACTOR", child_alt_chance = 100,
        explosive = false
    },

    -- ---------------- WRECKS: Rakshari roster ----------------

    WRECK_RAIDER = {
        color = { r = 74, g = 60, b = 56 },
        wreck_of = "RAIDER",
        wreck_damage = { light = 3, heavy = 4 },
        -- The live Raider has no nozzle hardware (its flame comes from the
        -- default emitter); the dead one shows three cold ones. A dormant
        -- Raider reads these too, so the disguise matches.
        thrusters = { { -7, 13 }, { 7, 13 }, { 0, 17 } },
        base_size = 0.97, density = 3.2, score_reward = 60,
        speed_range = { 5.2, 6.8 },
        size_variance_min = 0.92, size_variance_max = 1.08,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 1, 2 },
        tier = 0, explosive = false
    },

    WRECK_BERSERKER = {
        color = { r = 76, g = 58, b = 54 },
        wreck_of = "BERSERKER",
        wreck_damage = { light = 3, heavy = 4 },
        base_size = 1.73, density = 4.2, score_reward = 120,
        speed_range = { 4.4, 5.8 },
        size_variance_min = 0.92, size_variance_max = 1.08,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 2, 3 },
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    WRECK_MANIAC = {
        color = { r = 80, g = 64, b = 50 },
        wreck_of = "MANIAC",
        wreck_damage = { light = 3, heavy = 4 },
        base_size = 1.32, density = 4.2, score_reward = 110,
        speed_range = { 4.6, 6.0 },
        size_variance_min = 0.92, size_variance_max = 1.08,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 2, 3 },
        tier = 1, child_type = "SCRAP_CHUNK", explosive = false
    },

    WRECK_BARGE = {
        color = { r = 78, g = 62, b = 56 },
        wreck_of = "BARGE",
        wreck_damage = { light = 3, heavy = 4 },
        -- Engine block nozzles; the live Barge only emits flame there.
        thrusters = { { 7, 68 }, { -7, 68 } },
        base_size = 2.75, density = 7.0, score_reward = 340,
        speed_range = { 3.2, 4.2 },
        size_variance_min = 0.94, size_variance_max = 1.06,
        metallic = true, child_inherit_color = true,
        scrap_drop = { 4, 6 },
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
    --
    -- Now drawn with the lab's UNSTABLE CORE model (armoured vessel, hot core,
    -- six plates, open vents) -- the same object as UNSTABLE_CORE below, just
    -- smaller and only ever coughed up by a heavy hull.
    REACTOR = {
        color = { r = 96, g = 52, b = 34 },
        detail = "unstable_core",
        base_size = 0.55, density = 6.5, hp = 48, score_reward = 120,
        speed_range = { 3.5, 4.6 },
        size_variance_min = 0.94, size_variance_max = 1.08, hp_follows_size = 1.0,
        metallic = true,
        scrap_drop = { 1, 2 },
        tier = 3,
        explosive = true,
        explosion_radius = 150.0,
        explosion_damage = 120.0,
        particle_count = 90,
        glow_intensity = 1.0,
        child_type = "SCRAP_CHUNK", burst_children = 4,   -- lab: core -> 4 small scrap
    },

    -- ---------------- UNSTABLE CORE (Rakshari yard only) ----------------
    -- The scrapyard's answer to the MAGMATIC rock: same slot in the spawn
    -- table, same blast, same parry-launch rule, same score -- but it reads
    -- as salvage, not geology. Open space keeps MAGMATIC, whose model is
    -- untouched. On detonation it throws four SCRAP_CHUNKs after the blast.
    UNSTABLE_CORE = {
        color = { r = 85, g = 29, b = 12 },
        detail = "unstable_core",
        base_size = 0.90, density = 6.0, hp = 50, score_reward = 75,
        speed_range = { 4.0, 5.0 },
        size_variance_min = 0.80, size_variance_max = 1.25, hp_follows_size = 1.0,
        metallic = true,
        scrap_drop = { 1, 2 },
        tier = 3,
        explosive = true,
        explosion_radius = 150.0,
        explosion_damage = 120.0,
        particle_count = 80,
        glow_intensity = 0.8,
        child_type = "SCRAP_CHUNK", burst_children = 4,
    },
}


-- ============================================================================
-- SCRAP TEMPLATES -- the authored piles, one per size
-- ============================================================================
-- Copied point for point from the lab page. Units are the lab's; the model is
-- normalised to the rolled size at spawn, so these numbers only set SHAPE.
--   chunks   plates, drawn in order (later ones sit on top), each with its
--            own fill and edge. Hex strings or { r=, g=, b= } both work.
--   struts   welded bars across the pile, drawn over the plates
--   gaps     holes punched through where nothing bridged
-- Edit, F5, spawn one from the dev menu to see it.
-- ============================================================================
local SCRAP_FILL, SCRAP_PLATE, SCRAP_DARK, SCRAP_STROKE = "#38271e", "#543b2e", "#1e130d", "#914e2d"

scrap_templates = {
    junk_s = {
        chunks = {
            { body = { { -4, -8 }, { 5, -10 }, { 8, -2 }, { 2, 6 }, { -6, 2 } },     fill = SCRAP_FILL,  stroke = SCRAP_STROKE },
            { body = { { 0, -4 }, { 7, 0 }, { 4, 8 }, { -3, 10 }, { -5, 3 } },       fill = SCRAP_PLATE, stroke = "#aa5533" },
            { body = { { -8, -2 }, { -2, -6 }, { 0, 4 }, { -7, 6 } },                fill = SCRAP_DARK,  stroke = "#663322" },
        },
        struts = { { { -4, -8 }, { 4, 8 } }, { { 7, 0 }, { -7, 6 } } },
        gaps   = { { { -1, -2 }, { 2, -2 }, { 1, 2 }, { -2, 1 } } },
    },

    junk_m = {
        chunks = {
            { body = { { -12, -15 }, { 10, -18 }, { 18, -4 }, { 12, 14 }, { -8, 18 }, { -18, 2 } }, fill = SCRAP_DARK,  stroke = SCRAP_STROKE },
            { body = { { -16, -10 }, { -4, -16 }, { 2, -6 }, { -10, 4 } },                       fill = SCRAP_PLATE, stroke = "#aa5533" },
            { body = { { 4, -12 }, { 16, -10 }, { 22, 2 }, { 10, 0 } },                          fill = SCRAP_FILL,  stroke = "#884422" },
            { body = { { -6, 2 }, { 14, 4 }, { 16, 18 }, { -2, 16 } },                           fill = SCRAP_PLATE, stroke = "#bb6633" },
            { body = { { -20, -2 }, { -10, 0 }, { -12, 12 }, { -22, 8 } },                       fill = SCRAP_FILL,  stroke = SCRAP_STROKE },
        },
        struts = { { { -12, -15 }, { 12, 14 } }, { { 18, -4 }, { -18, 2 } }, { { -4, -16 }, { 16, 18 } } },
        gaps   = { { { -4, -4 }, { 3, -6 }, { 5, 1 }, { -2, 3 } }, { { 6, 6 }, { 11, 4 }, { 9, 10 } } },
    },

    junk_l = {
        chunks = {
            { body = { { -25, -35 }, { 18, -42 }, { 38, -15 }, { 28, 25 }, { -12, 40 }, { -38, 15 }, { -30, -20 } }, fill = SCRAP_DARK,  stroke = SCRAP_STROKE },
            { body = { { -10, -45 }, { 12, -38 }, { 5, -20 }, { -18, -25 } },                                  fill = SCRAP_PLATE, stroke = "#bb6633" },
            { body = { { 18, -25 }, { 35, -20 }, { 42, 5 }, { 22, 0 } },                                       fill = SCRAP_FILL,  stroke = "#aa5533" },
            { body = { { -35, -10 }, { -15, -12 }, { -18, 15 }, { -42, 8 } },                                  fill = SCRAP_PLATE, stroke = "#994422" },
            { body = { { -8, 10 }, { 25, 8 }, { 30, 32 }, { -2, 38 } },                                        fill = SCRAP_FILL,  stroke = SCRAP_STROKE },
            { body = { { -28, 12 }, { -5, 15 }, { -10, 35 }, { -32, 28 } },                                    fill = SCRAP_PLATE, stroke = "#bb6633" },
        },
        struts = {
            { { -25, -35 }, { 30, 32 } }, { { 38, -15 }, { -32, 28 } },
            { { -10, -45 }, { 28, 25 } }, { { -42, 8 }, { 42, 5 } },
        },
        gaps = {
            { { -8, -15 }, { 4, -18 }, { 8, -8 }, { -5, -5 } },
            { { 12, 12 }, { 20, 8 }, { 16, 22 }, { 8, 18 } },
            { { -20, 2 }, { -12, -2 }, { -10, 8 }, { -18, 12 } },
        },
    },
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

    -- ===== LAB MODELS (rocks, scrap, unstable core) =====
    field_glow = 1,               -- 1 = the lab's soft effects: scrap glints and
                                  -- the unstable core's orange breath.
                                  -- 0 = flat only (ore flicker and rivet
                                  -- shimmer stay; they are not glows).

    -- ===== SHIP WRECKS =====
    wreck_force_tier  = -1,       -- QA: 1 (light) or 2 (heavy) forces every
                                  -- NEW wreck to that tier. -1 = roll from
                                  -- each type's wreck_damage. 0 is refused:
                                  -- that look belongs to dormant enemies.
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
