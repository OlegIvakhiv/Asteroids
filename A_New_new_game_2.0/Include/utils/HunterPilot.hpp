/**
 * @file HunterPilot.hpp
 * @brief An AI that flies a player ship by pressing the player's keys.
 *
 * Used by the terminal's live feed and the doctrine scenes (utils/ShadowWorld).
 * It never touches a body, a timer or a component: it READS the world and
 * WRITES a VirtualPad -- W A S D, mouse aim, fire, dash, parry, Rift, vent --
 * so every mechanic it shows is the real one, with the real costs, windows
 * and cooldowns. If the pilot can do something, a player can; if a mechanic
 * changes, the pilot shows the change.
 *
 * WHAT IT DOES, in priority order every frame:
 *   1. SURVIVE   Berserker / Bloodseeker rams (dash across the lane),
 *                bashes (parry the lunge), incoming rounds and rockets
 *                (parry, or dash when the parry is down), Lancer locks and
 *                Barge cones (dash out of the line), rocks on a collision
 *                course (parry them: a parried rock is a homing round).
 *   2. VENT      a QTE on screen is answered: perfect if it can, good if not.
 *   3. FIGHT     pick a target, hold a range that suits it, strafe, lead the
 *                shot, keep the gun out of overheat unless told otherwise.
 *                Rift Bolt into a cluster, detonated at the closest pass.
 *   4. SALVAGE   no hostiles: collect scrap, break rocks, wander, turbo.
 *
 * SKILL is what makes a feed hunter mortal: reaction time, aim wobble, the
 * chance to read a ram at all. A skilled pilot dodges LATE -- the i-frames
 * eat the hit and it is a PERFECT DODGE, exactly as for a player.
 *
 * ORDERS let a doctrine scene script the ship without leaving the real input
 * path: fly to a point, aim at a point, never fire, always dodge perfectly...
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/ShadowWorld.hpp"
#include <SFML/System/Vector2.hpp>
#include <unordered_map>
#include <string>
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace pilot {

    /// The player's bindings, read from Lua once per build (F5 safe: rebuilt).
    struct Binds {
        std::string up = "W", down = "S", left = "A", right = "D";
        std::string dash = "Space", fire = "MouseLeft", sprint = "LShift";
        std::string parry = "R", rift = "MouseRight", vent = "E";

        void load(sol::state& lua) {
            sol::optional<sol::table> b = lua["key_bindings"];
            if (b) {
                const sol::table& t = *b;
                up = t["up"].get_or(up); down = t["down"].get_or(down);
                left = t["left"].get_or(left); right = t["right"].get_or(right);
                dash = t["dash"].get_or(dash); fire = t["fire"].get_or(fire);
                sprint = t["sprint"].get_or(sprint); parry = t["parry"].get_or(parry);
                vent = t["vent"].get_or(vent);
            }
            // WeaponSystem reads the GLOBAL, not key_bindings.rift_detonate_key.
            rift = lua["rift_detonate_key"].get_or(rift);
        }
    };

    struct Skill {
        float reaction = 0.14f;   ///< s before a new round is noticed
        float aimWobble = 3.f;    ///< deg of aim error, slowly varying
        float readRam = 0.95f;    ///< chance a given ram is read at all
        float readShot = 0.85f;   ///< chance an incoming round is answered
        float perfectVent = 0.7f; ///< chance to go for amber instead of blue
        float riftUse = 0.5f;     ///< appetite for the Rift Bolt
        bool  lateDodge = true;   ///< dodge into the hit (perfect) rather than early

        static Skill random() {
            auto f = [](float a, float b) { return a + (b - a) * (std::rand() / static_cast<float>(RAND_MAX)); };
            Skill s;
            s.reaction = f(0.10f, 0.24f);
            s.aimWobble = f(1.5f, 6.f);
            s.readRam = f(0.70f, 0.98f);
            s.readShot = f(0.55f, 0.92f);
            s.perfectVent = f(0.3f, 0.85f);
            s.riftUse = f(0.2f, 0.8f);
            s.lateDodge = f(0.f, 1.f) < 0.7f;
            return s;
        }
    };

    /// What a scene may override. Defaults = a free hunter.
    struct Orders {
        bool engage = true;          ///< pick targets, hold range, strafe
        bool fire = true;
        bool dodge = true;
        bool parry = true;
        bool rift = true;
        bool vent = true;
        bool turbo = true;
        bool salvage = true;         ///< chase scrap / break rocks when idle
        bool shootRocks = true;
        bool fireThroughHeat = false;///< keep shooting into the overheat (vent lesson)
        bool alwaysPerfectVent = false;
        bool hasMove = false;  sf::Vector2f moveTo;   ///< fly here (arrive, then hold)
        bool hasAim = false;   sf::Vector2f aimAt;    ///< nose here
        bool forceTurbo = false;
        bool forceFire = false;      ///< hold fire whatever the aim says
        bool forceRift = false;      ///< charge a Rift as soon as it is allowed
        uint32_t riftTarget = 0;     ///< detonate beside this entity (0 = best guess)
        float moveSlack = 40.f;      ///< arrive radius for moveTo
    };

    /// Inputs held this frame, by action -- the doctrine keycaps light from this.
    struct Pressed {
        bool up = false, down = false, left = false, right = false;
        bool fire = false, dash = false, sprint = false, parry = false, rift = false, vent = false;
        bool aimMoving = false;
    };

    class HunterPilot {
    public:
        Skill  skill;
        Orders orders;

        void reset(sol::state& lua) {
            m_binds.load(lua);
            m_seen.clear();
            m_ramRead.clear();
            m_strafeSign = (std::rand() & 1) ? 1.f : -1.f;
            m_strafeT = 0.f;
            m_wobble = 0.f; m_wobbleT = 0.f;
            m_target = 0;
            m_riftPrev = false; m_riftPlan = false;
            m_boltClosest = 1e9f; m_boltId = 0;
            m_lastAim = { 0.f, 0.f };
            m_dashLock = 0.f; m_parryLock = 0.f;
            m_ventPrev = false;
            m_wander = 0.f; m_wanderT = 0.f;
            m_pressed = Pressed{};
            m_intent = "STANDBY";
        }

        uint32_t target() const { return m_target; }
        const Pressed& pressed() const { return m_pressed; }
        const char* intent() const { return m_intent; }

        /// Decide this frame's input and write it into the world's pad.
        void think(ShadowWorld& w, float dt) {
            VirtualPad& pad = w.pad();
            pad.clear();
            m_pressed = Pressed{};
            const std::size_t pi = w.playerIndex();
            if (pi == (std::size_t)-1) return;

            EntityManager& em = w.em();
            const auto& tf = em.transforms[pi];
            const auto& ps = em.players[pi];
            const sf::Vector2f P = tf.position;
            const b2Vec2 bv = b2Body_GetLinearVelocity(em.physics[pi].bodyId);
            const sf::Vector2f V{ bv.x * SCALE, bv.y * SCALE };

            m_dashLock = std::max(0.f, m_dashLock - dt);
            m_parryLock = std::max(0.f, m_parryLock - dt);
            m_strafeT -= dt;
            if (m_strafeT <= 0.f) { m_strafeSign = -m_strafeSign; m_strafeT = 1.4f + frand() * 2.2f; }
            m_wobbleT -= dt;
            if (m_wobbleT <= 0.f) { m_wobbleTarget = (frand() * 2.f - 1.f) * skill.aimWobble; m_wobbleT = 0.3f + frand() * 0.5f; }
            m_wobble += (m_wobbleTarget - m_wobble) * std::min(1.f, dt * 6.f);

            // ---------------- what is out there ----------------
            scan(w, P);

            // ---------------- 1. SURVIVE ----------------
            sf::Vector2f dodgeDir{ 0.f, 0.f };
            bool wantDash = false, wantParry = false;
            if (orders.dodge || orders.parry) threats(w, P, V, ps, dt, dodgeDir, wantDash, wantParry);

            // ---------------- 2. VENT ----------------
            bool ventKey = false;
            if (orders.vent && ps.qteActive) {
                const float d = std::fabs(ps.qtePos - ps.qteGoodCenter);
                const bool goPerfect = orders.alwaysPerfectVent || m_ventAimPerfect;
                const float zone = goPerfect ? ps.qtePerfectHalf * 0.7f : ps.qteGoodHalf * 0.6f;
                if (d < zone && !m_ventPrev) ventKey = true;
            }
            else m_ventAimPerfect = frand() < skill.perfectVent;
            m_ventPrev = ventKey;

            // ---------------- 3/4. FIGHT or IDLE ----------------
            sf::Vector2f moveDir{ 0.f, 0.f };
            sf::Vector2f aim = P + forward(tf.rotation) * 300.f;
            bool fire = false, sprint = false;
            const Contact* tgt = pickTarget(P);
            m_target = tgt ? tgt->id : 0;

            if (orders.hasMove) {
                const sf::Vector2f d = orders.moveTo - P;
                const float L = len(d);
                if (L > orders.moveSlack) moveDir = d / L;
                else if (len(V) > 60.f) moveDir = -V / len(V) * 0.6f;   // brake: thrust against the drift
                m_intent = "MANOEUVRE";
            }

            if (tgt && orders.engage) {
                const sf::Vector2f d = tgt->pos - P;
                const float dist = std::max(1.f, len(d));
                const sf::Vector2f u = d / dist;
                if (!orders.hasMove) {
                    const float want = tgt->range;
                    const float radial = std::clamp((dist - want) / want, -1.f, 1.f);
                    const sf::Vector2f tang{ -u.y * m_strafeSign, u.x * m_strafeSign };
                    moveDir = u * radial * 1.2f + tang * 0.85f;
                }
                // Lead the shot.
                const float bs = m_bulletSpeed;
                const float t = dist / bs;
                aim = tgt->pos + (tgt->vel - V * 0.3f) * t;
                aim = rotateAbout(aim, P, m_wobble);
                fire = orders.fire && dist < 720.f && aimedAt(tf.rotation, P, aim, 9.f);
                m_intent = tgt->hostile ? "ENGAGING" : "BREAKING ROCK";
                if (orders.turbo && dist > 950.f && !ps.riftCharging) sprint = true;
            }
            else if (!orders.hasMove && orders.salvage) {
                idle(w, P, V, dt, moveDir, aim, fire, sprint);
            }
            if (orders.hasAim) { aim = orders.aimAt; fire = fire && aimedAt(tf.rotation, P, aim, 9.f); }

            // Heat discipline: a patient pilot lets the gun breathe.
            const float heat01 = ps.weaponHeat / std::max(1.f, ps.maxWeaponHeat);
            if (!orders.fireThroughHeat && ps.overdriveTimer <= 0.f && heat01 > 0.86f) fire = false;
            if (ps.weaponOverheated) fire = false;
            if (orders.forceFire && !ps.weaponOverheated) fire = true;
            if (!orders.fire && !orders.forceFire) fire = false;

            // Avoid rocks unless a scene is steering.
            if (!orders.hasMove) moveDir += rockAvoid(P) * 1.4f;

            // ---------------- Rift Bolt ----------------
            bool riftKey = false;
            if (orders.rift) riftKey = riftLogic(w, P, ps, tgt, aim, fire);

            // ---------------- dash overrides movement keys ----------------
            bool dashKey = false;
            if (wantDash && orders.dodge && m_dashLock <= 0.f && ps.dashCooldown <= 0.f
                && ps.dashTimer <= 0.f && ps.energyDrive >= ps.dashEnergyCost) {
                moveDir = dodgeDir;
                dashKey = true;
                m_dashLock = 0.25f;
                m_intent = "DODGE";
            }
            bool parryKey = false;
            if (wantParry && orders.parry && m_parryLock <= 0.f && ps.parryCooldown <= 0.f && ps.parryTimer <= 0.f) {
                parryKey = true;
                m_parryLock = 0.12f;
                m_intent = "PARRY";
            }
            if (orders.forceTurbo) sprint = true;
            if (!orders.turbo && !orders.forceTurbo) sprint = false;
            if (sprint) {                       // turbo burns along the nose
                aim = orders.hasAim ? orders.aimAt : (len(moveDir) > 0.1f ? P + norm(moveDir) * 400.f : aim);
            }

            // ---------------- write the pad ----------------
            writeMove(pad, moveDir);
            pad.aimSet = true;
            pad.aim = aim;
            m_pressed.aimMoving = len(aim - m_lastAim) > 2.f;
            m_lastAim = aim;
            if (fire && !ps.riftCharging) { pad.set(m_binds.fire, true); m_pressed.fire = true; }
            if (sprint) { pad.set(m_binds.sprint, true); m_pressed.sprint = true; }
            if (dashKey) { pad.set(m_binds.dash, true); m_pressed.dash = true; }
            if (parryKey) { pad.set(m_binds.parry, true); m_pressed.parry = true; }
            if (ventKey) { pad.set(m_binds.vent, true); m_pressed.vent = true; }
            if (riftKey) { pad.set(m_binds.rift, true); m_pressed.rift = true; }
        }

    private:
        struct Contact {
            uint32_t id = 0;
            std::size_t idx = 0;
            sf::Vector2f pos, vel;
            float dist = 0.f;
            float range = 300.f;     ///< preferred engagement distance
            float radius = 30.f;
            bool hostile = false;
            bool elite = false;
            std::string key;
        };

        static float frand() { return std::rand() / static_cast<float>(RAND_MAX); }
        static float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }
        static sf::Vector2f norm(sf::Vector2f v) { const float L = len(v); return L > 1e-4f ? v / L : sf::Vector2f{ 0.f, 0.f }; }
        static float dot(sf::Vector2f a, sf::Vector2f b) { return a.x * b.x + a.y * b.y; }
        static float cross(sf::Vector2f a, sf::Vector2f b) { return a.x * b.y - a.y * b.x; }
        static sf::Vector2f forward(float rotDeg) {
            const float r = (rotDeg - 90.f) * 3.14159265f / 180.f;
            return { std::cos(r), std::sin(r) };
        }
        static sf::Vector2f rotateAbout(sf::Vector2f p, sf::Vector2f c, float deg) {
            const float r = deg * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
            const sf::Vector2f d = p - c;
            return c + sf::Vector2f{ d.x * cs - d.y * sn, d.x * sn + d.y * cs };
        }
        static bool aimedAt(float rotDeg, sf::Vector2f P, sf::Vector2f aim, float tolDeg) {
            const sf::Vector2f f = forward(rotDeg), d = norm(aim - P);
            return dot(f, d) > std::cos(tolDeg * 3.14159265f / 180.f);
        }

        // ------------------------------------------------------------------
        void scan(ShadowWorld& w, sf::Vector2f P) {
            EntityManager& em = w.em();
            m_contacts.clear();
            m_rocks.clear();
            m_bulletSpeed = w.lua()["bullet_speed"].get_or(800.f);
            const std::size_t pi = w.playerIndex();
            for (std::size_t i = 0; i < em.physics.size(); ++i) {
                if (i == pi || !b2Body_IsValid(em.physics[i].bodyId)) continue;
                const BodyUserData* ud = bodyUD(em.physics[i].bodyId);
                if (!ud) continue;
                if (ud->type != BodyType::Enemy && ud->type != BodyType::Asteroid) continue;
                if (em.healths[i].currentHp <= 0.f) continue;
                Contact c;
                c.id = em.transforms[i].entityId;
                c.idx = i;
                c.pos = em.transforms[i].position;
                const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[i].bodyId);
                c.vel = { v.x * SCALE, v.y * SCALE };
                c.dist = len(c.pos - P);
                if (ud->type == BodyType::Enemy && i < em.enemies.size()) {
                    const auto& e = em.enemies[i];
                    if (!e.powered()) continue;            // a wreck posing as one is scenery
                    c.hostile = true;
                    const auto* def = w.registry().byId(e.archetype);
                    c.key = def ? def->key : std::string();
                    c.radius = def ? def->radius : 40.f;
                    c.range = rangeFor(c.key);
                    c.elite = (c.key == "BARGE" || c.key == "BLOODSEEKER");
                    m_contacts.push_back(c);
                }
                else {
                    c.radius = std::max(10.f, em.healths[i].visualRadius);
                    c.range = 230.f;
                    m_rocks.push_back(c);
                }
            }
        }

        static float rangeFor(const std::string& key) {
            if (key == "BERSERKER") return 360.f;   // out of bash reach, inside gun range
            if (key == "BLOODSEEKER") return 380.f;
            if (key == "WARDOG") return 240.f;
            if (key == "BARGE") return 460.f;
            if (key == "RAIDER") return 300.f;
            if (key == "MANIAC") return 340.f;
            return 320.f;
        }

        const Contact* pickTarget(sf::Vector2f) {
            const Contact* best = nullptr;
            float bestScore = 1e9f;
            for (const auto& c : m_contacts) {
                if (c.dist > 1300.f) continue;
                float s = c.dist;
                if (c.id == m_target) s *= 0.7f;          // stick with a target
                if (c.elite) s *= 1.15f;
                if (s < bestScore) { bestScore = s; best = &c; }
            }
            if (best || !orders.shootRocks) return best;
            // No ships: the closest rock that is actually in the way of things.
            for (const auto& r : m_rocks) {
                if (r.dist > 520.f) continue;
                if (r.dist < bestScore) { bestScore = r.dist; best = &r; }
            }
            return best;
        }

        sf::Vector2f rockAvoid(sf::Vector2f P) const {
            sf::Vector2f push{ 0.f, 0.f };
            for (const auto& r : m_rocks) {
                const float clear = r.radius + 110.f;
                if (r.dist < clear && r.dist > 1.f) push += (P - r.pos) / r.dist * ((clear - r.dist) / clear);
            }
            return push;
        }

        // ------------------------------------------------------------------
        void threats(ShadowWorld& w, sf::Vector2f P, sf::Vector2f V, const PlayerComponent& ps,
            float dt, sf::Vector2f& dodgeDir, bool& wantDash, bool& wantParry) {
            EntityManager& em = w.em();
            const float lead = skill.lateDodge ? 0.07f : 0.20f;     // s before impact
            const float hitR = 34.f;

            // ---- enemy melee ----
            for (const auto& c : m_contacts) {
                const auto& e = em.enemies[c.idx];
                // RAM: unparryable. Dash across the lane.
                if (e.ramState == RamState::Charge || e.ramState == RamState::Windup) {
                    auto it = m_ramRead.find(c.id);
                    if (it == m_ramRead.end()) it = m_ramRead.emplace(c.id, frand() < skill.readRam).first;
                    if (!it->second) continue;
                    const sf::Vector2f dir = norm(e.ramDir.x || e.ramDir.y ? e.ramDir : (P - c.pos));
                    const sf::Vector2f rel = P - c.pos;
                    const float along = dot(rel, dir), side = cross(dir, rel);
                    if (along <= 0.f) continue;
                    if (std::fabs(side) > c.radius + hitR + 30.f) continue;
                    const float sp = std::max(200.f, len(c.vel));
                    const float tti = (along - c.radius - hitR) / sp;
                    if (e.ramState == RamState::Charge && tti < lead) {
                        wantDash = true;
                        const float sgn = side >= 0.f ? 1.f : -1.f;
                        dodgeDir = sf::Vector2f{ -dir.y, dir.x } * sgn;
                    }
                }
                else m_ramRead.erase(c.id);

                // BASH: the attack that is meant to be parried. Parry the lunge.
                if ((e.bashState == BashState::Lunge || (e.bashState == BashState::Windup && e.bashTimer < 0.10f))
                    && c.dist < c.radius + 170.f) wantParry = true;

                // LANCER lock / BARGE cone: get off the line.
                const bool lancerLock = e.duelAttack == DuelAttack::LancerLock && e.duelAtkTimer < 0.14f;
                const bool coneFire = (e.duelAttack == DuelAttack::ConeWindup && e.duelAtkTimer < 0.12f);
                const bool turretShot = e.telegraphActive && e.telegraphTimer < 0.12f && e.telegraphTimer > 0.f;
                if (lancerLock || coneFire || turretShot) {
                    const sf::Vector2f dir = norm(lancerLock || coneFire ? e.duelAtkDir : e.telegraphDir);
                    const sf::Vector2f rel = P - c.pos;
                    const float side = cross(dir, rel);
                    if (dot(rel, dir) > 0.f && std::fabs(side) < 120.f) {
                        wantDash = true;
                        dodgeDir = sf::Vector2f{ -dir.y, dir.x } * (side >= 0.f ? 1.f : -1.f);
                    }
                }
            }

            // ---- rounds, rockets ----
            const float now = w.time();
            for (std::size_t i = 0; i < em.bullets.size() && i < em.physics.size(); ++i) {
                const auto& b = em.bullets[i];
                if (!b.isActive || !b.isEnemyBullet || b.markedForDestroy) continue;
                if (!b2Body_IsValid(em.physics[i].bodyId)) continue;
                const uint32_t id = em.transforms[i].entityId;
                auto it = m_seen.find(id);
                if (it == m_seen.end()) it = m_seen.emplace(id, Seen{ now, frand() < skill.readShot }).first;
                if (!it->second.answer || now - it->second.t0 < skill.reaction) continue;
                const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[i].bodyId);
                const sf::Vector2f bv{ v.x * SCALE, v.y * SCALE };
                const sf::Vector2f rel = em.transforms[i].position - P;
                const sf::Vector2f rv = bv - V;
                const float rv2 = std::max(1.f, dot(rv, rv));
                const float tca = -dot(rel, rv) / rv2;              // time of closest approach
                if (tca < 0.f || tca > 0.45f) continue;
                const float miss = len(rel + rv * tca);
                if (miss > 46.f) continue;
                const bool parryUp = ps.parryCooldown <= 0.f && ps.parryTimer <= 0.f;
                if (orders.parry && parryUp && tca < 0.16f) wantParry = true;
                else if (orders.dodge && !parryUp && tca < 0.12f) {
                    wantDash = true;
                    const sf::Vector2f d = norm(bv);
                    dodgeDir = sf::Vector2f{ -d.y, d.x } * (cross(d, -rel) >= 0.f ? 1.f : -1.f);
                }
            }
            if (m_seen.size() > 256) m_seen.clear();

            // ---- rocks on a collision course: bat them back ----
            if (orders.parry) {
                for (const auto& r : m_rocks) {
                    const sf::Vector2f rel = r.pos - P, rv = r.vel - V;
                    if (dot(rel, rv) >= 0.f) continue;
                    const float rv2 = std::max(1.f, dot(rv, rv));
                    const float tca = -dot(rel, rv) / rv2;
                    if (tca > 0.18f) continue;
                    if (len(rel + rv * tca) < r.radius + 26.f && len(rv) > 140.f) wantParry = true;
                }
            }
            (void)dt;
        }

        // ------------------------------------------------------------------
        bool riftLogic(ShadowWorld& w, sf::Vector2f P, const PlayerComponent& ps,
            const Contact* tgt, sf::Vector2f aim, bool& fire) {
            EntityManager& em = w.em();
            bool key = false;

            // A bolt is out: hold off, then press once at its closest pass.
            if (ps.riftBoltInFlight) {
                const std::size_t bi = em.getEntityIndex(ps.riftBoltEntityId);
                if (bi != (std::size_t)-1) {
                    if (ps.riftBoltEntityId != m_boltId) { m_boltId = ps.riftBoltEntityId; m_boltClosest = 1e9f; }
                    const sf::Vector2f B = em.transforms[bi].position;
                    sf::Vector2f goal = tgt ? tgt->pos : aim;
                    if (orders.riftTarget) {
                        const std::size_t ti = em.getEntityIndex(orders.riftTarget);
                        if (ti != (std::size_t)-1) goal = em.transforms[ti].position;
                    }
                    const float d = len(B - goal);
                    const bool passing = d > m_boltClosest + 6.f;   // started moving away
                    m_boltClosest = std::min(m_boltClosest, d);
                    if ((d < 70.f || (passing && m_boltClosest < 220.f)) && !m_riftPrev) key = true;
                }
                m_riftPrev = key;
                fire = false;
                return key;
            }
            m_boltId = 0;

            // Charging: keep holding until WeaponSystem lets it go.
            if (ps.riftCharging) { m_riftPrev = true; fire = false; return true; }

            // Should we start one?
            if (!m_riftPlan) {
                bool worth = false;
                if (orders.forceRift) worth = true;
                else if (tgt && tgt->hostile && tgt->dist > 260.f && tgt->dist < 700.f) {
                    int near = 0;
                    for (const auto& c : m_contacts) if (len(c.pos - tgt->pos) < 260.f) ++near;
                    for (const auto& r : m_rocks) if (len(r.pos - tgt->pos) < 160.f) ++near;
                    worth = (near >= 2 || tgt->elite) && frand() < skill.riftUse * 0.02f;
                }
                // Same gates WeaponSystem uses, so the pilot never asks for a
                // bolt the gun will refuse (that is the grey "deny" puff).
                sol::optional<sol::table> wt = w.lua()["weapon"];
                const float riftHeat = wt ? (*wt)["rift_heat"].get_or(52.f) : 52.f;
                const float riftCost = wt ? (*wt)["rift_energy_cost"].get_or(24.f) : 24.f;
                const bool heatOk = ps.overdriveTimer > 0.f || ps.weaponHeat + riftHeat < ps.maxWeaponHeat;
                const float reserve = ps.dashEnergyCost + riftCost + 4.f;
                if (worth && heatOk && ps.riftCooldown <= 0.f && !ps.riftBoltInFlight
                    && !ps.weaponOverheated && ps.energyDrive > reserve)
                    m_riftPlan = true;
            }
            if (m_riftPlan) {
                // Release first if the key is still down from the last bolt.
                if (m_riftPrev) { m_riftPrev = false; return false; }
                if (ps.riftCooldown > 0.f || ps.weaponOverheated || ps.riftBoltInFlight) { m_riftPlan = false; return false; }
                m_riftPlan = false;
                m_riftPrev = true;
                fire = false;
                return true;
            }
            m_riftPrev = false;
            return false;
        }

        // ------------------------------------------------------------------
        void idle(ShadowWorld& w, sf::Vector2f P, sf::Vector2f V, float dt,
            sf::Vector2f& moveDir, sf::Vector2f& aim, bool& fire, bool& sprint) {
            EntityManager& em = w.em();
            // Scrap first: it is the point of the job.
            float best = 900.f;
            const ScrapPickup* sp = nullptr;
            for (const auto& s : em.scrapPickups) {
                const float d = len(s.position - P);
                if (d < best) { best = d; sp = &s; }
            }
            if (sp) {
                moveDir = norm(sp->position - P);
                aim = sp->position;
                m_intent = "SALVAGE";
                return;
            }
            m_wanderT -= dt;
            if (m_wanderT <= 0.f) { m_wander = frand() * 6.2831853f; m_wanderT = 2.5f + frand() * 3.f; }
            moveDir = { std::cos(m_wander), std::sin(m_wander) };
            aim = P + moveDir * 400.f;
            sprint = orders.turbo && len(V) < 520.f && frand() < 0.5f && m_wanderT > 1.5f;
            fire = false;
            m_intent = "PATROL";
        }

        void writeMove(VirtualPad& pad, sf::Vector2f d) {
            const float L = len(d);
            if (L < 0.08f) return;
            d /= L;
            const float t = 0.38f;          // 8 directions, like a keyboard
            if (d.y < -t) { pad.set(m_binds.up, true); m_pressed.up = true; }
            if (d.y > t) { pad.set(m_binds.down, true); m_pressed.down = true; }
            if (d.x < -t) { pad.set(m_binds.left, true); m_pressed.left = true; }
            if (d.x > t) { pad.set(m_binds.right, true); m_pressed.right = true; }
        }

        struct Seen { float t0; bool answer; };

        Binds m_binds;
        std::vector<Contact> m_contacts, m_rocks;
        std::unordered_map<uint32_t, Seen> m_seen;
        std::unordered_map<uint32_t, bool> m_ramRead;
        float m_bulletSpeed = 800.f;
        float m_strafeSign = 1.f, m_strafeT = 0.f;
        float m_wobble = 0.f, m_wobbleT = 0.f, m_wobbleTarget = 0.f;
        uint32_t m_target = 0;
        bool m_riftPrev = false, m_riftPlan = false;
        float m_boltClosest = 1e9f;
        uint32_t m_boltId = 0;
        sf::Vector2f m_lastAim;
        float m_dashLock = 0.f, m_parryLock = 0.f;
        bool m_ventPrev = false, m_ventAimPerfect = true;
        float m_wander = 0.f, m_wanderT = 0.f;
        Pressed m_pressed;
        const char* m_intent = "STANDBY";
    };

} // namespace pilot
