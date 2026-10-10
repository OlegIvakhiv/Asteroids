/**
 * @file DoctrineReels.hpp
 * @brief Field Doctrine as live reels: every mechanic shown, not described.
 *
 * The old doctrine was three pages of text: "Press Space to dash. Great for
 * evasion!" A player reads that, nods, and still has no idea what a perfect
 * dodge looks like or why a parried rock is worth more than a parried bullet.
 * Each lesson here is a short looping demonstration, rendered live in the
 * terminal, with the inputs lighting up on the keycaps the moment the reel
 * "presses" them.
 *
 * WHY SCRIPTED, NOT RECORDED
 * --------------------------
 * A recorded clip goes stale the first time a hull, a colour or a timing
 * changes. These reels are drawn every frame from the live data instead:
 *   - the ship is YOUR ship -- the refit bay's model, in your livery, with
 *     your dodge / parry / homing / plasma / thrust paints on the effects;
 *   - the enemies are the EnemyRegistry hulls, coloured as in flight;
 *   - the timings mirror the tunables (rift charge ~0.6s, Berserker wind-up,
 *     the vent's amber and blue zones).
 * They are choreography, not simulation: every beat is a function of the
 * reel clock, so a loop is identical every time and can never fail to show
 * the thing it is teaching.
 *
 * BASIC     FLIGHT, GUNNERY, TURBO, SALVAGE
 * ADVANCED  DODGE, PARRY, RIFT BOLT, VENT
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "utils/TermDraw.hpp"
#include "utils/UiPalette.hpp"
#include "utils/ShipLivery.hpp"
#include "core/EnemyArchetypes.hpp"

namespace reels {

    using tdraw::PI;
    using tdraw::TAU;

    struct Lesson {
        const char* name;
        bool        advanced;
        float       length;                 ///< loop length, seconds
        std::vector<const char*> keys;      ///< keycaps under the reel, in order
        const char* line1;                  ///< what to press
        const char* line2;                  ///< why it matters
    };

    inline const std::vector<Lesson>& lessons() {
        static const std::vector<Lesson> k = {
            { "FLIGHT",    false, 4.8f, { "W", "A", "S", "D", "MOUSE" },
              "W A S D MOVE YOU IN ANY DIRECTION. THE NOSE ALWAYS FOLLOWS THE MOUSE.",
              "LET GO AND YOU KEEP DRIFTING. SPACE HAS NO BRAKES." },
            { "GUNNERY",   false, 5.0f, { "LMB" },
              "LEFT MOUSE FIRES. EVERY SHOT ADDS HEAT.",
              "AT FULL HEAT THE GUN LOCKS UNTIL IT COOLS. OR VENT IT: SEE ADVANCED." },
            { "TURBO",     false, 4.4f, { "W", "SHIFT" },
              "HOLD LEFT SHIFT FOR TURBO. IT BURNS ENERGY.",
              "ENERGY IS SHARED WITH THE DODGE. A DRY TANK MEANS NO WAY OUT." },
            { "SALVAGE",   false, 5.6f, { "LMB" },
              "ROCKS BREAK LARGE TO MEDIUM TO SMALL. SALVAGE AND KILLS DROP SCRAP.",
              "FLY CLOSE AND SCRAP PULLS ITSELF IN. SCRAP PAYS FOR THE REFIT BAY." },
            { "DODGE",     true,  3.4f, { "SPACE" },
              "SPACE DASHES. THE DASH CANNOT BE HIT AND PASSES THROUGH SHIPS.",
              "DODGE LATE, INTO THE HIT: A PERFECT DODGE REFUNDS ENERGY AND RE-ARMS." },
            { "PARRY",     true,  3.2f, { "R" },
              "R PARRIES. A BULLET GOES BACK AT WHOEVER FIRED IT.",
              "BULLET AND SHIP PARRIES RE-ARM AT ONCE. A PARRIED ROCK BECOMES A HOMING ROUND." },
            { "RIFT BOLT", true,  7.2f, { "RMB" },
              "HOLD RIGHT MOUSE TO CHARGE THE RIFT BOLT. PRESS AGAIN TO DETONATE IT.",
              "OPEN SPACE: BURST.   BESIDE A ROCK: HIJACK.   BESIDE A SHIP: OVERLOAD." },
            { "VENT",      true,  3.8f, { "E" },
              "AT FULL HEAT, E OPENS THE VENT. STOP THE MARKER INSIDE A ZONE.",
              "BLUE CLEARS THE HEAT. AMBER CLEARS IT AND GRANTS OVERDRIVE: NO HEAT AT ALL." },
        };
        return k;
    }

    /// Draws one lesson at reel time `u`. Owns nothing but scratch buffers.
    class Projector {
    public:
        static constexpr float SCALE = 1.35f;

        void setShip(const tdraw::HullMesh* mesh, const ship::Livery* lv) { m_mesh = mesh; m_lv = lv; }
        void setRegistry(const enemyarch::EnemyRegistry* r) { m_reg = r; }

        /**
         * @param lit  out: one flag per Lesson::keys entry, true while the
         *             reel is "pressing" that input.
         */
        void draw(sf::RenderTarget& t, sf::FloatRect r, int lesson, float u,
            const sf::Font* font, std::vector<bool>& lit) {
            const auto& L = lessons();
            lesson = std::clamp(lesson, 0, static_cast<int>(L.size()) - 1);
            lit.assign(L[lesson].keys.size(), false);
            m_c = r.position + r.size * 0.5f;
            m_font = font;
            m_t = &t;
            m_labels.clear();

            m_b.clear();
            m_b.rect(r.position.x, r.position.y, r.size.x, r.size.y, sf::Color(3, 5, 8));
            // Floor grid: what makes motion readable with no stars. The turbo
            // reel scrolls it -- the ship holds still and the floor runs.
            const float gx = r.size.x / 12.f, gy = r.size.y / 8.f;
            const float shift = (lesson == 2) ? -std::fmod(turboDistance(u) * SCALE, gx) : 0.f;
            for (int i = 0; i <= 12; ++i) m_b.rect(r.position.x + i * gx + shift, r.position.y, 1.f, r.size.y, tdraw::alpha(ui::CYAN_LOW, 0.35f));
            for (int i = 1; i < 8; ++i) m_b.rect(r.position.x, r.position.y + i * gy, r.size.x, 1.f, tdraw::alpha(ui::CYAN_LOW, 0.35f));
            m_b.draw(t);

            m_b.clear();
            switch (lesson) {
            case 0: flight(u, lit); break;
            case 1: gunnery(u, lit); break;
            case 2: turbo(u, lit); break;
            case 3: salvage(u, lit); break;
            case 4: dodge(u, lit); break;
            case 5: parry(u, lit); break;
            case 6: rift(u, lit); break;
            default: vent(u, lit); break;
            }
            sf::RenderStates st;
            st.transform.translate(m_c).scale({ SCALE, SCALE });
            m_b.drawWith(t, st);

            if (font) for (const auto& l : m_labels) text(l.s, l.p, l.size, l.c, l.centre, l.sp);
        }

    private:
        // ---- paints ----
        sf::Color hullFill() const { return m_lv ? m_lv->paint.hull : sf::Color(40, 100, 255); }
        sf::Color hullEdge() const { return m_lv ? m_lv->paint.outline : sf::Color::White; }
        sf::Color paint(int which) const {
            if (!m_lv) return ui::CYAN;
            switch (which) {
            case 0: return m_lv->paint.plasma;
            case 1: return m_lv->paint.thrust;
            case 2: return m_lv->paint.turbo;
            case 3: return m_lv->paint.dodge;
            case 4: return m_lv->paint.parry;
            default: return m_lv->paint.homing;
            }
        }
        enum { PLASMA, THRUST, TURBO, DODGEP, PARRYP, HOMING };

        float shipR() const { return m_mesh ? m_mesh->radius : 30.f; }

        // ---- primitives in reel-world space ----
        void ship(sf::Vector2f p, float a, float alpha01 = 1.f, bool overdrive = false) {
            if (!m_mesh || !m_mesh->valid()) {
                const sf::Vector2f f{ std::cos(a), std::sin(a) }, s{ -f.y, f.x };
                m_b.fan({ p + f * 30.f, p - f * 20.f + s * 22.f, p - f * 20.f - s * 22.f }, tdraw::alpha(hullFill(), alpha01));
                return;
            }
            sf::Color edge = hullEdge();
            if (overdrive) edge = tdraw::mix(edge, ui::AMBER, 0.6f + 0.4f * std::sin(m_clock * 18.f));
            if (alpha01 >= 0.99f) tdraw::drawHull(m_b, *m_mesh, tdraw::hullXf(p, a, 1.f), hullFill(), edge, m_lv, &m_vscratch);
            else tdraw::drawHull(m_b, *m_mesh, tdraw::hullXf(p, a, 1.f), tdraw::alpha(hullFill(), alpha01), tdraw::alpha(edge, alpha01));
        }
        void ghost(sf::Vector2f p, float a, float k) {
            const sf::Color c = paint(DODGEP);
            if (!m_mesh || !m_mesh->valid()) return;
            tdraw::drawHull(m_b, *m_mesh, tdraw::hullXf(p, a, 1.f), tdraw::alpha(c, 0.16f * k), tdraw::alpha(c, 0.75f * k));
        }
        void flame(sf::Vector2f p, float a, float len, sf::Color c, float w = 8.f) {
            const sf::Vector2f back{ -std::cos(a), -std::sin(a) };
            const sf::Vector2f stern = p + back * (shipR() * 0.85f);
            m_b.line(stern, stern + back * len, w, c);
        }
        void enemy(const char* key, sf::Vector2f p, float a, float flash = 0.f, bool charge = false) {
            const enemyarch::ArchetypeDef* d = m_reg ? m_reg->byKey(key) : nullptr;
            if (!d) {
                const sf::Vector2f f{ std::cos(a), std::sin(a) }, s{ -f.y, f.x };
                m_b.fan({ p + f * 34.f, p - f * 24.f + s * 26.f, p - f * 24.f - s * 26.f },
                    tdraw::mix(sf::Color(200, 70, 55), sf::Color::White, flash));
                return;
            }
            sf::Color fill = charge ? sf::Color(255, 245, 215) : tdraw::enemyFill(*d);
            fill = tdraw::mix(fill, sf::Color::White, std::clamp(flash, 0.f, 1.f));
            tdraw::drawArchetype(m_b, *d, tdraw::hullXf(p, a, 1.f), fill, tdraw::enemyEdge(*d), 2.4f, &m_pscratch);
        }
        void enemyGhost(const char* key, sf::Vector2f p, float a, float k) {
            const enemyarch::ArchetypeDef* d = m_reg ? m_reg->byKey(key) : nullptr;
            if (!d) return;
            tdraw::drawArchetype(m_b, *d, tdraw::hullXf(p, a, 1.f), sf::Color(0, 0, 0, 0), tdraw::alpha(ui::RED, k), 2.f, &m_pscratch);
        }
        void rock(sf::Vector2f p, float r, float rot, float flash = 0.f, bool volcanic = false) {
            static const float kShape[9] = { 1.f, 0.82f, 1.08f, 0.9f, 1.f, 0.78f, 1.05f, 0.92f, 0.98f };
            const auto pts = tdraw::rockPoints(p, r, rot, kShape, 9);
            m_b.fan(pts, tdraw::mix(sf::Color(16, 19, 25), ui::TEXT, flash));
            m_b.loop(pts, r > 60.f ? 3.f : 2.f, volcanic ? ui::AMBER_HOT : ui::TEXT_DIM);
        }
        void bolt(sf::Vector2f p, sf::Vector2f dir, float len, float w, sf::Color c) { m_b.line(p, p - dir * len, w, c); }
        void spark(sf::Vector2f p, float sz, sf::Color c) { m_b.rect(p.x - sz * 0.5f, p.y - sz * 0.5f, sz, sz, c); }
        void burst(sf::Vector2f p, float k, sf::Color c, int n, float R, int seed) {
            for (int i = 0; i < n; ++i) {
                const float a = hash(seed * 31 + i) * TAU, s = 0.35f + 0.65f * hash(seed * 17 + i * 7);
                spark(p + sf::Vector2f(std::cos(a), std::sin(a)) * (R * s * k), 5.f, tdraw::alpha(c, 1.f - k));
            }
        }

        void say(const std::string& s, sf::Vector2f world, unsigned size, sf::Color c, float sp = 2.f) {
            m_labels.push_back({ s, m_c + world * SCALE, size, c, true, sp });
        }

        // ---- maths ----
        static float sstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
        static float win(float u, float a, float b) { return (u >= a && u < b) ? 1.f : 0.f; }
        static float hash(int i) { std::uint32_t x = static_cast<std::uint32_t>(i) * 2654435761u; x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15; return static_cast<float>(x % 10000u) / 10000.f; }
        static sf::Vector2f lerp(sf::Vector2f a, sf::Vector2f b, float t) { return a + (b - a) * t; }
        static float ang(sf::Vector2f d) { return std::atan2(d.y, d.x); }

        // ---- mini HUD gauges (reel space) ----
        void gauge(sf::Vector2f p, const char* name, float v, sf::Color on, bool locked = false) {
            const float w = 180.f, h = 10.f;
            const int cells = 18;
            const float cw = (w - (cells - 1) * 2.f) / cells;
            for (int i = 0; i < cells; ++i) {
                const bool l = (i + 0.5f) / cells <= v;
                m_b.rect(p.x + i * (cw + 2.f), p.y, cw, h, l ? on : tdraw::alpha(ui::CYAN_LOW, 0.7f));
            }
            say(name, p + sf::Vector2f(-46.f, -6.f), 12, locked ? ui::RED : ui::TEXT_DIM, 1.6f);
        }

        // ====================================================================
        // BASIC
        // ====================================================================

        void flight(float u, std::vector<bool>& lit) {
            m_clock = u;
            const sf::Vector2f C[5] = { { -140.f, 100.f }, { -140.f, -100.f }, { 140.f, -100.f }, { 140.f, 100.f }, { -140.f, 100.f } };
            auto posAt = [&](float t) {
                t = std::fmod(t + 4.8f, 4.8f);
                const int ph = std::min(3, static_cast<int>(t / 1.2f));
                const float k = sstep(0.f, 1.f, (t - ph * 1.2f) / 1.2f);
                return lerp(C[ph], C[ph + 1], k);
            };
            const int ph = std::min(3, static_cast<int>(u / 1.2f));
            static const int kKey[4] = { 0, 3, 2, 1 };   // W, D, S, A
            lit[kKey[ph]] = true;
            lit[4] = true;
            const float ra = u * 1.3f + 0.6f;
            const sf::Vector2f mouse{ std::cos(ra) * 400.f, std::sin(ra) * 140.f };
            for (int i = 1; i < 26; ++i) {
                const sf::Vector2f q = posAt(u - i * 0.05f);
                spark(q, 3.f, tdraw::alpha(ui::CYAN, 0.55f - i / 52.f));
            }
            const sf::Vector2f p = posAt(u);
            const sf::Vector2f mv = posAt(u + 0.02f) - posAt(u - 0.02f);
            const float a = ang(mouse - p);
            // RCS puff opposite the push: strafing has no main flame.
            if (std::hypot(mv.x, mv.y) > 0.5f) {
                const sf::Vector2f d = mv / std::hypot(mv.x, mv.y);
                m_b.line(p - d * (shipR() * 0.9f), p - d * (shipR() * 0.9f + 26.f), 6.f, tdraw::alpha(paint(THRUST), 0.9f));
            }
            ship(p, a);
            m_b.dashed(p, mouse, 1.5f, tdraw::alpha(ui::TEXT_DIM, 0.6f), 6.f, 8.f, u * 40.f);
            m_b.line(mouse - sf::Vector2f(14.f, 0.f), mouse + sf::Vector2f(14.f, 0.f), 2.f, ui::AMBER);
            m_b.line(mouse - sf::Vector2f(0.f, 14.f), mouse + sf::Vector2f(0.f, 14.f), 2.f, ui::AMBER);
            m_b.ring(mouse, 9.f, 2.f, ui::AMBER, 20);
            say("MOUSE", mouse + sf::Vector2f(0.f, 22.f), 12, ui::AMBER);
            static const char* kName[4] = { "W", "D", "S", "A" };
            say(kName[ph], p + sf::Vector2f(0.f, -shipR() - 40.f), 18, ui::TEXT);
        }

        void gunnery(float u, std::vector<bool>& lit) {
            m_clock = u;
            const sf::Vector2f sp{ -330.f, 0.f };
            const float a = std::sin(u * 1.4f) * 0.05f;
            const sf::Vector2f fw{ std::cos(a), std::sin(a) };
            const sf::Vector2f rp{ 270.f, 0.f };
            const float fireEnd = 3.0f;
            lit[0] = u >= 0.3f && u < fireEnd;
            float flash = 0.f;
            const sf::Vector2f muzzle = sp + fw * (shipR() + 4.f);
            for (int k = 0;; ++k) {
                const float tk = 0.3f + k * 0.15f;
                if (tk >= fireEnd || tk > u) break;
                const float dist = 1800.f * (u - tk);
                const float reach = (rp.x - 84.f) - muzzle.x;
                if (dist < reach) bolt(muzzle + fw * dist, fw, 22.f, 4.f, paint(PLASMA));
                else if (dist < reach + 300.f) {
                    const float kk = (dist - reach) / 300.f;
                    burst(muzzle + fw * reach, kk, paint(PLASMA), 5, 40.f, k);
                    flash = std::max(flash, 1.f - kk * 3.f);
                }
            }
            rock(rp, 92.f, u * 0.3f, std::clamp(flash, 0.f, 1.f));
            ship(sp, a);
            const float heat = (u < fireEnd) ? std::clamp((u - 0.3f) / (fireEnd - 0.3f), 0.f, 1.f)
                : std::clamp(1.f - (u - fireEnd - 0.5f) / 1.5f, 0.f, 1.f);
            const bool locked = u >= fireEnd && u < fireEnd + 1.6f;
            gauge(sp + sf::Vector2f(-60.f, 80.f), "HEAT", heat, heat > 0.85f ? ui::RED : ui::AMBER_HOT, locked);
            if (locked && static_cast<int>(u * 8.f) % 2 == 0) say("GUN LOCKED", sp + sf::Vector2f(30.f, -80.f), 18, ui::RED, 1.6f);
        }

        static float turboDistance(float u) { return 260.f * u + 390.f * std::clamp(u - 1.2f, 0.f, 1.6f); }

        void turbo(float u, std::vector<bool>& lit) {
            m_clock = u;
            lit[0] = true;
            const bool on = u >= 1.2f && u < 2.8f;
            lit[1] = on;
            const float x = turboDistance(u);
            const sf::Vector2f p{ -120.f, 0.f };
            const float spd = on ? 650.f : 260.f;
            if (on) for (int i = 0; i < 9; ++i) {
                const float y = -200.f + 50.f * i, off = std::fmod(x * 2.f + i * 137.f, 1300.f);
                m_b.line({ 640.f - off, y }, { 640.f - off + 120.f, y }, 2.f, tdraw::alpha(ui::TEXT, 0.18f));
            }
            flame(p, 0.f, on ? 90.f + std::sin(u * 50.f) * 10.f : 24.f, on ? paint(TURBO) : paint(THRUST), on ? 12.f : 8.f);
            ship(p, 0.f);
            const float energy = on ? 1.f - 0.65f * (u - 1.2f) / 1.6f
                : (u < 1.2f ? 1.f : std::min(1.f, 0.35f + 0.5f * (u - 2.8f)));
            gauge(p + sf::Vector2f(-60.f, 80.f), "ENERGY", energy, ui::BLUE_COOL);
            char buf[32]; std::snprintf(buf, sizeof buf, "%d M/S", static_cast<int>(spd / 30.f));
            say(buf, p + sf::Vector2f(0.f, -80.f), 16, on ? paint(TURBO) : ui::TEXT_DIM);
            if (on) say("TURBO", p + sf::Vector2f(0.f, -110.f), 18, ui::AMBER, 1.7f);
        }

        void salvage(float u, std::vector<bool>& lit) {
            m_clock = u;
            const float approach = sstep(2.6f, 3.8f, u);
            const sf::Vector2f sp = lerp({ -380.f, 40.f }, { -120.f, 30.f }, approach);
            const sf::Vector2f L{ 180.f, 0.f };
            const float tBreakL = 1.25f, tBreakM = 2.35f;
            sf::Vector2f target = L;
            if (u >= tBreakL) target = L + sf::Vector2f(40.f, -90.f) * (std::min(u, tBreakM) - tBreakL);
            const sf::Vector2f dv = target - sp;
            const float a = ang(dv);
            const sf::Vector2f fw{ std::cos(a), std::sin(a) };
            const bool firing = (u >= 0.2f && u < 1.2f) || (u >= 1.6f && u < 2.3f);
            lit[0] = firing;
            const sf::Vector2f muzzle = sp + fw * (shipR() + 4.f);
            auto shots = [&](float t0, float t1, sf::Vector2f tgt, float rad, int seed) {
                for (int k = 0;; ++k) {
                    const float tk = t0 + k * 0.15f;
                    if (tk >= t1 || tk > u) break;
                    const sf::Vector2f d = tgt - muzzle;
                    const float L2 = std::hypot(d.x, d.y) - rad, dist = 1800.f * (u - tk);
                    const sf::Vector2f dir = d / std::hypot(d.x, d.y);
                    if (dist < L2) bolt(muzzle + dir * dist, dir, 22.f, 4.f, paint(PLASMA));
                    else if (dist < L2 + 300.f) burst(muzzle + dir * L2, (dist - L2) / 300.f, paint(PLASMA), 4, 30.f, seed + k);
                }
            };
            if (u < tBreakL) { rock(L, 92.f, u * 0.25f); shots(0.2f, 1.2f, L, 84.f, 10); }
            else {
                const float s = u - tBreakL;
                if (s < 0.5f) burst(L, s / 0.5f, ui::TEXT_DIM, 22, 200.f, 3);
                const sf::Vector2f m1 = L + sf::Vector2f(40.f, -90.f) * std::min(s, tBreakM - tBreakL);
                const sf::Vector2f m2 = L + sf::Vector2f(50.f, 85.f) * s;
                if (u < tBreakM) { rock(m1, 52.f, s * 0.8f); shots(1.6f, 2.3f, m1, 46.f, 40); }
                else {
                    const float s2 = u - tBreakM;
                    if (s2 < 0.4f) burst(m1, s2 / 0.4f, ui::TEXT_DIM, 14, 140.f, 9);
                    rock(m1 + sf::Vector2f(-40.f, -60.f) * s2, 26.f, s2 * 1.5f);
                    rock(m1 + sf::Vector2f(60.f, -30.f) * s2, 26.f, -s2 * 1.2f);
                }
                rock(m2, 52.f, -s * 0.6f);
            }
            // Scrap: thrown at each break, pulled in once the hunter is close.
            auto scrap = [&](sf::Vector2f origin, float t0, int n, int seed) {
                if (u < t0) return;
                for (int i = 0; i < n; ++i) {
                    const float a2 = hash(seed + i) * TAU;
                    const sf::Vector2f rest = origin + sf::Vector2f(std::cos(a2), std::sin(a2)) * (40.f + 30.f * hash(seed * 3 + i));
                    const sf::Vector2f drift = lerp(origin, rest, sstep(t0, t0 + 0.5f, u));
                    const float pull = sstep(3.2f + i * 0.08f, 3.9f + i * 0.08f, u);
                    if (pull >= 1.f) continue;
                    const sf::Vector2f q = lerp(drift, sp, pull * pull);
                    spark(q, 9.f, ui::AMBER);
                }
            };
            scrap(L, tBreakL, 3, 100);
            scrap(L + sf::Vector2f(40.f, -90.f) * (tBreakM - tBreakL), tBreakM, 2, 200);
            ship(sp, a);
            if (u > 3.95f && u < 5.0f) say("+ SCRAP", sp + sf::Vector2f(0.f, -80.f - 30.f * (u - 3.95f)), 18, ui::AMBER, 2.f);
            if (u >= 2.6f && u < 3.9f) m_b.ring(sp, 150.f, 1.5f, tdraw::alpha(ui::AMBER, 0.35f), 48);
        }

        // ====================================================================
        // ADVANCED
        // ====================================================================

        void dodge(float u, std::vector<bool>& lit) {
            m_clock = u;
            const sf::Vector2f h0{ -80.f, 0.f };
            const float winding = u < 1.2f ? 1.f : 0.f;
            const float bx = u < 1.2f ? 380.f : 380.f - 1800.f * (u - 1.2f);
            const bool charging = u >= 1.2f && u < 1.75f;
            const float dk = sstep(1.42f, 1.58f, u);
            const sf::Vector2f hp = h0 + sf::Vector2f(0.f, -110.f * dk);
            lit[0] = u >= 1.42f && u < 1.7f;
            if (winding > 0.f) {
                m_b.dashed({ 380.f, 0.f }, { -560.f, 0.f }, 4.f, tdraw::alpha(ui::RED, 0.4f + 0.5f * u / 1.2f), 20.f, 16.f, u * 120.f);
                say("WIND-UP", { 380.f, -80.f }, 14, ui::RED);
            }
            if (u >= 1.42f && u < 2.0f)
                for (int i = 0; i < 6; ++i) {
                    const float k = i / 5.f;
                    ghost(h0 + sf::Vector2f(0.f, -110.f * dk * k), 0.f, (1.f - k * 0.5f) * (1.f - sstep(1.6f, 2.0f, u)));
                }
            if (charging) for (int i = 1; i < 5; ++i) enemyGhost("BERSERKER", { bx + i * 36.f, 0.f }, PI, 0.5f - i * 0.1f);
            const float fl = (winding > 0.f && static_cast<int>(u * 14.f) % 2) ? 0.8f : 0.f;
            if (bx > -700.f) enemy("BERSERKER", { bx, 0.f }, PI, fl, charging);
            ship(hp, 0.f);
            if (u > 1.5f && u < 2.7f) {
                const float k = (u - 1.5f) / 1.2f;
                m_b.ring(hp, 50.f + 160.f * k, 4.f, tdraw::alpha(paint(DODGEP), 1.f - k));
                m_b.ring(hp, 30.f + 100.f * k, 2.f, tdraw::alpha(paint(DODGEP), 1.f - k));
                say("PERFECT DODGE", hp + sf::Vector2f(-230.f, -30.f), 22, paint(DODGEP), 1.7f);
                say("+ ENERGY   RE-ARMED", hp + sf::Vector2f(-230.f, 0.f), 12, ui::TEXT_DIM);
            }
        }

        void parry(float u, std::vector<bool>& lit) {
            m_clock = u;
            const sf::Vector2f hp{ -260.f, 0.f }, rp{ 300.f, 0.f };
            lit[0] = u >= 0.9f && u < 1.12f;
            const sf::Vector2f muzzleE = rp + sf::Vector2f(-40.f, 0.f), muzzleH = hp + sf::Vector2f(shipR() + 10.f, 0.f);
            if (u >= 0.1f && u < 1.0f) {
                const sf::Vector2f b = lerp(muzzleE, muzzleH, (u - 0.1f) / 0.9f);
                bolt(b, { -1.f, 0.f }, 18.f, 6.f, ui::AMBER_HOT);
            }
            if (u >= 0.92f && u < 1.25f) {
                const float k = (u - 0.92f) / 0.33f;
                m_b.ring(hp, 46.f + 120.f * k, 4.f, tdraw::alpha(paint(PARRYP), 1.f - k));
            }
            float rflash = 0.f;
            if (u >= 1.0f && u < 1.55f) {
                const float k = (u - 1.0f) / 0.55f;
                sf::Vector2f b = lerp(muzzleH, muzzleE, k);
                b.y += std::sin(k * PI * 3.f) * 26.f * (1.f - k);
                const sf::Vector2f b2 = lerp(muzzleH, muzzleE, std::max(0.f, k - 0.04f));
                m_b.line(b2, b, 5.f, paint(HOMING));
                spark(b, 9.f, paint(HOMING));
            }
            if (u >= 1.55f && u < 2.1f) {
                const float k = (u - 1.55f) / 0.55f;
                m_b.ring(rp, 20.f + 160.f * k, 3.f, tdraw::alpha(paint(HOMING), 1.f - k));
                burst(rp, k, ui::AMBER_HOT, 14, 120.f, 5);
                rflash = 1.f - k;
            }
            enemy("RAIDER", rp, PI, rflash);
            ship(hp, 0.f);
            if (u > 1.0f && u < 2.5f) say("PARRY", hp + sf::Vector2f(0.f, -84.f), 22, paint(PARRYP), 1.7f);
        }

        void rift(float u, std::vector<bool>& lit) {
            m_clock = u;
            const int seg = std::min(2, static_cast<int>(u / 2.4f));
            const float s = u - seg * 2.4f;
            const sf::Vector2f hp{ -380.f, 0.f };
            const sf::Vector2f nose = hp + sf::Vector2f(shipR() + 8.f, 0.f);
            static const float kDet[3] = { 1.20f, 1.05f, 1.12f };
            const float det = kDet[seg];
            lit[0] = (s < 0.6f) || (s >= det - 0.04f && s < det + 0.12f);
            const sf::Color V = ui::VIOLET;

            // charge: sparks converge on the nose
            if (s < 0.6f) {
                const float k = s / 0.6f;
                for (int i = 0; i < 10; ++i) {
                    const float a = hash(i * 13 + seg) * TAU + s * 3.f;
                    const float R = (110.f - 90.f * std::fmod(k * 2.f + hash(i) , 1.f));
                    const sf::Vector2f p = nose + sf::Vector2f(std::cos(a), std::sin(a)) * R;
                    m_b.line(p, p + (nose - p) * 0.25f, 3.f, tdraw::alpha(V, 0.5f + 0.5f * k));
                }
                m_b.ring(nose, 6.f + 10.f * k, 3.f, V, 16);
                say("CHARGING", hp + sf::Vector2f(0.f, -80.f), 14, V);
            }
            const sf::Vector2f boltP = nose + sf::Vector2f(1100.f * std::max(0.f, s - 0.6f), 0.f);
            if (s >= 0.6f && s < det) bolt(boltP, { 1.f, 0.f }, 46.f, 9.f, V);

            if (seg == 0) {
                if (s >= det && s < det + 0.8f) {
                    const float k = (s - det) / 0.8f;
                    const sf::Vector2f c = nose + sf::Vector2f(1100.f * (det - 0.6f), 0.f);
                    m_b.ring(c, 20.f + 150.f * k, 5.f, tdraw::alpha(V, 1.f - k));
                    burst(c, k, V, 18, 170.f, 7);
                    say("BURST", c + sf::Vector2f(0.f, -90.f), 22, V, 1.7f);
                }
            }
            else if (seg == 1) {
                const sf::Vector2f rk{ 150.f, 64.f }, en{ 400.f, -120.f };
                float rflash = 0.f;
                sf::Vector2f rkp = rk;
                if (s >= det) {
                    const float k = sstep(det + 0.1f, det + 0.6f, s);
                    rkp = lerp(rk, en, k);
                    if (s < det + 0.35f) m_b.ring(rk, 30.f + 120.f * (s - det) / 0.35f, 4.f, tdraw::alpha(paint(HOMING), 1.f - (s - det) / 0.35f));
                    if (k > 0.f && k < 1.f) m_b.line(lerp(rk, en, std::max(0.f, k - 0.15f)), rkp, 6.f, tdraw::alpha(paint(HOMING), 0.7f));
                    say("HIJACK", rk + sf::Vector2f(0.f, 80.f), 22, paint(HOMING), 1.7f);
                    if (s >= det + 0.6f) {
                        const float kk = std::clamp((s - det - 0.6f) / 0.6f, 0.f, 1.f);
                        burst(en, kk, ui::AMBER_HOT, 20, 150.f, 11);
                        m_b.ring(en, 20.f + 150.f * kk, 4.f, tdraw::alpha(paint(HOMING), 1.f - kk));
                        rflash = 1.f - kk;
                    }
                }
                enemy("RAIDER", en, ang(hp - en), rflash);
                if (s < det + 0.6f) rock(rkp, 52.f, s * 0.7f, 0.f);
                if (s >= det && s < det + 0.6f) m_b.ring(rkp, 60.f, 2.f, tdraw::alpha(paint(HOMING), 0.8f));
            }
            else {
                const sf::Vector2f en{ 260.f, 52.f };
                float fl = 0.f;
                sf::Vector2f shake{ 0.f, 0.f };
                if (s >= det) {
                    const float k = std::clamp((s - det) / 1.2f, 0.f, 1.f);
                    fl = static_cast<int>(s * 30.f) % 2 ? 0.7f : 0.2f;
                    shake = { std::sin(s * 90.f) * 4.f, std::cos(s * 77.f) * 3.f };
                    for (int i = 0; i < 6; ++i) {
                        // stun arcs: short zig-zags around the hull
                        const float a = hash(i * 7 + static_cast<int>(s * 12.f)) * TAU;
                        sf::Vector2f p = en + sf::Vector2f(std::cos(a), std::sin(a)) * 34.f;
                        for (int j = 0; j < 3; ++j) {
                            const sf::Vector2f q = p + sf::Vector2f(std::cos(a + (j % 2 ? 0.9f : -0.9f)), std::sin(a + (j % 2 ? 0.9f : -0.9f))) * 14.f;
                            m_b.line(p, q, 2.f, tdraw::alpha(V, 1.f - k * 0.5f));
                            p = q;
                        }
                    }
                    if (s < det + 0.5f) m_b.ring(en, 30.f + 140.f * (s - det) / 0.5f, 5.f, tdraw::alpha(V, 1.f - (s - det) / 0.5f));
                    say("OVERLOAD", en + sf::Vector2f(0.f, -92.f), 22, V, 1.7f);
                    say("X4 DAMAGE   STUNNED", en + sf::Vector2f(0.f, 70.f), 12, ui::TEXT_DIM);
                }
                enemy("RAIDER", en + shake, PI, fl);
            }
            ship(hp, 0.f);
            if (s >= det - 0.04f && s < det + 0.3f) say("DETONATE", hp + sf::Vector2f(0.f, -80.f), 14, ui::AMBER);
        }

        void vent(float u, std::vector<bool>& lit) {
            m_clock = u;
            const sf::Vector2f hp{ -300.f, -50.f };
            const float perfectAt = 1.70f;
            lit[0] = u >= perfectAt && u < perfectAt + 0.14f;
            const bool overdrive = u >= perfectAt;
            const sf::Vector2f fw{ 1.f, 0.f };
            const sf::Vector2f muzzle = hp + fw * (shipR() + 4.f);
            auto shots = [&](float t0, float t1) {
                for (int k = 0;; ++k) {
                    const float tk = t0 + k * 0.12f;
                    if (tk >= t1 || tk > u) break;
                    const float d = 1800.f * (u - tk);
                    if (d < 1000.f) bolt(muzzle + fw * d, fw, 22.f, 4.f, overdrive ? ui::AMBER : paint(PLASMA));
                }
            };
            shots(0.f, 0.8f);
            shots(2.1f, 3.6f);
            ship(hp, 0.f, 1.f, overdrive);
            const float heat = u < 0.8f ? u / 0.8f : (u < perfectAt ? 1.f : 0.f);
            gauge(hp + sf::Vector2f(-60.f, 80.f), "HEAT", heat, heat > 0.85f ? ui::RED : ui::AMBER_HOT, u >= 0.8f && u < perfectAt);
            if (u >= 0.8f && u < perfectAt && static_cast<int>(u * 8.f) % 2 == 0) say("OVERHEAT", hp + sf::Vector2f(0.f, -84.f), 18, ui::RED, 1.6f);

            // the vent bar: blue = good, amber = perfect
            if (u >= 0.9f && u < perfectAt + 0.5f) {
                const sf::Vector2f o{ -80.f, 90.f };
                const float w = 560.f, h = 26.f;
                m_b.rect(o.x, o.y, w, h, sf::Color(10, 14, 20));
                m_b.rect(o.x + w * 0.55f, o.y, w * 0.25f, h, tdraw::alpha(ui::BLUE_COOL, 0.75f));
                m_b.rect(o.x + w * 0.64f, o.y, w * 0.06f, h, ui::AMBER_HOT);
                m_b.loop({ o, o + sf::Vector2f(w, 0.f), o + sf::Vector2f(w, h), o + sf::Vector2f(0.f, h) }, 2.f, ui::CYAN_MID);
                const float m = std::clamp((std::min(u, perfectAt) - 0.9f) / 1.2f, 0.f, 1.f);
                m_b.rect(o.x + w * m - 2.f, o.y - 8.f, 4.f, h + 16.f, ui::TEXT);
                say("VENT", o + sf::Vector2f(-50.f, 4.f), 14, ui::TEXT_DIM);
                say("GOOD", o + sf::Vector2f(w * 0.59f, h + 10.f), 10, ui::BLUE_COOL, 1.6f);
                say("PERFECT", o + sf::Vector2f(w * 0.67f, -24.f), 10, ui::AMBER_HOT, 1.6f);
            }
            if (overdrive) {
                say("PERFECT VENT", hp + sf::Vector2f(60.f, -112.f), 22, ui::AMBER_HOT, 1.7f);
                say("OVERDRIVE  //  NO HEAT", hp + sf::Vector2f(60.f, -78.f), 14, ui::AMBER);
            }
        }

        // ---- text, drawn after the batch ----
        void text(const std::string& s, sf::Vector2f p, unsigned size, sf::Color c, bool centre, float sp) {
            sf::Text t(*m_font, s, size);
            t.setLetterSpacing(sp);
            t.setFillColor(c);
            if (centre) { const sf::FloatRect b = t.getLocalBounds(); t.setOrigin({ b.position.x + b.size.x * 0.5f, 0.f }); }
            t.setPosition({ std::round(p.x), std::round(p.y) });
            m_t->draw(t);
        }

        struct Label { std::string s; sf::Vector2f p; unsigned size; sf::Color c; bool centre; float sp; };

        const tdraw::HullMesh* m_mesh = nullptr;
        const ship::Livery* m_lv = nullptr;
        const enemyarch::EnemyRegistry* m_reg = nullptr;
        const sf::Font* m_font = nullptr;
        sf::RenderTarget* m_t = nullptr;
        sf::Vector2f m_c;
        float m_clock = 0.f;
        tdraw::Batch m_b;
        std::vector<Label> m_labels;
        std::vector<sf::Vertex> m_vscratch;
        std::vector<sf::Vector2f> m_pscratch;
    };

} // namespace reels
