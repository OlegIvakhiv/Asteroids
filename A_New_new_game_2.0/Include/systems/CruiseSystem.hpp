/**
 * @file CruiseSystem.hpp
 * @brief Exploration vs combat: CRUISE when nothing is on you, HUNT when it is.
 *
 * ============================================================================
 * THE RULE
 * ============================================================================
 * A generated sector is big, and the ship's combat tuning is too slow to
 * cross it. So while nothing hostile is engaged the ship CRUISES: turbo is
 * faster and nearly free, shots cost half the heat and energy (clearing rocks
 * between fights). The moment anything engages, all of it is gone.
 *
 *   ENGAGED   an awake enemy within flee_distance that is in COMBAT, or that
 *             the player damaged in the last few seconds (provoke(): it also
 *             counts for a sleeper -- shooting one is starting the fight).
 *   HUNT      anything engaged -> cruise drops to 0 THAT frame.
 *   BACK      every engaged enemy dead -> cruise fades back in at once;
 *             or none of them within flee_distance for flee_time seconds
 *             (you outran them) -> the same.
 *   BLOCKED   a sub-zone with `cruise = false` in zones.lua (the asteroid
 *             field: full turbo through a rock pile is not exploring).
 *
 * The fade back in (cruise.fade_in) is there so a fight cannot be flicked in
 * and out of CRUISE to farm the cheap shots.
 *
 * Tuning: `cruise = { ... }` in player.lua. Everything has a default here.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/ZoneArchetypes.hpp"
#include <sol/sol.hpp>
#include <unordered_map>
#include <algorithm>
#include <cmath>

class CruiseSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_zone = ctx.zone;
        m_playerEntityId = ctx.playerEntityId;
        m_engaged.clear();
        m_farT = 0.f;
        m_wasCruise = true;
        m_first = true;
    }

    void update(float dt) override {
        if (!m_em) return;
        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        if (pi == (size_t)-1 || pi >= m_em->players.size()) return;
        auto& ps = m_em->players[pi];
        const sf::Vector2f pp = m_em->transforms[pi].position;

        const float fleeDist = cfg("flee_distance", 4000.f);
        const float fleeTime = cfg("flee_time", 5.f);
        const float fadeIn = std::max(0.05f, cfg("fade_in", 1.f));

        // ---- Who is on us ----
        for (size_t i = 0; i < m_em->enemies.size() && i < m_em->transforms.size(); ++i) {
            auto& ec = m_em->enemies[i];
            if (ec.playerHitTimer > 0.f) ec.playerHitTimer -= dt;
            if (i == pi || !isEnemy(i)) continue;
            const bool hunting = !ec.dormant && ec.visualState == EnemyState::COMBAT;
            if (!hunting && ec.playerHitTimer <= 0.f) continue;
            const sf::Vector2f q = m_em->transforms[i].position;
            if (std::hypot(q.x - pp.x, q.y - pp.y) > fleeDist) continue;
            m_engaged[m_em->transforms[i].entityId] = 1;
        }

        // ---- Dead or gone leave the list; far away for long enough clears it ----
        bool anyNear = false;
        for (auto it = m_engaged.begin(); it != m_engaged.end();) {
            const size_t i = m_em->getEntityIndex(it->first);
            if (i == (size_t)-1 || m_em->healths[i].currentHp <= 0.f) { it = m_engaged.erase(it); continue; }
            const sf::Vector2f q = m_em->transforms[i].position;
            if (std::hypot(q.x - pp.x, q.y - pp.y) <= fleeDist) anyNear = true;
            ++it;
        }
        if (!m_engaged.empty()) {
            m_farT = anyNear ? 0.f : m_farT + dt;
            if (m_farT >= fleeTime) { m_engaged.clear(); m_farT = 0.f; }
        }
        else m_farT = 0.f;

        // ---- The zone may forbid it outright ----
        bool blocked = false;
        if (m_zone)
            if (const zonearch::ZoneDef* z = m_zone->def())
                if (z->config.valid()) blocked = !z->config["cruise"].get_or(true);

        ps.cruiseBlocked = blocked;
        ps.huntedBy = static_cast<int>(m_engaged.size());
        // A boss arena is a fight from the horn to the last wall stone.
        const bool want = m_engaged.empty() && !blocked && !m_em->bossArena.active;
        if (!want) ps.cruise = 0.f;
        else ps.cruise = std::min(1.f, ps.cruise + dt / fadeIn);
        if (m_first) { ps.cruise = want ? 1.f : 0.f; m_first = false; m_wasCruise = want; return; }

        // ---- One short cue on each change (not for the dense-zone block) ----
        if (want != m_wasCruise) {
            if (!want && !blocked) m_em->sfxUi("hud.mode.hunt");
            else if (want)         m_em->sfxUi("hud.mode.cruise");
            m_wasCruise = want;
        }
    }

    /// Multipliers for the systems that read them. `c` is PlayerComponent::cruise.
    static float turboForce(sol::state& lua, float c) { return 1.f + (get(lua, "turbo_speed", 1.5f) - 1.f) * c; }
    static float turboDrain(sol::state& lua, float c) { return 1.f + (get(lua, "turbo_drain", 0.2f) - 1.f) * c; }
    static float shotCost(sol::state& lua, float c)   { return 1.f + (get(lua, "shot_cost", 0.5f) - 1.f) * c; }

private:
    bool isEnemy(size_t i) const {
        if (i >= m_em->physics.size()) return false;
        const BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
        return ud && ud->type == BodyType::Enemy;
    }
    float cfg(const char* k, float d) const { return m_lua ? get(*m_lua, k, d) : d; }
    static float get(sol::state& lua, const char* k, float d) {
        sol::optional<sol::table> t = lua["cruise"];
        return t ? (*t)[k].get_or(d) : d;
    }

    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    zonearch::ZoneState* m_zone = nullptr;
    uint32_t m_playerEntityId = 0;
    std::unordered_map<uint32_t, char> m_engaged;   ///< entity ids engaged with the player
    float m_farT = 0.f;
    bool  m_wasCruise = true;
    bool  m_first = true;
};
