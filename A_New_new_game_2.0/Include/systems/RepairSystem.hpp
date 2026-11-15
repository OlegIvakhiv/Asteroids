/**
 * @file RepairSystem.hpp
 * @brief Repair kits: Q to mend the hull, slowly, at a price.
 *
 * ============================================================================
 * THE RULE
 * ============================================================================
 *   Q             spends one kit (a run starts with repair.start_kits).
 *                 Refused at full hull, mid-tumble, or with a kit already running.
 *   THE MEND      half of max HP (repair.amount), given over repair.time_hunt
 *                 seconds -- or repair.time_cruise while CRUISE is up, so a
 *                 patch-up between fights is quick and one mid-fight is a gamble.
 *   WHILE IT RUNS the ship crawls (repair.move_scale of its thrust, no turbo),
 *                 cannot shoot or parry -- but CAN dodge. The hull outline
 *                 breathes yellow and sparks come off it.
 *   A HIT         any hull damage stops it. What was left of that kit is gone.
 *
 * InputSystem and WeaponSystem read PlayerComponent::repairing; this system
 * owns everything else. Tuning: `repair = { ... }` in player.lua.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/InputRegistry.hpp"
#include <sol/sol.hpp>
#include <algorithm>
#include <cmath>
#include <string>

class RepairSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_keyWasDown = true;   // a Q held through a restart must not fire
        m_lastHp = -1.f;
        m_sparkT = 0.f;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;
        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        if (pi == (size_t)-1 || pi >= m_em->players.size()) return;
        auto& ps = m_em->players[pi];
        auto& hp = m_em->healths[pi];

        // ---- A hit stops it ----
        if (ps.repairing && m_lastHp >= 0.f && hp.currentHp < m_lastHp - 0.01f) {
            stop(ps);
            m_em->sfxUi("player.repair.break");
        }

        // ---- Q ----
        sol::table binds = (*m_lua)["key_bindings"];
        const std::string key = binds["repair"].get_or(std::string("Q"));
        const bool down = InputRegistry::isPressed(key);
        if (down && !m_keyWasDown) {
            const bool canStart = !ps.repairing && ps.repairKits > 0
                && hp.currentHp < hp.maxHp - 0.5f && ps.staggerTimer <= 0.f;
            if (canStart) {
                --ps.repairKits;
                ps.repairing = true;
                ps.repairLeft = hp.maxHp * cfg("amount", 0.5f);
                ps.repairPulse = 0.f;
                m_em->sfxUi("player.repair.start");
            }
            else if (!ps.repairing && ps.repairKits <= 0) m_em->sfxUi("player.energy.dry");
        }
        m_keyWasDown = down;

        // ---- The mend ----
        if (ps.repairing) {
            const float total = hp.maxHp * cfg("amount", 0.5f);
            const float time = ps.cruise >= 0.5f ? cfg("time_cruise", 5.f) : cfg("time_hunt", 10.f);
            const float give = std::min(ps.repairLeft, total / std::max(0.5f, time) * dt);
            hp.currentHp = std::min(hp.maxHp, hp.currentHp + give);
            ps.repairLeft -= give;
            ps.repairPulse += dt;
            if (ps.repairLeft <= 0.01f || hp.currentHp >= hp.maxHp - 0.01f) {
                stop(ps);
                m_em->sfxUi("player.repair.done");
            }

            // Sparks: a few a second, off the hull, yellow-white.
            m_sparkT -= dt;
            if (m_sparkT <= 0.f) {
                m_sparkT = 0.06f;
                const sf::Vector2f p = m_em->transforms[pi].position;
                const float r = std::max(18.f, hp.visualRadius > 0.f ? hp.visualRadius : 28.f);
                for (int k = 0; k < 2; ++k) {
                    const float a = (rand() % 628) / 100.f;
                    const sf::Vector2f dir(std::cos(a), std::sin(a));
                    const float sp = 60.f + rand() % 90;
                    const float life = 0.25f + (rand() % 30) / 100.f;
                    m_em->particles.push_back({ p + dir * r * 0.9f, dir * sp,
                        sf::Color(255, 225 + rand() % 30, 90 + rand() % 90), life, life, 2.f + (rand() % 20) / 10.f });
                }
            }
        }
        m_lastHp = hp.currentHp;
    }

private:
    void stop(PlayerComponent& ps) { ps.repairing = false; ps.repairLeft = 0.f; }

    float cfg(const char* k, float d) const {
        sol::optional<sol::table> t = (*m_lua)["repair"];
        return t ? (*t)[k].get_or(d) : d;
    }

    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    uint32_t m_playerEntityId = 0;
    bool  m_keyWasDown = true;
    float m_lastHp = -1.f;
    float m_sparkT = 0.f;
};
