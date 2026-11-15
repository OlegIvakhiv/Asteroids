/**
 * @file EnemySystem.hpp
 * @brief Asteroid spawner and faction-driven enemy spawn director.
 *
 * ASTEROIDS are unchanged: one timer, a population cap, a magmatic roll.
 *
 * ENEMIES used to be the same thing -- a 4-second timer and a hardcoded cap of
 * six identical pirates. That does not survive contact with a roster. This
 * version walks the ACTIVE FACTIONS and, for each, rolls a unit from that
 * faction's archetype list.
 *
 * Three independent gates decide whether a roll is allowed to land:
 *
 *   1. archetype max_active   "no more than 2 Barges"
 *   2. faction   max_active   "no more than 8 Rakshari on screen at once"
 *   3. faction   max_threat   the budget -- every live unit costs threat_cost
 *
 * Gate 3 is the one doing real work. Caps 1 and 2 alone let the field fill
 * with whatever the dice favoured: two Barges plus six Raiders satisfies both
 * caps and is an unplayable wall. The budget makes heavy units crowd out light
 * ones the way they should, without needing a hand-written rule per pairing.
 *
 * SUMMON-ONLY units (Wardogs) are skipped by the director entirely. The roster
 * doc is explicit that seeing them must always mean something bigger called
 * them in, so the only way one appears is another entity spawning it.
 *
 * @author Oleg Ivakhiv
 * @version 2.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"
#include "utils/LevelGen.hpp"
#include <SFML/System/Clock.hpp>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstdlib>
#include <vector>
#include <string>
#include <iostream>

class EnemySystem : public ISystem {
public:

    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;
        m_dev = ctx.dev;
        m_zone = ctx.zone;
        m_level = ctx.level;

        m_asteroidSpawnClock.restart();
        m_factionClocks.clear();
    }

    void update(float dt) override {
        if (!m_em || !m_lua || !m_ef) return;

        const size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;

        // Dev toggles. The clocks still run while a spawner is off; they are
        // reset on the next attempt, so switching one back on gives a single
        // spawn, not a burst of banked time.
        // A boss arena is sealed: nothing spawns while it stands.
        const bool sealed = m_em->bossArena.active;
        if (!sealed && (!m_dev || m_dev->asteroidSpawnerOn)) updateAsteroids(playerPos);
        if (!sealed && (!m_dev || m_dev->directorOn))        updateFactions(dt, playerPos);

        // The level's placed contents: fields stream in and out around the
        // ship, sleeping packs arm and spring. Runs even with the spawners
        // off -- a field is terrain, not a spawn.
        if (m_level && m_level->active) {
            m_fieldTimer -= dt;
            if (m_fieldTimer <= 0.f) {
                m_fieldTimer = 0.2f;
                updateField(playerPos);
                updatePacks(playerPos);
            }
        }
    }

    /**
     * @brief Create one asteroid of a named asteroids.lua type.
     *
     * The director's spawn path and the dev menu share this, so a dev-spawned
     * MAGMATIC is armed exactly like a natural one -- same explosion values,
     * same spin. Two copies of this would drift within a week.
     *
     * @return The new entity id, or 0 if the type does not exist.
     */
    uint32_t spawnAsteroid(const std::string& typeKey, sf::Vector2f pos, sf::Vector2f vel, float sizeRoll = -1.f) {
        if (!m_em || !m_lua || !m_ef) return 0;

        sol::optional<sol::table> cfgOpt = (*m_lua)["asteroid_types"][typeKey];
        if (!cfgOpt) {
            std::cerr << "[EnemySystem] spawnAsteroid(\"" << typeKey
                << "\") -- no such asteroid type.\n";
            return 0;
        }
        sol::table config = *cfgOpt;

        const uint32_t astId = m_ef->createAsteroid(*m_em, pos, vel,
            config["base_size"], config, m_worldId, sizeRoll);

        const size_t astIdx = m_em->getEntityIndex(astId);
        if (astIdx != (size_t)-1) {
            // Armed from the CONFIG, not from the key name. This used to read
            // `typeKey == "MAGMATIC"`, which meant any new explosive type --
            // a Rakshari reactor wreck, say -- spawned inert while still
            // being drawn with the glowing magma shader. One hardcoded string
            // is one silent bug per new asteroid type.
            if (config["explosive"].get_or(false)) {
                m_em->healths[astIdx].isExplosive = true;
                m_em->healths[astIdx].explosionRadius = config["explosion_radius"].get_or(150.0f);
                m_em->healths[astIdx].explosionDamage = config["explosion_damage"].get_or(30.0f);
            }
            const float randomSpin = ((rand() % 200) - 100.f) / 50.f;
            b2Body_SetAngularVelocity(m_em->physics[astIdx].bodyId, randomSpin);
        }
        return astId;
    }

    /**
     * @brief Spawn a unit by archetype key, ignoring all director gates.
     *
     * This is the door for summon-only units. A Barge calling in Wardogs, or a
     * scripted POI dropping a squad, goes through here -- NOT through the
     * director, which would refuse them.
     *
     * @param dormant  Spawn TURNED OFF -- an ambusher (see AISystem 2.3). The
     *                 director rolls this itself from `ambush_chance`; this
     *                 door is for POIs and the dev menu.
     * @return The new entity id, or 0 if the archetype does not exist.
     */
    uint32_t summon(const std::string& archetypeKey, sf::Vector2f pos, bool dormant = false) {
        if (!m_registry) return 0;
        const uint8_t id = m_registry->idOf(archetypeKey);
        if (id == enemyarch::INVALID_ARCHETYPE) {
            std::cerr << "[EnemySystem] summon(\"" << archetypeKey
                << "\") -- no such archetype.\n";
            return 0;
        }
        return m_ef->createEnemy(*m_em, pos, *m_lua, m_worldId, *m_registry, id, dormant);
    }

private:

    // ========================================================================
    // ASTEROIDS -- behaviour unchanged from v1.1
    // ========================================================================

    /**
     * @brief Drift one rock in from off-screen.
     *
     * The rock TABLE belongs to the zone when a zone is loaded: how often, how
     * many, how far out, and which types in what proportion. Without a zone
     * this falls back to `spawn_settings` and the old even SMALL/MEDIUM/LARGE
     * roll with a magmatic override, so an install missing scripts/zones.lua
     * plays exactly as it did before zones existed.
     */
    void updateAsteroids(sf::Vector2f playerPos) {
        const zonearch::ZoneDef* zone = m_zone ? m_zone->def() : nullptr;

        sol::table astSettings = (*m_lua)["spawn_settings"];
        const float astInterval = zone ? zone->rockInterval
            : astSettings["interval"].get_or(1.0f);
        const int   maxAstCount = zone ? zone->rockMaxCount
            : astSettings["max_count"].get_or(40);
        const float spawnRadius = zone ? zone->rockSpawnRadius
            : astSettings["spawn_radius"].get_or(1500.f);

        if (m_asteroidSpawnClock.getElapsedTime().asSeconds() <= astInterval) return;

        // ROCK MODE (zones.lua `rock_mode`):
        //   "drift"  (default) rocks drift in from every bearing at the ship
        //   "field"  none at all -- the sub-zone's placed field IS its rocks
        //            (an asteroid field with a drifting stream on top is a
        //            billiard table)
        //   "stream" one-way comet stream: every rock comes from one side,
        //            the heading rolled per cell
        const std::string mode = zone && zone->config.valid()
            ? zone->config["rock_mode"].get_or<std::string>("drift") : std::string("drift");
        if (mode == "field") { m_asteroidSpawnClock.restart(); return; }

        // The cap counts only what drifted in: a placed field never crowds it out.
        int currentAsteroids = 0;
        const bool levelOn = m_level && m_level->active;
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Asteroid) continue;
            if (levelOn && m_level->placedById.count(m_em->transforms[i].entityId)) continue;
            ++currentAsteroids;
        }
        if (currentAsteroids >= maxAstCount) return;

        sol::table types = (*m_lua)["asteroid_types"];

        std::string selectedType;
        if (zone) {
            selectedType = zone->rollRock();
            // A zone with an empty rock table is a legitimate design (the Void
            // has no rocks at all), so this is a quiet return, not a warning.
            if (selectedType.empty()) { m_asteroidSpawnClock.restart(); return; }
        }
        else {
            const char* typeKeys[] = { "SMALL", "MEDIUM", "LARGE" };
            selectedType = typeKeys[rand() % 3];

            const float magmaticChance = astSettings["magmatic_chance"].get_or(0.15f);
            const bool isMagmatic = ((rand() % 100) / 100.f) < magmaticChance;
            if (isMagmatic && types["MAGMATIC"].valid()) selectedType = "MAGMATIC";
        }

        sol::optional<sol::table> cfgOpt = types[selectedType];
        if (!cfgOpt) {
            std::cerr << "[EnemySystem] zone rock table names \"" << selectedType
                << "\" but asteroid_types has no such entry.\n";
            m_asteroidSpawnClock.restart();
            return;
        }
        sol::table config = *cfgOpt;

        sol::table speedRange = config["speed_range"];
        float speed = speedRange[1].get<float>() +
            (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

        // ---- The comet stream: upstream of the ship, all one way ----
        float heading = std::numeric_limits<float>::quiet_NaN();
        if (mode == "stream" && m_level && m_level->active) {
            const int cell = m_level->cellOf(playerPos);
            if (cell >= 0 && static_cast<size_t>(cell) < m_level->level.cellStream.size())
                heading = m_level->level.cellStream[static_cast<size_t>(cell)];
        }
        if (!std::isnan(heading)) {
            sol::optional<sol::table> st = zone->config["stream"];
            const float spread = (st ? (*st)["spread"].get_or(16.f) : 16.f) * 3.14159f / 180.f;
            const float speedK = st ? (*st)["speed"].get_or(1.7f) : 1.7f;
            const sf::Vector2f fwd(std::cos(heading), std::sin(heading)), side(-fwd.y, fwd.x);
            sf::Vector2f spawnPos;
            bool ok = false;
            for (int tries = 0; tries < 6 && !ok; ++tries) {
                // A wide front upstream, so the stream fills the screen rather
                // than aiming at the ship: a point on the upstream half of the
                // spawn circle. (Kept ON the circle -- anything further out
                // than PhysicsSystem's 3000 px cull is deleted the frame it
                // is born.)
                const float lateral = ((rand() % 2001) / 1000.f - 1.f) * spawnRadius * (st ? (*st)["width"].get_or(0.6f) : 0.6f);
                const float along = std::sqrt(std::max(0.f, spawnRadius * spawnRadius - lateral * lateral));
                spawnPos = playerPos - fwd * along + side * lateral;
                ok = !m_level || m_level->spawnable(spawnPos, 300.f, 120.f);
            }
            if (!ok) { m_asteroidSpawnClock.restart(); return; }
            const float a = heading + ((rand() % 2001) / 1000.f - 1.f) * spread;
            spawnAsteroid(selectedType, spawnPos, sf::Vector2f(std::cos(a), std::sin(a)) * (speed * speedK));
            m_asteroidSpawnClock.restart();
            return;
        }

        // In a level, nothing is born in the Void (or inside a rock pile):
        // try a few bearings for one that lands clear, and skip the tick if
        // none does.
        float angle = 0.f;
        sf::Vector2f spawnPos;
        bool placed = false;
        for (int tries = 0; tries < 6 && !placed; ++tries) {
            angle = (rand() % 360) * 3.14159f / 180.f;
            spawnPos = playerPos + sf::Vector2f(std::cos(angle) * spawnRadius, std::sin(angle) * spawnRadius);
            placed = !m_level || m_level->spawnable(spawnPos, 300.f, 120.f);
        }
        if (!placed) { m_asteroidSpawnClock.restart(); return; }

        const sf::Vector2f offset((rand() % 400) - 200.f, (rand() % 400) - 200.f);
        const sf::Vector2f dir = (playerPos + offset) - spawnPos;
        const float len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));

        spawnAsteroid(selectedType, spawnPos, (dir / len) * speed);

        m_asteroidSpawnClock.restart();
    }

    // ========================================================================
    // PLACED FIELDS -- streamed in and out around the ship
    // ========================================================================
    /**
     * @brief Bring the level's placed objects to life near the ship.
     *
     * A field cell holds thousands of rocks; only the ones within STREAM_IN
     * are entities. PhysicsSystem culls asteroids past its despawn radius
     * (3000 px) -- that is how they leave. Telling a culled object from a
     * destroyed one is done by where it was last seen: gone while far away
     * means culled (it comes back when you do), gone while close means
     * something broke it (it stays broken, and gives its nav cells back).
     */
    void updateField(sf::Vector2f playerPos) {
        levelgen::Level& L = m_level->level;
        if (L.placed.empty()) return;
        static constexpr float STREAM_IN = 2350.f, CULLED = 2700.f;

        // ---- Live ones: still there? ----
        for (auto it = m_level->placedById.begin(); it != m_level->placedById.end();) {
            levelgen::Placed& o = L.placed[static_cast<size_t>(it->second)];
            const size_t idx = m_em->getEntityIndex(it->first);
            if (idx == (size_t)-1) {
                const float d = std::hypot(o.pos.x - playerPos.x, o.pos.y - playerPos.y);
                if (d > CULLED) o.state = 0;          // culled: waits for you
                else {
                    o.state = 2;                      // destroyed
                    if (o.still) L.navStamp(o.pos, o.radius + 55.f, -1);
                }
                o.id = 0;
                it = m_level->placedById.erase(it);
                continue;
            }
            o.pos = m_em->transforms[idx].position;   // a drifter resumes where it went
            o.angle = m_em->transforms[idx].rotation; // ...facing the way it was
            ++it;
        }

        // ---- Waiting ones in reach: spawn, nearest first, a budget a tick ----
        const int pc = m_level->cellOf(playerPos);
        if (pc < 0) return;
        m_streamCandidates.clear();
        const sf::Vector2i g = L.cells[static_cast<size_t>(pc)];
        for (size_t ci = 0; ci < L.cells.size(); ++ci) {
            const sf::Vector2i d = L.cells[ci] - g;
            if (std::abs(d.x) > 1 || std::abs(d.y) > 1) continue;
            for (int oi : L.placedByCell[ci]) {
                const levelgen::Placed& o = L.placed[static_cast<size_t>(oi)];
                if (o.state != 0) continue;
                const float dd = (o.pos.x - playerPos.x) * (o.pos.x - playerPos.x) + (o.pos.y - playerPos.y) * (o.pos.y - playerPos.y);
                if (dd < STREAM_IN * STREAM_IN) m_streamCandidates.push_back({ dd, oi });
            }
        }
        std::sort(m_streamCandidates.begin(), m_streamCandidates.end());
        const size_t budget = 70;
        for (size_t k = 0; k < m_streamCandidates.size() && k < budget; ++k) {
            levelgen::Placed& o = L.placed[static_cast<size_t>(m_streamCandidates[k].second)];
            const uint32_t id = spawnAsteroid(L.placedTypes[o.type], o.pos, o.vel / SCALE, o.sizeRoll);
            if (!id) { o.state = 2; continue; }
            const size_t idx = m_em->getEntityIndex(id);
            if (idx != (size_t)-1 && b2Body_IsValid(m_em->physics[idx].bodyId)) {
                const b2BodyId b = m_em->physics[idx].bodyId;
                b2Body_SetTransform(b, b2Body_GetPosition(b), b2MakeRot(o.angle * 3.14159265f / 180.f));
                m_em->transforms[idx].rotation = o.angle;
                b2Body_SetAngularVelocity(b, o.spin);
                // Still objects settle after a shove instead of drifting off:
                // a field that slowly disperses is a field that is not there.
                // Their slow turn is left alone -- that is what keeps a still
                // field alive.
                if (o.still) b2Body_SetLinearDamping(b, 0.9f);
            }
            o.state = 1;
            o.id = id;
            m_level->placedById[id] = m_streamCandidates[k].second;
        }
    }

public:
    /// Put a unit in a link group (EnemyComponent::linkGroup).
    void linkTo(uint32_t id, uint32_t group) {
        const size_t i = m_em->getEntityIndex(id);
        if (i != (size_t)-1 && i < m_em->enemies.size()) m_em->enemies[i].linkGroup = group;
    }

    /// A dormant hull lying among placed wrecks has to move like them: the
    /// usual ambush drift (20-50 px/s) is a tell in a cemetery where every
    /// real wreck hangs still. Nearly still, a slow turn, its own heading.
    void settleLikeWreck(uint32_t id, float faceDeg) {
        const size_t i = m_em->getEntityIndex(id);
        if (i == (size_t)-1 || !b2Body_IsValid(m_em->physics[i].bodyId)) return;
        const b2BodyId b = m_em->physics[i].bodyId;
        const float da = (rand() % 1000) / 1000.f * 6.2831853f;
        const float sp = (0.03f + (rand() % 1000) / 1000.f * 0.10f);      // m/s: 1-4 px/s
        b2Body_SetTransform(b, b2Body_GetPosition(b), b2MakeRot(faceDeg * 3.14159265f / 180.f));
        m_em->transforms[i].rotation = faceDeg;
        b2Body_SetLinearVelocity(b, { std::cos(da) * sp, std::sin(da) * sp });
        b2Body_SetAngularVelocity(b, ((rand() % 2001) / 1000.f - 1.f) * 0.06f);
    }

private:
    /// A director ambusher in a zone of placed wrecks (`ambush_cover = true`)
    /// lies down beside one instead of out in the open. Returns false when
    /// there is no live wreck near enough to hide by.
    bool hideBesideCover(sf::Vector2f& pos) {
        if (!m_level || !m_level->active) return false;
        const levelgen::Level& L = m_level->level;
        float best = 1400.f * 1400.f; int bi = -1;
        for (const auto& kv : m_level->placedById) {
            const levelgen::Placed& o = L.placed[static_cast<size_t>(kv.second)];
            if (o.radius < 28.f) continue;
            const float dd = (o.pos.x - pos.x) * (o.pos.x - pos.x) + (o.pos.y - pos.y) * (o.pos.y - pos.y);
            if (dd < best) { best = dd; bi = kv.second; }
        }
        if (bi < 0) return false;
        const levelgen::Placed& o = L.placed[static_cast<size_t>(bi)];
        for (int t = 0; t < 6; ++t) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const sf::Vector2f q = o.pos + sf::Vector2f(std::cos(a), std::sin(a)) * (o.radius + 70.f + rand() % 60);
            if (m_level->spawnable(q, 600.f, 60.f)) { pos = q; return true; }
        }
        return false;
    }

    // ========================================================================
    // SLEEPING PACKS -- the wreck cemetery's ambushes
    // ========================================================================
    void updatePacks(sf::Vector2f playerPos) {
        levelgen::Level& L = m_level->level;
        for (auto& s : L.packs) {
            if (s.state == 2) continue;
            const float d = std::hypot(s.pos.x - playerPos.x, s.pos.y - playerPos.y);
            if (s.state == 0) {
                // Armed: the sleepers are laid out before you can see them.
                if (d > 3300.f) continue;
                s.ids.clear();
                const uint32_t link = m_em->newLinkGroup();   // a pack wakes, and fights, as one
                for (size_t k = 0; k < s.units.size(); ++k) {
                    sf::Vector2f at = s.pos;
                    float face = (rand() % 360) * 1.f;
                    if (k < s.slots.size()) { at = s.slots[k]; face = s.slotAngle[k]; }
                    else {
                        const float a = (k + 0.3f) * 6.2831853f / std::max<size_t>(1, s.units.size());
                        at += sf::Vector2f(std::cos(a), std::sin(a)) * (180.f + rand() % 300);
                    }
                    const uint32_t id = summon(s.units[k], at, true);
                    if (!id) continue;
                    s.ids.push_back(id);
                    linkTo(id, link);
                    settleLikeWreck(id, face);
                }
                s.state = 1;
                continue;
            }
            // Sleeping. Gone while you were far = culled: lay them out again later.
            bool anyAlive = false, anyAwake = false;
            for (uint32_t id : s.ids) {
                const size_t i = m_em->getEntityIndex(id);
                if (i == (size_t)-1 || i >= m_em->enemies.size()) continue;
                anyAlive = true;
                if (!m_em->enemies[i].dormant) anyAwake = true;
            }
            if (!anyAlive) { s.state = (d > 6000.f) ? 0 : 2; continue; }
            // Close enough, or one of them already up: they ALL come up.
            if (d < s.wakeRadius || anyAwake) {
                for (uint32_t id : s.ids) {
                    const size_t i = m_em->getEntityIndex(id);
                    if (i != (size_t)-1 && i < m_em->enemies.size() && m_em->enemies[i].dormant)
                        m_em->enemies[i].provoked = true;
                }
                s.state = 2;
            }
        }
    }

    // ========================================================================
    // ENEMY SPAWN DIRECTOR
    // ========================================================================

    void updateFactions(float dt, sf::Vector2f playerPos) {
        if (!m_registry || m_registry->empty()) return;

        // ---- One O(n) census per frame, shared by every faction ----
        //
        // Counting inside the per-faction loop would rescan the entity list for
        // each faction. One pass, indexed by archetype id, serves all of them.
        const auto& archetypes = m_registry->all();
        m_liveCount.assign(archetypes.size(), 0);

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            const uint8_t a = m_em->enemies[i].archetype;
            if (a < m_liveCount.size()) ++m_liveCount[a];
        }

        // ---- Per-faction timers ----
        const auto& factions = m_registry->factions();
        if (m_factionClocks.size() != factions.size()) {
            m_factionClocks.assign(factions.size(), 0.f);
        }

        const zonearch::ZoneDef* zone = m_zone ? m_zone->def() : nullptr;

        for (size_t f = 0; f < factions.size(); ++f) {
            const auto& fac = factions[f];
            if (fac.units.empty()) continue;

            // WHICH factions may field units is a property of WHERE YOU ARE.
            // With a zone loaded, its slot list replaces enemy.lua's global
            // `active_factions` outright: no slot, no spawns. Without a zone
            // the global list still decides, so nothing changes for an install
            // with no zones.lua.
            const zonearch::FactionSlot* slot = zone ? zone->slotFor(fac.key) : nullptr;
            if (zone) { if (!slot) continue; }
            else if (!fac.active) continue;

            const float interval = fac.spawnInterval * (slot ? slot->intervalScale : 1.f);

            m_factionClocks[f] += dt;
            if (m_factionClocks[f] < interval) continue;

            // Reset regardless of whether the attempt succeeds. Otherwise a
            // full field would bank up elapsed time and dump a burst of units
            // the instant one slot opened.
            m_factionClocks[f] = 0.f;

            trySpawnForFaction(fac, playerPos, slot);
        }
    }

    void trySpawnForFaction(const enemyarch::FactionDef& fac, sf::Vector2f playerPos,
        const zonearch::FactionSlot* slot)
    {
        const auto& archetypes = m_registry->all();

        // Zone scales MULTIPLY the faction's own numbers rather than replacing
        // them, so a zone stays correct after enemy.lua is retuned. threat is
        // the one that actually shapes a fight -- caps 1 and 2 alone let the
        // field fill with whatever the dice favoured.
        const int maxActive = slot
            ? std::max(1, static_cast<int>(std::lround(fac.maxActive * slot->activeScale)))
            : fac.maxActive;
        const int maxThreat = slot
            ? std::max(1, static_cast<int>(std::lround(fac.maxThreat * slot->threatScale)))
            : fac.maxThreat;

        // ---- Current faction load ----
        int liveUnits = 0;
        int liveThreat = 0;
        for (uint8_t id : fac.units) {
            const int n = m_liveCount[id];
            liveUnits += n;
            liveThreat += n * archetypes[id].threatCost;
        }
        if (liveUnits >= maxActive) return;

        // ---- The zone's own say (zones.lua) ----
        //   unit_weights = { BARGE = 1.6 }   some units come more (or less) often here
        //   ambush_scale = 1.8               more of the field's units arrive dormant
        //   squads = { chance = 0.5, groups = { { "RAIDER", "RAIDER", "RAIDER" }, ... } }
        const zonearch::ZoneDef* zdef = m_zone ? m_zone->def() : nullptr;
        sol::optional<sol::table> zcfg;
        if (zdef && zdef->config.valid()) zcfg = zdef->config;
        const auto weightOf = [&](uint8_t id) {
            float w = archetypes[id].weight;
            if (zcfg) w *= (*zcfg)["unit_weights"][archetypes[id].key].get_or(1.f);
            return w;
        };

        // ---- Build the candidate list ----
        m_candidates.clear();
        float totalWeight = 0.f;

        for (uint8_t id : fac.units) {
            const auto& a = archetypes[id];
            if (a.summonOnly)                          continue;   // Wardogs et al
            if (weightOf(id) <= 0.f)                   continue;
            if (m_liveCount[id] >= a.maxActive)        continue;
            if (liveThreat + a.threatCost > maxThreat) continue;

            m_candidates.push_back(id);
            totalWeight += weightOf(id);
        }
        if (m_candidates.empty()) return;

        // ---- Weighted roll ----
        float roll = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * totalWeight;
        uint8_t chosen = m_candidates.back();
        for (uint8_t id : m_candidates) {
            roll -= weightOf(id);
            if (roll <= 0.f) { chosen = id; break; }
        }

        // ---- Place it ----
        //
        // Heavier units spawn further out. A Barge appearing 1200px away is a
        // wall that arrives before the player has read it; the extra distance
        // buys the approach time its silhouette is supposed to be doing work
        // during.
        const auto& def = archetypes[chosen];

        // ---- Ambush roll ----
        // A dormant unit is useless behind the player: nobody turns round to
        // inspect a wreck. It goes AHEAD, inside a cone around the direction
        // of travel, far enough out to arrive off-screen and near enough that
        // the player's own course brings them to it. Standing still, there is
        // no "ahead", so it falls back to a random bearing like everything
        // else.
        const float ambushChance = def.config["ambush_chance"].get_or(0.f)
            * (zcfg ? (*zcfg)["ambush_scale"].get_or(1.f) : 1.f);
        const bool dormant = ambushChance > 0.f
            && (rand() % 10000) < static_cast<int>(ambushChance * 10000.f);

        float dist = 1200.f + def.threatCost * 60.f;
        float angle = (rand() % 360) * 3.14159f / 180.f;
        if (dormant) {
            dist = def.config["ambush_spawn_distance"].get_or(1250.f);
            const size_t pIdx = m_em->getEntityIndex(m_playerEntityId);
            if (pIdx != (size_t)-1) {
                const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[pIdx].bodyId);
                if (v.x * v.x + v.y * v.y > 1.f)   // > 1 m/s: actually going somewhere
                    angle = std::atan2(v.y, v.x) + ((rand() % 1000) / 1000.f - 0.5f) * 1.2f;   // +-34 deg
            }
        }
        sf::Vector2f spawnPos = playerPos +
            sf::Vector2f(std::cos(angle) * dist, std::sin(angle) * dist);
        // Never in the Void. A blocked bearing tries others; if the whole
        // ring is Void (flying out past the edge), nobody comes.
        for (int tries = 0; tries < 8 && m_level && !m_level->spawnable(spawnPos, 600.f); ++tries) {
            angle = (rand() % 360) * 3.14159f / 180.f;
            spawnPos = playerPos + sf::Vector2f(std::cos(angle) * dist, std::sin(angle) * dist);
        }
        // ...and never inside a rock pile.
        for (int tries = 0; tries < 8 && m_level && !m_level->spawnable(spawnPos, 600.f, 160.f); ++tries) {
            angle = (rand() % 360) * 3.14159f / 180.f;
            spawnPos = playerPos + sf::Vector2f(std::cos(angle) * dist, std::sin(angle) * dist);
        }
        if (m_level && !m_level->spawnable(spawnPos, 600.f, 160.f)) return;

        // ---- Squadrons: a zone that fields them sends a group, not a unit ----
        if (!dormant && zcfg) {
            sol::optional<sol::table> sq = (*zcfg)["squads"];
            // Per-unit squads first: `by_unit = { RAIDER = { chance = 1, groups = {...} } }`
            // decides how THAT unit arrives (a Raider never alone, say);
            // anything not listed falls back to the zone's general groups.
            sol::optional<sol::table> pick;
            if (sq) {
                sol::optional<sol::table> bu = (*sq)["by_unit"];
                sol::optional<sol::table> mine;
                if (bu) mine = (*bu)[def.key].get<sol::optional<sol::table>>();
                sol::optional<sol::table> src = mine ? mine : sq;
                if ((rand() % 1000) < static_cast<int>((*src)["chance"].get_or(0.f) * 1000.f)) pick = src;
            }
            const int overUnits = sq ? (*sq)["overflow_units"].get_or(2) : 2;
            const int overThreat = sq ? (*sq)["overflow_threat"].get_or(6) : 6;
            if (pick) {
                sol::optional<sol::table> groups = (*pick)["groups"];
                if (groups && groups->size() > 0) {
                    sol::optional<sol::table> grp = (*groups)[1 + rand() % groups->size()];
                    if (grp && grp->size() > 0) {
                        int units = liveUnits, threat = liveThreat, spawned = 0;
                        const uint32_t link = m_em->newLinkGroup();   // a squad fights as one
                        for (size_t k = 1; k <= grp->size(); ++k) {
                            const uint8_t id = m_registry->idOf((*grp)[k].get_or<std::string>(""));
                            if (id == enemyarch::INVALID_ARCHETYPE) continue;
                            const auto& a = archetypes[id];
                            // The first one always comes (it replaces the unit
                            // the director rolled); the rest only while the
                            // zone's budget still has room for them.
                            if (spawned > 0 && (units >= maxActive + overUnits || threat + a.threatCost > maxThreat + overThreat)) break;
                            const float ga = k * 2.1f, gr = (k == 1) ? 0.f : 150.f + rand() % 90;
                            sf::Vector2f gp = spawnPos + sf::Vector2f(std::cos(ga), std::sin(ga)) * gr;
                            if (m_level && !m_level->spawnable(gp, 400.f, 120.f)) gp = spawnPos;
                            linkTo(m_ef->createEnemy(*m_em, gp, *m_lua, m_worldId, *m_registry, id, false), link);
                            ++units; threat += a.threatCost; ++spawned;
                            if (id < m_liveCount.size()) ++m_liveCount[id];
                        }
                        if (spawned > 0) return;
                    }
                }
            }
        }

        bool hidden = false;
        if (dormant && zcfg && (*zcfg)["ambush_cover"].get_or(false)) {
            hidden = hideBesideCover(spawnPos);
            if (!hidden) return;   // a sleeper alone in the open is no disguise: skip it
        }
        const uint32_t newId = m_ef->createEnemy(*m_em, spawnPos, *m_lua, m_worldId, *m_registry, chosen, dormant);
        if (hidden && newId) settleLikeWreck(newId, (rand() % 360) * 1.f);

        // ---- Escort: a pack leader arrives with his pack ----
        // `escort = { "RAIDER", ... }` on the archetype (the Bloodseeker). In
        // list order, as many as still fit the SAME gates the director just
        // applied -- per-unit cap, faction cap, threat budget -- so an escort
        // never breaks a zone's numbers. Never for an ambusher: a dead hull
        // with a live squad around it is no disguise.
        if (dormant) return;
        sol::optional<sol::table> escort = def.config["escort"];
        if (!escort) return;
        // Leader and escort fight as one.
        const uint32_t link = m_em->newLinkGroup();
        linkTo(newId, link);
        int units = liveUnits + 1;
        int threat = liveThreat + def.threatCost;
        ++m_liveCount[chosen];
        for (size_t k = 1; k <= escort->size(); ++k) {
            const std::string key = (*escort)[k].get_or<std::string>("");
            const uint8_t id = m_registry->idOf(key);
            if (id == enemyarch::INVALID_ARCHETYPE) continue;
            const auto& a = archetypes[id];
            if (units >= maxActive || threat + a.threatCost > maxThreat) break;
            if (m_liveCount[id] >= a.maxActive) continue;
            const float ea = (rand() % 360) * 3.14159f / 180.f;
            const float er = 130.f + rand() % 90;
            linkTo(m_ef->createEnemy(*m_em, spawnPos + sf::Vector2f(std::cos(ea), std::sin(ea)) * er,
                *m_lua, m_worldId, *m_registry, id, false), link);
            ++units; threat += a.threatCost; ++m_liveCount[id];
        }
    }

    // ---- Dependencies ----
    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId      m_worldId;
    uint32_t       m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    DevState* m_dev = nullptr;
    zonearch::ZoneState* m_zone = nullptr;
    levelgen::LevelState* m_level = nullptr;
    float m_fieldTimer = 0.f;
    std::vector<std::pair<float, int>> m_streamCandidates;

    // ---- Timers ----
    sf::Clock          m_asteroidSpawnClock;
    std::vector<float> m_factionClocks;      ///< One accumulator per faction

    // ---- Scratch, kept as members so the per-frame census allocates once ----
    std::vector<int>     m_liveCount;        ///< Live units, indexed by archetype id
    std::vector<uint8_t> m_candidates;
};