-- ============================================================================
-- codex.lua
-- What the terminal's CODEX says about everything you meet in the field.
--
-- KEYS
--   Hostiles: the archetype key from enemy.lua (`archetype_order`). A unit
--   with no entry here still appears in the codex once met -- with its
--   display name and "NO FIELD NOTES" -- so a new unit never silently
--   vanishes from the bestiary.
--   Objects: the `codex` key each type in asteroids.lua carries
--   (ASTEROID, MAGMATIC, SALVAGE, WRECK, REACTOR, UNSTABLE_CORE).
--
-- FIELDS
--   class      short role line under the name
--   threat     0..1, the bar
--   behaviour  list of lines, what it does
--   counter    one line, what to do about it
--
-- Entries open when the hunter first SEES the thing in a contract; kill
-- counts come from the run. Both live in saves/hunter_record.txt.
--
-- Hot-reloads with F5 like every other script.
--
-- @author Oleg Ivakhiv
-- @version 1.0
-- ============================================================================

codex_entries = {

    -- ===== RAKSHARI MARAUDERS =====

    WARDOG = {
        class     = "SWARM  //  HARASSER",
        threat    = 0.25,
        behaviour = { "CHEAP, FAST, ALWAYS IN PACKS.",
                      "NEVER ARRIVES ALONE: SOMETHING BIGGER CALLED IT IN." },
        counter   = "THIN THE PACK OR KILL WHAT SUMMONED IT.",
    },

    RAIDER = {
        class     = "GUNNER  //  SUPPORT",
        threat    = 0.40,
        behaviour = { "THE BASELINE PIRATE. HANGS BACK AND SHOOTS.",
                      "KEEPS ITS DISTANCE WHILE THE BRAWLERS WORK." },
        counter   = "PARRY ITS ROUNDS BACK INTO IT.",
    },

    BARGE = {
        class     = "HEAVY CRUISER  //  ELITE",
        threat    = 0.80,
        behaviour = { "THE TURRET FIGHTS, NOT THE HULL: WATCH THE BARREL.",
                      "HEAVY SLUG, SIX-ROUND BARRAGE, CLOSE SHOTGUN.",
                      "ITS RAM CANNOT BE PARRIED OR STOPPED." },
        counter   = "CUT ACROSS THE TURRET SWING. WHEN IT RAMS, LEAVE.",
    },

    BERSERKER = {
        class     = "ASSAULT  //  RAMMER",
        threat    = 0.60,
        behaviour = { "CYAN CRESCENT AT THE PROW: A BASH. PARRY IT.",
                      "AMBER LANE AND A WHITE-HOT HULL: A RAM. DO NOT PARRY.",
                      "PACKS TAKE TURNS. ONE OR TWO COME AT A TIME." },
        counter   = "DASH THROUGH THE RAM, LATE. PUNISH THE RECOVERY.",
    },

    MANIAC = {
        class     = "HAZARD  //  WILD",
        threat    = 0.65,
        behaviour = { "SCRAPFIRE, SKID ROCKETS, A PANIC BASH.",
                      "BELOW A THIRD OF ITS HULL IT IGNITES AND COMES AT YOU." },
        counter   = "SHOOT IT DOWN EARLY. A PARRIED ROCKET HUNTS SOMEONE ELSE.",
    },

    BLOODSEEKER = {
        class     = "DUELIST  //  ELITE",
        threat    = 0.90,
        behaviour = { "TWO MODES. ORANGE LAMP: MELEE. BLUE LAMP: RANGE.",
                      "RANGE MODE: THE LANCER BEAM AND THE CONE.",
                      "COMMITS TO AN EXECUTION ON A BROKEN HULL." },
        counter   = "READ THE LAMP. DO NOT DODGE EARLY. KILL IT AND THE PACK TURNS.",
    },

    -- ===== VOID OBJECTS =====

    ASTEROID = {
        class     = "FIELD DEBRIS",
        threat    = 0.15,
        behaviour = { "LARGE BREAKS INTO MEDIUM, MEDIUM INTO SMALL.",
                      "A HEAVY ROCK AT SPEED STAGGERS WHATEVER IT HITS." },
        counter   = "PARRY ONE AND IT BECOMES YOUR HOMING ROUND.",
    },

    MAGMATIC = {
        class     = "HAZARD  //  EXPLOSIVE",
        threat    = 0.50,
        behaviour = { "A BURNING ROCK. GOES UP IN A BLAST WHEN IT BREAKS.",
                      "THE BLAST HURTS EVERYONE IN IT, HUNTER INCLUDED." },
        counter   = "LURE A PACK ONTO IT, THEN SHOOT IT.",
    },

    SALVAGE = {
        class     = "SCRAP HEAP",
        threat    = 0.05,
        behaviour = { "STRAPPED-TOGETHER JUNK. BREAKS LIKE ROCK.",
                      "PAYS OUT SCRAP WHEN IT DOES." },
        counter   = "SHOOT IT. FLY CLOSE. COLLECT.",
    },

    WRECK = {
        class     = "DEAD HULL",
        threat    = 0.10,
        behaviour = { "WHAT IS LEFT OF A SHIP. SCRAP INSIDE.",
                      "SOME OF THEM ARE NOT AS DEAD AS THEY LOOK." },
        counter   = "SHOOT IT OPEN FOR THE SCRAP. GIVE THE QUIET ONES ROOM.",
    },

    REACTOR = {
        class     = "SALVAGE  //  EXPLOSIVE",
        threat    = 0.55,
        behaviour = { "A HEAVY HULL'S CORE, SPILLED WHEN THE WRECK BREAKS.",
                      "DETONATES HARD AND THROWS SCRAP." },
        counter   = "KEEP YOUR DISTANCE WHEN YOU CRACK IT.",
    },

    UNSTABLE_CORE = {
        class     = "HAZARD  //  EXPLOSIVE",
        threat    = 0.60,
        behaviour = { "A LOOSE CORE, DRIFTING. ONE GOOD HIT SETS IT OFF." },
        counter   = "A RIFT HIJACK THROWS IT INTO A PACK.",
    },
}

-- Order and display names of the object group. Hostiles take their order from
-- enemy.lua's archetype_order and their grouping from its factions.
codex_objects = {
    { key = "ASTEROID",      name = "ASTEROID" },
    { key = "MAGMATIC",      name = "MAGMA ROCK" },
    { key = "SALVAGE",       name = "SALVAGE HEAP" },
    { key = "WRECK",         name = "WRECK" },
    { key = "REACTOR",       name = "REACTOR" },
    { key = "UNSTABLE_CORE", name = "UNSTABLE CORE" },
}
