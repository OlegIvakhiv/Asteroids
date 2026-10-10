# utils/

Shared data, config, and the small library layer that sits under the
systems. Nothing here is a system — nothing here implements `ISystem` and
nothing here is called by `SystemManager`.

The folder holds four different kinds of thing. That is why it can feel
like a junk drawer. This README sorts them.

================================================================================
COMPONENTS — the data attached to entities
================================================================================

### `components.hpp`
Every struct that lives in one of `EntityManager`'s parallel arrays:
`TransformComponent`, `PlayerComponent`, `EnemyComponent`,
`BulletComponent`, `RenderComponent`, `PhysicsComponent`,
`HealthComponent`, `Star`, `Particle`, `DebrisChunk`, `ScreenFlash`,
`ShockRing`, `PhysicsShapeData`, and the AI enums (EnemyState, Maneuver,
AlertIcon, RamState, BashState, FrenzyState, MineRunState).

Also `AIState` — the AI's private decision memory. Even though only
AISystem reads it, it lives here because it is a component in every
other sense: plain data, no logic.

Rule: if you are adding a field that RenderSystem, DamageSystem, or any
other system needs to READ, it goes on `EnemyComponent` (or the relevant
component). If it is decision-making that only AISystem cares about, it
goes on `AIState`.

================================================================================
CONFIG & TUNING — values that should be hot-reloadable
================================================================================

### `ClassTuning.hpp`
Per-class feel: dodge tier, thermal profile, parry window, regen, poise,
damage reduction, shoulder bash, ramming. Light / Medium / Heavy.

Layers on top of the ship's derived stats. ShipDesign derives what
physics can derive (mass, thrust, reactor); this is the rest — rules
that are a class identity rather than a consequence of geometry.

Every value can be overridden from Lua and hot-reloads with F5. Missing
keys fall back to the defaults in `defaultFeel()`.

### `GameConfig.hpp`
Compile-time constants: window size, physics scale, spawn defaults,
colors, asset paths, debug flags. The values that should never change
at runtime.

Do not move runtime-tunable values in here. If you want to change it
without recompiling, it belongs in a Lua table, not in this file.

================================================================================
GAME STATE FLAGS — small enums and singletons
================================================================================

### `GameState.hpp`
Top-level state machine: MainMenu, Playing, Paused, GameOver, Tutorial,
Refit. Plus `MenuAction` — the return value of confirming a menu row,
consumed by SystemManager.

### `DevState.hpp`
Every dev-menu flag in one plain struct. Also `dev::ENTITY_BUDGET` and
`dev::ENTITY_RESERVE` — the entity caps the dev menu respects.

**Lives here, not on EntityManager, on purpose:** `EntityManager::reset()`
runs on every restart and wipes everything. A god-mode flag stored there
would silently switch itself off the first time you died — exactly when
you wanted it. This struct survives restarts by construction.

### `InputRegistry.hpp`
Static key/mouse name → SFML key map. Systems ask for `isPressed("W")`,
not `sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W)`. Also owns the
"blocked" flags the dev menu uses to stop a menu click from also firing
a shot.

Every gameplay input read goes through `isPressed()`. That is the one
chokepoint that lets the dev menu block WASD without touching a single
system.

It is also what lets a hidden world be flown by an AI: while a
`VirtualScope` holds a `VirtualPad`, `isPressed()` answers from the pad
(physical key names, exactly what `key_bindings` maps to) and
`virtualAim()` replaces InputSystem's mouse read with a world point.
Nothing the player holds in the menu can leak into it.

================================================================================
SHIP DESIGN — the player-authored hull
================================================================================

### `ShipDesign.hpp`
The class that IS the refit bay's data model: hull points, mounts, model,
stats, derived kit. Pure data and math — no rendering, no Box2D, no Lua,
no UI.

Two layers: the HITBOX (`outline()`, <= 8 convex points, what Box2D
collides with) and the MODEL (`decor()`, any shape up to 64 points, what
the player sees). The model must cover the hitbox, stay inside the
envelope (hitbox × 1.5 about its centroid), and use <= 150% of the
hitbox's area. `validate()` is the full check.

`kit()` derives the runtime `KitProfile` the movement and weapon systems
actually read.

### `ShipLivery.hpp`
Paint colours, decals, cockpit. Pure data and geometry — both the
renderer and the refit bay build their vertices from the same
`decalGeometry()` / `cockpitGeometry()` calls, so a detail can never look
different in the editor than in the fight.

Colour helpers (`fromHSV`, `toHSV`, `colorToHex`, `colorFromHex`) also
live here because the refit bay's picker is the only thing that needs
them.

### `ShipFile.hpp`
Save and load a whole ship — hull, mounts, model, paint — as Lua.
One plain table, no binary, no versioned struct dump.

Loading runs in a BARE `sol::state` (no libraries, no io, no os) because
the file is data from the internet. The worst a malicious ship file can
do is describe a bad ship, which then fails `ShipDesign`'s own validation.

================================================================================
PROCEDURAL DETAIL — small generators that build geometry
================================================================================

### `ScrapDetail.hpp`
Procedural interior detail for field objects. `buildCluster()` produces
overlapping chunk polygons with welded struts and punched gaps.
`buildRock()` cuts facets and craters into an existing silhouette.

Everything is rolled per object — chunk count, placement, shade, strut
routing — so two piles of the same type never come out the same. Colours
are baked into vertices at build time.

================================================================================
UI INFRASTRUCTURE — shared by every screen-space system
================================================================================

### `UiPalette.hpp`
THE one place Void Hunter's interface colours and chrome live. Palette,
metrics, and small drawing primitives (`panel`, `brackets`, `segBar`,
`label`, `scanlines`).

If you need a colour in the HUD or a menu, it comes from here. Do not
hardcode. Do not add a second palette in another file. Two interfaces on
two languages in one game is exactly the failure mode this file exists
to prevent.

### `TerminalUI.hpp`
Shared immediate-mode widget layer. Panels, buttons, icon buttons,
glossary overlay, transition veil.

Handles three things every screen was doing badly:
  1. Live screen view — never `getDefaultView()`, which is frozen at
     window creation and breaks mouse input after resize.
  2. Held keys across screen changes — `primeInput()` clears the key
     table so the Enter that opened a screen cannot immediately close it.
  3. Clip-local coordinates — widgets drawn inside a `beginClip()`
     hit-test correctly without the caller converting coordinates by hand.

Optional DESIGN SPACE: `setDesignSize({1920, 1080})` authors a screen on a
fixed canvas, letterboxed into the window at a uniform scale. The terminal
uses it; the refit bay does not (yet).

### `TermDraw.hpp`
Batched flat geometry for the live terminal panels (one draw call for a
whole feed frame), plus hull meshes: the player's refit-bay design with
its livery, and enemy archetypes coloured as in flight. Anything the menu
draws of a SHIP comes through here, so a hull edit shows up on F5.

### `ShadowWorld.hpp`
A second, hidden copy of the game: its own EntityManager, factory, Box2D
world, camera and every gameplay system, wired the way SystemManager wires
the real ones, rendering into its own `sf::RenderTexture` (through
`SystemContext::target`). The hunter is flown through a `VirtualPad`
(InputRegistry) instead of the keyboard, installed only while its logic
runs. Cut off from the real game on purpose: its own ZoneState (never
dirty, no ZoneSystem), its own DevState, no HunterRecord, no game over.
Must not move after `build()` (systems hold pointers into it); `build()`
again restarts the scene.

### `HunterPilot.hpp`
An AI that flies a player ship by pressing the player's keys -- move,
lead-aim, fire, dash across rams and rounds (late: a perfect dodge), parry
bashes / rounds / rocks, Rift Bolt into a cluster detonated at the closest
pass, answer the vent QTE, collect scrap. `Skill` makes feed hunters
mortal; `Orders` let a doctrine scene script the ship without leaving the
real input path.

### `LiveFeed.hpp`
The tactical feed: a ShadowWorld running the real director, asteroid
spawner and AI, with a random hunter (class, paint, skill). The camera is
the game's CameraSystem, leaned toward the hunter's target. A quiet field
gets a new pack. At zero hull the feed ends its hunter (own-hull wreckage),
SIGNAL LOST, and drops in the next one.

### `DoctrineStage.hpp`
FIELD DOCTRINE: eight looping scenes of real gameplay (flight, gunnery,
turbo, salvage; dodge, parry, rift bolt, vent) on a ShadowWorld with the
player's own ship and paints and the real HUD. Each scene places the
enemy / rocks and gives the pilot orders; the pilot reacts to the enemy's
actual state, so the perfect dodge in DODGE is a real ram eaten by real
i-frames. `lit()` drives the keycaps.

### `HunterRecord.hpp`
The terminal's memory, `saves/hunter_record.txt`: contracts, hunters
lost, the last 16 runs, and CODEX sightings / kills. Plain text, one
record per line, written at the end of a run and on quit.

### `ZoneArchetypes.hpp`
Zone definitions loaded from `zones.lua` — the "tileset" layer. A zone
says what a stretch of space LOOKS like (sky, stars, dust, rocks) and
what lives in it (faction slots). Not yet where it ends.

Also `ZoneState` — the runtime "which zone is live right now" holder.
Lives as a pointer in `SystemContext` (like `DevState`) because systems
cache the context by value and a value would freeze at whatever zone was
active when the game started.

### `EnemyArchetypes.hpp`
Enemy unit archetypes and faction spawn tables, loaded from `enemy.lua`.
One table per unit, inheriting shared defaults.

For every archetype this holds TWO hulls derived from one authored
silhouette: the visual polygon (any point count, may be concave,
ear-clipped at load for rendering) and the physics hull (convex, <= 8
points, derived automatically so the artist never has to think about
Box2D's hard 8-point ceiling).

Also owns `FactionDef` — which units a faction can field, and at what
weights and caps.

================================================================================
THE RULE FOR THIS FOLDER
================================================================================

`utils/` is the folder you create when you haven't decided what something
is. Everything here has now been decided:

  components          → data attached to entities
  config and tuning   → values that should be hot-reloadable
  game state flags    → enum-shaped flags and singletons
  ship design         → the player-authored hull model
  procedural detail   → geometry generators
  UI infrastructure   → shared by every screen-space system
  archetypes          → definition tables loaded from Lua

If you find yourself wanting to add a new file and none of those headers
feel right, that is a signal. Either the file belongs in `core/`,
`systems/`, or you need a genuinely new kind of thing — in which case
add a new header above rather than dropping it in unnamed.