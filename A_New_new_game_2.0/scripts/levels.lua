-- ============================================================================
-- levels.lua
-- Generated levels: a map of sub-zone cells, walled by the Void.
--
-- A level is a handful of square CELLS joined edge to edge. Each cell is one
-- sub-zone (a zone in zones.lua). Outside the cells is the Void: its border
-- warns first (stars thin, dust stops, the screen goes wrong), then hurts.
--
-- ENDLESS TEST CYCLE (Nov 2026): every level has one Void rift, far from the
-- start and marked on the map. Fly into it and a new level is generated.
-- Kinds cycle OPEN -> MIXED -> SCRAPYARD -> OPEN ... (the demo's order).
-- Bosses will later decide when the rift opens; nothing else here changes.
--
-- Missing file = the old unbounded space, exactly as before levels existed.
-- ============================================================================

level = {
    -- ---- Size -------------------------------------------------------------
    -- Cruise speed is ~620 px/s, so a 24000 px cell takes ~40 s to cross
    -- straight, and a level is 3-4 minutes edge to edge without fighting.
    cell_size = 24000,
    cells_min = 8,
    cells_max = 12,
    -- 0 = round blobs, 1 = long snaking corridors.
    stretch   = 0.55,

    -- ---- The Void border ---------------------------------------------------
    border = {
        edge_noise     = 2600,   -- px the edge wanders in and out (organic, not square)
        noise_scale    = 9000,   -- px between its bumps
        warn_distance  = 3200,   -- px inside the edge where the warnings start
        -- Periodic hull damage inside the Void. Placeholder until the Void
        -- Predator exists -- set damage_enabled = false to switch it off.
        damage_enabled  = true,
        damage          = 3,     -- per tick, at the edge
        damage_interval = 0.5,   -- s
        damage_deep     = 2.5,   -- x damage this far in ...
        damage_deep_at  = 2500,  -- ... px past the edge
    },

    -- ---- The rift ----------------------------------------------------------
    rift = {
        radius = 140,     -- px: fly inside this and you jump
        pull   = 900,     -- px: inside this it drags the ship in
    },

    -- ---- Map ---------------------------------------------------------------
    map = {
        scan_radius   = 2600,   -- px of live auspex around the ship
        fog_cell      = 1000,   -- px per fog-of-war square
        minimap_range = 6500,   -- px from the ship to the minimap's edge
    },

    -- ---- Sub-zones ---------------------------------------------------------
    -- key = a zone in zones.lua.  map_color = its tint on the map once seen.
    subzones = {
        OPEN_FIELD    = { map_color = { r = 78,  g = 86,  b = 100 } },   -- rock grey
        OPEN_EMPTY    = { map_color = { r = 30,  g = 54,  b = 74  } },   -- deep blue
        OPEN_COMET    = { map_color = { r = 44,  g = 96,  b = 122 } },   -- ice blue
        YARD_SCRAP    = { map_color = { r = 104, g = 60,  b = 36  } },   -- rust
        YARD_CEMETERY = { map_color = { r = 86,  g = 46,  b = 58  } },   -- bruise
        YARD_CITADEL  = { map_color = { r = 136, g = 40,  b = 30  } },   -- furnace red
    },

    -- ---- Level kinds -------------------------------------------------------
    -- near / far: weighted sub-zone pools. A cell uses `far` once its distance
    -- from the start is at least far_from x the level's deepest distance.
    -- deepest: if set, the farthest cell (where the rift is) is always this.
    cycle = { "OPEN", "MIXED", "SCRAPYARD" },
    kinds = {
        OPEN = {
            display = "Open Space",
            start = "OPEN_EMPTY",
            near = { OPEN_FIELD = 1.0, OPEN_EMPTY = 1.0, OPEN_COMET = 1.0 },
            far  = { OPEN_FIELD = 1.0, OPEN_EMPTY = 1.0, OPEN_COMET = 1.0 },
            far_from = 1.0,
        },
        MIXED = {
            display = "Mixed Field",
            start = "OPEN_EMPTY",
            near = { OPEN_FIELD = 1.0, OPEN_EMPTY = 0.8, OPEN_COMET = 1.0 },
            far  = { YARD_SCRAP = 1.0, YARD_CEMETERY = 1.0, YARD_CITADEL = 0.35 },
            far_from = 0.5,
        },
        SCRAPYARD = {
            display = "Rakshari Scrapyard",
            start = "YARD_SCRAP",
            near = { YARD_SCRAP = 1.2, YARD_CEMETERY = 1.0 },
            far  = { YARD_SCRAP = 0.8, YARD_CEMETERY = 1.0, YARD_CITADEL = 0.9 },
            far_from = 0.6,
            deepest = "YARD_CITADEL",
        },
    },
}
