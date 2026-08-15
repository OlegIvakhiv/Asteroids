# 🚀 Modular Space Engine

A custom 2D space combat engine built from scratch in C++, inspired by *Spacewar!* and *Asteroids*. Built around a data-oriented Entity-Component-System, with real-time physics, FSM-driven enemy AI, and a fully Lua-scriptable gameplay layer for balancing without recompiling.

![gameplay](https://github.com/user-attachments/assets/6c89f26f-5f2c-46a8-97cf-b8edb269c1d3)

---

## Highlights

- **Custom ECS core** — struct-of-arrays component storage, swap-and-pop O(1) entity destruction, stable entity IDs across frame-to-frame reordering
- **Box2D v3 physics** — continuous collision detection, force-based movement, collision filtering by category
- **FSM-based enemy AI** — `PATROL → ALERT → COMBAT`, with reaction delay and bullet-dodge logic tuned to feel human rather than robotic
- **Live Lua scripting** (via sol2) — ship stats, weapon behavior, asteroid types, and enemy config are all data-driven and hot-reloadable at runtime (`F5`)
- **Combat systems** — standard weapon, chargeable "Rift Bolt" with detonation, and a parry mechanic that reflects incoming asteroids into homing projectiles
- **Cascading asteroid destruction** — large asteroids split into mediums, mediums into smalls, plus a volcanic/explosive asteroid variant with area damage
- **Particle & parallax systems** — single-draw-call vertex array rendering for both starfield and particle effects

---

## Tech Stack

| Layer | Tech |
|---|---|
| Language | C++17 |
| Rendering | SFML 3.0 |
| Physics | Box2D v3.0 |
| Scripting | Lua 5.4 + sol2 |
| Architecture | Entity-Component-System (Struct-of-Arrays) |
| Build | CMake + vcpkg (manifest mode) |

---

## Architecture

```
Lua config (.lua)  →  EntityFactory  →  EntityManager (SoA component storage)  →  Systems (Physics, AI, Damage, Render, ...)
```

Each system implements a common `ISystem` interface (`init` / `update`) and receives its dependencies through a `SystemContext`, avoiding global state. `SystemManager` owns and drives all systems each frame.

Entity destruction uses **swap-and-pop**: the last element in every component vector is moved into the freed slot and popped, giving O(1) removal instead of O(n) array shifting — critical for a game spawning/despawning dozens of asteroids per second. Persistent entity IDs (not raw vector indices) are used for all cross-entity references, so nothing breaks when the underlying arrays get reordered mid-frame.

---

## Getting Started

### Requirements
- C++17-compatible compiler (MSVC 2022 recommended)
- [CMake](https://cmake.org/) 3.20+
- [vcpkg](https://github.com/microsoft/vcpkg)
- [SFML 3.0](https://www.sfml-dev.org/download/sfml/3.0.0/)

### Build

```bash
git clone https://github.com/OlegIvakhiv/Asteroids.git
cd Asteroids

cmake --preset default
cmake --build build --config Release

cd build/Release
./ModularSpaceEngine.exe
```

`box2d`, `lua`, and `sol2` are pulled automatically by vcpkg on first configure. SFML is expected via `SFML_ROOT` or `CMAKE_PREFIX_PATH`.

---

## Controls

| Key | Action |
|---|---|
| `W / A / S / D` | Move |
| Mouse | Aim |
| `LMB` | Fire / detonate Rift Bolt |
| `RMB` (hold) | Charge Rift Bolt |
| `Shift` | Turbo boost (drains energy) |
| `Space` | Dash |
| `R` | Parry |
| `F5` | Hot-reload Lua scripts |
| `F3` | Toggle physics debug view |

---

## Scripting Example

All gameplay values live in Lua and reload live — no recompiling to tune balance:

```lua
-- player.lua
engine_power = 150.0
sprint_power_multiplier = 2.5
bullet_speed = 800.0
fire_rate = 0.2
```

```lua
-- asteroids.lua
MAGMATIC = {
    color = { r = 255, g = 80, b = 40 },
    hp = 30,
    explosive = true,
    explosion_radius = 300.0,
    explosion_damage = 120.0
}
```

---

## Performance

Measured on Intel i5-10400F, 16GB RAM, Release build (`-O2`):

| Scenario | Entities | FPS |
|---|---|---|
| Light load | ~20 | 60 |
| Standard combat | ~52 | 60 |
| Peak load | ~103 | 60 |

Physics + AI combined stay under 0.5% of the frame budget at 52 active entities.

---

## Notable Problems Solved

| Problem | Fix |
|---|---|
| Fast bullets tunneling through asteroids | Enabled Box2D continuous collision detection (`isBullet = true`) |
| Entity references breaking after swap-and-pop deletion | Switched all cross-entity references to persistent `entityId` instead of vector index |
| Component vectors desyncing after adding a new component | Enforced a strict rule: every new component vector must be swapped + popped in `destroyEntity` |
| Balance tweaks requiring a full rebuild | Lua hot-reload (`F5`) applies changes without restarting |

---

## Gameplay

![gameplay2](https://github.com/user-attachments/assets/de188c57-e369-4bab-9ab2-6ae02b1576ec)
![gameplay3](https://github.com/user-attachments/assets/16d9d5cb-5eaf-4817-9f1d-17ac77859142)

---

## Author

**Oleg Ivakhiv**
Built as a deep-dive into engine architecture, ECS design, and real-time systems programming in C++.

---

## References

- [SFML 3.0 Documentation](https://www.sfml-dev.org/documentation/)
- [Box2D v3.0 Documentation](https://box2d.org/documentation/)
- [sol2 Documentation](https://sol2.readthedocs.io/)
- [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/)
- Gregory, J. — *Game Engine Architecture*, 3rd ed., CRC Press, 2018
- Nystrom, R. — *Game Programming Patterns*, 2014 ([gameprogrammingpatterns.com](https://gameprogrammingpatterns.com/))
- Fabian, R. — *Data-Oriented Design*, 2018 ([dataorienteddesign.com](https://www.dataorienteddesign.com/dodbook/))
