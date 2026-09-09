/**
 * @file VentQTESystem.hpp
 * @brief Timed vent minigame that turns the overheat lockout into a decision.
 *
 * THE PROBLEM IT SOLVES
 * ---------------------
 * Overheat is currently dead air. You hit the cap, you wait ~1.4 seconds, you
 * shoot again. The player's only input during the most dangerous moment in the
 * loop is "stop pressing the button", which is not a mechanic -- it is a
 * timeout. Meanwhile the fight keeps happening around them.
 *
 * The QTE converts that window into a skill check with three outcomes:
 *
 *   PERFECT (narrow amber)  heat zeroed, plus OVERDRIVE: NO weapon generates
 *                           heat at all for a few seconds. Plasma and Rift
 *                           alike -- see the addHeat note in the integration
 *                           doc for why a half-working version is the likely
 *                           failure mode.
 *   GOOD    (wider blue)    heat zeroed. No overdrive. Where most attempts
 *                           should land.
 *   MISS / TIMEOUT          the normal vent, from full.
 *
 *
 * THE BAR FREEZES, AND THAT MAKES MISSING COST SOMETHING
 * ------------------------------------------------------
 * Heat is PINNED at maximum for as long as the QTE is open -- the gauge stops
 * dead at the overheat line and becomes the minigame. Venting only resumes once
 * the QTE resolves.
 *
 * That changes the risk shape from the first draft, and it is worth being
 * explicit about: because venting is paused, a timeout costs you the whole QTE
 * window ON TOP of the vent you already owed. The QTE is no longer upside-only.
 * It is a gamble -- time wagered for the chance at overdrive.
 *
 * Two consequences fall out, and both are good:
 *   - qte_timeout has to be SHORT (2.4s, not 3.2s), or the wager is unfair.
 *   - a wrong press CLOSES the QTE immediately, so a player who knows they have
 *     missed can bail out and start venting rather than waiting out the clock.
 *     Deliberately missing early is the correct play, and it is available
 *     without any extra input.
 *
 *
 * WHY THE SWEEP ACCELERATES ON A STREAK
 * -------------------------------------
 * "Perfect vent grants unlimited fire" is a loop that closes on itself: fire
 * freely, overheat, hit perfect, fire freely again. If the check is the same
 * difficulty every time, a player who can hit it once can hit it forever, and
 * the heat system stops existing for them.
 *
 * So consecutive perfects speed the marker up (`qte_streak_speed_step`, capped
 * by `qte_speed_max`). The streak resets on anything that is not a perfect.
 * The loop stays open to a skilled player for a while and then closes on its
 * own, without a hard cap that would feel arbitrary.
 *
 *
 * WHY A DEDICATED KEY
 * -------------------
 * The vent key is NOT the fire button. Players mash fire the instant they
 * overheat -- that is the reflex the lockout creates -- so binding the QTE to
 * fire would auto-fail it before the bar was even read. It has to be a
 * deliberate, separate press.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/UiPalette.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

class VentQTESystem : public ISystem {
public:

    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_ventWasDown = false;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;

        const size_t idx = m_em->getEntityIndex(m_playerEntityId);
        if (idx == (size_t)-1) return;

        auto& ps = m_em->players[idx];
        const auto& tf = m_em->transforms[idx];

        // ---- Overdrive countdown ----
        //
        // Ticks here rather than in WeaponSystem::updateHeat because it must
        // keep running while the gun is idle. A reward that only decays while
        // you are shooting would quietly last forever if you stopped.
        if (ps.overdriveTimer > 0.f) {
            ps.overdriveTimer = std::max(0.f, ps.overdriveTimer - dt);

            if (ps.overdriveTimer <= 0.f) {
                // Coolant runs out. Optional exit heat so the loop is not
                // strictly free; defaults to 0, which is the plain reading of
                // "heat doesn't rise for a few seconds".
                const float exitHeat = cfg("overdrive_exit_heat", 0.f);
                if (exitHeat > 0.f)
                    ps.weaponHeat = std::min(ps.maxWeaponHeat, exitHeat);

                m_em->spawnShockRing(tf.position, 14.f, 90.f, 0.28f,
                    sf::Color(120, 220, 255), 3.f, 190.f);
            }
            else {
                // Coolant plume, so overdrive is legible from the ship and not
                // only from the HUD.
                if ((rand() % 100) < 45) {
                    const float a = (rand() % 360) * 3.14159f / 180.f;
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        tf.position + sf::Vector2f(std::cos(a), std::sin(a)) * 22.f,
                        sf::Vector2f(std::cos(a), std::sin(a)) * (40.f + rand() % 60),
                        sf::Color(140, 230, 255, 190),
                        0.28f, 0.34f, 2.f + rand() % 3 });
                }
            }
        }

        if (ps.qteResultFlash > 0.f) ps.qteResultFlash -= dt;

        // ---- Open the QTE the moment the lockout starts ----
        //
        // Detected here by edge rather than started from addHeat(), so the two
        // ways to overheat (chip damage from normal fire, and a Rift Shot
        // dumping 52 at once) both route through one place.
        if (ps.weaponOverheated && !m_wasOverheated) beginQTE(ps);
        if (!ps.weaponOverheated && ps.qteActive)    closeQTE(ps, Result::Miss);
        m_wasOverheated = ps.weaponOverheated;

        if (!ps.qteActive) { readVentEdge(); return; }

        // ---- Heat is pinned while the QTE is open ----
        //
        // Asserted here as well as in WeaponSystem::updateHeat. updateHeat runs
        // after this system, so it owns the freeze; this line only guards
        // against the gauge visibly sagging for a frame if the ordering is ever
        // changed. The gauge stopping DEAD at the overheat line is the whole
        // read -- a bar that drifts down during the QTE tells the player they
        // can just wait, which is exactly the behaviour the minigame replaces.
        ps.weaponHeat = ps.maxWeaponHeat;

        // ---- Sweep ----
        ps.qtePos += ps.qteDir * ps.qteSpeed * dt;
        if (ps.qtePos >= 1.f) { ps.qtePos = 1.f; ps.qteDir = -1.f; ps.qteSweeps++; }
        if (ps.qtePos <= 0.f) { ps.qtePos = 0.f; ps.qteDir = 1.f;  ps.qteSweeps++; }

        ps.qteTimeout -= dt;

        const int maxSweeps = static_cast<int>(cfg("qte_max_sweeps", 3.f));
        if (ps.qteTimeout <= 0.f || ps.qteSweeps >= maxSweeps) {
            // Ran out of chances. Not a failure state -- just the normal vent
            // you would have had anyway.
            closeQTE(ps, Result::Miss);
            readVentEdge();
            return;
        }

        // ---- Input ----
        if (readVentEdge()) {
            const float d = std::fabs(ps.qtePos - ps.qteGoodCenter);

            if (d <= ps.qtePerfectHalf)   resolve(ps, tf, idx, Result::Perfect);
            else if (d <= ps.qteGoodHalf) resolve(ps, tf, idx, Result::Good);
            else                          resolve(ps, tf, idx, Result::Miss);
        }
    }

private:

    enum class Result { Perfect = 1, Good = 2, Miss = 3 };

    float cfg(const char* key, float fallback) const {
        return (*m_lua)[key].get_or(fallback);
    }

    /**
     * @brief Rising edge on the vent key.
     *
     * TWO separate things are going on here, and getting either wrong makes the
     * key appear dead.
     *
     * 1. InputRegistry::isPressed takes a PHYSICAL KEY NAME ("E", "Space",
     *    "MouseLeft"), not an action name. The action -> key indirection is the
     *    caller's job, exactly as InputSystem and WeaponSystem do it. Passing
     *    "vent" straight in misses both lookup tables and returns false
     *    forever, with no error -- isPressed has no way to distinguish "that
     *    key is up" from "I have never heard of that name".
     *
     * 2. isPressed reports HELD state. Without edge tracking, a single press
     *    would resolve the QTE on the first frame and then re-resolve it on
     *    every frame the key stayed down.
     *
     * get_or rather than get<std::string>() so a player.lua that predates the
     * `vent` binding falls back to E instead of throwing.
     */
    bool readVentEdge() {
        sol::table binds = (*m_lua)["key_bindings"];
        const std::string key = binds.valid()
            ? binds["vent"].get_or<std::string>("E") : std::string("E");

        const bool down = InputRegistry::isPressed(key);
        const bool edge = down && !m_ventWasDown;
        m_ventWasDown = down;
        return edge;
    }

    void beginQTE(PlayerComponent& ps) {
        ps.qteActive = true;
        ps.qtePos = 0.f;
        ps.qteDir = 1.f;
        ps.qteSweeps = 0;
        ps.qteResult = 0;
        ps.qteTimeout = cfg("qte_timeout", 2.4f);

        // Target moves every time. A fixed centre becomes muscle memory in
        // about five attempts, at which point the check stops being a check.
        const float margin = 0.22f;
        ps.qteGoodCenter = margin +
            (static_cast<float>(rand()) / RAND_MAX) * (1.f - margin * 2.f);

        ps.qteGoodHalf = cfg("qte_good_half", 0.105f);
        ps.qtePerfectHalf = cfg("qte_perfect_half", 0.035f);

        // Streak ramp. See the header note on why this exists.
        const float base = cfg("qte_speed", 1.30f);
        const float step = cfg("qte_streak_speed_step", 0.16f);
        const float cap = cfg("qte_speed_max", 2.35f);
        ps.qteSpeed = std::min(cap, base + step * static_cast<float>(ps.qteStreak));
    }

    void closeQTE(PlayerComponent& ps, Result r) {
        ps.qteActive = false;
        ps.qteResult = static_cast<int>(r);
        ps.qteResultFlash = 0.55f;
        if (r != Result::Perfect) ps.qteStreak = 0;
    }

    void resolve(PlayerComponent& ps, const TransformComponent& tf,
        size_t idx, Result r)
    {
        switch (r) {

        case Result::Perfect: {
            ps.weaponHeat = 0.f;
            ps.weaponOverheated = false;
            ps.heatCoolDelay = 0.f;
            ps.overdriveTimer = cfg("overdrive_duration", 4.0f);
            ps.qteStreak++;

            m_em->spawnScreenFlash(sf::Color(255, 190, 70), 0.22f, 90.f);
            m_em->spawnShockRing(tf.position, 16.f, 190.f, 0.38f,
                ui::AMBER_HOT, 5.f, 300.f);
            m_em->requestHitstop(0.02f, 0.10f, 0.40f);
            m_em->addTrauma(cfg("qte_perfect_trauma", 0.28f));

            for (int k = 0; k < 26; ++k) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({
                    m_em->nextEntityId++, tf.position,
                    sf::Vector2f(std::cos(a), std::sin(a)) * (150.f + rand() % 220),
                    ui::alpha(ui::AMBER_HOT, 0.90f), 0.42f, 0.5f, 3.f + rand() % 3 });
            }
            closeQTE(ps, Result::Perfect);
            ps.qteStreak = std::max(1, ps.qteStreak);   // closeQTE must not clear it
            break;
        }

        case Result::Good: {
            ps.weaponHeat = 0.f;
            ps.weaponOverheated = false;
            ps.heatCoolDelay = 0.f;

            m_em->spawnShockRing(tf.position, 12.f, 130.f, 0.30f,
                ui::BLUE_COOL, 4.f, 230.f);
            for (int k = 0; k < 14; ++k) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({
                    m_em->nextEntityId++, tf.position,
                    sf::Vector2f(std::cos(a), std::sin(a)) * (90.f + rand() % 130),
                    ui::alpha(ui::BLUE_COOL, 0.82f), 0.34f, 0.4f, 2.f + rand() % 3 });
            }
            closeQTE(ps, Result::Good);
            break;
        }

        case Result::Miss:
        default: {
            // Nothing is taken away. The gun vents on its normal schedule,
            // which is exactly the situation the player was already in.
            m_em->spawnShockRing(tf.position, 10.f, 60.f, 0.20f,
                sf::Color(150, 90, 70), 2.f, 140.f);
            closeQTE(ps, Result::Miss);
            break;
        }
        }
    }

    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    uint32_t    m_playerEntityId = 0;

    bool m_ventWasDown = false;
    bool m_wasOverheated = false;
};