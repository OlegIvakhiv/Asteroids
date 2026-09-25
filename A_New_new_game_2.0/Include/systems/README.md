# systems/

Every system that runs during a frame. All of them implement `ISystem`
(from `core/`) and are owned by `SystemManager`, which calls `init()` once
and `update(dt)` every frame in a fixed order.

This folder is flat, but the systems fall into three roles. The role tells
you two things: whether removing the system breaks the game or just makes
it invisible, and roughly where in the frame it runs.

  SIM     — mutates world state. Order-sensitive. Removing it breaks the game.
  RENDER  — reads world state, produces pixels. Removing it makes the game
            invisible, not broken.
  UI      — screen space. Owns its own input. Removing it hides the
            interface, not the game.

================================================================================
SIM — mutates the world
================================================================================

### `InputSystem.hpp`
Reads player input (WASD, mouse aim, dash, sprint, parry) and writes it to
`PlayerComponent`. Also owns the refit kit path: when `kit.valid`, movement
reads the mounted drives instead of a flat engine_power, and dash/dodge
reads class feel instead of Lua defaults.

Runs FIRST. Everything downstream reads what this writes.

### `PhysicsSystem.hpp`
Steps the Box2D world with a variable dt (safe only because `game.cpp`
clamps dt to 0.05s — do not remove that clamp) and syncs body
position/rotation back to `TransformComponent`.

Bullets orient by velocity, asteroids by physics rotation, player by
InputSystem. `cleanup()` culls asteroids and enemies that drifted past
the despawn radius from the player.

### `ShipAnimSystem.hpp`
Player procedural animation. Owns `visualOffsetAngle`, `visualPivot`,
`visualScale` — the three fields that let a ship *look* distorted without
touching its actual transform.

The stagger tumble is the one exception: while `staggerTimer > 0`, this
system owns `tf.rotation` outright and writes it to Box2D every frame,
because a stagger that only *looked* like a tumble while the ship still
aimed straight would be a lie.

### `EnemySystem.hpp`
The spawn director. Two jobs:
- Asteroids: one timer, a population cap, weighted by zone rock table.
- Enemies: per-faction timer, rolls a unit by spawn weight, gated by three
  caps (per-archetype max_active, per-faction max_active, threat budget).

Summon-only units (Wardogs) are skipped by the director entirely — the
only way one appears is if another entity calls `summon()`.

### `AISystem.hpp`
Everything an enemy decides. Perception (vision cone, suspicion, state
transitions), manoeuvres (approach/strafe/fallback/attack-run/reposition/
circle), the committed state machines (ram, bash, mine-run, frenzy), and
movement intent.

**State split:** `AIState` is private to this system. `EnemyComponent`
holds the presentation/impact bits other systems need to read (stagger
timers, telegraph state, ram state, etc.). If RenderSystem or DamageSystem
needs to know something about an enemy's behaviour, it goes on
`EnemyComponent`, not `AIState`.

### `TurretSystem.hpp`
Independently-aiming turrets. Runs immediately AFTER AISystem so the hull
transform for this frame is final before mount points resolve into world
space.

The turret has full 360° traverse, so there is no safe angle — the
COUNTERPLAY is the traverse RATE. Cut across the gun's arc and it has to
catch up. Two fire modes: AIMED (predictive single shot) and BURST
(fanned arc the player cannot sidestep).

### `WeaponSystem.hpp`
Player shooting, weapon heat, Rift Shot, projectile steering. Owns the
two-resource design:
  ENERGY — strategic budget, shared with dash and turbo
  HEAT   — tactical rhythm, weapon-only, hard-locks the gun at max

Refit ships fire from mounted guns (cycling left to right); legacy ships
fire from the nose. Rift Shot is the heavy attack, gated by cooldown.

### `VentQTESystem.hpp`
The overheat-vent minigame. Runs BEFORE WeaponSystem, because WeaponSystem
reads `overdriveTimer` to decide whether the gun generates heat this
frame. If you reorder these two, the overdrive buff silently stops working.

Heat is PINNED at maximum for the duration of the QTE — the player is
wagering time for a chance at the overdrive, not getting a free reward.

### `DamageSystem.hpp`
Contact events, damage application, parry resolution, ram/bash strikes,
rocket/mine detonation, Maniac frenzy resolution, and enemy death FX.

This file is very large. Its job is to be the ONE place that knows what
happens when two bodies touch. Every hull-damage site goes through
`damagePlayer()` on EntityManager so class armour applies consistently.
Every kill route (bullet, contact, blast, bash, ram) funnels through
`spawnEnemyDeath` so the death FX is consistent.

### `ZoneSystem.hpp`
Two jobs at two points of the frame:
- `applyIfDirty()` — early: the zone changed, regenerate the starfield at
  the new density/tint and reseed the background prop layers.
- `update(dt)` — world pass: wrap the prop pool around the camera and draw
  it. Props are NOT entities — no body, no components — so they never touch
  the entity reserve.

================================================================================
RENDER — reads the world, draws it
================================================================================

### `CameraSystem.hpp`
THE single owner of the world view. Writes to `ctx.gameView`; every other
system reads that. Nothing else centres the camera.

Owns: follow + look-ahead, speed zoom, turbo zoom, dev view scale, free
camera, and the trauma shake model (shake = trauma², from smooth 1D value
noise, not `rand()`).

Provides `makeStarView()` — the screen-space view for the parallax
starfield. The stars live in a FIXED rectangle, so the view is centred on
`starFieldSize * 0.5`, NOT on the window centre. Getting this wrong shows
bare edges on zoom-out.

### `RenderSystem.hpp`
Walks every entity and draws it by body type. Reads the archetype registry
for enemy visual polygons (which are ear-clipped at load because
`sf::ConvexShape` cannot fill a concave silhouette — it fans from the
bounding-box centre and fills in every notch).

Handles: hull fill priority (parry flash > stagger flicker > dash flash >
heat tint), armour plates, scars, thrusters, turrets, ram wake, ram
windup glow, frenzy corona, bash crescent, telegraph lines, alert icons.

### `EffectsSystem.hpp`
Spawns particle effects. Does NOT draw them — ParticleSystem does.
Player thrusters (with per-drive nozzle positions on refit ships), dash
bursts, parry sparks, enemy thrusters (with per-archetype exhaust config).

The enemy exhaust reads what the unit is *doing*: roaring through a
charge or lunge, choked off while a bash coils. For those units the
engine is part of the telegraph.

### `ParticleSystem.hpp`
Updates and draws all particles from `EntityManager::particles` as ONE
vertex array. Swap-and-pop removal.

Every particle shares one array, so a single NaN or absurd coordinate
produces a triangle that smears across the entire map. There is a sanity
guard in `drawParticles()` — keep it.

### `DebrisSystem.hpp`
Decorative, NON-PHYSICAL rock shards from fractures. Drawn as one vertex
array.

Split from ParticleSystem deliberately: particles are dots (dust, sparks,
smoke), shards are angular pieces with straight edges. Making every shard
a real Box2D body would triple collision load for no gameplay benefit.

### `BackgroundSystem.hpp`
Parallax starfield. Stars move opposite to player velocity with varying
parallax factors, wrap around screen edges, drawn as one vertex array.

Stars live in SCREEN space, not world space — they are backdrop, they do
not participate in the camera. `CameraSystem::makeStarView()` builds the
view they expect.

### `SpaceDustSystem.hpp`
Near-field motion motes. The starfield sells DEPTH; the dust sells SPEED,
because it lives in world space next to the ship.

Toroidal wrap around the camera, fake depth (size/alpha/streak scale),
speed-gated alpha (invisible when drifting slowly). Zone tint overrides
the default colour when a zone is loaded.

### `DebugSystem.hpp`
Wireframe collision shapes, AoE markers. Toggled by F3. Expires AoE
markers on every frame regardless of whether it is enabled — the expiry
loop is bookkeeping, not rendering, and belongs above the enabled guard.

================================================================================
UI — screen space, owns its own input
================================================================================

### `HudSystem.hpp`
The in-flight HUD: health bar with damage ghost, poise strip, energy
drive, dash cost marker, rift readiness, weapon heat + vent QTE (unified
widget), score.

Segmented bars, skewed parallelograms, threshold pips. Every colour it
uses comes from `UiPalette.hpp` — do not hardcode a colour here, the HUD
and menu are supposed to speak the same language.

### `MenuSystem.hpp`
The terminal front end. Main menu, pause, game over, tutorial. Boot
sequence on cold start only — coming back from a dead run snaps in,
because the machine is already awake.

Purely screen-space. Owns its own selection state. Does NOT own the
GameState itself; `SystemManager` calls `setState()` so this system knows
which screen to draw, and reads `confirmSelection()`'s result to
transition.

### `RefitSystem.hpp`
The refit bay. Hull editor (hitbox), model editor (silhouette), paint
editor (colours, decals, cockpit). Three modes, M / Tab to cycle.

Edits a `ship::ShipDesign` that lives on SystemManager and persists
between runs. Read `core/README.md` and `data/README.md` for the
ShipDesign contract — the editor is just a UI on top of it.

### `DevSystem.hpp`
The tilde dev menu. Cheats, spawning, enemy control, time, camera, AI
overlays, stats panel.

**Do not give this a normal `update()`.** It acts at five fixed points of
the frame (`frameBegin`, `beginPass`, `endPass`, `drawWorld`,
`drawOverlay`) because creating or destroying entities from a normal
system mid-iteration pulls vectors out from under every other system. All
its actions run from `drawOverlay()` — the last thing in the frame, when
nobody is iterating.

================================================================================
Not a system
================================================================================

### `Systems.hpp`
Empty. Delete it.