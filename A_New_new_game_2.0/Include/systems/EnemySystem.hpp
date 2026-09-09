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
#include <SFML/System/Clock.hpp>
#include <cmath>
#include <cstdlib>
#include <vector>

class EnemySystem : public ISystem {
public:

    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;

        m_asteroidSpawnClock.restart();
        m_factionClocks.clear();
    }

    void update(float dt) override {
        if (!m_em || !m_lua || !m_ef) return;

        const size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;

        updateAsteroids(playerPos);
        updateFactions(dt, playerPos);
    }

    /**
     * @brief Spawn a unit by archetype key, ignoring all director gates.
     *
     * This is the door for summon-only units. A Barge calling in Wardogs, or a
     * scripted POI dropping a squad, goes through here -- NOT through the
     * director, which would refuse them.
     *
     * @return The new entity id, or 0 if the archetype does not exist.
     */
    uint32_t summon(const std::string& archetypeKey, sf::Vector2f pos) {
        if (!m_registry) return 0;
        const uint8_t id = m_registry->idOf(archetypeKey);
        if (id == enemyarch::INVALID_ARCHETYPE) {
            std::cerr << "[EnemySystem] summon(\"" << archetypeKey
                << "\") -- no such archetype.\n";
            return 0;
        }
        return m_ef->createEnemy(*m_em, pos, *m_lua, m_worldId, *m_registry, id);
    }

private:

    // ========================================================================
    // ASTEROIDS -- behaviour unchanged from v1.1
    // ========================================================================

    void updateAsteroids(sf::Vector2f playerPos) {
        sol::table astSettings = (*m_lua)["spawn_settings"];
        const float astInterval = astSettings["interval"].get_or(1.0f);
        const int   maxAstCount = astSettings["max_count"].get_or(40);
        const float spawnRadius = astSettings["spawn_radius"].get_or(1500.f);

        if (m_asteroidSpawnClock.getElapsedTime().asSeconds() <= astInterval) return;

        int currentAsteroids = 0;
        for (const auto& p : m_em->physics) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(p.bodyId);
            if (ud && ud->type == BodyType::Asteroid) ++currentAsteroids;
        }
        if (currentAsteroids >= maxAstCount) return;

        sol::table types = (*m_lua)["asteroid_types"];

        const char* typeKeys[] = { "SMALL", "MEDIUM", "LARGE" };
        const char* selectedType = typeKeys[rand() % 3];

        const float magmaticChance = (*m_lua)["spawn_settings"]["magmatic_chance"].get_or(0.15f);
        const bool isMagmatic = ((rand() % 100) / 100.f) < magmaticChance;
        if (isMagmatic && (*m_lua)["asteroid_types"]["MAGMATIC"].valid()) {
            selectedType = "MAGMATIC";
        }

        sol::table config = types[selectedType];

        const float angle = (rand() % 360) * 3.14159f / 180.f;
        const sf::Vector2f spawnPos = playerPos +
            sf::Vector2f(std::cos(angle) * spawnRadius, std::sin(angle) * spawnRadius);

        const sf::Vector2f offset((rand() % 400) - 200.f, (rand() % 400) - 200.f);
        const sf::Vector2f dir = (playerPos + offset) - spawnPos;
        const float len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));

        sol::table speedRange = config["speed_range"];
        const float speed = speedRange[1].get<float>() +
            (rand() % 100 / 100.f) * (speedRange[2].get<float>() - speedRange[1].get<float>());

        const uint32_t astId = m_ef->createAsteroid(*m_em, spawnPos, (dir / len) * speed,
            config["base_size"], config, m_worldId);

        const size_t astIdx = m_em->getEntityIndex(astId);
        if (astIdx != (size_t)-1) {
            if (isMagmatic) {
                m_em->healths[astIdx].isExplosive = true;
                m_em->healths[astIdx].explosionRadius = config["explosion_radius"].get_or(150.0f);
                m_em->healths[astIdx].explosionDamage = config["explosion_damage"].get_or(30.0f);
            }
            const float randomSpin = ((rand() % 200) - 100.f) / 50.f;
            b2Body_SetAngularVelocity(m_em->physics[astIdx].bodyId, randomSpin);
        }

        m_asteroidSpawnClock.restart();
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
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            const uint8_t a = m_em->enemies[i].archetype;
            if (a < m_liveCount.size()) ++m_liveCount[a];
        }

        // ---- Per-faction timers ----
        const auto& factions = m_registry->factions();
        if (m_factionClocks.size() != factions.size()) {
            m_factionClocks.assign(factions.size(), 0.f);
        }

        for (size_t f = 0; f < factions.size(); ++f) {
            const auto& fac = factions[f];
            if (!fac.active || fac.units.empty()) continue;

            m_factionClocks[f] += dt;
            if (m_factionClocks[f] < fac.spawnInterval) continue;

            // Reset regardless of whether the attempt succeeds. Otherwise a
            // full field would bank up elapsed time and dump a burst of units
            // the instant one slot opened.
            m_factionClocks[f] = 0.f;

            trySpawnForFaction(fac, playerPos);
        }
    }

    void trySpawnForFaction(const enemyarch::FactionDef& fac, sf::Vector2f playerPos) {
        const auto& archetypes = m_registry->all();

        // ---- Current faction load ----
        int liveUnits = 0;
        int liveThreat = 0;
        for (uint8_t id : fac.units) {
            const int n = m_liveCount[id];
            liveUnits += n;
            liveThreat += n * archetypes[id].threatCost;
        }
        if (liveUnits >= fac.maxActive) return;

        // ---- Build the candidate list ----
        m_candidates.clear();
        float totalWeight = 0.f;

        for (uint8_t id : fac.units) {
            const auto& a = archetypes[id];
            if (a.summonOnly)                          continue;   // Wardogs et al
            if (a.weight <= 0.f)                       continue;
            if (m_liveCount[id] >= a.maxActive)        continue;
            if (liveThreat + a.threatCost > fac.maxThreat) continue;

            m_candidates.push_back(id);
            totalWeight += a.weight;
        }
        if (m_candidates.empty()) return;

        // ---- Weighted roll ----
        float roll = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * totalWeight;
        uint8_t chosen = m_candidates.back();
        for (uint8_t id : m_candidates) {
            roll -= archetypes[id].weight;
            if (roll <= 0.f) { chosen = id; break; }
        }

        // ---- Place it ----
        //
        // Heavier units spawn further out. A Barge appearing 1200px away is a
        // wall that arrives before the player has read it; the extra distance
        // buys the approach time its silhouette is supposed to be doing work
        // during.
        const auto& def = archetypes[chosen];
        const float dist = 1200.f + def.threatCost * 60.f;
        const float angle = (rand() % 360) * 3.14159f / 180.f;
        const sf::Vector2f spawnPos = playerPos +
            sf::Vector2f(std::cos(angle) * dist, std::sin(angle) * dist);

        m_ef->createEnemy(*m_em, spawnPos, *m_lua, m_worldId, *m_registry, chosen);
    }

    // ---- Dependencies ----
    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId      m_worldId;
    uint32_t       m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;

    // ---- Timers ----
    sf::Clock          m_asteroidSpawnClock;
    std::vector<float> m_factionClocks;      ///< One accumulator per faction

    // ---- Scratch, kept as members so the per-frame census allocates once ----
    std::vector<int>     m_liveCount;        ///< Live units, indexed by archetype id
    std::vector<uint8_t> m_candidates;
};