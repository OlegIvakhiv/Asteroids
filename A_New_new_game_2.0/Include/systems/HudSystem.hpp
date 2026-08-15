/**
 * @file HudSystem.hpp
 * @brief Stylized screen-space HUD (health, energy, heat, score)
 *
 * Pulled out of game.cpp, which was building SFML shapes inline every frame.
 *
 * DESIGN NOTES — why these bars look the way they do:
 *
 *  1. SKEWED PARALLELOGRAMS, not rectangles. A 12px shear costs nothing and
 *     instantly stops the HUD reading as programmer-art. Everything is drawn
 *     as sf::ConvexShape quads so the skew is free.
 *
 *  2. SEGMENTED FILLS. Ticks cut into the bar every N units. Continuous bars
 *     are hard to read at a glance — segments let you count remaining chunks
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
 * Drawn in screen space — SystemManager must set the default view first.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include <SFML/Graphics.hpp>
#include <cmath>
#include <algorithm>

class HudSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_time = 0.f;
        m_ghostHp = -1.f;
        m_displayScore = 0.f;
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

        // ---- Score counts up rather than snapping ----
        m_displayScore += (static_cast<float>(m_em->totalScore) - m_displayScore)
            * (1.f - std::exp(-9.f * dt));

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

        // ================================================================
        // ENERGY DRIVE
        // ================================================================
        const float enRatio = clamp01(ps.energyDrive / std::max(1.f, ps.maxEnergyDrive));

        sf::Color enCol = (ps.overheatTimer > 0.f)
            ? pulse(sf::Color(255, 69, 0), sf::Color(255, 150, 60), 12.f)
            : sf::Color(0, 191, 255);

        // Dim the segment you're about to spend on a dash — shows affordability
        // before you commit, which is the whole point of a resource bar.
        const float dashCost = (*m_lua)["dash_energy_cost"].get_or(30.f);
        const float costRatio = clamp01(dashCost / std::max(1.f, ps.maxEnergyDrive));

        drawBar(x, y, 260.f, 13.f, enRatio, enCol,
            -1.f, sf::Color::Transparent,
            20.f / std::max(1.f, ps.maxEnergyDrive), false);

        if (enRatio > costRatio) {
            drawCostMarker(x, y, 260.f, 13.f, enRatio - costRatio);
        }
        y += 20.f;

        // ================================================================
        // WEAPON HEAT
        // ================================================================
        const float heatRatio = clamp01(ps.weaponHeat / std::max(1.f, ps.maxWeaponHeat));

        sf::Color heatCol = ps.weaponOverheated
            ? pulse(sf::Color(255, 70, 30), sf::Color(255, 220, 180), 16.f)
            : heatRamp(heatRatio);

        drawBar(x, y, 260.f, 13.f, heatRatio, heatCol,
            -1.f, sf::Color::Transparent,
            0.1f, false);

        // Threshold pips: the vent-unlock point and the overheat point.
        const float unlockAt = wcfg("heat_unlock_threshold", 30.f)
            / std::max(1.f, ps.maxWeaponHeat);
        drawPip(x, y, 260.f, 13.f, unlockAt, sf::Color(120, 220, 255, 210));
        drawPip(x, y, 260.f, 13.f, 0.999f, sf::Color(255, 90, 40, 230));
        y += 26.f;

        // ================================================================
        // SCORE
        // ================================================================
        if (m_font) {
            sf::Text score(*m_font);
            score.setCharacterSize(28);
            score.setString(std::to_string(static_cast<int>(m_displayScore + 0.5f)));
            score.setPosition({ x + 3.f, y });

            // Cheap faux-shadow: same glyphs offset and darkened.
            sf::Text shadow = score;
            shadow.setFillColor(sf::Color(0, 0, 0, 170));
            shadow.setPosition({ x + 5.f, y + 2.f });
            m_window->draw(shadow);

            score.setFillColor(sf::Color(255, 225, 120));
            m_window->draw(score);
        }
    }

private:
    // ========================================================================
    // DRAWING
    // ========================================================================

    /// Skewed quad. `skew` shifts the top edge right, giving the whole HUD
    /// a consistent forward lean.
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
        // ---- Backing plate + inner well ----
        quad(px - 3.f, py - 3.f, w + 6.f, h + 6.f, sf::Color(15, 18, 26, 220));
        quad(px, py, w, h, sf::Color(38, 44, 58, 235));

        // ---- Damage ghost (behind the real fill) ----
        if (ghostRatio > 0.f && ghostRatio > ratio) {
            quad(px, py, w * ghostRatio, h, ghostCol);
        }

        // ---- Fill ----
        if (ratio > 0.001f) {
            quad(px, py, w * ratio, h, fill);

            // Top highlight: a lighter sliver sells depth without a gradient.
            sf::Color hi = fill;
            hi.r = static_cast<uint8_t>(std::min(255, hi.r + 60));
            hi.g = static_cast<uint8_t>(std::min(255, hi.g + 60));
            hi.b = static_cast<uint8_t>(std::min(255, hi.b + 60));
            hi.a = 150;
            quad(px, py, w * ratio, h * 0.34f, hi);
        }

        // ---- Segment ticks ----
        if (segmentStep > 0.001f) {
            for (float t = segmentStep; t < 0.999f; t += segmentStep) {
                quad(px + w * t, py, 2.f, h, sf::Color(12, 14, 20, 200), 12.f);
            }
        }

        // ---- I-frame flash ----
        if (flashFrame) {
            const float f = 0.5f + 0.5f * std::sin(m_time * 30.f);
            quad(px, py, w, h, sf::Color(255, 255, 255,
                static_cast<uint8_t>(60 * f)));
        }
    }

    /// Vertical marker line at a normalised position along the bar.
    void drawPip(float px, float py, float w, float h, float t, sf::Color c) const {
        quad(px + w * clamp01(t) - 1.f, py - 3.f, 2.5f, h + 6.f, c, 12.f);
    }

    /// Bracket showing where energy would sit after paying for a dash.
    void drawCostMarker(float px, float py, float w, float h, float t) const {
        quad(px + w * clamp01(t) - 1.f, py - 2.f, 2.f, h + 4.f,
            sf::Color(255, 255, 255, 130), 12.f);
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

    /// Matches WeaponSystem::heatColor and RenderSystem::heatRamp.
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

    float wcfg(const char* key, float def) const {
        if (!m_lua) return def;
        sol::optional<sol::table> v = (*m_lua)["weapon"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    EntityManager* m_em = nullptr;
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    const sf::Font* m_font = nullptr;
    uint32_t m_playerEntityId = 0;

    float m_time = 0.f;
    float m_ghostHp = -1.f;      ///< Lagging health value for the damage ghost
    float m_displayScore = 0.f;  ///< Eased score for the count-up
};