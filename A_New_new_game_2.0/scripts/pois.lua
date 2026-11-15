-- ============================================================================
-- pois.lua -- POINTS OF INTEREST (read when a level is generated; F5 reloads
-- the tables, a NEW level picks them up)
-- ============================================================================
--
-- Which POIs a sub-zone gets is in zones.lua: `pois = { MINE = { 1, 2 }, ... }`
-- (min..max per cell). What each POI IS lives here.
--
--   display   what the map and the discovery banner call it
--   icon      map glyph: mine outpost scouts derelict distress squad rig
--             camp crane nest lair dock
--   radius    footprint px: no field rock, pile or wreck is generated inside
--   shield    px; a force shield that keeps OUTSIDE rocks and junk out (the
--             comet stream, a drifting pile). Ships and bullets pass through
--             it like fog. 0 = none: the moving groups (scouts, squads), the
--             drifting derelict, and the pack nest -- a shield would give the
--             sleepers away.
--   parts     { TYPE, x, y [, angle_deg] } -- asteroids.lua types placed at the
--             POI's centre + (x, y), turned with the POI. Structures and loot.
--   scatter   { type, count = { a, b }, ring = { r0, r1 } } -- loose loot
--             strewn round the centre (ore for a mine, caches for a camp)
--   guards    { count = { a, b }, units = { KEY = weight }, lead = "KEY",
--             fixed = { "KEY", ... }, sleepers = { "KEY", ... },
--             dormant = false, leash = 900 }
--             Out when you come within 3300 px. They hold their post (leash)
--             until they see you. `lead` always comes, first, and counts
--             toward `count`; `fixed` always come on top; `sleepers` are laid
--             out dormant close in (docked) whatever `dormant` says.
--             All of a POI's guards are ONE post: hit one, all answer.
--   loot_guard  the lead carries the loot (the barge of a squad):
--             { scrap = x2.5, kit = 0.6, beacon = 0.5 }
--   variants  { name = weight } -- rolled once per POI; a variant's table
--             (same keys) is laid over the base. Distress: trap or convoy.
--   drift     px/s: the whole POI moves (a derelict). 0 = anchored.
--   per_level { a, b } -- how many the whole LEVEL holds (rolled once); the
--             per-cell counts in zones.lua are spent until it runs out.
--   min_depth  not in cells closer to the start than this (0 = the start cell).
--   guards.at_centre  the lead holds the POI's centre (the Dreadnought).
-- ============================================================================

poi_types = {

    -- ---- Asteroid Field --------------------------------------------------
    MINE = {
        display = "Mining Claim", icon = "mine", radius = 760, shield = 1000,
        parts = { { "POI_RIG", 0, 0 }, { "POI_ANTENNA", -170, -150 } },
        scatter = { type = "ORE_RICH", count = { 6, 9 }, ring = { 260, 640 } },
        guards = { count = { 2, 3 }, units = { RAIDER = 3, BERSERKER = 1 }, leash = 900 },
    },
    OUTPOST = {
        display = "Outpost", icon = "outpost", radius = 720, shield = 950,
        parts = { { "POI_MODULE", 0, 0 }, { "POI_ANTENNA", 200, -170 },
                  { "SCRAP_CACHE_STEEL", -210, 150 }, { "SCRAP_CACHE_STEEL", 120, 220 } },
        guards = { count = { 2, 4 }, units = { RAIDER = 3, MANIAC = 1, BERSERKER = 1 }, leash = 800 },
    },
    SCOUTS = {
        display = "Scout Patrol", icon = "scouts", radius = 380, shield = 0,
        guards = { count = { 3, 4 }, units = { RAIDER = 4, MANIAC = 1 }, leash = 2200 },
    },

    -- ---- Empty Space ------------------------------------------------------
    DERELICT = {
        display = "Derelict", icon = "derelict", radius = 900, shield = 0, drift = 6,
        parts = { { "POI_HULK", 0, 0 } },
        scatter = { type = "SCRAP_CACHE_STEEL", count = { 2, 3 }, ring = { 380, 620 } },
        guards = { count = { 0, 2 }, units = { RAIDER = 2, MANIAC = 1 }, leash = 1000 },
    },
    DISTRESS = {
        display = "Distress Call", icon = "distress", radius = 700, shield = 900,
        parts = { { "POI_BEACON", 0, 0 } },
        variants = {
            -- a real wreck: what is left of a convoy, and its cargo
            convoy = { weight = 1,
                scatter = { type = "SCRAP_CACHE_STEEL", count = { 3, 5 }, ring = { 220, 560 } },
                extra = { { "WRECK_BARGE", -300, 120, 40 }, { "WRECK_MEDIUM", 260, -180, 200 }, { "WRECK_LIGHT", 120, 300, 90 } },
            },
            -- bait: the call is the Rakshari's, and they are lying in wait
            trap = { weight = 1,
                extra = { { "WRECK_MEDIUM", 220, 160, 30 }, { "SCRAP_CACHE_STEEL", -160, -120 } },
                guards = { count = { 4, 6 }, units = { RAIDER = 3, BERSERKER = 2, MANIAC = 1 }, dormant = true, leash = 700 },
            },
        },
    },
    -- The Dreadnought's station. It reads like a distress call from afar
    -- (same glyph family, red), it is a battlecruiser sitting alone in open
    -- space, and it is a boss fight: BossSystem takes over when you close.
    -- radius 1300 keeps the generator's rocks out of where the cage goes up.
    BATTLECRUISER = {
        display = "Heavy Battlecruiser Activity", icon = "battlecruiser", radius = 1300, shield = 0,
        per_level = { 1, 2 }, min_depth = 1,   -- never in the cell you start in
        guards = { count = { 1, 1 }, units = { DREADNOUGHT = 1 }, lead = "DREADNOUGHT", at_centre = true, leash = 400 },
    },
    SQUAD = {
        display = "Rakshari Squad", icon = "squad", radius = 450, shield = 0,
        guards = { count = { 5, 6 }, units = { RAIDER = 4, BERSERKER = 2, MANIAC = 1 }, lead = "BARGE", leash = 1800 },
        loot_guard = { scrap = 2.5, kit = 0.6, beacon = 0.5 },
    },

    -- ---- Comet Stream (shielded: the stream goes round them) ---------------
    RIG = {
        display = "Mining Rig", icon = "rig", radius = 720, shield = 1050,
        parts = { { "POI_RIG", 0, 0 }, { "POI_ANTENNA", 190, 170 }, { "SCRAP_CACHE_STEEL", -200, -160 } },
        scatter = { type = "ORE_RICH", count = { 3, 5 }, ring = { 300, 620 } },
        guards = { count = { 2, 3 }, units = { RAIDER = 3, BERSERKER = 1 }, leash = 900 },
    },
    OUTPOST_SHIELDED = {
        display = "Shielded Outpost", icon = "outpost", radius = 720, shield = 1050,
        parts = { { "POI_MODULE", 0, 0 }, { "POI_ANTENNA", 200, -170 },
                  { "SCRAP_CACHE_STEEL", -210, 150 }, { "SCRAP_CACHE_STEEL", 120, 220 } },
        guards = { count = { 2, 4 }, units = { RAIDER = 3, MANIAC = 1, BERSERKER = 1 }, leash = 800 },
    },

    -- ---- Scrapyard ----------------------------------------------------------
    CAMP = {
        display = "Scavenger Camp", icon = "camp", radius = 700, shield = 900,
        parts = { { "POI_TENT", 0, 0 }, { "POI_TENT", 230, 120, 40 }, { "POI_ANTENNA_RUST", -200, -170 } },
        scatter = { type = "SCRAP_CACHE", count = { 3, 4 }, ring = { 200, 560 } },
        guards = { count = { 3, 4 }, units = { RAIDER = 3, MANIAC = 1 }, leash = 800 },
    },
    CRANE = {
        display = "Salvage Crane", icon = "crane", radius = 760, shield = 950,
        parts = { { "POI_CRANE", 0, 0 } },
        scatter = { type = "SCRAP_LARGE", count = { 4, 6 }, ring = { 260, 640 } },
        extra = { { "SCRAP_CACHE", 180, 220 } },
        guards = { count = { 2, 3 }, units = { RAIDER = 3, BERSERKER = 1 }, leash = 900 },
    },

    -- ---- Wreck Cemetery -------------------------------------------------------
    WRECK_OUTPOST = {
        display = "Scavenger Outpost", icon = "outpost", radius = 760, shield = 1000,
        parts = { { "POI_MODULE_RUST", 0, 0 }, { "WRECK_HEAVY", -300, 80, 60 }, { "WRECK_MEDIUM", 260, -200, 150 } },
        scatter = { type = "SCRAP_CACHE", count = { 2, 3 }, ring = { 220, 520 } },
        guards = { count = { 3, 4 }, units = { RAIDER = 3, BERSERKER = 1, MANIAC = 1 }, leash = 800 },
    },
    NEST = {
        display = "Pack Nest", icon = "nest", radius = 820, shield = 0,
        parts = { { "SCRAP_CACHE", 0, 0 }, { "SCRAP_CACHE", 80, 60 } },
        extra = { { "WRECK_BERSERKER", -260, 120, 20 }, { "WRECK_MANIAC", 240, 160, 200 }, { "WRECK_RAIDER", 40, -300, 110 },
                  { "WRECK_RAIDER", -200, -220, 300 }, { "WRECK_LIGHT", 320, -80, 70 } },
        guards = { count = { 6, 8 }, units = { RAIDER = 3, BERSERKER = 2, MANIAC = 1.5 }, dormant = true, leash = 600 },
    },

    -- ---- Citadel Zone -----------------------------------------------------
    CAMP_SMALL = {
        display = "Raider Camp", icon = "camp", radius = 560, shield = 760,
        parts = { { "POI_TENT", 0, 0 } },
        scatter = { type = "SCRAP_CACHE", count = { 1, 2 }, ring = { 180, 380 } },
        guards = { count = { 3, 4 }, units = { RAIDER = 3, BERSERKER = 1, MANIAC = 1 }, leash = 800 },
    },
    OUTPOST_HEAVY = {
        display = "Fortified Outpost", icon = "outpost", radius = 860, shield = 1100,
        parts = { { "POI_MODULE_RUST", 0, 0 }, { "POI_MODULE_RUST", 290, 110, 30 }, { "POI_ANTENNA_RUST", -220, -200 } },
        scatter = { type = "SCRAP_CACHE", count = { 3, 4 }, ring = { 260, 640 } },
        guards = { count = { 5, 7 }, units = { RAIDER = 2, BERSERKER = 2, MANIAC = 2, BARGE = 0.6 }, leash = 900 },
    },
    LAIR = {
        display = "Bloodseeker Lair", icon = "lair", radius = 820, shield = 1050,
        parts = { { "POI_LAIR", 0, 0 } },
        scatter = { type = "SCRAP_CACHE", count = { 2, 3 }, ring = { 300, 620 } },
        -- The Bloodseeker at home, with two Berserkers at his side.
        guards = { count = { 1, 1 }, units = { BLOODSEEKER = 1 }, lead = "BLOODSEEKER",
                   fixed = { "BERSERKER", "BERSERKER" }, leash = 700 },
    },
    DOCK = {
        display = "Docking Station", icon = "dock", radius = 900, shield = 1150,
        parts = { { "POI_DOCK", 0, 0 } },
        scatter = { type = "SCRAP_CACHE", count = { 2, 3 }, ring = { 520, 760 } },
        -- Two Barges docked and asleep; their Wardogs on watch round them, with
        -- a Berserker or a Maniac or two. Engage the watch and the Barges
        -- come up (one post: linked).
        guards = { count = { 1, 2 }, units = { BERSERKER = 1, MANIAC = 1 },
                   fixed = { "WARDOG", "WARDOG", "WARDOG", "WARDOG" },
                   sleepers = { "BARGE", "BARGE" }, leash = 800 },
    },
}
