/**
 * @file FeedSim.hpp
 * @brief The tactical feed: another hunter's fight, watched from the terminal.
 *
 * The PHASE 2 HOOK in the old menu said "swap the starfield for the live AI
 * skirmish render". This is that skirmish -- an attract-mode fight between a
 * random hunter and a Rakshari pack, in a field of rocks, filmed by a camera
 * that frames the duel.
 *
 * WHY A SEPARATE LITTLE SIMULATION, NOT THE REAL WORLD
 * ----------------------------------------------------
 * The real game is one EntityManager, one Box2D world and twenty systems that
 * cache the player id, draw straight to the window and read the keyboard. A
 * second copy running behind the menu would mean making all of that
 * instantiable twice, and every system that assumes "there is one player" a
 * place for a menu bug to become a gameplay bug. The feed is a SHOW, so it is
 * a show: a few hundred lines that move the same ships the same way and can
 * never touch the run.
 *
 * What it shares with the game is everything you can SEE: enemy hulls come
 * from the EnemyRegistry (so a hull edit in enemy.lua shows here on F5), the
 * hunter flies a real class preset from ShipDesign, and the beats are the
 * game's beats -- the Berserker winds up behind a marching lane and rams, the
 * hunter dashes through it (PERFECT DODGE), Raider bursts get parried back,
 * rocks split large to medium to small, magma rocks go up, kills drop scrap
 * that gets pulled in.
 *
 * And hunters die. When the watched hull goes, the feed drops to static,
 * SIGNAL LOST, and reacquires on the next hunter in the sector. The terminal's
 * casualty counter takes the hit.
 *
 * World: a 4000 x 2800 torus in game pixels. Everything wraps, so the fight
 * never runs out of field and the camera never meets an edge.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "utils/TermDraw.hpp"
#include "utils/UiPalette.hpp"
#include "core/EnemyArchetypes.hpp"
#include "utils/ShipDesign.hpp"

namespace feed {

    using tdraw::PI;
    using tdraw::TAU;

    class FeedSim {
    public:
        // ---- tuning (game pixels, seconds) ----
        static constexpr float WW = 4000.f, WH = 2800.f;
        static constexpr float ZOOM = 0.62f;

        void setRegistry(const enemyarch::EnemyRegistry* r) { m_reg = r; }

        void reset() {
            ++m_feedNo;
            static const char* kCalls[] = { "VESK-11", "ORRA-04", "TALLOW-19", "KAIN-02",
                "MERIDIAN-33", "SOT-08", "HALCYON-21", "BRAND-14", "IVERS-05", "NULL-29" };
            m_call = kCalls[(m_feedNo * 3 + (next() % 3)) % 10];

            // A random hunter: a real class preset in a random paint.
            const int cls = static_cast<int>(next() % 3);
            const ship::ShipDesign d = ship::ShipDesign::preset(static_cast<ship::HullClass>(cls));
            m_hunterMesh = tdraw::meshFromDesign(d, 2.4f);
            int pc = 0;
            const sf::Color* pal = ship::liveryPalette(pc);
            static const int kHull[] = { 0, 2, 3, 4, 5, 7, 8, 9, 10, 11 };
            m_hunterFill = pal[kHull[next() % 10] % pc];
            m_hunterEdge = sf::Color(236, 240, 245);

            m_h = Hunter{};
            m_h.p = { WW * 0.5f, WH * 0.5f };
            m_h.a = -PI * 0.5f;
            m_ships.clear(); m_bullets.clear(); m_fx.clear(); m_ghosts.clear(); m_floats.clear(); m_scrap.clear();
            if (m_rocks.empty()) for (int i = 0; i < 12; ++i) addRock(2, { frand() * WW, frand() * WH });
            m_lost = -1.f;
            m_spawnT = 0.6f;
            m_cam = m_h.p;
            if (m_stars.empty()) {
                for (int L = 0; L < 3; ++L)
                    for (int i = 0; i < 60; ++i)
                        m_stars.push_back({ frand(), frand(), 0.18f + 0.3f * L, next() % 7 == 0 });
            }
        }

        // ---- read-outs for the panels around the feed ----
        int feedNo() const { return m_feedNo; }
        const std::string& callsign() const { return m_call; }
        float hull01() const { return m_lost >= 0.f ? 0.f : std::clamp(m_h.hp / 100.f, 0.f, 1.f); }
        int kills() const { return m_h.kills; }
        int hostiles() const { return static_cast<int>(m_ships.size()); }
        bool signalLost() const { return m_lost >= 0.f; }
        /// Total deaths the feed has shown (hunters and Rakshari). The ledger's
        /// casualty counter adds this, so a kill on screen moves the number.
        long long casualties() const { return m_casualties; }

        // ====================================================================
        // STEP
        // ====================================================================
        void step(float dt) {
            dt = std::clamp(dt, 0.f, 1.f / 30.f);
            m_time += dt;
            if (m_feedNo == 0) reset();

            if (m_lost >= 0.f) {
                m_lost += dt;
                if (m_lost > 2.6f) reset();
                stepFx(dt);
                return;
            }

            spawnDirector(dt);
            retarget();
            stepHunter(dt);
            stepShips(dt);
            stepBullets(dt);
            resolveDeaths();
            retarget();
            stepRocks(dt);
            stepScrap(dt);

            if (m_h.hp <= 0.f) {
                m_lost = 0.f;
                boom(m_h.p, ui::CYAN, 40, 760.f, 140.f);
                boom(m_h.p, sf::Color::White, 12, 400.f, 0.f);
                ++m_casualties;
            }

            // Camera frames the duel: the hunter, a share of the way to its
            // target, a little ahead of its drift.
            sf::Vector2f f = m_h.v * 0.2f;
            if (m_aim >= 0 && m_aimDist < 1040.f) f += delta(m_ships[m_aim].p, m_h.p) * 0.3f;
            const float k = std::min(1.f, dt * 2.6f);
            m_cam.x += wrapD(m_h.p.x + f.x - m_cam.x, WW) * k;
            m_cam.y += wrapD(m_h.p.y + f.y - m_cam.y, WH) * k;
            wrap(m_cam);

            stepFx(dt);
        }

        /// Debug / preview: end the watched hunter now.
        void killHunter() { if (m_lost < 0.f) m_h.hp = -999.f; }

        // ====================================================================
        // DRAW -- into the current view, local rect `r` (panel coordinates)
        // ====================================================================
        void draw(sf::RenderTarget& t, sf::FloatRect r, const sf::Font* font) {
            const sf::Vector2f c = r.position + r.size * 0.5f;
            const float hw = r.size.x * 0.5f / ZOOM + 140.f, hh = r.size.y * 0.5f / ZOOM + 140.f;

            // ---- ground + parallax stars (screen space) ----
            m_b.clear();
            m_b.rect(r.position.x, r.position.y, r.size.x, r.size.y, sf::Color(3, 5, 8));
            for (const auto& s : m_stars) {
                const float x = r.position.x + posMod(s.x * r.size.x - m_cam.x * s.f * ZOOM, r.size.x);
                const float y = r.position.y + posMod(s.y * r.size.y - m_cam.y * s.f * ZOOM, r.size.y);
                const float sz = s.big ? 2.f : 1.f;
                m_b.rect(x, y, sz, sz, tdraw::alpha(ui::TEXT, 0.25f + s.f * 0.9f));
            }
            m_b.draw(t);

            // ---- world (camera-relative, scaled by ZOOM) ----
            m_b.clear();
            auto W = [&](sf::Vector2f p) { return delta(p, m_cam); };
            auto visible = [&](sf::Vector2f q, float pad) { return std::fabs(q.x) < hw + pad && std::fabs(q.y) < hh + pad; };

            for (const auto& k : m_rocks) {
                const sf::Vector2f q = W(k.p);
                if (!visible(q, k.r)) continue;
                const auto pts = tdraw::rockPoints(q, k.r, k.a, k.shape.data(), 9);
                m_b.fan(pts, k.flash > 0.f ? ui::TEXT : sf::Color(16, 19, 25));
                m_b.loop(pts, k.size == 2 ? 3.f : 2.f, k.volcanic ? ui::AMBER_HOT : ui::TEXT_DIM);
                if (k.volcanic) {
                    for (int i = 0; i < 3; ++i) {
                        const float a = k.a * 1.3f + i * 2.1f;
                        const float pulse = 0.6f + 0.4f * std::sin(m_time * 6.f + i);
                        const sf::Vector2f e = q + sf::Vector2f(std::cos(a), std::sin(a)) * (k.r * 0.4f);
                        m_b.rect(e.x - 4.f, e.y - 4.f, 8.f, 8.f, tdraw::alpha(ui::AMBER_HOT, pulse));
                    }
                }
            }
            for (const auto& s : m_scrap) { const sf::Vector2f q = W(s.p); m_b.rect(q.x - 4.f, q.y - 4.f, 8.f, 8.f, ui::AMBER); }
            for (const auto& b : m_bullets) {
                const sf::Vector2f q = W(b.p);
                if (!visible(q, 40.f)) continue;
                const float sp = std::max(1.f, std::hypot(b.v.x, b.v.y));
                const float L = b.own ? 24.f : 16.f;
                m_b.line(q, q - b.v / sp * L, b.own ? 4.f : 6.f, b.c);
            }
            for (const auto& s : m_ships) {
                const sf::Vector2f q = W(s.p);
                if (!visible(q, 80.f)) continue;
                if (s.st == Windup) {
                    const sf::Vector2f u{ std::cos(s.a), std::sin(s.a) };
                    const float k = std::clamp(s.stT / 0.6f, 0.f, 1.f);
                    m_b.dashed(q, q + u * 660.f, 4.f, tdraw::alpha(ui::RED, 0.4f + 0.5f * k), 20.f, 16.f, m_time * 120.f);
                }
                const enemyarch::ArchetypeDef* a = arche(s.kind);
                if (s.st == Charge && a) {
                    for (int i = 1; i < 5; ++i)
                        tdraw::drawArchetype(m_b, *a, tdraw::hullXf(q - s.v * (0.012f * i), s.a, 1.f),
                            sf::Color(0, 0, 0, 0), tdraw::alpha(ui::RED, 0.5f - i * 0.1f), 2.f, &m_scratch);
                }
                const bool flash = s.flash > 0.f || (s.st == Windup && static_cast<int>(m_time * 14.f) % 2 == 0);
                if (a) {
                    sf::Color fill = tdraw::enemyFill(*a);
                    if (s.st == Charge) fill = sf::Color(255, 245, 215);
                    if (flash) fill = tdraw::mix(fill, sf::Color::White, 0.8f);
                    tdraw::drawArchetype(m_b, *a, tdraw::hullXf(q, s.a, 1.f), fill, tdraw::enemyEdge(*a), 2.4f, &m_scratch);
                }
                else {
                    const float sz = s.kind == Wardog ? 20.f : 30.f;
                    std::vector<sf::Vector2f> p = { q + sf::Vector2f(std::cos(s.a), std::sin(s.a)) * sz,
                        q + sf::Vector2f(std::cos(s.a + 2.5f), std::sin(s.a + 2.5f)) * sz,
                        q + sf::Vector2f(std::cos(s.a - 2.5f), std::sin(s.a - 2.5f)) * sz };
                    m_b.fan(p, flash ? sf::Color::White : sf::Color(200, 70, 55));
                }
            }
            if (m_lost < 0.f) {
                for (const auto& g : m_ghosts) {
                    const float k = g.life / 0.35f;
                    tdraw::drawHull(m_b, m_hunterMesh, tdraw::hullXf(W(g.p), g.a, 1.f),
                        tdraw::alpha(m_hunterFill, 0.14f * k), tdraw::alpha(m_hunterFill, 0.7f * k));
                }
                const sf::Vector2f q = W(m_h.p);
                const sf::Vector2f back{ -std::cos(m_h.a), -std::sin(m_h.a) };
                const float fl = 12.f + m_h.thrust * 34.f + std::sin(m_time * 40.f) * 6.f;
                const sf::Vector2f stern = q + back * (m_hunterMesh.radius * 0.85f);
                m_b.line(stern, stern + back * fl, 8.f, tdraw::alpha(ui::AMBER_HOT, 0.9f));
                tdraw::drawHull(m_b, m_hunterMesh, tdraw::hullXf(q, m_h.a, 1.f), m_hunterFill, m_hunterEdge);
                if (m_h.parryFx > 0.f)
                    m_b.ring(q, 44.f + (0.25f - m_h.parryFx) * 240.f, 4.f, tdraw::alpha(ui::AMBER, m_h.parryFx * 4.f));
            }
            for (const auto& p : m_fx) {
                const sf::Vector2f q = W(p.p);
                const float a = std::clamp(p.life / p.max, 0.f, 1.f);
                if (p.ring) m_b.ring(q, p.r1 + (p.r0 - p.r1) * a, 4.f, tdraw::alpha(p.c, a));
                else m_b.rect(q.x - p.sz * 0.5f, q.y - p.sz * 0.5f, p.sz, p.sz, tdraw::alpha(p.c, a));
            }
            sf::RenderStates st;
            st.transform.translate(c).scale({ ZOOM, ZOOM });
            drawBatch(t, st);

            // ---- camera overlay (screen space) ----
            m_b.clear();
            auto S = [&](sf::Vector2f p) { return c + delta(p, m_cam) * ZOOM; };
            if (m_lost < 0.f && m_aim >= 0) {
                const sf::Vector2f tq = S(m_ships[m_aim].p);
                bracket(tq, 30.f, ui::RED);
            }
            if (m_lost >= 0.f && m_lost > 0.25f) {
                for (int i = 0; i < 900; ++i)
                    m_b.rect(r.position.x + frand() * r.size.x, r.position.y + frand() * r.size.y,
                        1.f + frand() * 3.f, 1.f, tdraw::alpha(ui::TEXT, frand() * 0.5f));
                for (int i = 0; i < 6; ++i)
                    m_b.rect(r.position.x, r.position.y + frand() * r.size.y, r.size.x, 2.f + frand() * 8.f, tdraw::alpha(ui::TEXT, 0.06f));
            }
            m_b.draw(t);

            if (!font) return;
            for (const auto& f : m_floats)
                label(t, *font, f.s, S(f.p), 14, tdraw::alpha(f.c, std::clamp(f.life * 2.f, 0.f, 1.f)), true, 1.6f);
            if (m_lost < 0.f) {
                label(t, *font, m_call, S(m_h.p) + sf::Vector2f(26.f, 18.f), 12, tdraw::alpha(ui::CYAN, 0.9f));
                if (m_aim >= 0) {
                    const Ship& s = m_ships[m_aim];
                    const sf::Vector2f tq = S(s.p);
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%s  %.2f KM", kindName(s.kind), m_aimDist / 2000.f);
                    label(t, *font, buf, tq + sf::Vector2f(36.f, -32.f), 12, ui::RED);
                    if (s.st == Windup && static_cast<int>(m_time * 8.f) % 2 == 0)
                        label(t, *font, "CHARGING", tq + sf::Vector2f(36.f, -14.f), 12, ui::AMBER);
                }
            }
            else {
                sf::RectangleShape box;
                box.setFillColor(sf::Color(2, 3, 5, 230));
                if (m_lost > 0.4f && m_lost < 1.9f) {
                    box.setSize({ 440.f, 96.f }); box.setPosition(c - sf::Vector2f(220.f, 48.f)); t.draw(box);
                    label(t, *font, "SIGNAL LOST", c + sf::Vector2f(0.f, -38.f), 30, ui::RED, true, 1.8f);
                    label(t, *font, "HUNTER " + m_call + "  //  BEACON SILENT", c + sf::Vector2f(0.f, 10.f), 12, ui::TEXT, true);
                }
                else if (m_lost >= 1.9f) {
                    box.setSize({ 440.f, 60.f }); box.setPosition(c - sf::Vector2f(220.f, 30.f)); t.draw(box);
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "REACQUIRING FEED %02d ...", m_feedNo + 1);
                    label(t, *font, buf, c + sf::Vector2f(0.f, -12.f), 18, ui::AMBER, true);
                }
            }
        }

    private:
        enum Kind { Berserker = 0, Raider = 1, Wardog = 2 };
        enum State { Approach, Windup, Charge, Recover };

        struct Rock {
            sf::Vector2f p, v; float a = 0.f, va = 0.f, r = 92.f, hp = 40.f, flash = 0.f;
            int size = 2; bool volcanic = false; std::array<float, 9> shape{};
        };
        struct Ship {
            int kind = Berserker; sf::Vector2f p, v; float a = 0.f, hp = 30.f;
            State st = Approach; float stT = 0.f, fire = 1.f, bt = 0.f, flash = 0.f, orbit = 1.f;
            int burst = 0; bool hit = false, dodged = false;
        };
        struct Bullet { sf::Vector2f p, v; float life = 1.f; bool own = true; float dmg = 6.f; sf::Color c; };
        struct Fx { sf::Vector2f p, v; float life = 0.5f, max = 0.5f; sf::Color c; float sz = 4.f; bool ring = false; float r0 = 0.f, r1 = 0.f; };
        struct Ghost { sf::Vector2f p; float a = 0.f, life = 0.35f; };
        struct Float { sf::Vector2f p; std::string s; sf::Color c; float life = 1.1f; };
        struct Scrap { sf::Vector2f p, v; float life = 6.f; };
        struct Star { float x, y, f; bool big; };
        struct Hunter {
            sf::Vector2f p, v; float a = 0.f, hp = 100.f, fire = 0.f, dash = 0.f, dashCd = 0.f,
                parryCd = 0.f, parryFx = 0.f, iframe = 0.f, thrust = 0.f, ghostT = 0.f;
            int kills = 0;
        };

        // ---- maths on the torus ----
        static float wrapD(float d, float L) { return d - std::round(d / L) * L; }
        static sf::Vector2f delta(sf::Vector2f a, sf::Vector2f b) { return { wrapD(a.x - b.x, WW), wrapD(a.y - b.y, WH) }; }
        static void wrap(sf::Vector2f& p) { p.x = posMod(p.x, WW); p.y = posMod(p.y, WH); }
        static float posMod(float v, float m) { float r = std::fmod(v, m); return r < 0.f ? r + m : r; }
        static float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }

        std::uint32_t next() { m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5; return m_rng; }
        float frand() { return static_cast<float>(next() % 100000u) / 100000.f; }
        float rr(float a, float b) { return a + frand() * (b - a); }

        static const char* kindName(int k) { return k == Berserker ? "BERSERKER" : k == Raider ? "RAIDER" : "WARDOG"; }
        const enemyarch::ArchetypeDef* arche(int k) const { return m_reg ? m_reg->byKey(kindName(k)) : nullptr; }
        static float hitRadius(int k) { return k == Wardog ? 22.f : 34.f; }

        void addRock(int size, sf::Vector2f p, sf::Vector2f v = { 9999.f, 0.f }) {
            static const float kR[] = { 26.f, 52.f, 92.f }, kHp[] = { 6.f, 16.f, 40.f };
            Rock k;
            k.size = size; k.p = p;
            k.v = (v.x > 9000.f) ? sf::Vector2f(rr(-80.f, 80.f), rr(-80.f, 80.f)) : v;
            k.a = frand() * TAU; k.va = rr(-0.6f, 0.6f); k.r = kR[size]; k.hp = kHp[size];
            k.volcanic = size == 2 && frand() < 0.2f;
            for (auto& s : k.shape) s = rr(0.72f, 1.12f);
            wrap(k.p);
            m_rocks.push_back(k);
        }

        void spawn(int kind) {
            Ship s;
            s.kind = kind;
            const float a = frand() * TAU, d = rr(1120.f, 1520.f);
            s.p = m_h.p + sf::Vector2f(std::cos(a), std::sin(a)) * d;
            wrap(s.p);
            s.a = a + PI;
            s.hp = kind == Berserker ? 30.f : kind == Raider ? 22.f : 12.f;
            s.fire = rr(0.5f, 1.5f);
            s.orbit = frand() < 0.5f ? 1.f : -1.f;
            m_ships.push_back(s);
        }

        void boom(sf::Vector2f p, sf::Color c, int n, float sp, float ring) {
            for (int i = 0; i < n; ++i) {
                const float a = frand() * TAU, s = rr(0.2f, 1.f) * sp;
                Fx f; f.p = p; f.v = { std::cos(a) * s, std::sin(a) * s };
                f.life = rr(0.3f, 0.8f); f.max = 0.8f; f.c = c; f.sz = frand() < 0.3f ? 6.f : 4.f;
                m_fx.push_back(f);
            }
            if (ring > 0.f) { Fx f; f.p = p; f.ring = true; f.life = f.max = 0.45f; f.c = c; f.r0 = 12.f; f.r1 = ring; m_fx.push_back(f); }
        }
        void floatText(sf::Vector2f p, const char* s, sf::Color c) { m_floats.push_back({ p - sf::Vector2f(0.f, 60.f), s, c, 1.1f }); }

        int count(int kind) const { int n = 0; for (const auto& s : m_ships) n += s.kind == kind; return n; }

        void spawnDirector(float dt) {
            m_spawnT -= dt;
            if (m_spawnT <= 0.f) {
                if (count(Berserker) < 2) spawn(Berserker);
                else if (count(Wardog) < 2) spawn(Wardog);
                else if (count(Raider) < 1) spawn(Raider);
                m_spawnT = rr(1.2f, 2.6f);
            }
            int large = 0;
            for (const auto& k : m_rocks) large += k.size == 2;
            if (large < 8) {
                const float a = frand() * TAU;
                addRock(2, m_h.p + sf::Vector2f(std::cos(a), std::sin(a)) * 1800.f);
            }
        }

        /// Nearest hostile. Re-run after deaths so the index never dangles.
        void retarget() {
            m_aim = -1; m_aimDist = 1e9f;
            for (int i = 0; i < static_cast<int>(m_ships.size()); ++i) {
                const float d = len(delta(m_ships[i].p, m_h.p));
                if (d < m_aimDist) { m_aimDist = d; m_aim = i; }
            }
        }

        void stepHunter(float dt) {
            Hunter& h = m_h;
            sf::Vector2f acc{ 0.f, 0.f };
            if (m_aim >= 0) {
                const Ship& t = m_ships[m_aim];
                const sf::Vector2f dv = delta(t.p, h.p);
                const float d = std::max(1.f, len(dv));
                const float want = t.kind == Raider ? 600.f : 460.f;
                const float radial = (d - want) / want;
                acc += dv / d * radial * 1040.f;
                acc += sf::Vector2f(-dv.y / d, dv.x / d) * 520.f;
                const float lead = d / 1800.f;
                const sf::Vector2f l = dv + (t.v - h.v) * lead;
                const float da = wrapD(std::atan2(l.y, l.x) - h.a, TAU);
                h.a += std::clamp(da, -5.5f * dt, 5.5f * dt);
                h.fire -= dt;
                if (std::fabs(da) < 0.18f && d < 1120.f && h.fire <= 0.f && h.dash <= 0.f) {
                    h.fire = 0.15f;
                    const sf::Vector2f fw{ std::cos(h.a), std::sin(h.a) };
                    const sf::Vector2f m = h.p + fw * (m_hunterMesh.radius + 6.f);
                    m_bullets.push_back({ m, fw * 1800.f + h.v, 0.7f, true, 6.f, ui::CYAN });
                    Fx f; f.p = m; f.life = f.max = 0.06f; f.c = sf::Color::White; f.sz = 8.f; m_fx.push_back(f);
                }
            }
            else {
                h.a += 0.6f * dt;
                acc += sf::Vector2f(std::cos(h.a), std::sin(h.a)) * 400.f;
            }
            for (const auto& k : m_rocks) {
                const sf::Vector2f dv = delta(h.p, k.p);
                const float d = len(dv), m = k.r + 180.f;
                if (d < m && d > 0.f) acc += dv / d * (1800.f * (m - d) / 180.f);
            }
            h.dashCd -= dt; h.parryCd -= dt; h.iframe -= dt; h.parryFx -= dt;
            for (const auto& s : m_ships) {
                if (s.kind != Berserker || h.dashCd > 0.f || s.st != Charge) continue;
                const sf::Vector2f dv = delta(h.p, s.p);
                if (len(dv) < 340.f) {
                    const sf::Vector2f perp{ -std::sin(s.a), std::cos(s.a) };
                    const float side = (dv.x * perp.x + dv.y * perp.y) >= 0.f ? 1.f : -1.f;
                    h.v = perp * side * 1520.f;
                    h.dash = 0.2f; h.iframe = 0.28f; h.dashCd = 0.9f;
                }
            }
            if (h.dash > 0.f) {
                h.dash -= dt;
                h.ghostT -= dt;
                if (h.ghostT <= 0.f) { h.ghostT = 0.025f; m_ghosts.push_back({ h.p, h.a, 0.35f }); }
            }
            else {
                h.v += acc * dt;
                h.thrust = std::clamp(len(acc) / 1000.f, 0.f, 1.f);
            }
            const float sp = len(h.v), mx = h.dash > 0.f ? 1600.f : 680.f;
            if (sp > mx) h.v *= mx / sp;
            h.v *= std::pow(0.6f, dt);
            h.p += h.v * dt;
            wrap(h.p);
            h.hp = std::min(100.f, h.hp + 2.2f * dt);
        }

        void stepShips(float dt) {
            Hunter& h = m_h;
            for (auto& s : m_ships) {
                const sf::Vector2f dv = delta(h.p, s.p);
                const float d = std::max(1.f, len(dv)), toA = std::atan2(dv.y, dv.x);
                s.stT += dt; s.flash -= dt;
                sf::Vector2f acc{ 0.f, 0.f };
                if (s.kind == Berserker) {
                    if (s.st == Approach) {
                        s.a += std::clamp(wrapD(toA - s.a, TAU), -4.f * dt, 4.f * dt);
                        acc = sf::Vector2f(std::cos(s.a), std::sin(s.a)) * 840.f;
                        if (d < 640.f && s.stT > 1.f) { s.st = Windup; s.stT = 0.f; }
                    }
                    else if (s.st == Windup) {
                        s.a += std::clamp(wrapD(toA - s.a, TAU), -6.f * dt, 6.f * dt);
                        s.v *= std::pow(0.02f, dt);
                        if (s.stT > 0.6f) { s.st = Charge; s.stT = 0.f; s.v = sf::Vector2f(std::cos(s.a), std::sin(s.a)) * 1720.f; }
                    }
                    else if (s.st == Charge) {
                        if (d < 64.f) {
                            if (h.iframe > 0.f) {
                                if (!s.dodged) { s.dodged = true; floatText(h.p, "PERFECT DODGE", ui::CYAN); }
                            }
                            else if (!s.hit) {
                                s.hit = true; h.hp -= 22.f;
                                boom(h.p, ui::AMBER_HOT, 14, 520.f, 60.f);
                                h.v += s.v * 0.5f;
                            }
                        }
                        if (s.stT > 0.45f) { s.st = Recover; s.stT = 0.f; s.hit = s.dodged = false; }
                    }
                    else {
                        s.v *= std::pow(0.2f, dt);
                        if (s.stT > 0.9f) { s.st = Approach; s.stT = 0.f; }
                    }
                }
                else {
                    const float want = s.kind == Raider ? 760.f : 320.f;
                    const float radial = (d - want) / want;
                    const float orb = s.kind == Wardog ? 840.f : 320.f;
                    acc = dv / d * radial * 840.f + sf::Vector2f(-dv.y / d, dv.x / d) * (s.orbit * orb);
                    s.a += std::clamp(wrapD(toA - s.a, TAU), -5.f * dt, 5.f * dt);
                    s.fire -= dt;
                    if (s.fire <= 0.f && d < 1200.f) {
                        if (s.kind == Raider) { s.burst = 3; s.fire = 1.7f; }
                        else { s.burst = 1; s.fire = rr(0.8f, 1.3f); }
                    }
                    s.bt -= dt;
                    if (s.burst > 0 && s.bt <= 0.f) {
                        --s.burst; s.bt = 0.12f;
                        const float spd = s.kind == Raider ? 860.f : 960.f;
                        m_bullets.push_back({ s.p, sf::Vector2f(std::cos(s.a), std::sin(s.a)) * spd, 1.6f, false,
                            s.kind == Raider ? 6.f : 3.f, s.kind == Raider ? ui::AMBER_HOT : ui::RED });
                    }
                    if (frand() < 0.003f) s.orbit = -s.orbit;
                }
                s.v += acc * dt;
                if (s.st != Charge) {
                    const float sp = len(s.v), mx = s.kind == Wardog ? 660.f : 520.f;
                    if (sp > mx) s.v *= mx / sp;
                    s.v *= std::pow(0.5f, dt);
                }
                s.p += s.v * dt;
                wrap(s.p);
            }
        }

        void stepBullets(float dt) {
            Hunter& h = m_h;
            for (auto& b : m_bullets) {
                b.p += b.v * dt; b.life -= dt; wrap(b.p);
                if (!b.own) {
                    const sf::Vector2f dv = delta(b.p, h.p);
                    const float d = len(dv);
                    // Parry: batted back as the hunter's round, at speed.
                    if (d < 108.f && h.parryCd <= 0.f && (dv.x * b.v.x + dv.y * b.v.y) < 0.f && frand() < 0.5f) {
                        h.parryCd = 0.7f; h.parryFx = 0.25f;
                        b.own = true; b.v = -b.v * 1.8f; b.c = ui::AMBER; b.life = 0.9f; b.dmg = 12.f;
                        floatText(h.p, "PARRY", ui::AMBER);
                        continue;
                    }
                    if (d < 28.f && h.iframe <= 0.f) { h.hp -= b.dmg; b.life = 0.f; boom(b.p, b.c, 6, 320.f, 0.f); }
                }
                else {
                    for (auto& s : m_ships) {
                        if (b.life <= 0.f) break;
                        if (len(delta(b.p, s.p)) < hitRadius(s.kind)) { s.hp -= b.dmg; s.flash = 0.06f; b.life = 0.f; boom(b.p, ui::CYAN, 4, 240.f, 0.f); }
                    }
                }
                for (auto& k : m_rocks) {
                    if (b.life <= 0.f) break;
                    if (len(delta(b.p, k.p)) < k.r * 0.95f) {
                        b.life = 0.f;
                        if (b.own) { k.hp -= b.dmg; k.flash = 0.05f; }
                        boom(b.p, ui::TEXT_DIM, 4, 180.f, 0.f);
                    }
                }
            }
            m_bullets.erase(std::remove_if(m_bullets.begin(), m_bullets.end(),
                [](const Bullet& b) { return b.life <= 0.f; }), m_bullets.end());
        }

        void resolveDeaths() {
            for (auto it = m_ships.begin(); it != m_ships.end();) {
                if (it->hp > 0.f) { ++it; continue; }
                boom(it->p, ui::AMBER_HOT, 26, 640.f, 92.f);
                boom(it->p, ui::RED, 10, 360.f, 0.f);
                for (int i = 0; i < 4; ++i) m_scrap.push_back({ it->p, { rr(-180.f, 180.f), rr(-180.f, 180.f) }, 6.f });
                ++m_h.kills; ++m_casualties;
                it = m_ships.erase(it);
            }
        }

        void stepRocks(float dt) {
            Hunter& h = m_h;
            std::vector<Rock> born;
            for (auto it = m_rocks.begin(); it != m_rocks.end();) {
                Rock& k = *it;
                k.p += k.v * dt; k.a += k.va * dt; k.flash -= dt; wrap(k.p);
                const sf::Vector2f dv = delta(h.p, k.p);
                const float d = len(dv), m = k.r + 24.f;
                if (d < m && d > 0.f) {
                    const sf::Vector2f n = dv / d;
                    h.p = k.p + n * m; wrap(h.p);
                    const float vn = h.v.x * n.x + h.v.y * n.y;
                    if (vn < 0.f) h.v -= n * (1.6f * vn);
                }
                if (k.hp > 0.f) { ++it; continue; }
                boom(k.p, k.volcanic ? ui::AMBER_HOT : ui::TEXT_DIM, k.size == 2 ? 22 : 12, 400.f, k.volcanic ? 180.f : 0.f);
                if (k.size > 0) {
                    for (int i = 0; i < 2; ++i) {
                        const float a = frand() * TAU;
                        Rock c; c.size = k.size - 1;
                        c.p = k.p + sf::Vector2f(std::cos(a), std::sin(a)) * 20.f;
                        c.v = k.v + sf::Vector2f(std::cos(a), std::sin(a)) * 140.f;
                        born.push_back(c);
                    }
                }
                it = m_rocks.erase(it);
            }
            for (const auto& c : born) addRock(c.size, c.p, c.v);
        }

        void stepScrap(float dt) {
            for (auto& s : m_scrap) {
                const sf::Vector2f dv = delta(m_h.p, s.p);
                const float d = len(dv);
                if (d < 280.f && d > 0.f) s.v += dv / d * (1800.f * dt);
                s.v *= std::pow(0.4f, dt);
                s.p += s.v * dt; s.life -= dt; wrap(s.p);
                if (d < 28.f) s.life = 0.f;
            }
            m_scrap.erase(std::remove_if(m_scrap.begin(), m_scrap.end(),
                [](const Scrap& s) { return s.life <= 0.f; }), m_scrap.end());
        }

        void stepFx(float dt) {
            for (auto& f : m_fx) { f.p += f.v * dt; f.life -= dt; }
            m_fx.erase(std::remove_if(m_fx.begin(), m_fx.end(), [](const Fx& f) { return f.life <= 0.f; }), m_fx.end());
            for (auto& g : m_ghosts) g.life -= dt;
            m_ghosts.erase(std::remove_if(m_ghosts.begin(), m_ghosts.end(), [](const Ghost& g) { return g.life <= 0.f; }), m_ghosts.end());
            for (auto& f : m_floats) { f.life -= dt; f.p.y -= 60.f * dt; }
            m_floats.erase(std::remove_if(m_floats.begin(), m_floats.end(), [](const Float& f) { return f.life <= 0.f; }), m_floats.end());
        }

        void bracket(sf::Vector2f c, float b, sf::Color col) {
            const float L = 8.f, w = 2.f;
            const sf::Vector2f k[4] = { { -b, -b }, { b, -b }, { b, b }, { -b, b } };
            for (int i = 0; i < 4; ++i) {
                const sf::Vector2f p = c + k[i];
                const float sx = k[i].x < 0.f ? 1.f : -1.f, sy = k[i].y < 0.f ? 1.f : -1.f;
                m_b.line(p, p + sf::Vector2f(sx * L, 0.f), w, col);
                m_b.line(p, p + sf::Vector2f(0.f, sy * L), w, col);
            }
        }

        static void label(sf::RenderTarget& t, const sf::Font& f, const std::string& s, sf::Vector2f p,
            unsigned size, sf::Color c, bool centre = false, float spacing = 1.4f) {
            sf::Text tx(f, s, size);
            tx.setLetterSpacing(spacing);
            tx.setFillColor(c);
            if (centre) {
                const sf::FloatRect b = tx.getLocalBounds();
                tx.setOrigin({ b.position.x + b.size.x * 0.5f, 0.f });
            }
            tx.setPosition({ std::round(p.x), std::round(p.y) });
            t.draw(tx);
        }

        void drawBatch(sf::RenderTarget& t, const sf::RenderStates& st) {
            if (m_b.empty()) return;
            m_b.drawWith(t, st);
        }

        // ---- state ----
        const enemyarch::EnemyRegistry* m_reg = nullptr;
        std::uint32_t m_rng = 0x2545F491u;
        float m_time = 0.f;
        int m_feedNo = 0;
        std::string m_call;
        tdraw::HullMesh m_hunterMesh;
        sf::Color m_hunterFill = ui::CYAN, m_hunterEdge = ui::TEXT;
        Hunter m_h;
        std::vector<Ship> m_ships;
        std::vector<Rock> m_rocks;
        std::vector<Bullet> m_bullets;
        std::vector<Fx> m_fx;
        std::vector<Ghost> m_ghosts;
        std::vector<Float> m_floats;
        std::vector<Scrap> m_scrap;
        std::vector<Star> m_stars;
        int m_aim = -1;
        float m_aimDist = 1e9f;
        float m_lost = -1.f, m_spawnT = 0.f;
        sf::Vector2f m_cam;
        long long m_casualties = 0;
        tdraw::Batch m_b;
        std::vector<sf::Vector2f> m_scratch;
    };

} // namespace feed
