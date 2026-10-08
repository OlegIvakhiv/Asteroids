/**
 * @file HudSystem.hpp
 * @brief Stylized screen-space HUD (health, energy, heat, scrap)
 *
 * Pulled out of game.cpp, which was building SFML shapes inline every frame.
 *
 * DESIGN NOTES � why these bars look the way they do:
 *
 *  1. SKEWED PARALLELOGRAMS, not rectangles. A 12px shear costs nothing and
 *     instantly stops the HUD reading as programmer-art. Everything is drawn
 *     as sf::ConvexShape quads so the skew is free.
 *
 *  2. SEGMENTED FILLS. Ticks cut into the bar every N units. Continuous bars
 *     are hard to read at a glance � segments let you count remaining chunks
 *     in peripheral vision without looking directly at the HUD.
 *
 *  3. DAMAGE GHOST. The health bar keeps a second, slower bar behind the real
 *     one that drains to catch up over ~0.4s. This is the single highest-value
 *     HUD trick there is: it tells you HOW MUCH you just lost, not merely that
 *     you lost something.
 *
 *  4. THRESHOLD PIPS. The heat bar marks the overheat point and the vent
 *     unlock point. A resource with invisible thresholds feels arbitrary.
 *
 *  5. STATE-DRIVEN COLOUR. Heat uses the same cyan->orange->white ramp as the
 *     ship's nose glow, so the bar and the ship tell the same story with the
 *     same vocabulary.
 *
 * Drawn in screen space � SystemManager must set the default view first.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "utils/LuaConfig.hpp"
#include "core/EntityManager.hpp"
#include "utils/UiPalette.hpp"       
#include <SFML/Graphics.hpp>
#include <cmath>
#include <algorithm>
#include <cstdio>

class HudSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_time = 0.f;
        m_ghostHp = -1.f;
        m_displayScrap = 0.f;
        m_lastScrap = -1;
        m_gainAccum = 0;
        m_gainTimer = 0.f;
        m_scrapPulse = 0.f;
        m_qteOpen = 0.f;
    }

    /// Fonts are owned by game.cpp; pass one in after init().
    void setFont(const sf::Font* font) { m_font = font; }

    void update(float dt) override {
        if (!m_em || !m_window) return;

        size_t idx = m_em->getEntityIndex(m_playerEntityId);
        if (idx == (size_t)-1) return;

        m_time += dt;

        const auto& hp = m_em->healths[idx];
        const auto& ps = m_em->players[idx];

        // ---- Damage ghost: snaps up, drains down slowly ----
        if (m_ghostHp < 0.f) m_ghostHp = hp.currentHp;
        if (hp.currentHp > m_ghostHp) m_ghostHp = hp.currentHp;
        else m_ghostHp += (hp.currentHp - m_ghostHp) * (1.f - std::exp(-4.5f * dt));

        // ---- Scrap counts up rather than snapping, and every gain is shown
        //      as a "+N" that keeps adding while cubes keep arriving ----
        const int bal = m_em->scrap;
        if (m_lastScrap < 0 || bal < m_lastScrap) {          // first frame, or spent
            m_lastScrap = bal;
            m_displayScrap = static_cast<float>(bal);
        }
        else if (bal > m_lastScrap) {
            m_gainAccum += bal - m_lastScrap;
            m_gainTimer = 1.4f;
            m_scrapPulse = 1.f;
            m_lastScrap = bal;
        }
        m_displayScrap += (static_cast<float>(bal) - m_displayScrap) * (1.f - std::exp(-9.f * dt));
        m_scrapPulse = std::max(0.f, m_scrapPulse - dt * 4.f);
        if (m_gainTimer > 0.f) { m_gainTimer -= dt; if (m_gainTimer <= 0.f) m_gainAccum = 0; }

        const float x = 26.f;
        float y = 26.f;

        // ================================================================
        // HEALTH
        // ================================================================
        const float hpRatio = clamp01(hp.currentHp / std::max(1.f, hp.maxHp));
        const float ghostRatio = clamp01(m_ghostHp / std::max(1.f, hp.maxHp));

        sf::Color hpCol = (hpRatio < 0.25f)
            ? pulse(sf::Color(255, 60, 60), sf::Color(255, 160, 160), 8.f)
            : sf::Color(235, 55, 70);

        drawBar(x, y, 260.f, 22.f, hpRatio, hpCol,
            /*ghost*/ ghostRatio, sf::Color(150, 40, 45, 190),
            /*segment*/ 25.f / std::max(1.f, hp.maxHp),
            /*iframe*/ hp.invulTimer > 0.f);
        y += 30.f;

        // ---- Poise: thin amber strip under the hull bar, only if the class has any ----
        // White when it just absorbed a hit, red when it just broke. Reading it
        // is the whole skill of a heavy: "can I eat this one?"
        if (ps.poiseMax > 0.f) {
            const float pr = clamp01(ps.poise / ps.poiseMax);
            sf::Color pc = (ps.poiseBreakFlash > 0.f) ? pulse(sf::Color(255, 60, 40), sf::Color(255, 180, 160), 14.f)
                : (ps.poiseHitFlash > 0.f) ? sf::Color(255, 245, 220)
                : sf::Color(255, 185, 40);
            quad(x, y - 3.f, 260.f, 5.f, sf::Color(30, 24, 14, 220), 5.f);
            quad(x, y - 3.f, 260.f * pr, 5.f, pc, 5.f);
            y += 8.f;
        }

        // ================================================================
        // ENERGY DRIVE
        // ================================================================
        const float enRatio = clamp01(ps.energyDrive / std::max(1.f, ps.maxEnergyDrive));

        sf::Color enCol = (ps.overheatTimer > 0.f)
            ? pulse(sf::Color(255, 69, 0), sf::Color(255, 150, 60), 12.f)
            : sf::Color(0, 191, 255);

        // Written every frame by InputSystem, class-scaled for refit ships.
        const float dashCost = ps.dashEnergyCost;
        const float costRatio = clamp01(dashCost / std::max(1.f, ps.maxEnergyDrive));

        drawBar(x, y, 260.f, 13.f, enRatio, enCol,
            -1.f, sf::Color::Transparent,
            20.f / std::max(1.f, ps.maxEnergyDrive), false);

        if (enRatio > costRatio) {
            drawCostMarker(x, y, 260.f, 13.f, enRatio - costRatio);
        }

        // ---- Perfect dodge verdict: beside the bar the refund lands in ----
        // In the player's own dodge paint; a chain shows its count.
        if (ps.dodgeVerdictFlash > 0.f && m_font) {
            const float a = clamp01(ps.dodgeVerdictFlash / 0.9f);
            const float bar = clamp01(a * 1.6f);
            quad(x, y, 260.f * enRatio, 13.f, sf::Color(255, 255, 255, static_cast<uint8_t>(110 * bar)), 12.f);
            char vb[48];
            const char* what = ps.dodgeVerdictBash ? "PERFECT SHOULDER BASH" : "PERFECT DODGE";
            if (ps.dodgeChain > 1) std::snprintf(vb, sizeof(vb), "%s x%d", what, ps.dodgeChain);
            else                   std::snprintf(vb, sizeof(vb), "%s", what);
            const sf::Color pc = ps.livery.paint.dodge;
            ui::label(*m_window, m_font, x + 282.f, y - 3.f, vb, 15,
                ui::alpha(sf::Color(pc.r, pc.g, pc.b), a));
        }
        y += 20.f;

        // ---- Rift readiness: thin violet strip, fills as the cooldown runs ----
        // The heavy attack is gated by time now; the player needs to SEE when.
        {
            const float cdMax = std::max(0.001f, ps.riftCooldownMax);
            const float ready = (ps.riftCooldown > 0.f) ? clamp01(1.f - ps.riftCooldown / cdMax) : 1.f;
            const bool full = ready >= 1.f;
            const sf::Color violet = full
                ? pulse(sf::Color(175, 95, 255), sf::Color(215, 170, 255), 3.f)
                : sf::Color(95, 55, 140);
            quad(x, y - 4.f, 260.f, 4.f, sf::Color(20, 16, 30, 220), 4.f);
            quad(x, y - 4.f, 260.f * ready, 4.f, violet, 4.f);
        }
        y += 6.f;

        // ================================================================
        // WEAPON HEAT / VENT QTE � unified widget
        // ================================================================
        const float qteT = ps.qteActive ? 1.f : 0.f;
        m_qteOpen += (qteT - m_qteOpen) * (1.f - std::exp(-11.f * dt));

        const float bw = 260.f + 80.f * m_qteOpen;   // 260 -> 340
        const float bh = 13.f + 15.f * m_qteOpen;    // 13  -> 28

        drawHeatWidget(x, y, bw, bh, ps);
        y += bh + 13.f + 18.f * m_qteOpen;

        // ================================================================
        // SCRAP
        // ================================================================
        drawScrap(x, y);
    }

private:
    // ========================================================================
    // DRAWING HELPERS (legacy skew quads)
    // ========================================================================

    void quad(float px, float py, float w, float h, sf::Color c, float skew = 12.f) const {
        sf::ConvexShape s(4);
        s.setPoint(0, { px + skew,     py });
        s.setPoint(1, { px + skew + w, py });
        s.setPoint(2, { px + w,        py + h });
        s.setPoint(3, { px,            py + h });
        s.setFillColor(c);
        m_window->draw(s);
    }

    void drawBar(float px, float py, float w, float h,
        float ratio, sf::Color fill,
        float ghostRatio, sf::Color ghostCol,
        float segmentStep, bool flashFrame) const
    {
        quad(px - 3.f, py - 3.f, w + 6.f, h + 6.f, sf::Color(15, 18, 26, 220));
        quad(px, py, w, h, sf::Color(38, 44, 58, 235));

        if (ghostRatio > 0.f && ghostRatio > ratio) {
            quad(px, py, w * ghostRatio, h, ghostCol);
        }

        if (ratio > 0.001f) {
            quad(px, py, w * ratio, h, fill);
            sf::Color hi = fill;
            hi.r = static_cast<uint8_t>(std::min(255, hi.r + 60));
            hi.g = static_cast<uint8_t>(std::min(255, hi.g + 60));
            hi.b = static_cast<uint8_t>(std::min(255, hi.b + 60));
            hi.a = 150;
            quad(px, py, w * ratio, h * 0.34f, hi);
        }

        if (segmentStep > 0.001f) {
            for (float t = segmentStep; t < 0.999f; t += segmentStep) {
                quad(px + w * t, py, 2.f, h, sf::Color(12, 14, 20, 200), 12.f);
            }
        }

        if (flashFrame) {
            const float f = 0.5f + 0.5f * std::sin(m_time * 30.f);
            quad(px, py, w, h, sf::Color(255, 255, 255,
                static_cast<uint8_t>(60 * f)));
        }
    }

    void drawPip(float px, float py, float w, float h, float t, sf::Color c) const {
        quad(px + w * clamp01(t) - 1.f, py - 3.f, 2.5f, h + 6.f, c, 12.f);
    }

    void drawCostMarker(float px, float py, float w, float h, float t) const {
        quad(px + w * clamp01(t) - 1.f, py - 2.f, 2.f, h + 4.f,
            sf::Color(255, 255, 255, 130), 12.f);
    }

    // ========================================================================
    // HEAT GAUGE / VENT QTE (unified)
    // ========================================================================
    void drawHeatWidget(float x, float y, float w, float h,
        const PlayerComponent& ps) {
        const float heatRatio = clamp01(ps.weaponHeat /
            std::max(1.f, ps.maxWeaponHeat));

        // ---- Chassis ----
        ui::panel(*m_window, x - 3.f, y - 3.f, w + 6.f, h + 6.f, ui::PANEL_BG);
        ui::brackets(*m_window, x - 3.f, y - 3.f, w + 6.f, h + 6.f,
            ps.qteActive ? ui::CYAN_MID : ui::CYAN_LOW);

        if (!ps.qteActive) {
            // ---- Normal gauge ----
            const sf::Color hot = ps.weaponOverheated
                ? pulse(ui::HAZARD, sf::Color(255, 200, 170), 16.f)
                : ui::mix(ui::CYAN, ui::HAZARD, heatRatio);

            ui::segBar(*m_window, x, y, w, h, heatRatio, 20,
                hot, ui::alpha(ui::CYAN_LOW, 0.35f));

            // Vent-unlock threshold
            // Threshold scales with class capacity exactly as WeaponSystem's
            // does, so the ratio is capacity-independent: raw Lua / raw Lua.
            const float unlockAt = wcfg("heat_unlock_threshold", 30.f)
                / std::max(1.f, wcfg("max_weapon_heat", 100.f));
            ui::vline(*m_window, x + unlockAt * w, y - 2.f, h + 4.f, ui::CYAN_MID);

            // ---- Overdrive readout ----
            if (ps.overdriveTimer > 0.f) {
                const float od = clamp01(ps.overdriveTimer /
                    std::max(0.01f, wcfgLua("overdrive_duration", 4.f)));
                ui::fill(*m_window, x, y + h + 3.f, w * od, 2.f,
                    pulse(ui::AMBER_HOT, ui::AMBER, 13.f));
                ui::label(*m_window, m_font, x, y + h + 7.f,
                    "COOLANT LOCK", 12, ui::alpha(ui::AMBER_HOT, 0.92f));
            }

            // ---- Verdict display (right after QTE ends) ----
            if (ps.qteResultFlash > 0.f && m_font) {
                const float a = clamp01(ps.qteResultFlash / 0.55f);
                const char* txt = (ps.qteResult == 1) ? "PERFECT VENT"
                    : (ps.qteResult == 2) ? "VENT OK"
                    : "VENT MISSED";
                const sf::Color col = (ps.qteResult == 1) ? ui::AMBER_HOT
                    : (ps.qteResult == 2) ? ui::BLUE_COOL
                    : ui::TEXT_DIM;
                ui::label(*m_window, m_font, x, y - 19.f, txt, 16,
                    ui::alpha(col, a * 255.f));
            }
            return;
        }

        // ================================================================
        // QTE MODE
        // ================================================================

        // Track
        ui::fill(*m_window, x, y, w, h, ui::alpha(ui::CYAN_LOW, 0.30f));

        // Frozen heat (still visible at full)
        ui::fill(*m_window, x, y, w, h, ui::alpha(ui::HAZARD, 0.18f));

        // ---- Zones (nested: amber inside blue) ----
        auto zone = [&](float half, sf::Color c, float inset) {
            const float zx = x + (ps.qteGoodCenter - half) * w;
            ui::fill(*m_window, zx, y + inset, half * 2.f * w, h - inset * 2.f, c);
            };
        zone(ps.qteGoodHalf, ui::alpha(ui::BLUE_COOL, 0.55f), 1.f);
        zone(ps.qtePerfectHalf, ui::alpha(ui::AMBER_HOT, 0.95f), 1.f);

        // Tick marks above/below the amber zone
        const float ax = x + ps.qteGoodCenter * w;
        ui::vline(*m_window, ax, y - 5.f, 4.f, ui::AMBER_HOT);
        ui::vline(*m_window, ax, y + h + 1.f, 4.f, ui::AMBER_HOT);

        // ---- Marker ----
        const float mx = x + ps.qtePos * w;
        ui::fill(*m_window, mx - 1.f, y - 4.f, 2.f, h + 8.f, sf::Color(245, 250, 255));
        ui::fill(*m_window, mx - ps.qteDir * 13.f, y + h * 0.35f, 13.f, h * 0.3f,
            ui::alpha(sf::Color(200, 235, 255), 0.28f));

        // ---- Timeout (converging from both ends) ----
        const float tR = clamp01(ps.qteTimeout /
            std::max(0.01f, wcfgLua("qte_timeout", 2.4f)));
        const float gone = (1.f - tR) * w * 0.5f;
        ui::fill(*m_window, x, y + h + 3.f, gone, 2.f, ui::alpha(ui::HAZARD, 0.85f));
        ui::fill(*m_window, x + w - gone, y + h + 3.f, gone, 2.f,
            ui::alpha(ui::HAZARD, 0.85f));

        // ---- Prompt ----
        ui::label(*m_window, m_font, x, y - 19.f, "COOLANT VENT", 13,
            pulse(ui::CYAN, ui::CYAN_MID, 7.f));
        ui::label(*m_window, m_font, x + w - 34.f, y - 19.f, "[E]", 13, ui::AMBER);

        if (ps.qteStreak > 0) {
            ui::label(*m_window, m_font, x + w - 96.f, y - 19.f,
                "SYNC X" + std::to_string(ps.qteStreak), 12,
                ui::alpha(ui::AMBER_HOT, 0.9f));
        }

        // ---- Scanlines for QTE state ----
        ui::scanlines(*m_window, x, y, w, h, 26, 3.f);
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    static float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

    sf::Color pulse(sf::Color a, sf::Color b, float speed) const {
        const float f = 0.5f + 0.5f * std::sin(m_time * speed);
        return sf::Color(
            static_cast<uint8_t>(a.r + (b.r - a.r) * f),
            static_cast<uint8_t>(a.g + (b.g - a.g) * f),
            static_cast<uint8_t>(a.b + (b.b - a.b) * f));
    }

    static sf::Color heatRamp(float t) {
        t = std::clamp(t, 0.f, 1.f);
        float r, g, b;
        if (t < 0.5f) {
            const float u = t / 0.5f;
            r = 255.f * u;
            g = 220.f + (150.f - 220.f) * u;
            b = 200.f + (40.f - 200.f) * u;
        }
        else {
            const float u = (t - 0.5f) / 0.5f;
            r = 255.f;
            g = 150.f + (255.f - 150.f) * u;
            b = 40.f + (235.f - 40.f) * u;
        }
        return sf::Color(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
            static_cast<uint8_t>(b));
    }

    /// Lua `weapon` table, cached per config epoch (see LuaConfig.hpp).
    luacfg::Table m_cfgWeapon{ "weapon" };
    float wcfg(const char* key, float def) const {
        return m_cfgWeapon.get(m_lua, key, def);
    }

    float wcfgLua(const char* key, float fallback) const {
        if (!m_lua) return fallback;
        return (*m_lua)[key].get_or(fallback);
    }

    // ========================================================================
    // MEMBERS
    // ========================================================================
    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    const sf::Font* m_font = nullptr;
    uint32_t m_playerEntityId = 0;

    float m_time = 0.f;
    float m_ghostHp = -1.f;
    float m_displayScrap = 0.f;   ///< Counts up toward the real balance
    int   m_lastScrap = -1;       ///< Balance last frame; -1 = not seen yet
    int   m_gainAccum = 0;        ///< "+N" being shown
    float m_gainTimer = 0.f;      ///< How long the "+N" stays up
    float m_scrapPulse = 0.f;     ///< 1 on a pickup, decays: the icon kicks

    /**
     * @brief Scrap balance: the same amber cube the field drops, then the
     *        number, then a "+N" while pickups are landing.
     *
     * The icon IS the pickup, drawn the same way, so the player connects the
     * cubes they are flying through with the number going up without being
     * told. It kicks on every pickup; the "+N" sums a whole burst into one
     * figure instead of flickering through +3 +2 +4.
     */
    void drawScrap(float x, float y) {
        const float k = m_scrapPulse;
        const float cx = x + 14.f, cy = y + 17.f;
        const float half = 8.f + 3.f * k;
        const float rot = (18.f + 25.f * k) * 3.14159f / 180.f;
        const float cs = std::cos(rot), sn = std::sin(rot);
        const auto square = [&](float h, sf::Color c) {
            sf::ConvexShape q(4);
            q.setPoint(0, { cx + (-cs + sn) * h, cy + (-sn - cs) * h });
            q.setPoint(1, { cx + (cs + sn) * h,  cy + (sn - cs) * h });
            q.setPoint(2, { cx + (cs - sn) * h,  cy + (sn + cs) * h });
            q.setPoint(3, { cx + (-cs - sn) * h, cy + (-sn + cs) * h });
            q.setFillColor(c);
            m_window->draw(q);
            };
        square(half + 1.6f, sf::Color(255, 244, 214));
        square(half, sf::Color(255, static_cast<uint8_t>(170 + 60 * k), static_cast<uint8_t>(110 * k)));

        if (!m_font) return;
        sf::Text num(*m_font);
        num.setCharacterSize(28);
        num.setString(std::to_string(static_cast<int>(m_displayScrap + 0.5f)));
        num.setPosition({ x + 34.f, y });
        sf::Text shadow = num;
        shadow.setFillColor(sf::Color(0, 0, 0, 170));
        shadow.setPosition({ x + 36.f, y + 2.f });
        m_window->draw(shadow);
        num.setFillColor(sf::Color(255, 225, 120));
        m_window->draw(num);

        if (m_gainAccum > 0 && m_gainTimer > 0.f) {
            const float a = std::clamp(m_gainTimer / 0.5f, 0.f, 1.f);
            sf::Text plus(*m_font);
            plus.setCharacterSize(18);
            plus.setString("+" + std::to_string(m_gainAccum));
            plus.setPosition({ x + 42.f + num.getLocalBounds().size.x, y + 7.f });
            plus.setFillColor(sf::Color(255, 190, 70, static_cast<uint8_t>(255 * a)));
            m_window->draw(plus);
        }
    }
    float m_qteOpen = 0.f;          // 0..1 smooth expansion of QTE widget
};