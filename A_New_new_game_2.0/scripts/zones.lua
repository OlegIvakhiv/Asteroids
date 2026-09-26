-- ============================================================================
-- zones.lua
-- Zone archetypes -- the "tileset" layer.
--
-- A zone says what a stretch of space LOOKS like and what lives in it: sky
-- colour, star density, dust tint, which rocks drift through, which factions
-- the director may field, and what hangs in the background.
--
-- It does not yet say where a zone ends. There are no boundaries or
-- transitions -- see the header of ZoneArchetypes.hpp for why that waits on
-- run state.
--
-- ============================================================================
-- THE TEST THIS FILE HAS TO PASS
-- ============================================================================
-- Zone number three must be a change to THIS FILE and to the prop polygons
-- below -- no new C++. If adding a zone needs an engine change, the zone
-- layer is not finished and the engine is what should change, not this file.
--
-- ============================================================================
-- ONE STRUCTURAL IDEA PER ZONE
-- ============================================================================
-- A zone is not a prop list, it is a sentence, and every prop has to serve it:
--
--   CASUAL    nothing happened here
--   RAKSHARI  everything that worked has been taken
--
-- Planned, not yet built: EMPIRE (intact but dead), SEQUENCY (measured),
-- PHANTOM (grown), CULTIST (torn), VOID (absent).
--
-- ORDER IS LOAD-BEARING. `zone_order` assigns the uint8_t ids. APPEND to it,
-- never insert -- inserting renumbers every zone and the live one changes
-- under you on the next F5.
-- ============================================================================

zone_order  = { "CASUAL", "RAKSHARI" }
default_zone = "CASUAL"


-- ============================================================================
-- BACKGROUND GEOMETRY
-- ============================================================================
-- Authored as polygon point lists, exactly like ship hulls in enemy.lua. No
-- sprite atlas, no texture loading, no new art pipeline.
--
-- ---------------------------------------------------------------------------
-- A PROP IS A LIST OF PARTS
-- ---------------------------------------------------------------------------
--   parts = { { points = {...}, fill = {...}, line = {...} }, ... }
--
-- Parts draw in authored order -- painter's algorithm, no depth buffer -- so
-- write them back to front: rock first, then the armour bolted over it, then
-- what is welded on top of that. A prop that is one shape can skip the nesting
-- and put `points` directly on the prop.
--
-- Per part:
--   fill / line   its own colours
--   filled        false = outline only
--   outlined      false = fill only, for interior shapes that need no edge
--   glow          0 = inert. Above 0 adds a pulsing halo. The ONLY thing in a
--                 zone that emits light instead of reflecting it, so keep it
--                 to one or two parts -- a lit furnace is a landmark's whole
--                 claim to being inhabited, and means nothing if the entire
--                 station glows.
--
-- Concave shapes are fine now. Parts are ear-clipped at load by the same
-- triangulator that handles concave ship hulls, which replaced a centroid fan
-- that silently folded over itself on any deep notch -- which is to say, on
-- exactly the shapes that make scrap look like scrap.
--
-- ---------------------------------------------------------------------------
-- WHAT MAKES TWO PROPS LOOK DIFFERENT
-- ---------------------------------------------------------------------------
-- Not detail. At background distance and background alpha, interior detail is
-- invisible -- the eye sorts on OUTLINE, and an outline is two things:
--
--   ASPECT RATIO  a 13:1 spar and a 1.4:1 crate can never be confused
--   SIZE CLASS    a 20px chunk and a 120px barge can never be confused
--
-- Authored size IS the size class. `scale` is a per-prop tuning override on
-- top of it, so you can resize one prop without re-authoring its points.
--
-- COLOUR RULE: the player's hot cyan must always be the brightest thing on
-- screen. Crimson outlines are fine; the furnace yellow is deliberately dimmed
-- from the concept art, because at full value it out-shone the player's ship.
-- ============================================================================

zone_props = {

    -- ========================================================================
    -- RAKSHARI JUNK -- the near layer
    -- ========================================================================

    -- Speckle. Small enough to read as distance rather than as objects.
    CHUNK_A = {
        points = { { -22, -6 }, { -8, -16 }, { 12, -13 }, { 22, 2 }, { 6, 14 }, { -14, 11 } },
        fill = { r = 10, g = 7, b = 7 },
        line = { r = 68, g = 40, b = 28 },
    },
    CHUNK_B = {
        points = { { -13, -15 }, { 7, -18 }, { 18, -2 }, { 10, 16 }, { -10, 14 }, { -19, 1 } },
        fill = { r = 10, g = 7, b = 7 },
        line = { r = 62, g = 38, b = 28 },
    },

    -- Cargo containers still strapped together. Emptied, obviously. The ONE
    -- right-angled thing out here, which is what makes it read as built.
    CRATE_STACK = {
        points = {
            { -35, -25 }, {  -6, -25 }, {  -6, -14 }, {  35, -14 },
            {  35,  14 }, {   8,  14 }, {   8,  25 }, { -35,  25 },
        },
        fill = { r = 12, g = 8, b = 6 },
        line = { r = 92, g = 60, b = 32 },
    },

    -- Structural spar, bent where something tore free of it. The extreme of
    -- the aspect-ratio axis -- nothing else in the field is a line.
    SPAR_BENT = {
        scale = 1.1,
        points = {
            { -95, -2 }, { -24, -6 }, {  30, -9 }, {  95, -5 },
            {  95,  1 }, {  32,  -1 }, { -22,   3 }, { -95,   5 },
        },
        fill = { r = 10, g = 8, b = 8 },
        line = { r = 84, g = 50, b = 34 },
    },

    -- ------------------------------------------------------------------
    -- DEAD SHIPS -- the REAL hulls
    --
    -- These are the player's three stock patterns and the Rakshari roster,
    -- the same silhouettes that drift through the field as destructible
    -- wrecks. Nothing invented: seeing a dead Barge in the backdrop and then
    -- fighting a live one is the tie that makes the zone feel like one place.
    -- ------------------------------------------------------------------

    BG_WRECK_LIGHT = {
        scale = 1.60,
        points = {
            { 0, -48 }, { 3, -32 }, { 8, -12 }, { 15, 12 }, { 12.2, 14.7 }, { 7.3, 16.5 },
            { 9, 30 }, { 0, 28 }, { -9, 30 }, { -12, 24 }, { -20, 21 }, { -15, 12 },
            { -8, -12 }, { -3, -32 },
        },
        fill = { r = 13, g = 13, b = 14 }, line = { r = 86, g = 86, b = 92 },
    },

    BG_WRECK_MEDIUM = {
        scale = 1.50,
        points = {
            { 0, -42 }, { 5, -26 }, { 20, 2 }, { 28, -3 }, { 31, 10 }, { 36, 20 },
            { 24, 24 }, { 20, 34 }, { 8, 30 }, { 0, 32 }, { -8, 30 }, { -13.6, 26.1 },
            { -13, 17.4 }, { -24.4, 16.6 }, { -31, 10 }, { -28, -3 }, { -20, 2 }, { -5, -26 },
        },
        fill = { r = 12, g = 12, b = 13 }, line = { r = 78, g = 78, b = 84 },
    },

    BG_WRECK_HEAVY = {
        scale = 1.60,
        points = {
            { 0, -56 }, { 10, -30 }, { 24, -8 }, { 33, 2 }, { 38, -4 }, { 43, 12 },
            { 50, 22 }, { 40, 30 }, { 32, 46 }, { 14, 44 }, { 0, 48 }, { -14, 44 },
            { -32, 46 }, { -28.3, 24.5 }, { -30.5, 17.7 }, { -26.2, 11.6 }, { -26.9, 0.4 },
            { -33, 2 }, { -24, -8 }, { -10, -30 },
        },
        fill = { r = 12, g = 12, b = 13 }, line = { r = 74, g = 76, b = 82 },
    },

    BG_WRECK_RAIDER = {
        scale = 1.80,
        points = {
            { 0, -10 }, { 8, -25 }, { 12, -10 }, { 14.9, 3 }, { 14.9, 9 }, { 15, 10 },
            { 0, 20 }, { -15, 10 }, { -25, 15 }, { -25, 5 }, { -12, -10 }, { -8, -25 },
        },
        fill = { r = 14, g = 10, b = 9 }, line = { r = 96, g = 66, b = 50 },
    },

    BG_WRECK_BARGE = {
        scale = 1.50,
        points = {
            { 0, -78 }, { 9, -66 }, { 13, -46 }, { 26, -34 }, { 24, -16 }, { 34, -8 },
            { 33, 16 }, { 22, 22 }, { 27, 40 }, { 20, 52 }, { 24, 68 }, { 10, 62 },
            { 8, 76 }, { 0, 66 }, { -8, 76 }, { -10, 62 }, { -24, 68 }, { -14.4, 41 },
            { -16.9, 29.7 }, { -13.8, 18.4 }, { -23.8, 15 }, { -34, -8 }, { -24, -16 },
            { -26, -34 }, { -13, -46 }, { -9, -66 },
        },
        fill = { r = 13, g = 10, b = 9 }, line = { r = 90, g = 62, b = 48 },
    },

    -- ========================================================================
    -- RAKSHARI SCRAP CITADEL -- the far layer
    -- ========================================================================
    -- Drawn by utils/CitadelModel.hpp, a straight port of the design lab page
    -- "rust citadel.html" (Redesign Study v2), built from that page's own
    -- seed -- so the rock, craters, fissures and armour ring are the exact
    -- shapes that were signed off, not look-alikes.
    --
    -- The first version of this entry was a hand-translated polygon list,
    -- and cut the bolts, chains and trophies as "sub-pixel noise at this
    -- size". At landmark scale (2.2-3.0 below) the citadel lands at roughly
    -- the page's own 1.42x, so nothing is cut any more: ten spikes, three
    -- sensor masts, seven swaying trophy chains with their wrecks, the
    -- furnace, the bolted ring, six hero plates, the hangar and six turrets
    -- that track.
    --
    -- It is a MODEL, not a polygon list, because it moves -- `parts` cannot
    -- sway a chain. Rendered to its own texture and faded as one image, so
    -- the landmark alpha does not turn the armour into glass.
    --
    --   furnace_glow   the page's "Furnace Glow" button: heat bloom, lit
    --                  throat, muzzle and engine glows, hangar light
    --   trophy_chains  the page's "Trophy Chains" button
    -- ========================================================================
    STATION_CITADEL = {
        scale         = 1.0,
        builtin       = "RAKSHARI_CITADEL",
        furnace_glow  = true,
        trophy_chains = true,
    },
}


-- ============================================================================
-- ZONES
-- ============================================================================

zones = {

    -- ========================================================================
    -- CASUAL -- the control
    -- ========================================================================
    -- Deliberately identical to how the game played before zones existed. It
    -- is the baseline every other zone is measured against, so if a change
    -- here makes CASUAL feel different, the change is wrong.
    --
    -- Rock weights reproduce the old spawner exactly: it rolled evenly across
    -- SMALL/MEDIUM/LARGE, then overrode the result with MAGMATIC 15% of the
    -- time. 28/28/28/15 is the same distribution written as one table.
    -- ========================================================================
    CASUAL = {
        display = "Open Space",

        void_color       = { r = 2, g = 3, b = 5 },
        star_count       = 800,
        star_tint        = { r = 255, g = 255, b = 255 },
        star_alpha_scale = 1.0,

        dust_color = { r = 170, g = 200, b = 255 },
        dust_alpha = 150,

        rock_interval     = 0.5,
        rock_max_count    = 60,
        rock_spawn_radius = 2500,
        rocks = {
            SMALL    = 28,
            MEDIUM   = 28,
            LARGE    = 28,
            MAGMATIC = 15,
        },

        factions = {
            RAKSHARI = { interval_scale = 1.0, threat_scale = 1.0, active_scale = 1.0 },
        },

        -- Empty space is empty. No props, no landmarks.
    },

    -- ========================================================================
    -- RAKSHARI -- the scrapyard
    -- ========================================================================
    -- Sentence: everything that worked has been taken.
    --
    -- Nothing here is intact and nothing is symmetrical. The fight this zone
    -- promises is CLOSE: junk breaks sightlines into lanes, which is exactly
    -- when a Berserker ram becomes frightening. So the field is denser and
    -- spawns nearer than CASUAL, and the director pushes a little harder.
    --
    -- ------------------------------------------------------------------
    -- WHERE THE RED COMES FROM -- three knobs, in order of how much they
    -- actually tint the screen:
    --
    --   1. dust_color / dust_alpha   BIGGEST. Dust streaks cover the whole
    --      screen while you move, so this is the real wash. If the zone
    --      looks too red, turn this down first.
    --   2. star_tint                 Every star takes this colour. High
    --      saturation here reads as pink snow.
    --   3. void_color                Smallest. It is only visible in the
    --      gaps, and it is already near-black.
    --
    -- The look wanted is DARK SPACE THAT HAPPENS TO BE WARM, which comes
    -- from taking the blue OUT rather than putting red IN.
    -- ------------------------------------------------------------------
    -- ========================================================================
    RAKSHARI = {
        display = "Rakshari Scrapyard",

        -- BLACK IS THE BASE COLOUR, red is a layer on top of it. The zone
        -- had drifted into red fog: a bright wash across the whole frame with
        -- the stars losing to it. This is still dark space -- the red should
        -- read as something IN the black, not as a filter over it.
        void_color       = { r = 4, g = 2, b = 2 },
        -- Stars come BACK. Cutting them to 420 at 55% alpha is most of what
        -- made the zone read as fog rather than as space; a scrapyard is
        -- still somewhere, and you should be able to see the sky behind it.
        -- The tint is nearly white with a hint of warmth -- a saturated one
        -- turns the starfield pink, which is the other half of the fog look.
        star_count       = 620,
        star_tint        = { r = 255, g = 232, b = 222 },
        star_alpha_scale = 0.85,

        -- Deeper and much thinner. Dust streaks cover the entire screen while
        -- you move, so alpha here IS the wash -- 118 of a desaturated pink was
        -- painting the whole frame. A darker, more saturated red at roughly
        -- half the alpha reads as embers drifting through black instead.
        dust_color = { r = 142, g = 44, b = 34 },
        dust_alpha = 62,

        -- Denser field, closer in. Cover is the point of this zone.
        rock_interval     = 0.35,
        rock_max_count    = 78,
        rock_spawn_radius = 2100,

        -- Salvage, not geology. The chain breaks down in theme the whole way:
        -- HULK_WRECK -> 3x SCRAP -> 2x SCRAP_CHUNK -> shards.
        -- LARGE stays in at a low weight so there is still some honest stone
        -- to parry, and so the yard has something it was built around.
        -- Salvage only. No stone: the grey SMALL/MEDIUM/LARGE rocks belong to
        -- open space, and seeing them here broke the region read -- they were
        -- also arriving indirectly, since LARGE breaks into MEDIUM.
        --
        -- The chain stays in theme the whole way down:
        --   any wreck -> SCRAP -> SCRAP_CHUNK -> shards
        -- and every step now inherits the PARENT's colour, so a grey hull
        -- never sheds rust and a rust pile never sheds steel.
        --
        -- REACTOR is absent on purpose. It is no longer a natural spawn: the
        -- only way one exists is that the player cracked a big hull open.
        -- 75 / 25. Weights are out of 200, so the split is exact rather
        -- than approximate: formless junk is the FIELD, and a dead ship is an
        -- event inside it. Flip that and the ships stop being worth noticing.
        -- 75 / 25, weighted out of 200 so the split is exact.
        --
        -- Within the junk, the size mix mirrors the open-space rock table --
        -- roughly even thirds, tilted toward the big end. The field used to
        -- lean small twice over: the weights favoured chunks AND every
        -- destroyed pile adds more of them, so smalls accumulated on top of
        -- spawning more often to begin with.
        -- 75 / 25, weighted out of 200 so the split is exact.
        --
        -- THREE SHAPE FAMILIES, each with its own small / medium / large, and
        -- each breaking down inside itself:
        --     lump   SCRAP_LARGE -> SCRAP      -> SCRAP_CHUNK
        --     bar    SCRAP_SPINE -> SCRAP_BEAM -> SCRAP_SLIVER
        --     arc    SCRAP_ARC   -> SCRAP_CLAW -> SCRAP_HOOK
        --
        -- A warped girder used to shatter into round lumps, which broke the
        -- lineage: the pieces did not look like they came off the thing you
        -- just shot. Heavy ship wrecks feed the LUMP family, same as a big
        -- pile does, so those two breaks now match each other too.
        -- 75 / 25, weighted out of 200 so the split is exact.
        --
        -- ONE junk shape language in three sizes, each breaking into the one
        -- below it. Six junk types was the mistake: with bars and arcs in the
        -- mix there was no way to read size at a glance, because shape was
        -- carrying two meanings at once. Now shape means SIZE -- 2 lobes, 3
        -- lobes, 5 lobes -- and nothing else.
        --
        --     SCRAP_LARGE -> SCRAP -> SCRAP_CHUNK
        --
        -- Heavy ship wrecks feed the same chain, so a broken hull and a broken
        -- pile leave the same kind of debris behind.
        rocks = {
            -- Junk: 150 / 200 = 75%
            SCRAP_CHUNK     = 42,
            SCRAP           = 52,
            SCRAP_LARGE     = 56,

            -- Wrecks: 50 / 200 = 25%, heavy hulls kept rare because each one
            -- now drops a live core.
            WRECK_RAIDER    = 14,
            WRECK_LIGHT     = 10,
            WRECK_MANIAC    =  8,
            WRECK_BERSERKER =  8,
            WRECK_MEDIUM    =  6,
            WRECK_HEAVY     =  3,
            WRECK_BARGE     =  1,

            -- The yard's MAGMATIC: an unstable core adrift in the junk.
            -- 35 on top of 200 is ~15%, the same share MAGMATIC has in open
            -- space. It REPLACES the magmatic rock here; MAGMATIC itself stays
            -- out of this table, and its model is unchanged everywhere else.
            UNSTABLE_CORE   = 35,
        },

        factions = {
            -- Their own yard, so they field faster and carry a heavier budget.
            -- threat_scale is the knob that actually shapes the fight; keep an
            -- eye on it if you ever make two factions active at once.
            RAKSHARI = { interval_scale = 0.8, threat_scale = 1.15, active_scale = 1.1 },
        },

        -- Near layer: drifting scrap and dead ships. ~8 on screen at a time.
        props = {
            count     = 20,
            depth_min = 0.20,   -- far, slow, small
            depth_max = 0.50,   -- nearer, faster, bigger -- never 1.0, or junk
                                -- would sit in the combat plane and read as
                                -- cover the player cannot actually use
            scale_min = 0.50,
            scale_max = 0.95,
            spin_max  = 3.0,    -- slow tumble: nothing here is holding station
            alpha_far  = 0.26,
            alpha_near = 0.52,
            use = {
                "CHUNK_A", "CHUNK_B", "CRATE_STACK", "SPAR_BENT",
                "BG_WRECK_LIGHT", "BG_WRECK_MEDIUM", "BG_WRECK_HEAVY",
                "BG_WRECK_RAIDER", "BG_WRECK_BARGE",
            },
        },

        -- Far layer: the Citadel. It barely moves, and that stillness against
        -- the drifting junk in front of it is what sells the distance.
        --
        -- Only 3 in the pool with a wide wrap box, so roughly one is on screen
        -- at a time. A fortress you see constantly stops being a landmark.
        landmarks = {
            count     = 3,
            depth_min = 0.05,
            depth_max = 0.10,
            -- Bigger: the point of a fortress in the backdrop is SCALE, and
            -- at the previous size it read as a large object rather than as
            -- something you could never fight.
            scale_min = 2.20,
            scale_max = 3.00,
            spin_max  = 0.35,   -- almost, but not quite, stationary
            pad       = 2.2,    -- recycle far off screen; a station popping in
                                -- is the one thing the eye would catch
            -- Brighter than junk on purpose: this is the thing the player is
            -- meant to look at and wonder about.
            -- Dimmer to match. A thing that size at the old alpha competed
            -- with the fight in front of it; it should loom, not shout.
            alpha_far  = 0.34,
            alpha_near = 0.50,
            use = { "STATION_CITADEL" },
        },
    },
}