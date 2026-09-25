# core/

The engine skeleton. Everything here would still make sense in a game about
traffic cones and garden gnomes — nothing in this folder knows what a ship,
an asteroid, a Rakshari, or a bullet is.

If a file here starts mentioning `PlayerComponent` or `BodyType::Enemy`,
it does not belong in `core/`. That is the whole rule.

## What lives here

### `ISystem.hpp`
The contract every system in `systems/` signs. Two virtual methods — `init()`
and `update()` — plus the `SystemContext` struct that carries every dependency
a system could need (entity manager, world, window, Lua state, zone state,
dev flags).

`SystemContext` is passed by value into `init()`. That is deliberate: it
freezes the context at init time so a system cannot accidentally grab a
half-constructed world. It is also why anything MUTABLE the systems need
(the current zone, the dev flags) travels as a *pointer* into state owned
by `SystemManager` — a value would freeze at whatever it was when the game
started.

Nothing in `core/` derives from `ISystem`. It is the interface, not an
implementation.

### `EntityManager.hpp`
The ECS core. All game state lives in parallel vectors — one vector per
component type, indexed the same way, so `transforms[i]` and `healths[i]`
and `renders[i]` all describe the same entity.

Stores:
- Component arrays: transforms, renders, physics, healths, bullets, enemies,
  players, physicsShapes, scoreRewards
- Effect arrays: particles, debris, shockRings, screenFlashes, debugAoEs
- Background: stars, starFieldSize
- Time control: hitstop freeze / slomo, `advanceTime()` returns the scaled dt
- Camera feedback: `cameraTrauma`, `cameraZoomKick` (written by any system,
  read by CameraSystem)
- Score

**The reserve is load-bearing.** `reserveAll(8192)` is not an optimisation;
several systems hold `auto&` into these vectors across calls that create
entities (AISystem holds a transform ref and then fires a shot, which
push_backs a bullet). If capacity ever reallocated, every such reference
would dangle and the next write would corrupt the heap. **Do not lower it.**

Deletion is swap-and-pop — O(1), but it moves the last entity into the
freed slot. Anything holding an index across a destroy call is wrong;
hold the *entity id* instead and re-look-up.

### `EntityFactory.hpp`
Every entity creation function lives here: `createPlayer`,
`createPlayerFromDesign`, `createEnemy`, `createAsteroid`, `createBullet`,
`createEnemyBullet`, `createEnemyRocket`, `createMine`, `createRiftBolt`.

Each function is a flat sequence of `push_back` calls that keep every
parallel vector the same length. If you add a new component array to
EntityManager, every function here needs a matching push_back — including
the `push_back({})` for entities that do not use it. A mismatch here is a
heap corruption bug three frames later, not a compile error.

The `createXxx` functions are the ONLY places that construct a full entity.
`spawnExplosion`, `spawnImpact`, `spawnShockRing`, etc. on EntityManager
create *effects* — they write into particle/ring/flash arrays, not into
the component arrays, and they do not consume an entity id slot.

### `FractureImpl.hpp`
One function: `EntityManager::fractureAsteroid`. It sits in its own file
because `EntityManager` needs to call `EntityFactory::createAsteroid` and
`EntityFactory` includes `EntityManager` — a circular include. Defining
the body out-of-line here, after both headers, breaks the cycle without
forward-declaration gymnastics.

**Included exactly once, from `SystemManager.hpp`, after both headers.**

The function itself fans the dying asteroid's own outline into wedges —
the shards ARE the polygon that just died, cut up — throws them outward
biased along the impact direction, then optionally spawns real physical
children (a few) while spawning many more as decorative debris.

### `SystemManager.hpp`
The frame. Owns:
- The Box2D world
- The EntityManager, EntityFactory, and every system instance
- The player design + livery (persist across runs)
- The enemy and zone registries
- The current `GameState`
- The dev state

Runs in `update()`:
1. `m_zoneSystem.applyIfDirty()` — before anything draws, so the sky
   regenerates at the new zone's tint on the same frame the zone changes
2. Menu / Refit / Paused — early return, gameplay does not run
3. Dev `frameBegin()` — tilde, input capture, god-mode mirror
4. **Logic passes** (scaled by dev time control; >1 speed runs *more* passes,
   never a bigger step, because variable-step Box2D tunnels above 0.05s)
5. Camera + screen FX on real time
6. Stars in screen space
7. World-space render pass
8. Screen-space HUD
9. Dev overlay — LAST, because it is the only place that safely creates and
   destroys entities: every system has finished iterating by then

Also owns `restart()`, which is a full teardown and rebuild:
- `em.reset()` (does NOT touch Box2D — it frees user data, then the caller
  destroys the world)
- New Box2D world
- New player via `createPlayerFromDesign`
- Re-runs every system's `init()` with a fresh context

and `reloadScripts()` — the one implementation behind F5 *and* the dev
menu's RELOAD button.

## What does NOT belong here

- Any system implementation. `core/` has the `ISystem` interface; the
  implementations live under `systems/`.
- Any concrete component struct. Those are in `data/components.hpp`.
- Any ship, enemy, asteroid, or zone definition. Those are in `data/`
  and `data/archetypes/`.
- Any UI. Even the loading screen belongs in `systems/ui/`.
- Any Lua binding glue. Systems read Lua through the `sol::state*` in
  `SystemContext`; there is no C++↔Lua bridge layer in this project and
  adding one would be a regression.

## Who includes whom (roughly)

    ISystem.hpp          <- no includes from this folder
    EntityManager.hpp    <- data/components.hpp
    EntityFactory.hpp    <- EntityManager.hpp, data/archetypes/EnemyArchetypes.hpp
    FractureImpl.hpp     <- EntityManager.hpp + EntityFactory.hpp (both, in that order)
    SystemManager.hpp    <- everything above + every system + data/*

If you find yourself wanting to include `SystemManager.hpp` from anywhere
except `game.cpp`, stop. It is the top of the tree, not a utility.