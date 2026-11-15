/**
 * @file BountySystem.hpp
 * @brief "Hunting for your head": kill enough and the Rakshari send hunters.
 *
 * ============================================================================
 * THE RULE
 * ============================================================================
 *   HEAT     every kill the player's fight makes adds its threat cost to
 *            EntityManager::bountyHeat; heat bleeds off slowly (decay/s).
 *   CONTRACT past `trigger` heat, each further kill may (chance) put a price
 *            on your head -- never twice within `cooldown` s of the last
 *            hunt ending.
 *   WARNING  "BOUNTY ON YOUR HEAD // HUNTERS INBOUND" and a countdown
 *            (`warn_time`): time to finish the fight, run, or get ready.
 *   HUNTERS  one party, spawned off-screen round you, linked (they fight as
 *            one) and in COMBAT on arrival: they know where you are, that is
 *            the contract. The HUD counts them down; kill them all and the
 *            hunt is over (heat halves).
 *   RANK     which party comes is your record: sectors cleared, scrap ever
 *            picked up, ships killed (`rank = { ... }`). Each tier has a
 *            `min_rank`; the best one you qualify for comes most of the time,
 *            one tier lower the rest (`step_down`).
 *
 * Tuning: `bounty = { ... }` in enemy.lua.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/LevelGen.hpp"
#include "utils/UiPalette.hpp"
#include "systems/EnemySystem.hpp"
#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

class BountySystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_level = ctx.level;
        m_window = ctx.drawTarget();
        m_playerEntityId = ctx.playerEntityId;
        m_state = State::Idle;
        m_cooldown = cfg("first_cooldown", 60.f);
        m_hunters.clear();
        m_lastKills = m_em ? m_em->runKills : 0;
        m_time = 0.f;
    }
    void setEnemySystem(EnemySystem* e) { m_enemies = e; }
    void setFont(const sf::Font* f) { m_font = f; }

    void update(float dt) override {
        if (!m_em || !m_lua || !m_level || !m_level->active) return;
        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        if (pi == (size_t)-1) return;
        m_time += dt;
        m_em->bountyHeat = std::max(0.f, m_em->bountyHeat - cfg("decay", 0.05f) * dt);

        const int kills = m_em->runKills;
        const bool newKill = kills > m_lastKills;
        m_lastKills = kills;

        // A boss arena: no contract is let, no party sent, until it falls.
        if (m_em->bossArena.active) {
            if (m_state == State::Warning) m_timer = std::max(m_timer, 3.f);
            if (m_doneT > 0.f) m_doneT -= dt;
            return;
        }

        switch (m_state) {
        case State::Idle:
            m_cooldown -= dt;
            if (newKill && m_cooldown <= 0.f && m_em->bountyHeat >= cfg("trigger", 10.f)
                && (rand() % 1000) < static_cast<int>(cfg("chance", 0.5f) * 1000.f)) {
                m_tier = pickTier();
                if (m_tier >= 0) {
                    m_state = State::Warning;
                    m_timer = cfg("warn_time", 8.f);
                    m_em->sfxUi("hud.bounty.warning");
                }
            }
            break;
        case State::Warning:
            m_timer -= dt;
            if (m_timer <= 0.f) {
                spawnParty(m_em->transforms[pi].position);
                m_state = m_hunters.empty() ? State::Idle : State::Hunt;
                if (m_state == State::Idle) m_cooldown = 20.f;
            }
            break;
        case State::Hunt: {
            int alive = 0;
            for (uint32_t id : m_hunters) if (m_em->getEntityIndex(id) != (size_t)-1) ++alive;
            m_alive = alive;
            if (alive == 0) {
                m_state = State::Idle;
                m_cooldown = cfg("cooldown", 120.f);
                m_em->bountyHeat *= 0.5f;
                m_hunters.clear();
                m_doneT = 3.f;
                m_em->sfxUi("hud.bounty.cleared");
            }
            break;
        }
        }
        if (m_doneT > 0.f) m_doneT -= dt;
    }

    /// The warning, the countdown, the hunter count. Screen space.
    void drawHud(sf::Vector2f ws) {
        if (!m_font || !m_window || !m_level || !m_level->active) return;
        const float cx = ws.x * 0.5f, y = ws.y * 0.13f;
        if (m_state == State::Warning) {
            const float pulse = 0.6f + 0.4f * std::sin(m_time * 8.f);
            centred("BOUNTY ON YOUR HEAD", cx, y, 18, sf::Color(255, 70, 60, static_cast<uint8_t>(255 * pulse)));
            centred(upper(m_tierName) + "  //  HUNTERS INBOUND  " + std::to_string(static_cast<int>(std::ceil(m_timer))) + "s",
                cx, y + 28.f, 12, sf::Color(255, 190, 170));
        }
        else if (m_state == State::Hunt)
            centred("HUNTED  //  " + std::to_string(m_alive) + (m_alive == 1 ? " HUNTER LEFT" : " HUNTERS LEFT"),
                cx, y, 13, sf::Color(255, 110, 90));
        else if (m_doneT > 0.f)
            centred("THE HUNTERS ARE DEAD  //  BOUNTY LIFTED", cx, y, 13,
                sf::Color(255, 205, 110, static_cast<uint8_t>(255 * std::min(1.f, m_doneT))));
    }

private:
    enum class State { Idle, Warning, Hunt };

    float cfg(const char* k, float d) const {
        sol::optional<sol::table> t = (*m_lua)["bounty"];
        return t ? (*t)[k].get_or(d) : d;
    }

    /// The player's record, as a single number.
    float rank() const {
        sol::optional<sol::table> r = (*m_lua)["bounty"]["rank"].get<sol::optional<sol::table>>();
        const float perLevel = r ? (*r)["per_sector"].get_or(1.f) : 1.f;
        const float perScrap = r ? (*r)["per_scrap"].get_or(1.f / 150.f) : 1.f / 150.f;
        const float perKill = r ? (*r)["per_kill"].get_or(1.f / 12.f) : 1.f / 12.f;
        const int sectors = m_level->level.index;   // cleared before this one
        return sectors * perLevel + m_em->runScrap * perScrap + m_em->runKills * perKill;
    }

    int pickTier() {
        sol::optional<sol::table> tiers = (*m_lua)["bounty"]["tiers"].get<sol::optional<sol::table>>();
        if (!tiers || tiers->size() == 0) return -1;
        const float rk = rank();
        int best = 0;
        for (size_t k = 1; k <= tiers->size(); ++k)
            if ((*tiers)[k]["min_rank"].get_or(0.f) <= rk) best = static_cast<int>(k) - 1;
        if (best > 0 && (rand() % 1000) < static_cast<int>(cfg("step_down", 0.3f) * 1000.f)) --best;
        m_tierName = (*tiers)[best + 1]["name"].get_or(std::string("Bounty hunters"));
        return best;
    }

    void spawnParty(sf::Vector2f pp) {
        m_hunters.clear();
        if (!m_enemies) return;
        sol::optional<sol::table> tier = (*m_lua)["bounty"]["tiers"][m_tier + 1].get<sol::optional<sol::table>>();
        if (!tier) return;
        sol::optional<sol::table> units = (*tier)["units"];
        if (!units) return;
        // Off-screen, somewhere you can be reached from.
        const float dist = cfg("spawn_distance", 2600.f);
        sf::Vector2f at = pp;
        for (int t = 0; t < 12; ++t) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const sf::Vector2f q = pp + sf::Vector2f(std::cos(a), std::sin(a)) * dist;
            if (m_level->spawnable(q, 800.f, 200.f)) { at = q; break; }
        }
        const uint32_t link = m_em->newLinkGroup();
        for (size_t k = 1; k <= units->size(); ++k) {
            const std::string key = (*units)[k].get_or<std::string>("");
            if (key.empty()) continue;
            const float a = k * 2.3f, r = (k == 1) ? 0.f : 140.f + (rand() % 80);
            sf::Vector2f q = at + sf::Vector2f(std::cos(a), std::sin(a)) * r;
            if (!m_level->spawnable(q, 400.f, 120.f)) q = at;
            const uint32_t id = m_enemies->summon(key, q, false);
            if (!id) continue;
            const size_t i = m_em->getEntityIndex(id);
            if (i == (size_t)-1) continue;
            m_em->enemies[i].linkGroup = link;
            m_em->enemies[i].attackedByPlayer = true;   // social pass: straight into COMBAT, on you
            m_hunters.push_back(id);
        }
        m_alive = static_cast<int>(m_hunters.size());
        m_em->sfxUi("hud.bounty.arrive");
    }

    static std::string upper(std::string s) { for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); return s; }

    void centred(const std::string& s, float cx, float y, unsigned size, sf::Color c) {
        ui::label(*m_window, m_font, cx - ui::labelWidth(m_font, s, size) * 0.5f, y, s, size, c);
    }

    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    levelgen::LevelState* m_level = nullptr;
    sf::RenderTarget* m_window = nullptr;
    EnemySystem* m_enemies = nullptr;
    const sf::Font* m_font = nullptr;
    uint32_t m_playerEntityId = 0;

    State m_state = State::Idle;
    float m_timer = 0.f, m_cooldown = 0.f, m_time = 0.f, m_doneT = 0.f;
    int   m_tier = -1, m_alive = 0, m_lastKills = 0;
    std::string m_tierName;
    std::vector<uint32_t> m_hunters;
};
