/**
 * @file BossSystem.hpp
 * @brief The Dreadnought: arena, guns, attacks, armour, phases and death.
 *
 * ============================================================================
 * WHAT IT RUNS
 * ============================================================================
 *   Any enemy whose archetype has a `boss = { ... }` table is flown from
 *   here. AISystem and TurretSystem skip it (EnemyComponent::isBoss).
 *
 *   IDLE      at its station (the POI centre), turning slowly, guns sweeping.
 *             Shoot it from afar and it comes for you. Close to
 *             `trigger_range` and the fight starts.
 *   AMBUSH    the war horn; a pulse throws everything else out; six TETHER
 *             BUOYS fly from the hull, lock in a hexagon round you and light
 *             up: a high-voltage cage, plasma beams writhing between them.
 *             EntityManager::bossArena goes up -- nothing spawns, nothing
 *             gets in, rounds die on the beams -- and the camera frames it.
 *   FIGHT     he CIRCLES you, and two things run at once:
 *               - the HULL picks attacks at random (volley, wide-curving
 *                 rockets, bombardment, spin storm, shotgun sectors, and in
 *                 phase 2 the turbo-drift ram); nothing goes unpicked more
 *                 than `max_skip` times running. The PROTON LASER closes a
 *                 series of them, with every gun silent: it is about the
 *                 parry, not surviving. The SHOTGUN SECTORS and the
 *                 BOMBARDMENT (a chain of 1-3) are solo too: guns silent,
 *                 and never while his rockets still fly;
 *               - every GUN not held by the hull runs its own loop: bursts,
 *                 and the small shotgun cone.
 *             ARMOUR (80%) until a parried proton laser or a wall ram
 *             breaches it. Phase 1 runs 15% slower (`p1_speed`); PHASE 2
 *             under half HP: full speed, the ram.
 *   DYING     overheat: cracks from the reactor, glow, blasts; then he
 *             bursts into breakable chunks of his own hull (heavy: they barely
 *             drift) and guaranteed loot, and the cage switches off.
 *
 * Draw calls: drawUnder() before the ships (markers, wedges, ram lane,
 * afterimages, the pulse), drawOver() after them (the cage, buoys, shells,
 * cores, sight lines, the laser's path, cracks), drawHud() in screen space
 * (the boss bar, frame A, and the banners).
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
#include "utils/UiPalette.hpp"
#include "utils/TurretModel.hpp"
#include "utils/ShipShatter.hpp"
#include "systems/EnemySystem.hpp"
#include <SFML/Graphics.hpp>
#include <sol/sol.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

class BossSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_lua = ctx.lua;
        m_world = ctx.worldId;
        m_registry = ctx.enemyRegistry;
        m_window = ctx.drawTarget();
        m_playerId = ctx.playerEntityId;
        m_level = ctx.level;
        m_bossArch.clear();
        resetFight();
        m_buoys.clear();
        m_wallBody = b2_nullBodyId;   // the old world is gone with the old level
        m_collapse = -1.f;
        m_bannerT = -1.f;
        m_pending = false;
        m_bolts.clear();
        m_ghosts.clear();
        if (m_em) { m_em->bossArena = EntityManager::BossArena{}; m_em->camLock = false; }
    }
    void setEnemySystem(EnemySystem* e) { m_enemies = e; }
    void setFont(const sf::Font* f) { m_font = f; }

    // ========================================================================
    // UPDATE
    // ========================================================================
    void update(float dt) override {
        if (!m_em || !m_lua || !m_ef || !m_registry) return;
        m_time += dt;
        if (m_bannerT >= 0.f) { m_bannerT += dt; if (m_bannerT > 4.2f) m_bannerT = -1.f; }
        if (m_flashBar > 0.f) m_flashBar -= dt;
        updateFx(dt);

        const size_t pi = m_em->getEntityIndex(m_playerId);
        if (pi == (size_t)-1) return;
        const sf::Vector2f pp = m_em->transforms[pi].position;

        // ---- The burst a dead hull leaves (the frame after it is gone) ----
        if (m_pending && m_em->getEntityIndex(m_bossId) == (size_t)-1) {
            m_pending = false;
            burst();
            m_bossId = 0;
            m_stage = Stage::None;
        }

        // ---- The cage switching off ----
        if (m_collapse >= 0.f) updateCollapse(dt);

        // ---- Every boss hull: adopt it; idle the ones not fighting ----
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (i >= m_em->enemies.size()) break;
            const BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            auto& ec = m_em->enemies[i];
            if (!isBossArch(ec.archetype)) continue;
            if (!ec.isBoss) adopt(i);
            // Enemy headings live on the transform (AISystem's rule); this
            // hull is turned by its body, so read it back every frame.
            m_em->transforms[i].rotation = b2Rot_GetAngle(b2Body_GetRotation(m_em->physics[i].bodyId)) * 57.29578f;
            // The timers AISystem would have run down.
            if (ec.hitFlashTimer > 0.f) ec.hitFlashTimer = std::max(0.f, ec.hitFlashTimer - dt);
            if (m_em->healths[i].stunTimer > 0.f) m_em->healths[i].stunTimer = 0.f;   // a parry does not stun him
            if (m_em->transforms[i].entityId == m_bossId && m_stage != Stage::None) continue;
            idle(i, pp, dt);
            // Woken: close enough -- and nobody else is fighting.
            if (m_stage == Stage::None && m_collapse < 0.f && !m_pending) {
                const float d = len(m_em->transforms[i].position - pp);
                const sol::table bc = bossCfg(ec.archetype);
                if (d < bc["trigger_range"].get_or(1000.f)) begin(i, pp);
            }
        }

        if (m_stage == Stage::None) return;
        const size_t bi = m_em->getEntityIndex(m_bossId);
        if (bi == (size_t)-1) { if (!m_pending) startCollapse(); m_stage = Stage::None; return; }

        m_cfg = bossCfg(m_em->enemies[bi].archetype);
        m_def = &m_registry->resolve(m_em->enemies[bi].archetype);

        switch (m_stage) {
        case Stage::Horn:  updateHorn(dt, bi, pi); break;
        case Stage::Fight: updateFight(dt, bi, pi); break;
        case Stage::Dying: updateDying(dt, bi, pi); break;
        default: break;
        }

        if (m_stage != Stage::None) keepArena(dt);

        // Ghost trail on the bar.
        const size_t bj = m_em->getEntityIndex(m_bossId);
        if (bj != (size_t)-1) {
            const auto& h = m_em->healths[bj];
            const float frac = std::clamp(h.currentHp / std::max(1.f, h.maxHp), 0.f, 1.f);
            if (frac < m_barGhost) {
                m_ghostHold -= dt;
                if (m_ghostHold <= 0.f) m_barGhost = std::max(frac, m_barGhost - dt * 0.35f);
            }
            else { m_barGhost = frac; m_ghostHold = 0.6f; }
            if (frac < m_barLast - 0.0005f) m_ghostHold = 0.6f;
            m_barLast = frac;
        }
        m_barShown = std::min(1.f, m_barShown + dt * 1.6f);
    }

    // ========================================================================
    // DRAW: under the ships
    // ========================================================================
    void drawUnder() {
        if (!m_window) return;
        // ---- Turbo afterimages ----
        if (m_def && !m_def->visualTris.empty()) {
            for (const auto& g : m_ghosts) {
                const float u = std::clamp(g.life / g.maxLife, 0.f, 1.f);
                const sf::Color col = g.ram ? sf::Color(255, 235, 200, static_cast<uint8_t>(110 * u))
                    : sf::Color(255, 110, 50, static_cast<uint8_t>(80 * u));
                const float r = g.rot * 0.0174533f, cs = std::cos(r), sn = std::sin(r);
                sf::VertexArray va(sf::PrimitiveType::Triangles, m_def->visualTris.size());
                for (size_t k = 0; k < m_def->visualTris.size(); ++k) {
                    const sf::Vector2f l = m_def->visualTris[k];
                    va[k] = sf::Vertex{ g.pos + sf::Vector2f(l.x * cs - l.y * sn, l.x * sn + l.y * cs), col };
                }
                m_window->draw(va);
            }
        }
        if (m_stage == Stage::None) return;
        const float tt = m_time;
        // ---- The pulse ----
        if (m_stage == Stage::Horn && m_ringR > 0.f) {
            const float a = 1.f - std::clamp(m_ringR / (m_R + 300.f), 0.f, 1.f);
            ring(m_home, m_ringR, 10.f, sf::Color(255, 90, 60, static_cast<uint8_t>(60 + 150 * a)));
            ring(m_home, m_ringR * 0.92f, 3.f, sf::Color(255, 190, 150, static_cast<uint8_t>(90 * a)));
        }
        // ---- Shotgun sectors (the hull) ----
        for (const auto& s : m_sectors) {
            if (s.flash > 0.f) { wedgeFill(s.from, s.ang, s.half, s.reach, sf::Color(255, 220, 160, static_cast<uint8_t>(130 * s.flash / 0.3f))); continue; }
            if (s.fired) continue;
            const float u = std::clamp(1.f - (s.at - m_atkT) / std::max(0.05f, s.tell), 0.f, 1.f);
            coneTell(s.from, s.ang, s.half, s.reach, u, u > 0.85f);
        }
        // ---- Single-gun shotgun blasts: the Barge's cone ----
        const size_t bi = m_em ? m_em->getEntityIndex(m_bossId) : (size_t)-1;
        if (bi != (size_t)-1) {
            for (int g = 0; g < 3; ++g) {
                const Gun& gn = m_gun[g];
                if (gn.act != GunAct::Blast) continue;
                const float tell = gunTell("blast_tell", 0.45f);
                const sf::Vector2f o = muzzle(bi, g);
                const float a = (m_em->enemies[bi].mountAngle[g] - 90.f) * 0.0174533f;
                const float half = c("blast_half", 20.f) * 0.0174533f, reach = c("blast_range", 700.f);
                if (gn.fired) wedgeFill(o, a, half, reach, sf::Color(255, 220, 160, static_cast<uint8_t>(120 * std::max(0.f, 1.f - gn.t / 0.25f))));
                else coneTell(o, a, half, reach, gn.t / tell, gn.t > tell * 0.7f);
            }
        }
        // ---- Bombardment markers: the Barge's / Bloodseeker's area tell ----
        for (const auto& m : m_markers) {
            if (m.done) {
                if (m.flash > 0.f) disc(m.pos, m.r * (1.f + (0.35f - m.flash)), sf::Color(255, 210, 140, static_cast<uint8_t>(400 * m.flash)));
                continue;
            }
            if (m.t < 0.f) continue;
            const float u = std::clamp(m.t / m.T, 0.f, 1.f);
            circleTell(m.pos, m.r, u, u > 0.8f);
        }
        // ---- Ram lane ----
        if (bi != (size_t)-1 && m_atk == Atk::Drift && (m_driftStep == 2 || m_driftStep == 3)) {
            const float u = m_driftStep == 3 ? 1.f : std::clamp(m_stepT / std::max(0.05f, m_ramWindup), 0.f, 1.f);
            const sf::Vector2f p0 = m_driftStep == 3 ? m_ramFrom : m_em->transforms[bi].position;
            const float reach = wallReach(p0, m_ramDir, 2.f * m_R);
            const float w = hullR() * 0.8f;
            const sf::Vector2f n(-m_ramDir.y, m_ramDir.x);
            std::vector<sf::Vector2f> path = { p0 + n * w, p0 + n * w + m_ramDir * reach, p0 - n * w + m_ramDir * reach, p0 - n * w, p0 + n * w };
            quad(path[0], path[1], path[2], path[3], sf::Color(255, 60, 40, static_cast<uint8_t>(10 + 30 * u)));
            ants(path, u > 0.8f ? 3.f : 1.6f + u, sf::Color(255, 80, 55, static_cast<uint8_t>(110 + 140 * u)), u > 0.8f ? 260.f : 80.f + 120.f * u);
            for (float s = 0.3f; s < 0.95f; s += 0.3f)
                warnSign(p0 + m_ramDir * (reach * s), 13.f, u, u > 0.8f);
        }
        (void)tt;
    }

    // ========================================================================
    // DRAW: over the ships
    // ========================================================================
    void drawOver() {
        if (!m_window) return;
        // The cage (also while it switches off).
        drawCage();
        const size_t bi = m_em ? m_em->getEntityIndex(m_bossId) : (size_t)-1;
        // Reactor cores on every boss hull (idle ones too).
        for (size_t i = 0; m_em && i < m_em->enemies.size() && i < m_em->transforms.size(); ++i) {
            if (!m_em->enemies[i].isBoss) continue;
            drawCore(i, i == bi);
        }
        if (bi == (size_t)-1 || m_stage == Stage::None) return;
        const auto& tf = m_em->transforms[bi];

        // ---- Proton laser: the charge on the bow gun, and its path ----
        if (m_atk == Atk::Proton && m_step < 2) {
            const float u = m_step == 1 ? 1.f : std::clamp(m_atkT / std::max(0.05f, m_protonCharge), 0.f, 1.f);
            const sf::Vector2f mz = muzzle(bi, 0);
            const float flick = m_step == 1 ? 1.f : 0.6f + 0.4f * std::sin(m_time * 47.f);
            for (size_t k = 1; k < m_laserPath.size(); ++k) {
                line(m_laserPath[k - 1], m_laserPath[k], m_step == 1 ? 16.f : 6.f + 8.f * u,
                    sf::Color(60, 255, 140, static_cast<uint8_t>((m_step == 1 ? 70 : 20 + 40 * u) * flick)));
                line(m_laserPath[k - 1], m_laserPath[k], m_step == 1 ? 5.f : 2.f + 2.5f * u,
                    sf::Color(150, 255, 190, static_cast<uint8_t>((90 + 160 * u) * flick)));
            }
            for (size_t k = 1; k + 1 < m_laserPath.size(); ++k) disc(m_laserPath[k], 5.f + 3.f * u, sf::Color(150, 255, 190, static_cast<uint8_t>(160 * u)));
            const float r = 4.f + 12.f * u + (m_step == 1 ? 2.f * std::sin(m_time * 40.f) : 0.f);
            disc(mz, r * 1.9f, sf::Color(70, 255, 150, static_cast<uint8_t>(60 * u)));
            disc(mz, r, sf::Color(150, 255, 190, static_cast<uint8_t>(160 + 90 * u)));
            disc(mz, r * 0.45f, sf::Color(240, 255, 245, 255));
        }

        // ---- Rocket pods: hot before the volley ----
        if (m_podGlow > 0.f) {
            for (int s = 0; s < 2; ++s) for (int t = 0; t < 2; ++t) {
                const sf::Vector2f p = podMuzzle(bi, s, t);
                disc(p, 4.f + 5.f * m_podGlow, sf::Color(255, 120, 60, static_cast<uint8_t>(110 + 140 * m_podGlow)));
            }
        }

        // ---- Bombardment shells in flight: lobbed, so they swell and shrink ----
        for (const auto& m : m_markers) {
            if (m.done || m.t < 0.f) continue;
            const float u = std::clamp(m.t / m.T, 0.f, 1.f);
            const float lift = std::sin(u * 3.14159f);
            const sf::Vector2f p = m.from + (m.pos - m.from) * u;
            disc(p + sf::Vector2f(6.f, 10.f) * lift, 4.f, sf::Color(0, 0, 0, 70));   // shadow
            for (int k = 3; k >= 1; --k) {
                const float uk = std::max(0.f, u - k * 0.035f);
                const sf::Vector2f pk = m.from + (m.pos - m.from) * uk;
                disc(pk, (3.f + 5.f * std::sin(uk * 3.14159f)) * (1.f - k * 0.2f), sf::Color(255, 140, 60, static_cast<uint8_t>(110 - k * 25)));
            }
            disc(p, 5.f + 7.f * lift, sf::Color(255, 150, 70, 220));
            disc(p, 2.5f + 3.f * lift, sf::Color(255, 240, 210, 255));
        }

        // ---- Turbo drive: the stern burns; the ram's prow goes white ----
        if (m_atk == Atk::Drift && m_driftStep >= 1 && m_driftStep <= 3) {
            const float sc = m_def->config["scale"].get_or(2.f);
            const sf::Vector2f stern = localToWorld(bi, { 0.f, 40.f * sc });
            const float f = 0.7f + 0.3f * std::sin(m_time * 60.f);
            disc(stern, 26.f * f, sf::Color(255, 120, 40, 90));
            disc(stern, 13.f * f, sf::Color(255, 210, 140, 200));
            if (m_driftStep >= 2) {
                const float u = m_driftStep == 3 ? 1.f : std::clamp(m_stepT / std::max(0.05f, m_ramWindup), 0.f, 1.f);
                const sf::Vector2f prow = localToWorld(bi, { 0.f, -58.f * sc });
                disc(prow, 18.f + 26.f * u, sf::Color(255, 220, 170, static_cast<uint8_t>(60 + 120 * u)));
                disc(prow, 8.f + 10.f * u, sf::Color(255, 255, 240, static_cast<uint8_t>(150 + 100 * u)));
            }
        }

        // ---- Death: the cracks open, the hull goes white-hot ----
        if (m_stage == Stage::Dying) drawCracks(bi);
        (void)tf;
    }

    // ========================================================================
    // DRAW: the boss bar (frame A) and the banners -- screen space
    // ========================================================================
    void drawHud(sf::Vector2f ws) {
        if (!m_window) return;
        if (m_bannerT >= 0.f && m_font) {
            const float a = std::clamp(std::min(m_bannerT / 0.25f, (4.2f - m_bannerT) / 0.6f), 0.f, 1.f);
            const unsigned s = static_cast<unsigned>(std::round(ws.y / 1080.f * 22.f));
            const float y = ws.y * 0.31f;
            ui::label(*m_window, m_font, ws.x * 0.5f - ui::labelWidth(m_font, m_banner, s) * 0.5f, y, m_banner, s,
                sf::Color(m_bannerCol.r, m_bannerCol.g, m_bannerCol.b, static_cast<uint8_t>(255 * a)));
            if (!m_bannerSub.empty()) {
                const unsigned s2 = static_cast<unsigned>(std::round(ws.y / 1080.f * 13.f));
                ui::label(*m_window, m_font, ws.x * 0.5f - ui::labelWidth(m_font, m_bannerSub, s2) * 0.5f, y + s * 1.6f,
                    m_bannerSub, s2, sf::Color(220, 205, 200, static_cast<uint8_t>(220 * a)));
            }
        }
        if (m_stage == Stage::None || !m_font) return;
        const size_t bi = m_em->getEntityIndex(m_bossId);
        if (bi == (size_t)-1) return;
        drawBar(ws, bi);
    }

    /// For the dev harness and the HUD: is a boss fight on.
    bool fighting() const { return m_stage != Stage::None; }

    /// DEV: the hull's next pick ("proton", "drift", ...), or a gun's next
    /// action ("burst", "blast").
    void devForce(const std::string& name) {
        for (int a = 1; a < ATK_N; ++a) if (name == kAtkNames[a]) { m_forced = a; m_gapT = std::min(m_gapT, 0.2f); }
        if (name == "burst") m_forcedGun = 1;
        if (name == "blast") m_forcedGun = 4;
    }
    /// DEV: one line on the fight.
    std::string devInfo() const {
        char b[300];
        const size_t bi = m_em ? m_em->getEntityIndex(m_bossId) : (size_t)-1;
        std::snprintf(b, sizeof b, "stage %d atk %s/%d t%.2f/%.2f guns %d%d%d hp %.0f/%.0f armour %.2f breach %.1f stun %.1f p2 %d walled %d markers %zu arena %d sinceP %d",
            static_cast<int>(m_stage), kAtkNames[static_cast<int>(m_atk)], m_driftStep, m_atkT, m_stepT,
            static_cast<int>(m_gun[0].act), static_cast<int>(m_gun[1].act), static_cast<int>(m_gun[2].act),
            bi != (size_t)-1 ? m_em->healths[bi].currentHp : -1.f, bi != (size_t)-1 ? m_em->healths[bi].maxHp : -1.f,
            bi != (size_t)-1 ? m_em->healths[bi].armour : -1.f, m_breach, m_stun, m_phase2 ? 1 : 0,
            m_em && m_em->bossArena.walled ? 1 : 0, m_markers.size(), m_em && m_em->bossArena.active ? 1 : 0, m_sinceProton);
        return b;
    }

private:
    // ========================================================================
    // TYPES
    // ========================================================================
    enum class Stage { None, Horn, Fight, Dying };
    /// The HULL's attacks. The guns' own loop is GunAct.
    enum class Atk { None = 0, Volley, Rockets, Bombard, Spin, Sectors, Proton, Drift, Count };
    static constexpr int ATK_N = static_cast<int>(Atk::Count);
    static constexpr const char* kAtkNames[ATK_N] = { "none", "volley", "rockets", "bombard", "spin", "sectors", "proton", "drift" };
    enum class GunAct { Idle = 0, Burst = 1, Blast = 4 };

    struct Gun {
        GunAct act = GunAct::Idle;
        float t = 0.f;          ///< time in the current action / phase
        float cd = 1.f;         ///< until it may start another
        int   n = 0;            ///< rounds / shots done
        bool  fired = false;
    };
    struct Marker {
        sf::Vector2f pos, from;
        float r = 105.f;
        float t = 0.f;          ///< counts up; < 0 = the shell is not fired yet
        float T = 1.4f;         ///< impact at t == T
        int   gun = 0;
        bool  done = false;
        float flash = 0.f;
    };
    struct Sector {
        sf::Vector2f from;
        float ang = 0.f, half = 0.4f, reach = 3000.f;   ///< ang: radians, screen frame
        float at = 0.f, tell = 0.9f;
        bool  fired = false;
        float flash = 0.f;
    };
    struct Buoy {
        sf::Vector2f from, to, pos;
        float launchAt = 0.f, flight = 0.6f;
        bool  launched = false, locked = false;
        float lockFlash = 0.f;
        float ignite = 0.f;     ///< 0..1: the beam to the next buoy
        float spin = 0.f;
    };
    struct Bolt { std::vector<sf::Vector2f> pts; float life = 0.1f, maxLife = 0.1f; };
    struct Ghost { sf::Vector2f pos; float rot = 0.f, life = 0.2f, maxLife = 0.2f; bool ram = false; };
    struct Crack { std::vector<sf::Vector2f> pts; float at = 0.f; };

    // ========================================================================
    // HELPERS
    // ========================================================================
    static float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }
    static sf::Vector2f norm(sf::Vector2f v) { const float l = len(v); return l > 1e-4f ? v / l : sf::Vector2f(0.f, -1.f); }
    /// Game heading (deg, 0 = up, clockwise) <-> direction.
    static sf::Vector2f dirOf(float deg) { const float r = deg * 0.0174533f; return { std::sin(r), -std::cos(r) }; }
    static float headingOf(sf::Vector2f d) { return std::atan2(d.x, -d.y) * 57.29578f; }
    static float wrap(float a) { while (a > 180.f) a -= 360.f; while (a < -180.f) a += 360.f; return a; }
    static float frand() { return (rand() % 10000) / 10000.f; }
    static float frange(float a, float b) { return a + (b - a) * frand(); }

    float c(const char* k, float d) const { return m_cfg.valid() ? m_cfg[k].get_or(d) : d; }
    float cp(const char* k, const char* k2, float d) const { return m_phase2 ? c(k2, c(k, d)) : c(k, d); }
    /// Phase 2 runs every tell and interval faster.
    float sp() const { return std::max(0.2f, m_phase2 ? c("p2_speed", 1.f) : c("p1_speed", 0.85f)); }
    float gunTell(const char* k, float d) const { return c(k, d) / sp(); }
    std::pair<float, float> range2(const char* k, float a, float b) const {
        sol::optional<sol::table> t = m_cfg.valid() ? m_cfg[k].get<sol::optional<sol::table>>() : sol::optional<sol::table>();
        if (t) { a = (*t)[1].get_or(a); b = std::max(a, (*t)[2].get_or(b)); }
        return { a, b };
    }

    bool isBossArch(uint8_t a) {
        if (a >= m_bossArch.size()) m_bossArch.resize(static_cast<size_t>(a) + 1, -1);
        if (m_bossArch[a] < 0) {
            const auto& d = m_registry->resolve(a);
            sol::optional<sol::table> b = d.config["boss"];
            m_bossArch[a] = b ? 1 : 0;
        }
        return m_bossArch[a] == 1;
    }
    sol::table bossCfg(uint8_t a) const {
        const auto& d = m_registry->resolve(a);
        sol::optional<sol::table> b = d.config["boss"];
        return b ? *b : sol::table();
    }

    sf::Vector2f localToWorld(size_t i, sf::Vector2f l) const {
        const auto& tf = m_em->transforms[i];
        const float r = tf.rotation * 0.0174533f, cs = std::cos(r), sn = std::sin(r);
        return tf.position + sf::Vector2f(l.x * cs - l.y * sn, l.x * sn + l.y * cs);
    }
    sf::Vector2f mount(size_t i, int g) const {
        const auto& d = m_registry->resolve(m_em->enemies[i].archetype);
        if (g < 0 || static_cast<size_t>(g) >= d.turrets.size()) return m_em->transforms[i].position;
        return localToWorld(i, d.turrets[static_cast<size_t>(g)]);
    }
    int guns(size_t i) const {
        return static_cast<int>(std::min<size_t>(3, m_registry->resolve(m_em->enemies[i].archetype).turrets.size()));
    }
    float gunSize(size_t i) const { return m_registry->resolve(m_em->enemies[i].archetype).config["turret_size"].get_or(16.f); }
    sf::Vector2f muzzle(size_t i, int g) const {
        return mount(i, g) + dirOf(m_em->enemies[i].mountAngle[std::clamp(g, 0, 3)]) * (turretmodel::kMuzzle * gunSize(i));
    }
    /// Pod tubes along the wing plates: side 0 starboard, 1 port; tube 0 inner, 1 outer.
    sf::Vector2f podMuzzle(size_t i, int side, int tube) const {
        const float sc = m_registry->resolve(m_em->enemies[i].archetype).config["scale"].get_or(2.f);
        sf::Vector2f l = tube == 0 ? sf::Vector2f(26.9f, 0.4f) : sf::Vector2f(31.3f, -1.0f);
        if (side == 1) l.x = -l.x;
        return localToWorld(i, l * sc);
    }
    float hullR() const { return m_def ? m_def->radius : 120.f; }

    /// How far from p along d before the cage (or `cap` when there is none).
    float wallReach(sf::Vector2f p, sf::Vector2f d, float cap) const {
        const auto& A = m_em->bossArena;
        if (!A.active) return cap;
        float t; sf::Vector2f n;
        if (A.rayHit(p, d, t, n)) return std::min(cap, t);
        return cap;
    }
    /// Keep a point this far inside the cage.
    sf::Vector2f clampInside(sf::Vector2f p, float margin) const {
        const auto& A = m_em->bossArena;
        for (int it = 0; it < 3; ++it) {
            const float d = A.wallDist(p);
            if (d >= margin) break;
            p += norm(m_home - p) * (margin - d + 2.f);
        }
        return p;
    }

    // ---- Drawing primitives ----
    void disc(sf::Vector2f c0, float r, sf::Color col) {
        if (r <= 0.5f) return;
        sf::CircleShape s(r, r > 60.f ? 48 : 20);
        s.setOrigin({ r, r }); s.setPosition(c0); s.setFillColor(col);
        m_window->draw(s);
    }
    void ring(sf::Vector2f c0, float r, float w, sf::Color col) {
        if (r <= 1.f) return;
        sf::CircleShape s(r, r > 400.f ? 160 : 48);
        s.setOrigin({ r, r }); s.setPosition(c0); s.setFillColor(sf::Color::Transparent);
        s.setOutlineThickness(w); s.setOutlineColor(col);
        m_window->draw(s);
    }
    void line(sf::Vector2f a, sf::Vector2f b, float w, sf::Color col) {
        const sf::Vector2f d = norm(b - a), n(-d.y * w * 0.5f, d.x * w * 0.5f);
        quad(a + n, b + n, b - n, a - n, col);
    }
    void quad(sf::Vector2f a, sf::Vector2f b, sf::Vector2f c0, sf::Vector2f d, sf::Color col) {
        sf::VertexArray va(sf::PrimitiveType::TriangleFan, 4);
        va[0] = sf::Vertex{ a, col }; va[1] = sf::Vertex{ b, col }; va[2] = sf::Vertex{ c0, col }; va[3] = sf::Vertex{ d, col };
        m_window->draw(va);
    }
    /// A thick polyline into a triangle batch (one draw call for the lot).
    static void strip(sf::VertexArray& va, const std::vector<sf::Vector2f>& pts, float w, sf::Color col) {
        for (size_t k = 1; k < pts.size(); ++k) {
            const sf::Vector2f a = pts[k - 1], b = pts[k];
            const sf::Vector2f d = norm(b - a), n(-d.y * w * 0.5f, d.x * w * 0.5f);
            // a little past each joint so the bends do not crack open
            const sf::Vector2f e = d * (w * 0.35f);
            va.append(sf::Vertex{ a - e + n, col }); va.append(sf::Vertex{ b + e + n, col }); va.append(sf::Vertex{ b + e - n, col });
            va.append(sf::Vertex{ a - e + n, col }); va.append(sf::Vertex{ b + e - n, col }); va.append(sf::Vertex{ a - e - n, col });
        }
    }
    /// A wedge from o (bearing `ang` radians, screen frame), cut at the cage.
    std::vector<sf::Vector2f> wedgePts(sf::Vector2f o, float ang, float half, float reach) const {
        std::vector<sf::Vector2f> pts;
        pts.push_back(o);
        const int n = 18;
        for (int k = 0; k <= n; ++k) {
            const float a = ang - half + 2.f * half * k / n;
            const sf::Vector2f d(std::cos(a), std::sin(a));
            pts.push_back(o + d * wallReach(o, d, reach));
        }
        pts.push_back(o);
        return pts;
    }
    void wedgeFill(sf::Vector2f o, float ang, float half, float reach, sf::Color col) {
        const auto pts = wedgePts(o, ang, half, reach);
        sf::VertexArray va(sf::PrimitiveType::TriangleFan, pts.size() - 1);
        for (size_t k = 0; k + 1 < pts.size(); ++k) va[k] = sf::Vertex{ pts[k], col };
        m_window->draw(va);
    }
    /// Marching ants along a path (the game's area-tell language).
    void ants(const std::vector<sf::Vector2f>& path, float w, sf::Color col, float speed) {
        const float dash = 13.f, gap = 9.f, period = dash + gap;
        const float off = std::fmod(m_time * speed, period);
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        float s0 = -off, walked = 0.f;
        for (size_t k = 0; k + 1 < path.size(); ++k) {
            const sf::Vector2f p0 = path[k], p1 = path[k + 1];
            const float L = len(p1 - p0);
            if (L < 1e-3f) continue;
            const sf::Vector2f n = (p1 - p0) / L;
            for (float m = std::floor((walked - s0 - dash) / period); ; m += 1.f) {
                const float a0 = s0 + m * period, a1 = a0 + dash;
                if (a0 > walked + L) break;
                const float c0 = std::max(a0, walked) - walked, c1 = std::min(a1, walked + L) - walked;
                if (c1 > c0) strip(va, { p0 + n * c0, p0 + n * c1 }, w, col);
            }
            walked += L;
        }
        m_window->draw(va);
    }
    /// The warning triangle with its "!".
    void warnSign(sf::Vector2f c0, float sz, float charge, bool locked) {
        const float pulse = 0.5f + 0.5f * std::sin(m_time * (5.f + 12.f * charge));
        const auto tri = [&](float s) {
            return std::array<sf::Vector2f, 3>{ c0 + sf::Vector2f(0.f, -s), c0 + sf::Vector2f(s * 0.95f, s * 0.70f), c0 + sf::Vector2f(-s * 0.95f, s * 0.70f) };
            };
        const auto g = tri(sz * 1.45f);
        const sf::Color gc(255, 120, 80, static_cast<uint8_t>((locked ? 150.f : 50.f + 90.f * charge) * (0.4f + 0.6f * pulse)));
        for (int k = 0; k < 3; ++k) line(g[k], g[(k + 1) % 3], 1.6f, gc);
        const auto t = tri(sz);
        sf::VertexArray body(sf::PrimitiveType::Triangles, 3);
        for (int k = 0; k < 3; ++k) body[k] = sf::Vertex{ t[k], sf::Color(28, 8, 8, 210) };
        m_window->draw(body);
        const sf::Color ec(255, static_cast<uint8_t>(110 + 60 * pulse), 70, static_cast<uint8_t>(locked ? 255.f : 150.f + 100.f * charge));
        for (int k = 0; k < 3; ++k) line(t[k], t[(k + 1) % 3], 2.f, ec);
        line(c0 + sf::Vector2f(0.f, -sz * 0.52f), c0 + sf::Vector2f(0.f, sz * 0.18f), 2.4f, ec);
        line(c0 + sf::Vector2f(0.f, sz * 0.32f), c0 + sf::Vector2f(0.f, sz * 0.48f), 2.4f, ec);
    }
    void coneTell(sf::Vector2f o, float ang, float half, float reach, float charge, bool locked) {
        charge = std::clamp(charge, 0.f, 1.f);
        wedgeFill(o, ang, half, reach, sf::Color(255, 80, 55, static_cast<uint8_t>(locked ? 30.f : 8.f + 14.f * charge)));
        ants(wedgePts(o, ang, half, reach), locked ? 2.6f : 1.5f + 0.7f * charge,
            sf::Color(255, 80, 55, static_cast<uint8_t>(locked ? 240.f : 90.f + 130.f * charge)), locked ? 160.f : 45.f + 60.f * charge);
        const sf::Vector2f d(std::cos(ang), std::sin(ang));
        warnSign(o + d * std::min(260.f, wallReach(o, d, reach) * 0.42f), 12.f, charge, locked);
    }
    void circleTell(sf::Vector2f c0, float r, float charge, bool locked) {
        disc(c0, r, sf::Color(255, 80, 55, static_cast<uint8_t>(locked ? 40.f : 10.f + 22.f * charge)));
        disc(c0, r * charge, sf::Color(255, 90, 55, static_cast<uint8_t>(30.f + 40.f * charge)));
        std::vector<sf::Vector2f> path;
        for (int k = 0; k <= 32; ++k) { const float a = k * 6.2831853f / 32.f; path.push_back(c0 + sf::Vector2f(std::cos(a), std::sin(a)) * r); }
        ants(path, locked ? 2.8f : 1.5f + 0.8f * charge, sf::Color(255, 80, 55, static_cast<uint8_t>(locked ? 245.f : 100.f + 130.f * charge)),
            locked ? 170.f : 45.f + 70.f * charge);
        warnSign(c0, 13.f, charge, locked);
    }

    // ========================================================================
    // ADOPT / IDLE
    // ========================================================================
    void adopt(size_t i) {
        auto& ec = m_em->enemies[i];
        auto& h = m_em->healths[i];
        ec.isBoss = true;
        ec.perMount = true;
        ec.hideHpBar = true;
        ec.visualState = EnemyState::PATROL;
        if (ec.guardHome.x == 0.f && ec.guardHome.y == 0.f) ec.guardHome = m_em->transforms[i].position;
        h.holdDeath = true;
        h.armour = bossCfg(ec.archetype)["armour"].get_or(0.8f);
        h.armourMark = h.currentHp;
        for (int g = 0; g < 4; ++g) ec.mountAngle[g] = m_em->transforms[i].rotation + (g == 0 ? 0.f : (g == 1 ? -60.f : 60.f));
    }

    /// At its station: a slow turn, the guns sweeping. Shot at: it comes.
    void idle(size_t i, sf::Vector2f pp, float dt) {
        auto& ec = m_em->enemies[i];
        const b2BodyId b = m_em->physics[i].bodyId;
        if (!b2Body_IsValid(b)) return;
        const sf::Vector2f p = m_em->transforms[i].position;
        const bool hunting = ec.attackedByPlayer || ec.playerHitTimer > 0.f;
        const sf::Vector2f target = hunting ? pp : ec.guardHome;
        const sf::Vector2f to = target - p;
        const float cap = hunting ? 320.f : 60.f;
        const sf::Vector2f v = len(to) > 30.f ? norm(to) * std::min(cap, len(to)) : sf::Vector2f(0.f, 0.f);
        b2Body_SetLinearVelocity(b, { v.x / SCALE, v.y / SCALE });
        if (hunting) {
            const float diff = wrap(headingOf(pp - p) - m_em->transforms[i].rotation);
            b2Body_SetAngularVelocity(b, std::clamp(diff * 2.f, -60.f, 60.f) * 0.0174533f);
        }
        else b2Body_SetAngularVelocity(b, 0.06f);
        for (int g = 0; g < 3; ++g) {
            ec.mountAngle[g] = m_em->transforms[i].rotation + (g == 0 ? 0.f : (g == 1 ? -1.f : 1.f)) * 70.f
                + std::sin(m_time * 0.5f + g * 2.1f) * 50.f;
            ec.mountHeat[g] = std::max(0.f, ec.mountHeat[g] - dt * 2.f);
        }
    }

    // ========================================================================
    // THE AMBUSH: the horn, the buoys, the cage
    // ========================================================================
    void resetFight() {
        m_stage = Stage::None;
        m_bossId = 0;
        m_atk = Atk::None;
        m_atkT = 0.f;
        m_gapT = 1.0f;
        m_phase2 = false;
        m_breach = 0.f;
        m_stun = 0.f;
        m_ringR = 0.f;
        m_markers.clear();
        m_sectors.clear();
        m_cracks.clear();
        m_skip.fill(0);
        for (auto& g : m_gun) g = Gun{};
        m_barShown = 0.f;
        m_barGhost = 1.f;
        m_barLast = 1.f;
        m_podGlow = 0.f;
        m_laserPath.clear();
        m_lastProton = 0;
        m_driftStep = 0;
        m_igniteT = -1.f;
        m_sinceProton = 0;
        m_bombChain = 0;
    }

    void begin(size_t i, sf::Vector2f pp) {
        resetFight();
        m_bossId = m_em->transforms[i].entityId;
        m_stage = Stage::Horn;
        m_stageT = 0.f;
        auto& ec = m_em->enemies[i];
        m_cfg = bossCfg(ec.archetype);
        m_def = &m_registry->resolve(ec.archetype);
        m_R = c("arena_radius", 800.f);
        const sf::Vector2f bp = m_em->transforms[i].position;

        // The cage must hold you both: its centre is his station, unless
        // that leaves one of you near the beams -- then halfway between you.
        auto& A = m_em->bossArena;
        A = EntityManager::BossArena{};
        A.radius = m_R;
        A.sides = 6;
        A.rot = 0.f;   // flat top and bottom: the hexagon fills a wide screen best
        A.centre = ec.guardHome;
        if (A.wallDist(pp) < 180.f || A.wallDist(bp) < hullR() + 120.f) A.centre = (pp + bp) * 0.5f;
        m_home = A.centre;
        A.active = true;
        A.walled = false;

        ec.visualState = EnemyState::COMBAT;
        ec.attackedByPlayer = true;
        m_lastProton = ec.protonHits;
        m_name = m_cfg["bar_name"].get_or(std::string("DREADNOUGHT"));
        m_orbitAng = std::atan2(bp.y - pp.y, bp.x - pp.x);
        m_orbitDir = (rand() % 2) ? 1.f : -1.f;
        m_orbitFlip = frange(2.5f, 5.f);

        // The buoys: one per corner, fired off the hull in turn.
        m_buoys.clear();
        const float stag = c("buoy_stagger", 0.07f), flight = c("buoy_flight", 0.6f);
        for (int k = 0; k < A.sides; ++k) {
            Buoy b;
            b.to = A.corner(k);
            b.launchAt = 0.25f + k * stag;
            b.flight = flight;
            b.spin = frange(-12.f, 12.f);
            m_buoys.push_back(b);
        }

        m_em->camLock = true;
        m_em->camLockCentre = m_home + sf::Vector2f(0.f, c("view_shift", 60.f));   // the boss bar sits at the bottom
        const float margin = c("view_margin", 170.f);
        m_em->camLockSize = { 2.f * (m_R + margin), 2.f * (A.apothem() + margin) };

        m_em->sfxUi("boss.horn");
        m_em->addTrauma(0.6f);
        m_em->spawnScreenFlash(sf::Color(255, 60, 40), 0.5f, 80.f);
        banner("AMBUSH", "A RAKSHARI DREADNOUGHT HAS CAGED YOU", sf::Color(255, 90, 70));
    }

    /// The horn: the pulse runs out, the buoys fly and lock, the beams light.
    void updateHorn(float dt, size_t bi, size_t pi) {
        m_stageT += dt;
        const float T = c("horn_time", 2.0f);
        const float runT = 0.7f;
        const float prevR = m_ringR;
        m_ringR = std::min(m_R + 300.f, (m_stageT / runT) * (m_R + 300.f));
        const float ringSpeed = (m_R + 300.f) / runT;
        // Everything that is not you or him goes out ahead of the pulse.
        if (m_ringR < m_R + 299.f) {
            for (size_t j = 0; j < m_em->physics.size(); ++j) {
                if (j == bi || j == pi) continue;
                const b2BodyId b = m_em->physics[j].bodyId;
                if (!b2Body_IsValid(b)) continue;
                const BodyUserData* ud = bodyUD(b);
                if (!ud || b2Body_GetType(b) != b2_dynamicBody) continue;
                const sf::Vector2f o = m_em->transforms[j].position - m_home;
                const float d = len(o);
                if (d > m_ringR + 60.f || d < prevR - 400.f) continue;
                if (ud->type == BodyType::Bullet) { m_em->bullets[j].markedForDestroy = true; continue; }
                const sf::Vector2f v = norm(o) * (ringSpeed * 1.25f);
                b2Body_SetLinearVelocity(b, { v.x / SCALE, v.y / SCALE });
            }
        }
        const sf::Vector2f pp = m_em->transforms[pi].position;
        orbitMove(bi, pp, dt, 0.3f);
        aimGuns(bi, pp, dt, c("gun_traverse", 200.f));

        // ---- The buoys ----
        const sf::Vector2f bp = m_em->transforms[bi].position;
        bool allLocked = true;
        for (size_t k = 0; k < m_buoys.size(); ++k) {
            Buoy& b = m_buoys[k];
            if (!b.launched && m_stageT >= b.launchAt) {
                b.launched = true;
                b.from = bp + norm(b.to - bp) * hullR() * 0.6f;
                b.pos = b.from;
                m_em->sfx("maniac.rocket.launch", b.from, 0.7f, 0.8f);
                m_em->spawnShockRing(b.from, 4.f, 40.f, 0.2f, sf::Color(255, 140, 70), 3.f, 230.f);
            }
            if (b.launched && !b.locked) {
                const float u = std::clamp((m_stageT - b.launchAt) / b.flight, 0.f, 1.f);
                const float e = 1.f - (1.f - u) * (1.f - u) * (1.f - u);
                b.pos = b.from + (b.to - b.from) * e;
                if ((rand() % 100) < 70)
                    m_em->particles.push_back({ b.pos, sf::Vector2f(frange(-30.f, 30.f), frange(-30.f, 30.f)),
                        sf::Color(255, 150, 80, 200), 0.3f, 0.3f, 3.f });
                if (u >= 1.f) {
                    b.locked = true;
                    b.lockFlash = 0.3f;
                    m_em->spawnShockRing(b.to, 6.f, 70.f, 0.25f, sf::Color(255, 160, 80), 4.f, 240.f);
                    m_em->spawnExplosion(b.to, sf::Color(255, 180, 110), 10, 2.5f);
                    m_em->sfx("boss.bomb.mark", b.to, 0.5f, 0.7f);
                }
            }
            if (!b.locked) allLocked = false;
        }
        // ---- The beams light, corner to corner, round the ring ----
        if (allLocked) {
            if (m_igniteT < 0.f) { m_igniteT = 0.f; m_em->sfxUi("boss.wall.rise"); }
            m_igniteT += dt;
            const float each = c("beam_ignite", 0.1f);
            bool lit = true;
            for (size_t k = 0; k < m_buoys.size(); ++k) {
                m_buoys[k].ignite = std::clamp((m_igniteT - k * each * 0.7f) / each, 0.f, 1.f);
                if (m_buoys[k].ignite < 1.f) lit = false;
            }
            if (lit && !m_em->bossArena.walled) raiseWall(pi);
        }
        if (m_stageT >= T && m_em->bossArena.walled) {
            m_stage = Stage::Fight;
            m_gapT = 0.4f;
            for (int g = 0; g < 3; ++g) m_gun[g].cd = 0.3f + 0.25f * g;
        }
    }

    /// The beams are lit: a static body with one segment per side. It stops
    /// ships (you, him) and nothing else -- rounds are cut in keepArena.
    void raiseWall(size_t pi) {
        auto& A = m_em->bossArena;
        A.walled = true;
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = { m_home.x / SCALE, m_home.y / SCALE };
        m_wallBody = b2CreateBody(m_world, &bd);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.filter.categoryBits = CATEGORY_ASTEROID;
        sd.filter.maskBits = CATEGORY_PLAYER | CATEGORY_ENEMY;
        sd.enableContactEvents = false;   // no user data: DamageSystem must never see it
        sd.material.friction = 0.f;
        sd.material.restitution = 0.35f;
        for (int k = 0; k < A.sides; ++k) {
            const sf::Vector2f a = A.corner(k) - m_home, b = A.corner(k + 1) - m_home;
            b2Segment seg;
            seg.point1 = { a.x / SCALE, a.y / SCALE };
            seg.point2 = { b.x / SCALE, b.y / SCALE };
            b2CreateSegmentShape(m_wallBody, &sd, &seg);
        }
        m_em->addTrauma(0.5f);
        // You stay in.
        if (A.wallDist(m_em->transforms[pi].position) < 80.f) {
            const sf::Vector2f v = norm(m_home - m_em->transforms[pi].position) * 600.f;
            b2Body_SetLinearVelocity(m_em->physics[pi].bodyId, { v.x / SCALE, v.y / SCALE });
        }
    }

    /// Nothing gets in, nothing gets out, nothing spawns (EnemySystem,
    /// BountySystem and PoiSystem read bossArena). The beams bite.
    void keepArena(float dt) {
        const auto& A = m_em->bossArena;
        if (!A.walled) return;
        const size_t bi = m_em->getEntityIndex(m_bossId);
        const size_t pj = m_em->getEntityIndex(m_playerId);
        std::vector<uint32_t> gone;
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j == pj || j == bi) continue;
            const b2BodyId b = m_em->physics[j].bodyId;
            if (!b2Body_IsValid(b)) continue;
            const BodyUserData* ud = bodyUD(b);
            if (!ud) continue;
            const sf::Vector2f p = m_em->transforms[j].position;
            const float wd = A.wallDist(p);
            if (ud->type == BodyType::Bullet) {
                // Every round dies on the beams, from either side.
                if (wd < 0.f && wd > -400.f && !m_em->bullets[j].markedForDestroy) {
                    m_em->bullets[j].markedForDestroy = true;
                    m_em->spawnImpact(p, sf::Color(255, 150, 70), m_em->transforms[j].velocity * 0.3f);
                }
                continue;
            }
            if (b2Body_GetType(b) != b2_dynamicBody) continue;
            if (ud->type == BodyType::Enemy) {
                if (wd > 40.f) { gone.push_back(ud->entityId); continue; }   // inside: not in this fight
                if (wd > -360.f) {
                    const sf::Vector2f v = norm(p - m_home) * 380.f;
                    b2Body_SetLinearVelocity(b, { v.x / SCALE, v.y / SCALE });
                }
                continue;
            }
            // A rock or a pile that found its way in (streamed in, split off).
            if (ud->type == BodyType::Asteroid && wd > 60.f && m_stage != Stage::Dying) gone.push_back(ud->entityId);
        }
        for (uint32_t id : gone) {
            const size_t j = m_em->getEntityIndex(id);
            if (j == (size_t)-1) continue;
            m_em->spawnExplosion(m_em->transforms[j].position, sf::Color(170, 160, 150), 10, 3.f);
            m_em->destroyEntity(j);
        }

        // ---- You: inside, and the beams sting ----
        const size_t pi = m_em->getEntityIndex(m_playerId);
        if (pi == (size_t)-1) return;
        const b2BodyId pb = m_em->physics[pi].bodyId;
        const sf::Vector2f pp = m_em->transforms[pi].position;
        const float wd = A.wallDist(pp);
        if (wd < -10.f) {
            const sf::Vector2f back = clampInside(pp, 90.f);
            b2Body_SetTransform(pb, { back.x / SCALE, back.y / SCALE }, b2Body_GetRotation(pb));
        }
        m_zapT -= dt;
        if (wd < 34.f && m_zapT <= 0.f) {
            m_zapT = 0.18f;
            // the nearest edge
            sf::Vector2f n(0.f, 0.f); float best = -1e9f;
            for (int k = 0; k < A.sides; ++k) {
                const sf::Vector2f nk = A.normal(k);
                const float d = (pp.x - m_home.x) * nk.x + (pp.y - m_home.y) * nk.y;
                if (d > best) { best = d; n = nk; }
            }
            const sf::Vector2f at = pp + n * std::max(0.f, wd);
            for (int k = 0; k < 8; ++k)
                m_em->particles.push_back({ at, -n * frange(80.f, 260.f) + sf::Vector2f(frange(-120.f, 120.f), frange(-120.f, 120.f)),
                    sf::Color(255, 220, 150, 240), 0.2f, 0.2f, 2.5f });
            addBolt(at, -n, 40.f);
            const b2Vec2 v = b2Body_GetLinearVelocity(pb);
            const float out = v.x * n.x + v.y * n.y;
            const float kick = 8.f;   // m/s back in
            b2Body_SetLinearVelocity(pb, { v.x - n.x * (std::max(0.f, out) + kick), v.y - n.y * (std::max(0.f, out) + kick) });
            const float dmg = c("beam_touch_damage", 0.f);
            if (dmg > 0.f && m_em->healths[pi].invulTimer <= 0.f) m_em->damagePlayer(pi, dmg);
            m_em->sfx("shield.deflect", at, 0.8f, 1.6f);
        }
    }

    // ========================================================================
    // FIGHT
    // ========================================================================
    void updateFight(float dt, size_t bi, size_t pi) {
        auto& ec = m_em->enemies[bi];
        auto& h = m_em->healths[bi];
        const sf::Vector2f pp = m_em->transforms[pi].position;
        const sf::Vector2f bp = m_em->transforms[bi].position;

        // ---- Death ----
        if (h.currentHp <= 0.f) { startDying(bi); return; }

        // ---- Phase 2 ----
        if (!m_phase2 && h.currentHp <= h.maxHp * c("phase2_at", 0.5f)) {
            m_phase2 = true;
            m_em->sfxUi("boss.horn", 1.f, 0.82f);
            m_em->addTrauma(0.8f);
            m_em->spawnShockRing(bp, 30.f, 700.f, 0.6f, sf::Color(255, 80, 50), 10.f, 230.f);
            banner("THE DREADNOUGHT BURNS HOTTER", "TURBO-DRIVE ONLINE // IT WILL RAM", sf::Color(255, 120, 60));
        }

        // ---- Breach: a parried proton laser got home ----
        if (ec.protonHits > m_lastProton) {
            m_lastProton = ec.protonHits;
            breach(bi, "PROTON LASER RETURNED");
        }
        if (m_breach > 0.f) {
            m_breach -= dt;
            if (m_breach <= 0.f) { m_breach = 0.f; m_em->sfx("boss.breach", bp, 0.6f, 0.7f); }
        }
        h.armour = m_breach > 0.f ? 0.f : c("armour", 0.8f);

        // ---- Gun heat / flash decay ----
        for (int g = 0; g < 3; ++g) {
            if (ec.mountHeat[g] > 1.f) ec.mountHeat[g] = std::max(1.f, ec.mountHeat[g] - dt * 9.f);
            if (ec.mountHeat[g] <= 1.f && !m_heatHold[g]) ec.mountHeat[g] = std::max(0.f, ec.mountHeat[g] - dt * 3.f);
            m_heatHold[g] = false;
        }
        m_podGlow = std::max(0.f, m_podGlow - dt * 1.5f);
        for (auto& m : m_markers) if (m.done && m.flash > 0.f) m.flash -= dt;
        for (auto& s : m_sectors) if (s.flash > 0.f) s.flash -= dt;

        // ---- Bombardment shells fly on their own clock ----
        updateMarkers(dt, bi, pi);

        // ---- Stunned (he hit the wall) ----
        if (m_stun > 0.f) {
            m_stun -= dt;
            const b2BodyId b = m_em->physics[bi].bodyId;
            const b2Vec2 v = b2Body_GetLinearVelocity(b);
            b2Body_SetLinearVelocity(b, { v.x * std::exp(-3.f * dt), v.y * std::exp(-3.f * dt) });
            b2Body_SetAngularVelocity(b, 0.4f * std::sin(m_time * 2.f));
            if ((rand() % 100) < 30) {
                const sf::Vector2f sp2 = localToWorld(bi, { frange(-40.f, 40.f), frange(-40.f, 40.f) });
                m_em->particles.push_back({ sp2, { frange(-90.f, 90.f), frange(-90.f, 90.f) }, sf::Color(255, 200, 120, 230), 0.3f, 0.3f, 3.f });
            }
            for (int g = 0; g < 3; ++g) { ec.mountAngle[g] += std::sin(m_time * 3.f + g) * 20.f * dt; m_gun[g] = Gun{}; m_gun[g].cd = 0.5f; }
            if (m_stun <= 0.f) m_gapT = 0.5f;
            return;
        }

        // ---- The hull's attack (or the gap before the next) ----
        m_res.fill(false);
        if (m_atk == Atk::None) {
            orbitMove(bi, pp, dt, 1.f);
            m_gapT -= dt;
            if (m_gapT <= 0.f) pick(bi);
        }
        else {
            m_atkT += dt;
            bool done = true;
            switch (m_atk) {
            case Atk::Volley:  done = doVolley(dt, bi, pi); break;
            case Atk::Rockets: done = doRockets(dt, bi, pi); break;
            case Atk::Bombard: done = doBombard(dt, bi, pi); break;
            case Atk::Spin:    done = doSpin(dt, bi, pi); break;
            case Atk::Sectors: done = doSectors(dt, bi, pi); break;
            case Atk::Proton:  done = doProton(dt, bi, pi); break;
            case Atk::Drift:   done = doDrift(dt, bi, pi); break;
            default: break;
            }
            if (done) {
                m_atk = Atk::None;
                const auto g = range2(m_phase2 ? "gap_p2" : "gap", 0.45f, 0.85f);
                m_gapT = frange(g.first, g.second);
            }
        }

        // ---- ...and every gun the hull is not holding runs its own loop ----
        updateGuns(dt, bi, pi);
    }

    void breach(size_t bi, const char* why) {
        const bool fresh = m_breach <= 0.f;
        m_breach = c("breach_time", 6.f);
        m_flashBar = 0.6f;
        const sf::Vector2f bp = m_em->transforms[bi].position;
        m_em->sfx("boss.breach", bp);
        m_em->spawnShockRing(bp, 20.f, 260.f, 0.45f, sf::Color(255, 150, 60), 6.f, 240.f);
        m_em->spawnExplosion(bp, sf::Color(255, 170, 70), 30, 4.f);
        if (fresh) banner("ARMOUR BREACHED", why, sf::Color(255, 150, 60));
    }

    /// Weighted random with pity: repeats are fine, nothing goes unpicked
    /// more than `max_skip` times running.
    void pick(size_t bi) {
        sol::optional<sol::table> wt = m_cfg[m_phase2 ? "weights_p2" : "weights"];
        std::array<float, ATK_N> w{};
        for (int a = 1; a < ATK_N; ++a) w[a] = wt ? (*wt)[kAtkNames[a]].get_or(0.f) : (a == static_cast<int>(Atk::Drift) ? 0.f : 1.f);
        // One bombardment on the ground at a time: two at once leave no way out.
        bool shellsDown = false;
        for (const auto& m : m_markers) if (!m.done) { shellsDown = true; break; }
        if (shellsDown) w[static_cast<int>(Atk::Bombard)] = 0.f;
        // The shotgun sectors and the bombardment are SOLO attacks: nothing else
        // of his is in the air with them -- so not while his rockets still fly.
        if (rocketsUp()) { w[static_cast<int>(Atk::Sectors)] = 0.f; w[static_cast<int>(Atk::Bombard)] = 0.f; }
        // The proton laser closes a SERIES: never before `proton_after` other
        // attacks, always by `proton_by`, and never over live shells -- you
        // should be watching it, not the ground.
        const int P = static_cast<int>(Atk::Proton);
        w[P] = 0.f;
        m_skip[P] = 0;
        const auto ser = range2(m_phase2 ? "proton_series_p2" : "proton_series", 3.f, 5.f);
        if (!shellsDown && m_forced == 0 && m_sinceProton >= static_cast<int>(ser.first)
            && (m_sinceProton >= static_cast<int>(ser.second) || frand() < c("proton_chance", 0.4f))) {
            startAttack(Atk::Proton, bi);
            return;
        }
        const int maxSkip = static_cast<int>(c("max_skip", 4.f));
        int chosen = 0, forcedSkip = -1;
        for (int a = 1; a < ATK_N; ++a)
            if (w[a] > 0.f && m_skip[a] >= maxSkip && m_skip[a] > forcedSkip) { forcedSkip = m_skip[a]; chosen = a; }
        if (!chosen) {
            float total = 0.f;
            for (int a = 1; a < ATK_N; ++a) if (w[a] > 0.f) total += w[a] * (1.f + 0.35f * m_skip[a]);
            float r = frand() * total;
            for (int a = 1; a < ATK_N; ++a) {
                if (w[a] <= 0.f) continue;
                r -= w[a] * (1.f + 0.35f * m_skip[a]);
                if (r <= 0.f) { chosen = a; break; }
            }
            if (!chosen) for (int a = ATK_N - 1; a >= 1; --a) if (w[a] > 0.f) { chosen = a; break; }
        }
        if (m_forced > 0) { chosen = m_forced; m_forced = 0; }
        if (!chosen) { m_gapT = 0.5f; return; }
        for (int a = 1; a < ATK_N; ++a) if (w[a] > 0.f && a != chosen) ++m_skip[a];
        m_skip[chosen] = 0;
        startAttack(static_cast<Atk>(chosen), bi);
    }

    void startAttack(Atk a, size_t bi) {
        m_atk = a;
        m_atkT = 0.f;
        m_step = 0;
        m_stepT = 0.f;
        m_fired.fill(0);
        m_sectors.clear();
        m_driftStep = 0;
        m_laserPath.clear();
        m_bombChain = 0;
        if (a == Atk::Proton) m_sinceProton = 0; else ++m_sinceProton;
        const sf::Vector2f bp = m_em->transforms[bi].position;
        switch (a) {
        case Atk::Spin: m_em->sfx("raider.storm.spin", bp, 1.f, 0.7f); m_spinDir = (rand() % 2) ? 1.f : -1.f; break;
        case Atk::Proton:
            m_em->sfx("boss.proton.charge", bp);
            m_protonCharge = c("proton_charge", 1.1f) / sp();
            m_bankEdge = (frand() < c("proton_bank", 0.45f)) ? 0 : -1;   // chosen properly on the first frame
            m_bankPicked = false;
            break;
        case Atk::Rockets: m_em->sfx("barge.shotgun.charge", bp, 0.7f, 1.3f); break;
        default: break;
        }
    }

    // ---- Movement ----
    void face(size_t bi, float heading, float rateDeg) {
        const b2BodyId b = m_em->physics[bi].bodyId;
        const float diff = wrap(heading - m_em->transforms[bi].rotation);
        const float w = std::clamp(diff * 3.f, -rateDeg, rateDeg) * 0.0174533f;
        b2Body_SetAngularVelocity(b, w);
    }
    void steerVel(size_t bi, sf::Vector2f want, float dt, float accel) {
        const b2BodyId b = m_em->physics[bi].bodyId;
        const b2Vec2 v0 = b2Body_GetLinearVelocity(b);
        const sf::Vector2f v(v0.x * SCALE, v0.y * SCALE);
        const sf::Vector2f nv = v + (want - v) * std::min(1.f, dt * accel);
        b2Body_SetLinearVelocity(b, { nv.x / SCALE, nv.y / SCALE });
    }
    /// He CIRCLES you: round you at `orbit_radius`, the way flipping now and
    /// then, and off the beams. Facing you, so every gun bears.
    void orbitMove(size_t bi, sf::Vector2f pp, float dt, float mult) {
        const sf::Vector2f bp = m_em->transforms[bi].position;
        const float R = cp("orbit_radius", "orbit_radius_p2", 420.f);
        const float speed = cp("orbit_speed", "orbit_speed_p2", 300.f) * mult;
        m_orbitFlip -= dt;
        if (m_orbitFlip <= 0.f) { m_orbitDir = -m_orbitDir; const auto f = range2("orbit_flip", 2.5f, 5.f); m_orbitFlip = frange(f.first, f.second); }
        m_orbitAng += m_orbitDir * (speed / std::max(120.f, R)) * dt;
        sf::Vector2f target = pp + sf::Vector2f(std::cos(m_orbitAng), std::sin(m_orbitAng)) * R;
        // Off the beams: if the orbit would put him in them, turn back.
        const float margin = hullR() + 40.f;
        if (m_em->bossArena.wallDist(target) < margin) {
            if (m_flipCd <= 0.f) { m_orbitDir = -m_orbitDir; m_flipCd = 0.8f; }
            target = clampInside(target, margin);
        }
        if (m_flipCd > 0.f) m_flipCd -= dt;
        // Re-sync the angle to where he actually is, so a shove does not leave him chasing a ghost.
        const float actual = std::atan2(bp.y - pp.y, bp.x - pp.x);
        m_orbitAng += wrap((actual - m_orbitAng) * 57.29578f) * 0.0174533f * std::min(1.f, dt * 0.8f);
        const sf::Vector2f to = target - bp;
        sf::Vector2f want = to * 2.2f;
        const float cap = speed * 1.6f + 60.f;
        if (len(want) > cap) want = norm(want) * cap;
        steerVel(bi, want, dt, 3.f);
        face(bi, headingOf(pp - bp), cp("turn_rate", "turn_rate_p2", 80.f));
    }

    // ---- Guns ----
    void aimGun(size_t bi, int g, float heading, float rate, float dt) {
        auto& a = m_em->enemies[bi].mountAngle[g];
        const float d = wrap(heading - a);
        a += std::clamp(d, -rate * dt, rate * dt);
    }
    void aimGuns(size_t bi, sf::Vector2f at, float dt, float rate) {
        for (int g = 0; g < guns(bi); ++g) aimGun(bi, g, headingOf(at - mount(bi, g)), rate, dt);
    }
    /// Telegraph heat 0..1 on a gun (a flash, >1, runs out first).
    void heat(size_t bi, int g, float u) {
        float& h = m_em->enemies[bi].mountHeat[g];
        if (h <= 1.f) h = std::clamp(u, 0.f, 1.f);
        m_heatHold[g] = true;
    }
    void flash(size_t bi, int g) { m_em->enemies[bi].mountHeat[g] = 2.f; }

    /// Where you will be in `t` seconds, if you keep going.
    sf::Vector2f predict(size_t pi, float t) const {
        return m_em->transforms[pi].position + playerVel(pi) * t;
    }

    /// One round from the boss. Its rounds never stop on its own hull (they
    /// leave from inside it) and never hit another ship.
    size_t fire(sf::Vector2f at, sf::Vector2f dir, float speed, float dmg, float life, float iframes, int look) {
        const float angle = std::atan2(dir.y, dir.x) * 57.29578f + 90.f;
        const sol::table cfg = m_def->config;
        const uint32_t id = m_ef->createEnemyBullet(*m_em, at, dir * speed, angle, m_bossId, *m_lua, m_world, cfg);
        const size_t j = m_em->getEntityIndex(id);
        if (j == (size_t)-1) return j;
        const sf::Vector2f vel = dir * speed;
        const b2BodyId b = m_em->physics[j].bodyId;
        if (b2Body_IsValid(b)) {
            b2Body_SetLinearVelocity(b, { vel.x / SCALE, vel.y / SCALE });
            b2ShapeId shapes[2];
            const int n = b2Body_GetShapes(b, shapes, 2);
            for (int k = 0; k < n; ++k) {
                b2Filter f = b2Shape_GetFilter(shapes[k]);
                f.maskBits &= ~static_cast<uint64_t>(CATEGORY_ENEMY);
                b2Shape_SetFilter(shapes[k], f);
            }
        }
        m_em->transforms[j].velocity = vel;
        auto& bl = m_em->bullets[j];
        bl.damage = dmg;
        bl.lifetime = life;
        bl.playerIframes = iframes;
        auto& sh = m_em->renders[j].shape;
        if (look == 1) {          // heavy slug
            sh.setPoint(0, { 0.f, -26.f }); sh.setPoint(1, { 4.5f, 0.f }); sh.setPoint(2, { 0.f, 26.f }); sh.setPoint(3, { -4.5f, 0.f });
            sh.setFillColor(sf::Color(255, 236, 214));
            sh.setOutlineThickness(2.2f);
            sh.setOutlineColor(sf::Color(255, 70, 50, 235));
            bl.trail = 1;
        }
        else if (look == 2) {     // pellet
            sh.setPoint(0, { 0.f, -7.f }); sh.setPoint(1, { 2.4f, 0.f }); sh.setPoint(2, { 0.f, 7.f }); sh.setPoint(3, { -2.4f, 0.f });
            sh.setFillColor(sf::Color(255, 190, 120));
        }
        else if (look == 3) {     // spin storm
            sh.setPoint(0, { 0.f, -8.f }); sh.setPoint(1, { 3.f, 0.f }); sh.setPoint(2, { 0.f, 8.f }); sh.setPoint(3, { -3.f, 0.f });
            sh.setFillColor(sf::Color(255, 90, 70));
        }
        return j;
    }

    // ========================================================================
    // THE GUNS' OWN LOOP -- bursts and shotgun blasts -- alongside the hull
    // ========================================================================
    void updateGuns(float dt, size_t bi, size_t pi) {
        const sf::Vector2f pp = m_em->transforms[pi].position;
        const int G = guns(bi);
        int busy = 0;
        for (int g = 0; g < G; ++g) if (m_gun[g].act != GunAct::Idle) ++busy;
        const int maxBusy = static_cast<int>(cp("gun_busy_max", "gun_busy_max_p2", 2.f));
        const float trav = c("gun_traverse", 200.f);
        for (int g = 0; g < G; ++g) {
            Gun& gn = m_gun[g];
            if (m_res[g]) {   // the hull has it
                if (gn.act != GunAct::Idle) { gn = Gun{}; gn.cd = 0.35f; }
                continue;
            }
            switch (gn.act) {
            case GunAct::Idle: {
                aimGun(bi, g, headingOf(pp - mount(bi, g)), trav, dt);
                gn.cd -= dt;
                if (gn.cd > 0.f || busy >= maxBusy) break;
                // what this gun does next
                sol::optional<sol::table> wt = m_cfg["gun_weights"];
                const float wb = wt ? (*wt)["burst"].get_or(3.f) : 3.f;
                float wx = wt ? (*wt)["blast"].get_or(2.f) : 2.f;
                if (len(pp - mount(bi, g)) > c("blast_range", 700.f) * 0.9f) wx *= 0.3f;   // a cone that falls short is noise
                int act = 1;
                if (m_forcedGun > 0) { act = m_forcedGun; m_forcedGun = 0; }
                else {
                    act = frand() * (wb + wx) < wb ? 1 : 4;
                }
                if (act == 4) { gn = Gun{}; gn.act = GunAct::Blast; ++busy; m_em->sfx("barge.shotgun.charge", mount(bi, g), 0.5f, 1.3f); break; }
                gn = Gun{}; gn.act = GunAct::Burst; ++busy;
                break;
            }
            case GunAct::Burst: {
                gn.t += dt;
                const float tell = gunTell("burst_tell", 0.25f);
                const float iv = gunTell("burst_interval", 0.07f);
                const int rounds = static_cast<int>(cp("burst_rounds", "burst_rounds_p2", 4.f));
                if (gn.t < tell) {
                    const float tof = len(pp - mount(bi, g)) / m_def->config["bullet_speed"].get_or(640.f);
                    aimGun(bi, g, headingOf(predict(pi, tof * 0.6f) - mount(bi, g)), trav * 1.5f, dt);
                    heat(bi, g, gn.t / tell);
                    break;
                }
                while (gn.n < rounds && gn.t >= tell + gn.n * iv) {
                    const float k = rounds > 1 ? static_cast<float>(gn.n) / (rounds - 1) - 0.5f : 0.f;
                    const float a = m_em->enemies[bi].mountAngle[g] + k * c("burst_spread", 16.f) * ((g % 2) ? -1.f : 1.f);
                    fire(muzzle(bi, g), dirOf(a), m_def->config["bullet_speed"].get_or(640.f), c("burst_damage", 9.f),
                        m_def->config["bullet_lifetime"].get_or(3.2f), m_def->config["bullet_iframes"].get_or(0.25f), 0);
                    flash(bi, g);
                    if (gn.n == 0) m_em->sfx("barge.fan", mount(bi, g), 0.8f);
                    ++gn.n;
                }
                if (gn.n >= rounds) endGun(gn);
                break;
            }
            case GunAct::Blast: {
                gn.t += dt;
                const float tell = gunTell("blast_tell", 0.45f);
                if (!gn.fired) {
                    // tracks you for most of the tell, then holds: the honest beat
                    if (gn.t < tell * 0.7f) aimGun(bi, g, headingOf(predict(pi, 0.25f) - mount(bi, g)), trav * 1.4f, dt);
                    heat(bi, g, gn.t / tell);
                    if (gn.t >= tell) {
                        gn.fired = true;
                        gn.t = 0.f;
                        const int pel = static_cast<int>(c("blast_pellets", 9.f) * (m_phase2 ? 1.25f : 1.f));
                        const float half = c("blast_half", 20.f);
                        const float reach = c("blast_range", 700.f);
                        const sf::Vector2f mz = muzzle(bi, g);
                        for (int k = 0; k < pel; ++k) {
                            const float a = m_em->enemies[bi].mountAngle[g] + half * (2.f * (k + frand()) / pel - 1.f);
                            const float v = frange(700.f, 1050.f);
                            fire(mz, dirOf(a), v, c("blast_damage", 8.f), reach / v, 0.12f, 2);
                        }
                        flash(bi, g);
                        m_em->sfx("barge.shotgun.blast", mz, 0.8f, 1.15f);
                        m_em->addTrauma(0.15f);
                    }
                }
                else if (gn.t > 0.3f) endGun(gn);
                break;
            }
            }
        }
    }
    void endGun(Gun& gn) {
        const auto r = range2(m_phase2 ? "gun_cd_p2" : "gun_cd", 0.8f, 1.6f);
        gn = Gun{};
        gn.cd = frange(r.first, r.second);
    }
    /// Any of his rockets still in the air?
    bool rocketsUp() const {
        for (const auto& b : m_em->bullets) if (b.isActive && b.isRocket && b.ownerEntityId == m_bossId) return true;
        return false;
    }
    // ========================================================================
    // THE HULL'S ATTACKS -- each returns true when it is over
    // ========================================================================

    /// Every gun at once: the Barge's burst, three times over.
    bool doVolley(float dt, size_t bi, size_t pi) {
        const sf::Vector2f pp = m_em->transforms[pi].position;
        orbitMove(bi, pp, dt, 0.6f);
        const int G = guns(bi);
        for (int g = 0; g < G; ++g) m_res[g] = true;
        const float tell = c("volley_tell", 0.35f) / sp();
        const float stag = m_phase2 ? 0.f : c("volley_stagger", 0.12f);
        const float iv = c("volley_interval", 0.07f) / sp();
        const int rounds = static_cast<int>(cp("volley_rounds", "volley_rounds_p2", 6.f));
        const float spread = c("volley_spread", 24.f);
        bool all = true;
        for (int g = 0; g < G; ++g) {
            const float t0 = tell + g * stag;
            if (m_fired[g] < rounds) all = false;
            if (m_atkT < t0) {
                const float tof = len(pp - mount(bi, g)) / m_def->config["bullet_speed"].get_or(640.f);
                aimGun(bi, g, headingOf(predict(pi, tof * 0.5f) - mount(bi, g)), 300.f, dt);
                heat(bi, g, std::clamp((m_atkT - (t0 - tell)) / tell, 0.f, 1.f));
                continue;
            }
            while (m_fired[g] < rounds && m_atkT >= t0 + m_fired[g] * iv) {
                const float k = rounds > 1 ? static_cast<float>(m_fired[g]) / (rounds - 1) - 0.5f : 0.f;
                const float a = m_em->enemies[bi].mountAngle[g] + k * spread * ((g % 2) ? -1.f : 1.f);
                fire(muzzle(bi, g), dirOf(a), m_def->config["bullet_speed"].get_or(640.f),
                    c("volley_damage", 9.f), m_def->config["bullet_lifetime"].get_or(3.2f), m_def->config["bullet_iframes"].get_or(0.25f), 0);
                flash(bi, g);
                if (m_fired[g] == 0) m_em->sfx("barge.fan", mount(bi, g));
                ++m_fired[g];
            }
        }
        return all && m_atkT > tell + (G - 1) * stag + rounds * iv + 0.2f;
    }

    /// Two from each wing pod, tracking you. The guns stay free.
    bool doRockets(float dt, size_t bi, size_t pi) {
        orbitMove(bi, m_em->transforms[pi].position, dt, 0.9f);
        const float tell = c("rocket_tell", 0.45f) / sp();
        const float iv = c("rocket_interval", 0.1f) / sp();
        const int n = static_cast<int>(cp("rockets", "rockets_p2", 4.f));
        if (m_atkT < tell) { m_podGlow = std::max(m_podGlow, m_atkT / tell); return false; }
        m_podGlow = 1.f;
        while (m_fired[0] < n&& m_atkT >= tell + m_fired[0] * iv) {
            const int k = m_fired[0];
            const int side = k % 2, tube = (k / 2) % 2;
            const sf::Vector2f at = podMuzzle(bi, side, tube);
            // Out to the side, wide: they swing round and come at you from left and right.
            const float out = (side == 0 ? 1.f : -1.f) * (c("rocket_spread", 100.f) + 15.f * tube);
            const sol::table cfg = m_def->config;
            m_ef->createEnemyRocket(*m_em, at, m_em->transforms[bi].rotation + out, m_bossId, m_playerId, m_world, cfg);
            m_em->sfx("maniac.rocket.launch", at);
            ++m_fired[0];
        }
        return m_fired[0] >= n && m_atkT > tell + n * iv + 0.2f;
    }

    /// The guns lob shells; a marker shows where each comes down. Not dodged,
    /// not parried: left. A SOLO attack -- every gun is on the shells, nothing
    /// else flies -- and a CHAIN of 1-3 (`bomb_chain`): each bombardment lands
    /// before the next is marked. The hull is done when the last one is down.
    bool doBombard(float dt, size_t bi, size_t pi) {
        const sf::Vector2f pp = m_em->transforms[pi].position;
        orbitMove(bi, pp, dt, 0.8f);
        const int G = guns(bi);
        for (int g = 0; g < G; ++g) m_res[g] = true;
        if (m_step == 0 && m_bombChain == 0) {
            const auto ch = range2(m_phase2 ? "bomb_chain_p2" : "bomb_chain", 1.f, 3.f);
            const int lo = std::max(1, static_cast<int>(ch.first)), hi = std::max(lo, static_cast<int>(ch.second));
            m_bombChain = lo + rand() % (hi - lo + 1);
        }
        bool live = false;
        for (const auto& m : m_markers) if (!m.done) { live = true; break; }
        if (!live && m_step < m_bombChain) {
            m_stepT += dt;
            if (m_step == 0 || m_stepT >= c("bomb_chain_gap", 0.3f) / sp()) {
                placePattern(bi, pi, -1, 0);
                ++m_step;
                m_driftStep = m_step;   // shows as bombard/N in the dev line
                m_stepT = 0.f;
                live = true;
            }
        }
        bool unfired = false;
        for (const auto& m : m_markers) if (!m.done && m.t < 0.f) unfired = true;
        // the guns hold their next mark
        for (int g = 0; g < guns(bi); ++g) {
            if (!m_res[g]) continue;
            for (const auto& m : m_markers) if (!m.done && m.t < 0.f && m.gun == g) {
                aimGun(bi, g, headingOf(m.pos - mount(bi, g)), 700.f, dt);
                heat(bi, g, 0.8f);
                break;
            }
        }
        // the guns not lobbing stay on you, quiet
        for (int g = 0; g < G; ++g) {
            bool lobbing = false;
            for (const auto& m : m_markers) if (!m.done && m.t < 0.f && m.gun == g) { lobbing = true; break; }
            if (!lobbing) aimGun(bi, g, headingOf(pp - mount(bi, g)), 90.f, dt);
        }
        (void)unfired;
        if (m_step >= m_bombChain && !live) { m_bombChain = 0; return true; }
        return false;
    }

    /// kind: 0 X, 1 star, 2 circle round you (two gaps), 3 random; -1 any.
    void placePattern(size_t bi, size_t pi, int kind, int maxN) {
        const sf::Vector2f cen = predict(pi, 0.35f);
        const float r = c("bomb_radius", 105.f);
        const float s = c("bomb_spacing", 210.f);
        const float T = cp("bomb_delay", "bomb_delay_p2", 1.4f);
        if (kind < 0) kind = rand() % 4;
        std::vector<sf::Vector2f> pts;
        const float rot = frand() * 6.2831853f;
        if (kind == 0) {                       // X
            pts.push_back(cen);
            for (int arm = 0; arm < 4; ++arm) {
                const float a = rot + 0.785398f + arm * 1.570796f;
                for (int k = 1; k <= 2; ++k) pts.push_back(cen + sf::Vector2f(std::cos(a), std::sin(a)) * (s * k));
            }
        }
        else if (kind == 1) {                  // star
            pts.push_back(cen);
            for (int arm = 0; arm < 5; ++arm) {
                const float a = rot + arm * 1.256637f;
                for (int k = 1; k <= 2; ++k) pts.push_back(cen + sf::Vector2f(std::cos(a), std::sin(a)) * (s * k));
            }
        }
        else if (kind == 2) {                  // circle round you, two gaps out
            pts.push_back(cen);
            const int slots = 10;
            const int g1 = rand() % slots, g2 = (g1 + 3 + rand() % 4) % slots;
            for (int k = 0; k < slots; ++k) {
                if (k == g1 || k == g2) continue;
                const float a = rot + k * 6.2831853f / slots;
                pts.push_back(cen + sf::Vector2f(std::cos(a), std::sin(a)) * (s * 1.35f));
            }
        }
        else {                                 // random, never overlapping
            const auto rr = range2("bomb_random", 7.f, 9.f);
            const int n = static_cast<int>(rr.first) + rand() % (static_cast<int>(rr.second - rr.first) + 1);
            pts.push_back(cen);
            for (int t = 0; t < 200 && static_cast<int>(pts.size()) < n; ++t) {
                const float a = frand() * 6.2831853f, d = frange(r * 2.f, 620.f);
                const sf::Vector2f q = cen + sf::Vector2f(std::cos(a), std::sin(a)) * d;
                bool ok = true;
                for (const auto& o : pts) if (len(o - q) < r * 2.f + 16.f) { ok = false; break; }
                if (ok) pts.push_back(q);
            }
        }
        if (maxN > 0 && static_cast<int>(pts.size()) > maxN) pts.resize(static_cast<size_t>(maxN));
        const float fireIv = c("bomb_fire_interval", 0.05f) / sp();
        const int G = std::max(1, guns(bi));
        for (size_t k = 0; k < pts.size(); ++k) {
            Marker m;
            m.pos = clampInside(pts[k], r + 20.f);
            m.r = r; m.T = T;
            m.gun = static_cast<int>(k) % G;
            m.t = -fireIv * static_cast<float>(k);   // the guns fire them one after another, fast
            m_markers.push_back(m);
        }
        m_em->sfx("boss.bomb.mark", cen);
    }

    void updateMarkers(float dt, size_t bi, size_t pi) {
        if (m_markers.empty()) return;
        const sf::Vector2f pp = m_em->transforms[pi].position;
        bool hitThisFrame = false;
        for (auto& m : m_markers) {
            if (m.done) continue;
            const float before = m.t;
            m.t += dt;
            // the gun fires this one now
            if (before < 0.f && m.t >= 0.f) {
                if (bi != (size_t)-1 && m_stage == Stage::Fight) {
                    m.from = muzzle(bi, m.gun);
                    flash(bi, m.gun);
                    m_em->spawnShockRing(m.from, 3.f, 34.f, 0.15f, sf::Color(255, 170, 90), 3.f, 230.f);
                    m_em->sfx("barge.slug", m.from, 0.45f, 0.65f);
                }
                else m.from = m.pos;
            }
            if (m.t < m.T) continue;
            m.done = true;
            m.flash = 0.35f;
            m_em->spawnExplosion(m.pos, sf::Color(255, 120, 50), 26, 4.5f);
            m_em->spawnExplosion(m.pos, sf::Color(255, 230, 190), 10, 3.f);
            m_em->spawnShockRing(m.pos, 10.f, m.r * 1.15f, 0.3f, sf::Color(255, 140, 70), 5.f, 230.f);
            m_em->sfx("boss.bomb.impact", m.pos, 0.8f);
            m_em->addTrauma(0.18f);
            // No dodge, no parry: only a tumble you are already in spares you.
            const auto& ps = m_em->players[pi];
            if (!hitThisFrame && m_bombGrace <= 0.f && len(pp - m.pos) < m.r + 16.f && ps.staggerTimer <= 0.f) {
                hitThisFrame = true;
                m_bombGrace = 0.3f;
                m_em->damagePlayer(pi, m_em->healths[pi].maxHp * c("bomb_damage", 0.30f));
                m_em->healths[pi].invulTimer = std::max(m_em->healths[pi].invulTimer, 0.3f);
                m_em->spawnScreenFlash(sf::Color(255, 90, 40), 0.25f, 90.f);
                m_em->addTrauma(0.6f);
                m_em->sfxUi("player.hit.heavy");
                const sf::Vector2f kick = norm(pp - m.pos) * 420.f;
                const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[pi].bodyId);
                b2Body_SetLinearVelocity(m_em->physics[pi].bodyId, { v.x + kick.x / SCALE, v.y + kick.y / SCALE });
            }
        }
        if (m_bombGrace > 0.f) m_bombGrace -= dt;
        m_markers.erase(std::remove_if(m_markers.begin(), m_markers.end(),
            [](const Marker& m) { return m.done && m.flash <= 0.f; }), m_markers.end());
    }

    /// The guns whirl: a tight circular storm, while he keeps circling.
    bool doSpin(float dt, size_t bi, size_t pi) {
        const float wind = c("spin_windup", 0.6f) / sp();
        const float T = cp("spin_time", "spin_time_p2", 3.f);
        const float iv = c("spin_interval", 0.06f) / sp();
        orbitMove(bi, m_em->transforms[pi].position, dt, 0.45f);
        const int G = guns(bi);
        for (int g = 0; g < G; ++g) m_res[g] = true;
        const float rate = c("spin_rate", 230.f);
        if (m_atkT < wind) {
            for (int g = 0; g < G; ++g) {
                m_em->enemies[bi].mountAngle[g] += rate * (m_atkT / wind) * dt * m_spinDir;
                heat(bi, g, m_atkT / wind);
            }
            return false;
        }
        const float t = m_atkT - wind;
        for (int g = 0; g < G; ++g) {
            // Phase 2: the wing guns turn against the bow gun -- the arms cross.
            const float dir = (m_phase2 && g > 0) ? -m_spinDir : m_spinDir;
            m_em->enemies[bi].mountAngle[g] += rate * dir * dt;
            heat(bi, g, 1.f);
            while (m_fired[g] * iv <= t && t < T) {
                const float a = m_em->enemies[bi].mountAngle[g];
                for (int barrel = 0; barrel < 2; ++barrel) {
                    const sf::Vector2f mz = mount(bi, g) + dirOf(a) * (turretmodel::kMuzzle * gunSize(bi))
                        + dirOf(a + 90.f) * ((barrel ? 1.f : -1.f) * turretmodel::kBarrelX * gunSize(bi));
                    fire(mz, dirOf(a), c("spin_speed", 430.f), c("spin_damage", 8.f), (2.f * m_R) / c("spin_speed", 430.f), 0.25f, 3);
                }
                if (m_fired[g] % 6 == 0) m_em->sfx("wardog.fire", mount(bi, g), 0.5f);
                ++m_fired[g];
            }
        }
        return t >= T + 0.2f;
    }

    /// Wedges the length of the cage, fired in turn, overlapping a little.
    /// He holds still for it, and so do his guns.
    bool doSectors(float dt, size_t bi, size_t pi) {
        const sf::Vector2f pp = m_em->transforms[pi].position;
        const sf::Vector2f bp = m_em->transforms[bi].position;
        steerVel(bi, { 0.f, 0.f }, dt, 4.f);
        face(bi, headingOf(pp - bp), 60.f);
        // A SOLO attack: the guns fall silent and only the wedges fly.
        for (int g = 0; g < guns(bi); ++g) { m_res[g] = true; aimGun(bi, g, headingOf(pp - mount(bi, g)), 60.f, dt); }
        if (m_sectors.empty() && m_step == 0) {
            const int n = static_cast<int>(cp("sectors", "sectors_p2", 4.f));
            const float half = c("sector_half", 24.f) * 0.0174533f;
            const float step = c("sector_step", 40.f) * 0.0174533f;
            const float tell = c("sector_tell", 0.7f) / sp();
            const float iv = c("sector_interval", 0.3f) / sp();
            const float base = std::atan2(pp.y - bp.y, pp.x - bp.x);
            const bool rev = rand() % 2;
            for (int k = 0; k < n; ++k) {
                Sector s;
                s.from = bp;
                s.ang = base + step * (k - (n - 1) * 0.5f);
                s.half = half;
                s.reach = 2.f * m_R;
                s.tell = tell;
                s.at = tell + (rev ? n - 1 - k : k) * iv;
                m_sectors.push_back(s);
            }
            m_step = 1;
            m_em->sfx("barge.shotgun.charge", bp);
        }
        bool all = true;
        for (auto& s : m_sectors) {
            if (s.fired) continue;
            all = false;
            s.from = bp;
            if (m_atkT < s.at) continue;
            s.fired = true;
            s.flash = 0.3f;
            const int pel = static_cast<int>(c("sector_pellets", 22.f) * (m_phase2 ? 1.2f : 1.f));
            for (int k = 0; k < pel; ++k) {
                const float a = s.ang + s.half * (2.f * (k + frand()) / pel - 1.f);
                const sf::Vector2f d(std::cos(a), std::sin(a));
                const float v = frange(650.f, 1150.f);
                fire(bp + d * (hullR() * 0.4f), d, v, c("sector_damage", 10.f), wallReach(bp, d, s.reach) / v + 0.1f, 0.12f, 2);
            }
            m_em->sfx("barge.shotgun.blast", bp);
            m_em->addTrauma(0.25f);
        }
        return all && m_atkT > m_sectors.back().at + 0.3f;
    }

    // ---- The proton laser ----
    /// The path a beam fired from o along d would take: the cage reflects it.
    std::vector<sf::Vector2f> laserPath(sf::Vector2f o, sf::Vector2f d, int bounces, float range) const {
        std::vector<sf::Vector2f> pts{ o };
        const auto& A = m_em->bossArena;
        float left = range;
        for (int b = 0; b <= bounces && left > 1.f; ++b) {
            float t; sf::Vector2f n;
            if (!A.walled || !A.rayHit(o, d, t, n) || t > left) { pts.push_back(o + d * left); break; }
            o = o + d * t;
            pts.push_back(o);
            left -= t;
            const float dn = d.x * n.x + d.y * n.y;
            d = d - n * (2.f * dn);
            o += d * 2.f;
        }
        return pts;
    }
    /// Aim for a bank shot off edge k: the point to aim at is you mirrored
    /// in that wall. False when the first wall it meets is not that one.
    bool bankAim(sf::Vector2f o, sf::Vector2f target, int k, sf::Vector2f& dir) const {
        const auto& A = m_em->bossArena;
        const sf::Vector2f n = A.normal(k);
        const float dist = (target.x - m_home.x) * n.x + (target.y - m_home.y) * n.y - A.apothem();
        const sf::Vector2f mirror = target - n * (2.f * dist);
        dir = norm(mirror - o);
        float t; sf::Vector2f hn;
        if (!A.rayHit(o, dir, t, hn)) return false;
        return hn.x * n.x + hn.y * n.y > 0.999f && t > 120.f;
    }

    /// Charged on the bow gun -- a beam like the Bloodseeker's lance, green,
    /// that bounces off the cage. Sometimes he banks it off a wall at you.
    /// PARRY it and it goes straight home: big damage, armour breached.
    bool doProton(float dt, size_t bi, size_t pi) {
        const sf::Vector2f pp = m_em->transforms[pi].position;
        // Every gun falls silent: this one is about the parry, not surviving.
        for (int g = 0; g < 3; ++g) m_res[g] = true;
        for (int g = 1; g < guns(bi); ++g) { aimGun(bi, g, headingOf(pp - mount(bi, g)), 90.f, dt); }
        orbitMove(bi, pp, dt, m_step == 0 ? 0.45f : 0.15f);
        const sf::Vector2f mz = muzzle(bi, 0);
        const int bounces = static_cast<int>(cp("proton_bounces", "proton_bounces_p2", 2.f));
        const float range = c("proton_range", 5000.f);
        const float lock = c("proton_lock", 0.22f) / sp();
        if (m_step == 0) {
            // where you will be when it fires
            const sf::Vector2f target = predict(pi, std::max(0.f, m_protonCharge - m_atkT + lock) * 0.6f);
            sf::Vector2f dir = norm(target - mz);
            if (m_bankEdge >= 0) {
                if (!m_bankPicked) {
                    // the wall that makes the shortest bank shot
                    float best = 1e9f; int bk = -1;
                    for (int k = 0; k < m_em->bossArena.sides; ++k) {
                        sf::Vector2f d2;
                        if (!bankAim(mz, target, k, d2)) continue;
                        const auto path = laserPath(mz, d2, 1, range);
                        float L = 0.f; for (size_t q = 1; q < path.size(); ++q) L += len(path[q] - path[q - 1]);
                        if (L < best) { best = L; bk = k; }
                    }
                    m_bankEdge = bk;
                    m_bankPicked = true;
                }
                sf::Vector2f d2;
                if (m_bankEdge >= 0 && bankAim(mz, target, m_bankEdge, d2)) dir = d2;
            }
            aimGun(bi, 0, headingOf(dir), 360.f, dt);
            heat(bi, 0, m_atkT / m_protonCharge);
            m_laserPath = laserPath(mz, dirOf(m_em->enemies[bi].mountAngle[0]), bounces, range);
            if (m_atkT >= m_protonCharge) { m_step = 1; m_stepT = 0.f; m_em->sfx("tell.lance.lock", mz); }
            return false;
        }
        if (m_step == 1) {
            m_stepT += dt;
            heat(bi, 0, 1.f);
            m_laserPath = laserPath(mz, dirOf(m_em->enemies[bi].mountAngle[0]), bounces, range);
            if (m_stepT < lock) return false;
            // ---- FIRE: DamageSystem::traceBeam resolves it next frame ----
            BeamShot shot;
            shot.origin = mz;
            shot.dir = dirOf(m_em->enemies[bi].mountAngle[0]);
            shot.range = range;
            shot.width = c("proton_width", 22.f);
            shot.damage = m_em->healths[pi].maxHp * c("proton_damage", 0.45f);
            shot.iframes = 0.8f;
            shot.knockback = 900.f;
            shot.pierceDamage = 0.f;     // nothing else is in the cage
            shot.killHp = 0.f;
            shot.objectDamage = 0.f;
            shot.ownerId = m_bossId;
            shot.wallBounces = bounces;
            shot.parryHome = m_bossId;
            shot.parryDamage = c("proton_reflect", 450.f);
            shot.style = 1;
            m_em->beamShots.push_back(shot);
            flash(bi, 0);
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[bi].bodyId);
            b2Body_SetLinearVelocity(m_em->physics[bi].bodyId, { v.x - shot.dir.x * 260.f / SCALE, v.y - shot.dir.y * 260.f / SCALE });
            m_em->spawnShockRing(mz, 6.f, 90.f, 0.22f, sf::Color(120, 255, 170), 5.f, 250.f);
            m_em->spawnScreenFlash(sf::Color(200, 255, 220), 0.1f, 50.f);
            m_em->sfx("boss.proton.fire", mz);
            m_em->addTrauma(0.35f);
            m_step = 2;
            m_stepT = 0.f;
            m_laserPath.clear();
            return false;
        }
        m_stepT += dt;
        return m_stepT > 0.25f;
    }

    /// Phase 2: round the cage flat out, every gun on you, then the RAM.
    bool doDrift(float dt, size_t bi, size_t pi) {
        auto& ec = m_em->enemies[bi];
        const b2BodyId b = m_em->physics[bi].bodyId;
        const sf::Vector2f pp = m_em->transforms[pi].position;
        const sf::Vector2f bp = m_em->transforms[bi].position;
        for (int g = 0; g < 3; ++g) m_res[g] = true;
        m_stepT += dt;
        m_ramWindup = c("ram_windup", 0.55f);
        const float lane = std::max(200.f, m_em->bossArena.apothem() - c("drift_lane", 200.f));
        const float speed = c("drift_speed", 1350.f);
        if (m_driftStep == 0) {                // kick in the turbo, swing out to the lane
            if (m_step == 0) {
                m_step = 1;
                const sf::Vector2f o = bp - m_home;
                m_driftAng = std::atan2(o.y, o.x);
                m_driftDir = (rand() % 2) ? 1.f : -1.f;
                m_em->sfx("tell.ram.charge", bp, 0.9f, 0.7f);
                m_em->spawnShockRing(bp, 20.f, 220.f, 0.3f, sf::Color(255, 140, 60), 6.f, 230.f);
            }
            m_driftAng += m_driftDir * (speed * 0.5f / lane) * dt;
            const sf::Vector2f target = m_home + sf::Vector2f(std::cos(m_driftAng), std::sin(m_driftAng)) * lane;
            sf::Vector2f want = (target - bp) * 5.f;
            if (len(want) > speed) want = norm(want) * speed;
            steerVel(bi, want, dt, 6.f);
            face(bi, headingOf(want), 300.f);
            ghost(bi, false, dt);
            if (len(bp - target) < 90.f || m_stepT > 0.9f) {
                m_driftStep = 1; m_stepT = 0.f; m_fired.fill(0);
                placePattern(bi, pi, 3, 5);
            }
            return false;
        }
        if (m_driftStep == 1) {                // the drift
            m_driftAng += m_driftDir * (speed / lane) * dt;
            const sf::Vector2f target = m_home + sf::Vector2f(std::cos(m_driftAng), std::sin(m_driftAng)) * lane;
            const sf::Vector2f tang(-std::sin(m_driftAng) * m_driftDir, std::cos(m_driftAng) * m_driftDir);
            const sf::Vector2f want = tang * speed + (target - bp) * 4.f;
            b2Body_SetLinearVelocity(b, { want.x / SCALE, want.y / SCALE });
            // Nose half into the turn: a drift, not a train on a rail.
            face(bi, headingOf(tang) - 40.f * m_driftDir, 400.f);
            ghost(bi, false, dt);
            turboFx(bi, tang, false);
            // Every gun on you.
            const float iv = c("drift_fire_interval", 0.15f);
            for (int g = 0; g < guns(bi); ++g) {
                const float tof = len(pp - mount(bi, g)) / 760.f;
                aimGun(bi, g, headingOf(predict(pi, tof * 0.7f) - mount(bi, g)), 400.f, dt);
                heat(bi, g, 0.7f);
                while (m_fired[g] * iv <= m_stepT) {
                    const float a = ec.mountAngle[g] + frange(-5.f, 5.f);
                    fire(muzzle(bi, g), dirOf(a), 760.f, c("drift_damage", 8.f), 2.4f, 0.3f, 0);
                    flash(bi, g);
                    ++m_fired[g];
                }
            }
            if (m_fired[3] == 0 && m_stepT > c("drift_time", 3.6f) * 0.4f) {
                m_fired[3] = 1;
                for (int k = 0; k < 4; ++k) {
                    const int side = k % 2, tube = k / 2;
                    const sol::table cfg = m_def->config;
                    m_ef->createEnemyRocket(*m_em, podMuzzle(bi, side, tube),
                        m_em->transforms[bi].rotation + (side == 0 ? 1.f : -1.f) * c("rocket_spread", 100.f), m_bossId, m_playerId, m_world, cfg);
                }
                m_em->sfx("maniac.rocket.launch", bp);
            }
            if (m_stepT >= c("drift_time", 3.6f)) { m_driftStep = 2; m_stepT = 0.f; m_em->sfx("tell.ram", bp); }
            return false;
        }
        if (m_driftStep == 2) {                // wind up: brake hard, swing on you, lock
            const b2Vec2 v = b2Body_GetLinearVelocity(b);
            b2Body_SetLinearVelocity(b, { v.x * std::exp(-9.f * dt), v.y * std::exp(-9.f * dt) });
            if (m_stepT < m_ramWindup * 0.75f) m_ramDir = norm(predict(pi, 0.1f) - bp);
            face(bi, headingOf(m_ramDir), 600.f);
            for (int g = 0; g < guns(bi); ++g) heat(bi, g, m_stepT / m_ramWindup);
            ghost(bi, false, dt);
            if ((rand() % 100) < 70) {
                const sf::Vector2f sp2 = localToWorld(bi, { frange(-50.f, 50.f), frange(-60.f, 60.f) });
                m_em->particles.push_back({ sp2, (sp2 - bp) * 2.f, sf::Color(255, 200, 120, 230), 0.25f, 0.25f, 3.f });
            }
            if (m_stepT >= m_ramWindup) {
                m_driftStep = 3; m_stepT = 0.f; m_ramHit = false; m_ramStall = 0.f; m_ramFrom = bp;
                m_em->sfx("tell.ram.charge", bp, 1.f, 1.3f);
                m_em->spawnShockRing(bp, 30.f, 260.f, 0.25f, sf::Color(255, 240, 210), 8.f, 250.f);
                m_em->addTrauma(0.5f);
            }
            return false;
        }
        if (m_driftStep == 3) {                // THE RAM: lightning fast
            const float rs = c("ram_speed", 2600.f);
            const b2Vec2 rv = b2Body_GetLinearVelocity(b);
            if (m_stepT > 0.12f && len(sf::Vector2f(rv.x, rv.y) * SCALE) < rs * 0.3f) m_ramStall += dt;
            else m_ramStall = 0.f;
            const sf::Vector2f v = m_ramDir * rs;
            b2Body_SetLinearVelocity(b, { v.x / SCALE, v.y / SCALE });
            face(bi, headingOf(m_ramDir), 600.f);
            ghost(bi, true, dt);
            turboFx(bi, m_ramDir, true);
            // You: hit unless your dodge is up.
            const auto& ps = m_em->players[pi];
            const sf::Vector2f rel = pp - bp;
            const float along = rel.x * m_ramDir.x + rel.y * m_ramDir.y;
            const float side = std::fabs(rel.x * m_ramDir.y - rel.y * m_ramDir.x);
            if (!m_ramHit && along > -hullR() * 0.6f && along < hullR() + 40.f && side < hullR() * 0.8f) {
                if (ps.dodgeIframeTimer > 0.f) m_ramHit = true;          // dodged through
                else { m_ramHit = true; ramPlayer(pi, bp); m_driftStep = 4; m_stepT = 0.f; return false; }
            }
            // The cage: reached it, or stopped dead against it.
            if (m_em->bossArena.wallDist(bp) < hullR() * 0.85f || m_ramStall > 0.05f
                || m_stepT > (2.2f * m_R) / rs + 0.3f) {
                wallSlam(bi);
                return true;
            }
            return false;
        }
        // 4: he got you -- brake, back to the fight
        const b2Vec2 v = b2Body_GetLinearVelocity(b);
        b2Body_SetLinearVelocity(b, { v.x * std::exp(-4.f * dt), v.y * std::exp(-4.f * dt) });
        return m_stepT > 0.8f;
    }

    /// An afterimage of the hull, every few hundredths of a second.
    void ghost(size_t bi, bool ram, float dt) {
        m_ghostT -= dt;
        if (m_ghostT > 0.f) return;
        m_ghostT = ram ? 0.016f : 0.035f;
        Ghost g;
        g.pos = m_em->transforms[bi].position;
        g.rot = m_em->transforms[bi].rotation;
        g.maxLife = g.life = ram ? 0.22f : 0.3f;
        g.ram = ram;
        m_ghosts.push_back(g);
    }
    /// The turbo drive: a plasma plume off the stern, speed streaks round him.
    void turboFx(size_t bi, sf::Vector2f dir, bool ram) {
        const float sc = m_def->config["scale"].get_or(2.f);
        const sf::Vector2f bp = m_em->transforms[bi].position;
        for (int k = 0; k < (ram ? 6 : 3); ++k) {
            const sf::Vector2f stern = localToWorld(bi, { frange(-14.f, 14.f) * sc * 0.5f, 40.f * sc });
            m_em->particles.push_back({ stern, -dir * frange(300.f, 700.f) + sf::Vector2f(frange(-60.f, 60.f), frange(-60.f, 60.f)),
                (k % 2) ? sf::Color(255, 220, 160, 230) : sf::Color(255, 120, 50, 220), 0.3f, 0.3f, frange(3.f, 6.f) });
        }
        // speed streaks: long thin motes along the way he goes
        const sf::Vector2f side(-dir.y, dir.x);
        for (int k = 0; k < (ram ? 4 : 2); ++k) {
            const sf::Vector2f p = bp + side * frange(-hullR() * 1.3f, hullR() * 1.3f) + dir * frange(-hullR(), hullR());
            m_em->particles.push_back({ p, -dir * (ram ? 1600.f : 700.f), sf::Color(255, 230, 200, 160), 0.12f, 0.12f, 2.f });
        }
    }

    void ramPlayer(size_t pi, sf::Vector2f bp) {
        auto& ps = m_em->players[pi];
        m_em->damagePlayer(pi, m_em->healths[pi].maxHp * c("ram_damage", 0.5f));
        const sf::Vector2f away = norm(m_em->transforms[pi].position - bp);
        const sf::Vector2f kdir = norm(m_ramDir * 0.6f + away * 0.4f);
        ps.staggerImmuneTimer = 0.f;   // this one always lands
        m_em->staggerPlayer(pi, kdir, c("ram_knock", 1600.f), c("ram_stun", 6.f) - 0.6f, 0.6f, 280.f, 1e6f);
        m_em->spawnScreenFlash(sf::Color(255, 70, 40), 0.4f, 140.f);
        m_em->spawnShockRing(m_em->transforms[pi].position, 20.f, 240.f, 0.4f, sf::Color(255, 120, 60), 8.f, 240.f);
        m_em->requestHitstop(0.1f, 0.35f, 0.3f);
        m_em->addTrauma(1.f);
        m_em->sfxUi("player.hit.heavy");
        banner("RAMMED", "", sf::Color(255, 90, 60));
    }

    void wallSlam(size_t bi) {
        auto& h = m_em->healths[bi];
        const sf::Vector2f bp = m_em->transforms[bi].position;
        const b2BodyId b = m_em->physics[bi].bodyId;
        // the edge he hit
        const auto& A = m_em->bossArena;
        sf::Vector2f n(0.f, -1.f); float best = -1e9f;
        for (int k = 0; k < A.sides; ++k) {
            const sf::Vector2f nk = A.normal(k);
            const float d = (bp.x - m_home.x) * nk.x + (bp.y - m_home.y) * nk.y;
            if (d > best) { best = d; n = nk; }
        }
        const sf::Vector2f back = -n * 220.f;
        b2Body_SetLinearVelocity(b, { back.x / SCALE, back.y / SCALE });
        const float dmg = c("wall_damage", 300.f);
        h.currentHp -= dmg;
        if (h.armourMark >= 0.f) h.armourMark -= dmg;   // the cage goes through the armour
        m_em->enemies[bi].hitFlashTimer = 0.4f;
        m_stun = c("wall_stun", 6.f);
        const sf::Vector2f hit = bp + n * std::max(0.f, A.wallDist(bp));
        m_em->spawnExplosion(hit, sf::Color(255, 150, 70), 60, 6.f);
        m_em->spawnExplosion(hit, sf::Color(255, 230, 180), 30, 5.f);
        m_em->spawnShockRing(hit, 20.f, 420.f, 0.55f, sf::Color(255, 160, 80), 9.f, 240.f);
        for (int k = 0; k < 6; ++k) addBolt(hit + sf::Vector2f(frange(-60.f, 60.f), frange(-60.f, 60.f)), -n, 120.f);
        m_em->addTrauma(1.f);
        m_em->requestHitstop(0.06f, 0.3f, 0.35f);
        m_em->sfx("boss.ram.wall", hit);
        breach(bi, "HE HIT THE CAGE // STUNNED");
    }

    // ========================================================================
    // DEATH
    // ========================================================================
    void startDying(size_t bi) {
        m_stage = Stage::Dying;
        m_stageT = 0.f;
        m_atk = Atk::None;
        m_sectors.clear();
        for (auto& m : m_markers) m.done = true;
        for (auto& g : m_gun) g = Gun{};
        m_laserPath.clear();
        m_podGlow = 0.f;
        m_breach = 0.f;
        m_em->healths[bi].armour = 1.f;   // nothing more to take
        // Cracks from the reactor out to the hull's edge.
        m_cracks.clear();
        const auto& vis = m_def->visual;
        const sf::Vector2f core(0.f, -4.6f * m_def->config["scale"].get_or(2.f));
        const int n = 9;
        for (int k = 0; k < n && !vis.empty(); ++k) {
            const sf::Vector2f end = vis[(k * vis.size() / n + rand() % 2) % vis.size()] * 0.92f;
            Crack cr;
            cr.at = 0.15f + k * 0.22f + frand() * 0.1f;
            cr.pts.push_back(core);
            const int seg = 5;
            for (int s = 1; s <= seg; ++s) {
                const float u = static_cast<float>(s) / seg;
                sf::Vector2f p = core + (end - core) * u;
                if (s < seg) p += sf::Vector2f(frange(-7.f, 7.f), frange(-7.f, 7.f));
                cr.pts.push_back(p);
            }
            m_cracks.push_back(cr);
        }
        m_em->sfx("boss.death.alarm", m_em->transforms[bi].position);
        banner("REACTOR CRITICAL", "", sf::Color(255, 200, 120));
    }

    void updateDying(float dt, size_t bi, size_t pi) {
        m_stageT += dt;
        auto& ec = m_em->enemies[bi];
        const float T = c("death_time", 3.2f);
        const float u = std::clamp(m_stageT / T, 0.f, 1.f);
        const b2BodyId b = m_em->physics[bi].bodyId;
        const b2Vec2 v = b2Body_GetLinearVelocity(b);
        b2Body_SetLinearVelocity(b, { v.x * std::exp(-3.f * dt), v.y * std::exp(-3.f * dt) });
        b2Body_SetAngularVelocity(b, 0.25f + 0.6f * u);
        for (int g = 0; g < 3; ++g) { ec.mountHeat[g] = std::min(1.f, u * 1.4f); ec.mountAngle[g] += (g - 1) * 40.f * dt; }
        updateMarkers(dt, (size_t)-1, pi);
        m_em->cameraTrauma = std::max(m_em->cameraTrauma, 0.2f + 0.5f * u);
        // Small blasts across the hull, quicker as it goes.
        m_popT -= dt;
        if (m_popT <= 0.f) {
            m_popT = 0.32f - 0.24f * u;
            const auto& vis = m_def->visual;
            if (!vis.empty()) {
                const sf::Vector2f l = vis[rand() % vis.size()] * frange(0.2f, 0.8f);
                const sf::Vector2f p = localToWorld(bi, l);
                m_em->spawnExplosion(p, sf::Color(255, 140, 60), 14, 3.5f);
                m_em->spawnShockRing(p, 4.f, 60.f + 60.f * u, 0.25f, sf::Color(255, 180, 90), 3.f, 220.f);
                m_em->sfx("rock.magma.explode", p, 0.45f + 0.3f * u, 1.2f);
            }
        }
        if (m_stageT < T) return;

        // ---- The burst ----
        const sf::Vector2f bp = m_em->transforms[bi].position;
        m_deathPos = bp;
        m_deathRot = m_em->transforms[bi].rotation;
        m_deathVel = sf::Vector2f(v.x * SCALE, v.y * SCALE);
        m_em->spawnExplosion(bp, sf::Color(255, 240, 200), 70, 7.f);
        m_em->spawnExplosion(bp, sf::Color(255, 120, 50), 90, 8.f);
        m_em->spawnShockRing(bp, 40.f, 700.f, 0.8f, sf::Color(255, 200, 120), 14.f, 250.f);
        m_em->spawnShockRing(bp, 20.f, 420.f, 0.5f, sf::Color(255, 255, 240), 8.f, 250.f);
        m_em->spawnScreenFlash(sf::Color(255, 240, 210), 0.6f, 200.f);
        m_em->requestHitstop(0.12f, 0.6f, 0.25f);
        m_em->addTrauma(1.f);
        m_em->sfx("boss.death.blast", bp);
        // The hull becomes the chunks: no flying wreck shards on top of them.
        m_em->healths[bi].holdDeath = false;
        m_em->healths[bi].armourMark = -1.f;
        m_em->healths[bi].currentHp = -1.f;   // DamageSystem: scrap, the kill
        m_pending = true;
        m_cracks.clear();
    }

    /// The frame after: his hull in breakable chunks, the loot, the cage off.
    void burst() {
        if (!m_cfg.valid() || !m_def) return;
        spawnChunks();
        const sf::Vector2f bp = m_deathPos;
        sol::optional<sol::table> loot = m_cfg["loot"];
        const int kits = loot ? (*loot)["kits"].get_or(2) : 2;
        const int beacons = loot ? (*loot)["beacons"].get_or(2) : 2;
        for (int k = 0; k < kits; ++k) m_em->spawnPickup(bp, m_deathVel * 0.2f, 1);
        for (int k = 0; k < beacons; ++k) m_em->spawnPickup(bp, m_deathVel * 0.2f, 2);
        banner("DREADNOUGHT DESTROYED", "THE CAGE IS SHUTTING DOWN", sf::Color(255, 206, 110));
        m_em->sfxUi("hud.boss.defeated");
        startCollapse();
        m_em->camLock = false;
    }

    /// His own hull, cut (ShipShatter), each piece a breakable rock that
    /// barely drifts -- you are meant to break them, not chase them.
    void spawnChunks() {
        if (!m_enemies) return;
        const auto& d = *m_def;
        if (d.visualTris.size() < 3) return;
        const auto rr = range2("chunks", 4.f, 6.f);
        const int want = static_cast<int>(rr.first) + rand() % (static_cast<int>(rr.second - rr.first) + 1);
        auto pieces = shatter::slice(d.visual, d.visualTris, want <= 4 ? 2 : 3);
        std::sort(pieces.begin(), pieces.end(), [](const shatter::Piece& a, const shatter::Piece& b) { return a.area > b.area; });
        if (static_cast<int>(pieces.size()) > want) pieces.resize(static_cast<size_t>(want));

        const std::string type = m_cfg["chunk_type"].get_or(std::string("DREAD_CHUNK"));
        sol::optional<sol::table> tc = (*m_lua)["asteroid_types"][type];
        const float base = tc ? (*tc)["base_size"].get_or(2.4f) * SCALE : 72.f;
        const float vmin = tc ? (*tc)["size_variance_min"].get_or(0.5f) : 0.5f;
        const float vmax = tc ? (*tc)["size_variance_max"].get_or(1.4f) : 1.4f;
        const float damp = c("chunk_damping", 3.5f);

        const float rr0 = m_deathRot * 0.0174533f, cs = std::cos(rr0), sn = std::sin(rr0);
        const sf::Color hull = d.color;
        const auto shade = [](sf::Color col, float k, uint8_t a = 255) {
            return sf::Color(static_cast<uint8_t>(std::clamp(col.r * k, 0.f, 255.f)), static_cast<uint8_t>(std::clamp(col.g * k, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(col.b * k, 0.f, 255.f)), a);
            };
        const sf::Color fill = shade(hull, 0.62f);
        const sf::Color skin(static_cast<uint8_t>(std::min(255, hull.r + 40)), static_cast<uint8_t>(std::min(255, hull.g + 30)),
            static_cast<uint8_t>(std::min(255, hull.b + 30)), 230);
        const sf::Color scar(255, 140, 60, 235);

        for (const auto& pc : pieces) {
            if (pc.tris.size() < 3) continue;
            const sf::Vector2f wc(pc.centre.x * cs - pc.centre.y * sn, pc.centre.x * sn + pc.centre.y * cs);
            const sf::Vector2f at = m_deathPos + wc;
            const float roll = std::clamp((pc.radius * 0.8f / base - vmin) / std::max(0.01f, vmax - vmin), 0.f, 1.f);
            const sf::Vector2f out = norm(wc);
            const sf::Vector2f vel = (out * frange(40.f, 80.f) + m_deathVel * 0.15f) / SCALE;   // m/s, and it settles fast
            const uint32_t id = m_enemies->spawnAsteroid(type, at, vel, roll);
            const size_t j = id ? m_em->getEntityIndex(id) : (size_t)-1;
            if (j == (size_t)-1) continue;
            const b2BodyId b = m_em->physics[j].bodyId;
            if (b2Body_IsValid(b)) {
                b2Body_SetTransform(b, { at.x / SCALE, at.y / SCALE }, b2MakeRot(rr0));
                b2Body_SetLinearDamping(b, damp);
                b2Body_SetAngularDamping(b, damp);
                b2Body_SetAngularVelocity(b, frange(-0.6f, 0.6f));
            }
            m_em->transforms[j].rotation = m_deathRot;
            // ---- The look: this piece of HIS hull, plates and all ----
            auto& rd = m_em->renders[j];
            rd.detailTris.clear();
            rd.detailLines.clear();
            for (const auto& v : pc.tris) rd.detailTris.push_back(sf::Vertex{ v, fill });
            for (const auto& pl : d.plates) {
                if (pl.points.empty() || pl.tris.empty()) continue;
                sf::Vector2f cen(0.f, 0.f);
                for (const auto& p : pl.points) cen += p;
                cen /= static_cast<float>(pl.points.size());
                if (!insideTris(pc.tris, cen - pc.centre)) continue;
                const sf::Color pf = pl.accent ? shade(hull, 1.1f) : shade(hull, std::max(0.f, pl.shade) * 0.62f);
                for (const auto& v : pl.tris) rd.detailTris.push_back(sf::Vertex{ v - pc.centre, pf });
            }
            for (const auto& v : pc.skin) rd.detailLines.push_back(sf::Vertex{ v, skin });
            for (const auto& v : pc.scar) rd.detailLines.push_back(sf::Vertex{ v, scar });
            rd.shape.setFillColor(fill);
            rd.shape.setOutlineThickness(0.f);
            rd.fieldStyle = RenderComponent::FieldStyle::None;
        }
    }
    static bool insideTris(const std::vector<sf::Vector2f>& tris, sf::Vector2f p) {
        const auto cross = [](sf::Vector2f a, sf::Vector2f b, sf::Vector2f c0) { return (b.x - a.x) * (c0.y - a.y) - (b.y - a.y) * (c0.x - a.x); };
        for (size_t k = 0; k + 2 < tris.size(); k += 3) {
            const float d1 = cross(tris[k], tris[k + 1], p), d2 = cross(tris[k + 1], tris[k + 2], p), d3 = cross(tris[k + 2], tris[k], p);
            const bool neg = d1 < 0.f || d2 < 0.f || d3 < 0.f, pos = d1 > 0.f || d2 > 0.f || d3 > 0.f;
            if (!(neg && pos)) return true;
        }
        return false;
    }

    // ---- The cage switching off ----
    void startCollapse() {
        if (m_collapse >= 0.f) return;
        m_collapse = 0.f;
        if (b2Body_IsValid(m_wallBody)) b2DestroyBody(m_wallBody);
        m_wallBody = b2_nullBodyId;
        m_em->bossArena.walled = false;
        m_em->camLock = false;
        m_em->sfx("boss.breach", m_home, 0.6f, 0.6f);
    }
    void updateCollapse(float dt) {
        const float before = m_collapse;
        m_collapse += dt;
        const float T = 1.4f;
        // the buoys pop one by one, after the beams die
        for (size_t k = 0; k < m_buoys.size(); ++k) {
            const float at = 0.6f + k * 0.12f;
            if (before < at && m_collapse >= at) {
                const sf::Vector2f p = m_buoys[k].pos;
                m_em->spawnExplosion(p, sf::Color(255, 150, 80), 18, 3.5f);
                m_em->spawnExplosion(p, sf::Color(120, 100, 95), 10, 3.f);
                m_em->spawnShockRing(p, 4.f, 70.f, 0.25f, sf::Color(255, 170, 90), 3.f, 230.f);
                if (k % 2 == 0) m_em->sfx("rock.magma.explode", p, 0.5f, 1.4f);
            }
        }
        if (m_collapse >= T) {
            m_buoys.clear();
            m_collapse = -1.f;
            m_em->bossArena = EntityManager::BossArena{};
            m_em->camLock = false;
        }
    }

    // ========================================================================
    // THE CAGE -- buoys and high-voltage beams
    // ========================================================================
    void updateFx(float dt) {
        for (auto& g : m_ghosts) g.life -= dt;
        m_ghosts.erase(std::remove_if(m_ghosts.begin(), m_ghosts.end(), [](const Ghost& g) { return g.life <= 0.f; }), m_ghosts.end());
        for (auto& b : m_bolts) b.life -= dt;
        m_bolts.erase(std::remove_if(m_bolts.begin(), m_bolts.end(), [](const Bolt& b) { return b.life <= 0.f; }), m_bolts.end());
        for (auto& b : m_buoys) if (b.lockFlash > 0.f) b.lockFlash -= dt;
        // the beams' jitter: fresh every few hundredths
        m_jitT -= dt;
        if (m_jitT <= 0.f) {
            m_jitT = 0.045f;
            for (auto& e : m_jit) for (auto& v : e) v = frange(-1.f, 1.f);
        }
        // branches crawl off the live beams
        if (m_em && m_em->bossArena.walled && m_collapse < 0.f) {
            m_boltAcc += dt * 15.f * static_cast<float>(m_buoys.size());
            while (m_boltAcc >= 1.f && !m_buoys.empty()) {
                m_boltAcc -= 1.f;
                const size_t k = static_cast<size_t>(rand()) % m_buoys.size();
                const sf::Vector2f a = m_buoys[k].pos, b = m_buoys[(k + 1) % m_buoys.size()].pos;
                const float s = frange(0.08f, 0.92f);
                const sf::Vector2f d = norm(b - a), n(-d.y, d.x);
                const auto pts = beamPoints(a, b, static_cast<int>(k), 1.f);
                const sf::Vector2f at = pts[std::min(pts.size() - 1, static_cast<size_t>(s * (pts.size() - 1)))];
                addBolt(at, (rand() % 2 ? n : -n) + d * frange(-0.6f, 0.6f), frange(30.f, 95.f));
                // now and then a branch forks again
                if ((rand() % 100) < 30 && !m_bolts.empty()) {
                    const auto& last = m_bolts.back().pts;
                    addBolt(last[last.size() / 2], (rand() % 2 ? n : -n) + d * frange(-1.f, 1.f), frange(20.f, 45.f));
                }
            }
            if ((rand() % 100) < 25 && !m_buoys.empty()) {
                const sf::Vector2f p = m_buoys[static_cast<size_t>(rand()) % m_buoys.size()].pos;
                m_em->particles.push_back({ p, sf::Vector2f(frange(-120.f, 120.f), frange(-120.f, 120.f)), sf::Color(255, 210, 140, 230), 0.25f, 0.25f, 2.5f });
            }
        }
    }

    void addBolt(sf::Vector2f from, sf::Vector2f dir, float length) {
        Bolt b;
        b.pts.push_back(from);
        dir = norm(dir);
        const int segs = 3 + rand() % 3;
        sf::Vector2f p = from;
        for (int s = 0; s < segs; ++s) {
            const float a = std::atan2(dir.y, dir.x) + frange(-0.8f, 0.8f);
            p += sf::Vector2f(std::cos(a), std::sin(a)) * (length / segs);
            b.pts.push_back(p);
        }
        b.maxLife = b.life = frange(0.07f, 0.2f);
        m_bolts.push_back(b);
    }

    /// One beam, buoy to buoy: a slow wave and a fast crackle, pinned at the
    /// ends. `upTo` 0..1 draws it only part-way (it lighting up).
    std::vector<sf::Vector2f> beamPoints(sf::Vector2f a, sf::Vector2f b, int k, float upTo) const {
        const float L = len(b - a);
        const int N = std::clamp(static_cast<int>(L / 26.f), 8, 48);
        const sf::Vector2f d = norm(b - a), n(-d.y, d.x);
        const auto& jit = m_jit[static_cast<size_t>(k) % m_jit.size()];
        std::vector<sf::Vector2f> pts;
        const float amp = c("beam_wave", 10.f), jamp = c("beam_crackle", 5.f);
        for (int i = 0; i <= N; ++i) {
            const float s = static_cast<float>(i) / N;
            if (s > upTo + 1e-4f) { pts.push_back(a + (b - a) * upTo); break; }
            const float env = std::pow(std::sin(s * 3.14159f), 0.7f);
            const float wave = 0.6f * std::sin(6.2831853f * 1.5f * s + m_time * 6.f + k * 1.7f)
                + 0.35f * std::sin(6.2831853f * 4.f * s - m_time * 13.f + k);
            const float off = env * (amp * wave + jamp * jit[static_cast<size_t>(i) % jit.size()]);
            pts.push_back(a + (b - a) * s + n * off);
        }
        return pts;
    }

    void drawCage() {
        if (m_buoys.empty()) return;
        // the beams fade out (with a stutter) when the cage shuts down
        float alpha = 1.f;
        if (m_collapse >= 0.f) {
            alpha = std::clamp(1.f - m_collapse / 0.6f, 0.f, 1.f);
            if (std::fmod(m_time, 0.09f) < 0.04f) alpha *= 0.3f;
        }
        const auto A8 = [&](float a) { return static_cast<uint8_t>(std::clamp(a * alpha, 0.f, 255.f)); };
        sf::VertexArray glow(sf::PrimitiveType::Triangles), mid(sf::PrimitiveType::Triangles), core(sf::PrimitiveType::Triangles);
        const size_t n = m_buoys.size();
        for (size_t k = 0; k < n && alpha > 0.01f; ++k) {
            const Buoy& b0 = m_buoys[k];
            const Buoy& b1 = m_buoys[(k + 1) % n];
            if (b0.ignite <= 0.f) continue;
            const auto pts = beamPoints(b0.pos, b1.pos, static_cast<int>(k), b0.ignite);
            const float flick = 0.8f + 0.2f * std::sin(m_time * 37.f + k * 2.f);
            strip(glow, pts, 34.f, sf::Color(255, 55, 15, A8(45 * flick)));
            strip(glow, pts, 16.f, sf::Color(255, 90, 30, A8(80 * flick)));
            strip(mid, pts, 7.f, sf::Color(255, 140, 50, A8(190 * flick)));
            strip(core, pts, 3.f, sf::Color(255, 240, 200, A8(255)));
            // the lighting front
            if (b0.ignite < 1.f) disc(pts.back(), 12.f, sf::Color(255, 240, 200, A8(230)));
            // surges: a hot knot that runs along the cable now and then
            else {
                const float ph = std::fmod(m_time * 0.9f + k * 0.37f, 1.6f);
                if (ph < 1.f) {
                    const sf::Vector2f p = pts[std::min(pts.size() - 1, static_cast<size_t>(ph * (pts.size() - 1)))];
                    disc(p, 16.f, sf::Color(255, 120, 40, A8(90)));
                    disc(p, 6.f, sf::Color(255, 250, 225, A8(240)));
                }
            }
        }
        for (const auto& bo : m_bolts) {
            const float u = std::clamp(bo.life / bo.maxLife, 0.f, 1.f);
            strip(glow, bo.pts, 9.f, sf::Color(255, 90, 30, A8(80 * u)));
            strip(core, bo.pts, 2.f, sf::Color(255, 225, 170, A8(240 * u)));
        }
        m_window->draw(glow);
        m_window->draw(mid);
        m_window->draw(core);
        // ---- The buoys: crude hex pods, a hot eye, three clamp prongs ----
        for (size_t k = 0; k < n; ++k) {
            const Buoy& b = m_buoys[k];
            if (!b.launched) continue;
            const float rot = b.locked ? std::atan2(b.pos.y - m_home.y, b.pos.x - m_home.x) : m_time * b.spin;
            const float r = 13.f;
            for (int q = 0; q < 3; ++q) {
                const float a = rot + 3.14159f + (q - 1) * 0.7f;
                const sf::Vector2f tip = b.pos + sf::Vector2f(std::cos(a), std::sin(a)) * (r + (b.locked ? 9.f : 4.f));
                line(b.pos, tip, 3.f, sf::Color(70, 50, 46, 255));
            }
            sf::ConvexShape h(6);
            for (int q = 0; q < 6; ++q) { const float a = rot + q * 1.0471976f; h.setPoint(q, sf::Vector2f(std::cos(a), std::sin(a)) * r); }
            h.setPosition(b.pos);
            h.setFillColor(sf::Color(54, 36, 34));
            h.setOutlineThickness(2.f);
            h.setOutlineColor(sf::Color(255, 120, 50, A8(255)));
            m_window->draw(h);
            const float pulse = 0.6f + 0.4f * std::sin(m_time * 9.f + k);
            disc(b.pos, 9.f * pulse + (b.lockFlash > 0.f ? 14.f * b.lockFlash / 0.3f : 0.f), sf::Color(255, 90, 40, A8(90)));
            disc(b.pos, 5.f, sf::Color(255, 180, 110, A8(255)));
            disc(b.pos, 2.2f, sf::Color(255, 250, 230, A8(255)));
        }
    }

    // ========================================================================
    // DRAW HELPERS
    // ========================================================================
    void drawCore(size_t i, bool fighting) {
        const auto& d = m_registry->resolve(m_em->enemies[i].archetype);
        const float sc = d.config["scale"].get_or(2.f);
        const sf::Vector2f p = localToWorld(i, { 0.f, -4.6f * sc });
        float pulse = 0.5f + 0.5f * std::sin(m_time * (fighting ? 4.f : 1.6f));
        sf::Color col(255, 70, 60);
        float r = 3.6f * sc;
        if (fighting && m_breach > 0.f) { col = sf::Color(255, 150, 50); pulse = 0.5f + 0.5f * std::sin(m_time * 14.f); r *= 1.25f; }
        if (fighting && m_stage == Stage::Dying) {
            const float u = std::clamp(m_stageT / c("death_time", 3.2f), 0.f, 1.f);
            col = sf::Color(255, static_cast<uint8_t>(150 + 105 * u), static_cast<uint8_t>(90 + 165 * u));
            r *= 1.f + 1.8f * u;
            pulse = 0.5f + 0.5f * std::sin(m_time * (10.f + 30.f * u));
        }
        disc(p, r * (2.2f + 0.6f * pulse), sf::Color(col.r, col.g, col.b, static_cast<uint8_t>(40 + 40 * pulse)));
        disc(p, r, col);
        disc(p, r * 0.45f, sf::Color(255, 235, 220));
    }

    void drawCracks(size_t bi) {
        const float T = c("death_time", 3.2f);
        const float u = std::clamp(m_stageT / T, 0.f, 1.f);
        // the hull itself going hot
        const auto& d = *m_def;
        if (!d.visualTris.empty()) {
            sf::VertexArray va(sf::PrimitiveType::Triangles, d.visualTris.size());
            const sf::Color hot(255, static_cast<uint8_t>(120 + 100 * u), 60, static_cast<uint8_t>(150 * u * u));
            for (size_t k = 0; k < d.visualTris.size(); ++k) va[k] = sf::Vertex{ localToWorld(bi, d.visualTris[k]), hot };
            m_window->draw(va);
        }
        for (const auto& cr : m_cracks) {
            if (m_stageT < cr.at) continue;
            const float g = std::clamp((m_stageT - cr.at) / 0.5f, 0.f, 1.f);
            const size_t shown = 1 + static_cast<size_t>(std::ceil(g * (cr.pts.size() - 1)));
            for (size_t k = 1; k < shown && k < cr.pts.size(); ++k) {
                const sf::Vector2f a = localToWorld(bi, cr.pts[k - 1]), b = localToWorld(bi, cr.pts[k]);
                line(a, b, 7.f + 6.f * u, sf::Color(255, 120, 40, 120));
                line(a, b, 2.5f + 2.f * u, sf::Color(255, static_cast<uint8_t>(210 + 45 * u), static_cast<uint8_t>(150 + 100 * u)));
            }
        }
    }

    /// Frame A: the saw-tooth Rakshari frame, a small jawless skull after
    /// the name, % on the right, the armour strip under it.
    void drawBar(sf::Vector2f ws, size_t bi) {
        const auto& h = m_em->healths[bi];
        const float k = ws.y / 1080.f;
        const float w = std::min(900.f * k, ws.x * 0.56f), hh = 16.f * k, sk = 10.f * k;
        const float slide = 1.f - std::pow(1.f - m_barShown, 3.f);
        const float x = ws.x * 0.5f - w * 0.5f, y = ws.y - 96.f * k + (1.f - slide) * 140.f * k;
        const uint8_t A = static_cast<uint8_t>(255 * slide);
        const auto col = [&](int r, int g, int b) { return sf::Color(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), A); };
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        const auto tri = [&](sf::Vector2f a, sf::Vector2f b, sf::Vector2f c0, sf::Color cc) {
            va.append(sf::Vertex{ a, cc }); va.append(sf::Vertex{ b, cc }); va.append(sf::Vertex{ c0, cc });
            };
        const auto q4 = [&](sf::Vector2f a, sf::Vector2f b, sf::Vector2f c0, sf::Vector2f d, sf::Color cc) { tri(a, b, c0, cc); tri(a, c0, d, cc); };
        const auto para = [&](float px, float py, float pw, float ph, sf::Color cc, float s) {
            q4({ px + s, py }, { px + pw + s, py }, { px + pw, py + ph }, { px, py + ph }, cc);
            };
        const sf::Color RUST = col(150, 52, 44), RUST_HI = col(214, 96, 78), RUST_DK = col(64, 20, 20), BOLT = col(232, 150, 128);
        // ---- frame ----
        const float e8 = 8.f * k, e5 = 5.f * k;
        q4({ x - 14.f * k + sk, y - e8 }, { x + w + sk + 14.f * k, y - e8 }, { x + w + 14.f * k, y + hh + e8 }, { x - 14.f * k, y + hh + e8 }, RUST_DK);
        q4({ x - 10.f * k + sk, y - e5 }, { x + w + sk + 10.f * k, y - e5 }, { x + w + 10.f * k, y + hh + e5 }, { x - 10.f * k, y + hh + e5 }, RUST);
        for (float t = 0.f; t < w + 20.f * k; t += 22.f * k) {
            const float bx = x + sk - 4.f * k + t;
            tri({ bx, y - e8 }, { bx + 11.f * k, y - e8 }, { bx + 6.f * k, y - 15.f * k }, RUST);
        }
        // hooked blade ends
        q4({ x - 14.f * k, y + hh + e8 }, { x - 38.f * k, y + hh + 16.f * k }, { x - 22.f * k, y - e8 }, { x - 14.f * k + sk, y - e8 }, RUST);
        tri({ x - 14.f * k, y + hh + e8 }, { x - 38.f * k, y + hh + 16.f * k }, { x - 30.f * k, y + hh + e8 }, RUST_HI);
        q4({ x + w + sk + 14.f * k, y - e8 }, { x + w + sk + 38.f * k, y - 16.f * k }, { x + w + 22.f * k + sk, y + hh + e8 }, { x + w + 14.f * k, y + hh + e8 }, RUST);
        tri({ x + w + sk + 14.f * k, y - e8 }, { x + w + sk + 38.f * k, y - 16.f * k }, { x + w + sk + 30.f * k, y - e8 }, RUST_HI);
        for (float t = 0.f; t <= w; t += 90.f * k)
            q4({ x + t + 4.f * k, y + hh + e5 }, { x + t + 8.f * k, y + hh + e5 }, { x + t + 8.f * k, y + hh + 7.f * k }, { x + t + 4.f * k, y + hh + 7.f * k }, BOLT);
        // ---- the bar ----
        const float frac = std::clamp(h.currentHp / std::max(1.f, h.maxHp), 0.f, 1.f);
        const bool br = m_breach > 0.f;
        para(x, y, w, hh, col(60, 14, 22), sk);
        if (m_barGhost > frac) para(x + w * frac, y, w * (m_barGhost - frac), hh, col(255, 225, 215), sk);
        const sf::Color red = br ? col(255, 120, 40) : col(236, 56, 76), hi = br ? col(255, 170, 110) : col(252, 98, 112);
        if (frac > 0.f) { para(x, y, w * frac, hh, red, sk); para(x + sk * (1.f - 5.f / 16.f), y, w * frac, 5.f * k, hi, sk * 5.f / 16.f); }
        if (m_flashBar > 0.f) para(x, y, w, hh, sf::Color(255, 230, 200, static_cast<uint8_t>(A * m_flashBar / 0.6f * 0.7f)), sk);
        for (int s = 1; s < 10; ++s) {
            const float xx = x + w * s / 10.f;
            q4({ xx + sk - 1.5f * k, y }, { xx + sk + 1.5f * k, y }, { xx + 1.5f * k, y + hh }, { xx - 1.5f * k, y + hh }, col(14, 10, 16));
        }
        // ---- armour strip ----
        const float sy = y + hh + 16.f * k;
        para(x, sy, w, 5.f * k, col(40, 32, 14), 4.f * k);
        const float armour = br ? std::clamp(m_breach / std::max(0.1f, c("breach_time", 6.f)), 0.f, 1.f) : 1.f;
        if (m_stage != Stage::Dying) para(x, sy, w * armour, 5.f * k, br ? col(255, 120, 40) : col(255, 190, 40), 4.f * k);
        m_window->draw(va);

        // ---- text ----
        const unsigned big = static_cast<unsigned>(std::round(24.f * k)), small = static_cast<unsigned>(std::round(14.f * k));
        ui::label(*m_window, m_font, x + 4.f * k, y - 44.f * k, m_name, big, col(255, 206, 110));
        const float tw = ui::labelWidth(m_font, m_name, big);
        drawSkull(x + 4.f * k + tw + 10.f * k, y - 38.f * k, std::max(2.f, std::round(3.f * k)), A);
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(std::ceil(frac * 100.f - 0.001f)));
        const std::string ps(pct);
        ui::label(*m_window, m_font, x + w + sk - ui::labelWidth(m_font, ps, big), y - 44.f * k, ps, big, col(255, 232, 224));
        std::string sub;
        if (m_stage == Stage::Dying) sub = "REACTOR CRITICAL";
        else if (br) { char b[48]; std::snprintf(b, sizeof b, "ARMOUR BREACHED  %.1fs", m_breach); sub = b; }
        else { char b[32]; std::snprintf(b, sizeof b, "ARMOUR %d%%", static_cast<int>(std::round(c("armour", 0.8f) * 100.f))); sub = b; }
        ui::label(*m_window, m_font, x + 4.f * k, sy + 10.f * k, sub, small, br || m_stage == Stage::Dying ? col(255, 150, 60) : col(170, 172, 180));
        if (m_phase2 && m_stage == Stage::Fight) {
            const std::string p2 = "OVERDRIVE";
            ui::label(*m_window, m_font, x + w + sk - ui::labelWidth(m_font, p2, small), sy + 10.f * k, p2, small,
                col(255, static_cast<int>(110 + 60 * (0.5f + 0.5f * std::sin(m_time * 6.f))), 60));
        }
    }

    void drawSkull(float x, float y, float px, uint8_t A) {
        static const char* rows[6] = { ".LLLLL.", "LLLLLLL", "LDDLDDL", "LDDLDDL", "LLLDLLL", ".LMLML." };
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        for (int j = 0; j < 6; ++j) for (int i = 0; i < 7; ++i) {
            const char ch = rows[j][i];
            sf::Color cc;
            if (ch == 'L') cc = sf::Color(238, 176, 160, A);
            else if (ch == 'D') cc = sf::Color(46, 14, 16, A);
            else if (ch == 'M') cc = sf::Color(176, 92, 82, A);
            else continue;
            const sf::Vector2f a(x + i * px, y + j * px);
            va.append(sf::Vertex{ a, cc }); va.append(sf::Vertex{ a + sf::Vector2f(px, 0.f), cc }); va.append(sf::Vertex{ a + sf::Vector2f(px, px), cc });
            va.append(sf::Vertex{ a, cc }); va.append(sf::Vertex{ a + sf::Vector2f(px, px), cc }); va.append(sf::Vertex{ a + sf::Vector2f(0.f, px), cc });
        }
        m_window->draw(va);
    }

    void banner(const std::string& head, const std::string& sub, sf::Color col) {
        m_banner = head; m_bannerSub = sub; m_bannerCol = col; m_bannerT = 0.f;
    }

    sf::Vector2f playerVel(size_t pi) const {
        const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[pi].bodyId);
        return { v.x * SCALE, v.y * SCALE };
    }

    // ========================================================================
    // STATE
    // ========================================================================
    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    sol::state* m_lua = nullptr;
    b2WorldId m_world{};
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    sf::RenderTarget* m_window = nullptr;
    levelgen::LevelState* m_level = nullptr;
    EnemySystem* m_enemies = nullptr;
    const sf::Font* m_font = nullptr;
    uint32_t m_playerId = 0;

    std::vector<int8_t> m_bossArch;
    sol::table m_cfg;
    const enemyarch::ArchetypeDef* m_def = nullptr;

    Stage m_stage = Stage::None;
    uint32_t m_bossId = 0;
    float m_time = 0.f, m_stageT = 0.f;
    sf::Vector2f m_home{ 0.f, 0.f };
    float m_R = 800.f;
    bool  m_phase2 = false;
    float m_breach = 0.f, m_stun = 0.f;
    int   m_lastProton = 0;

    // movement
    float m_orbitAng = 0.f, m_orbitDir = 1.f, m_orbitFlip = 3.f, m_flipCd = 0.f;

    // the hull's attack
    Atk   m_atk = Atk::None;
    float m_atkT = 0.f, m_gapT = 1.f;
    int   m_step = 0;
    float m_stepT = 0.f;
    std::array<int, ATK_N> m_skip{};
    int   m_forced = 0, m_forcedGun = 0;
    int   m_bombChain = 0;     ///< bombardments in this chain (0 = not rolled yet)
    int   m_sinceProton = 0;   ///< hull attacks since the last proton laser (its series)
    std::array<int, 4> m_fired{};
    std::array<bool, 3> m_res{};
    bool  m_heatHold[4] = { false, false, false, false };

    // the guns
    std::array<Gun, 3> m_gun{};

    std::vector<Marker> m_markers;
    float m_bombGrace = 0.f;
    std::vector<Sector> m_sectors;
    float m_spinDir = 1.f;
    float m_podGlow = 0.f;
    float m_protonCharge = 1.1f;
    int   m_bankEdge = -1;
    bool  m_bankPicked = false;
    std::vector<sf::Vector2f> m_laserPath;
    int   m_driftStep = 0;
    float m_driftAng = 0.f, m_driftDir = 1.f, m_ramWindup = 0.55f;
    sf::Vector2f m_ramDir{ 0.f, -1.f }, m_ramFrom{ 0.f, 0.f };
    bool  m_ramHit = false;
    float m_ramStall = 0.f;
    std::vector<Ghost> m_ghosts;
    float m_ghostT = 0.f;

    // the cage
    float m_ringR = 0.f;
    std::vector<Buoy> m_buoys;
    float m_igniteT = -1.f;
    b2BodyId m_wallBody = b2_nullBodyId;
    std::array<std::array<float, 49>, 6> m_jit{};
    float m_jitT = 0.f;
    std::vector<Bolt> m_bolts;
    float m_boltAcc = 0.f;
    float m_zapT = 0.f;
    float m_collapse = -1.f;

    std::vector<Crack> m_cracks;
    float m_popT = 0.f;
    bool  m_pending = false;
    sf::Vector2f m_deathPos{ 0.f, 0.f }, m_deathVel{ 0.f, 0.f };
    float m_deathRot = 0.f;

    std::string m_name = "DREADNOUGHT";
    float m_barShown = 0.f, m_barGhost = 1.f, m_barLast = 1.f, m_ghostHold = 0.f, m_flashBar = 0.f;

    std::string m_banner, m_bannerSub;
    sf::Color m_bannerCol{ 255, 206, 110 };
    float m_bannerT = -1.f;
};