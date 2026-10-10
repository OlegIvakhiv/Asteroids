/**
 * @file DoctrineStage.hpp
 * @brief FIELD DOCTRINE: eight looping scenes of real gameplay.
 *
 * Each lesson is a tiny scripted encounter on a ShadowWorld: YOUR refit-bay
 * ship and paints, a hand-placed enemy or rock, and HunterPilot under scene
 * orders. The scene only sets up the situation and tells the pilot what it
 * may do; the mechanic itself is the game's. The perfect dodge in DODGE is a
 * real Berserker ram eaten by real i-frames, the reflected round in PARRY is
 * a real Raider burst, the overdrive in VENT is a real QTE answered on the
 * amber zone. The HUD in the corner is the real HUD.
 *
 * The pilot reacts to the enemy's actual state (ram lane, bullet closest
 * approach, QTE marker), not to a clock, which is what makes a loop land the
 * same way every time even though nothing in it is canned.
 *
 * A scene ends at its `length` and is rebuilt from scratch: fresh world,
 * fresh enemy, same set-up.
 *
 * Keycaps: Lesson::keys names the inputs the scene teaches; lit() reports
 * which of them the pilot is holding this frame (held a little longer for
 * one-frame presses, so a dash is visible on the caps).
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/ShadowWorld.hpp"
#include "utils/HunterPilot.hpp"
#include <memory>
#include <vector>
#include <map>
#include <cstdlib>
#include <string>

namespace doctrine {

    struct Lesson {
        const char* name;
        bool        advanced;
        float       length;                 ///< loop length, seconds
        std::vector<const char*> keys;      ///< keycaps under the scene, in order
        const char* line1;                  ///< what to press
        const char* line2;                  ///< why it matters
    };

    inline const std::vector<Lesson>& lessons() {
        static const std::vector<Lesson> k = {
            { "FLIGHT",    false, 7.0f, { "W", "A", "S", "D", "MOUSE" },
              "W A S D MOVE YOU IN ANY DIRECTION. THE NOSE ALWAYS FOLLOWS THE MOUSE.",
              "LET GO AND YOU KEEP DRIFTING. SPACE HAS NO BRAKES." },
            { "GUNNERY",   false, 5.5f, { "MOUSE", "LMB" },
              "LEFT MOUSE FIRES. EVERY SHOT ADDS HEAT.",
              "LARGE ROCKS SPLIT TO MEDIUM, MEDIUM TO SMALL. AT FULL HEAT THE GUN LOCKS." },
            { "TURBO",     false, 5.0f, { "W", "SHIFT" },
              "HOLD LEFT SHIFT FOR TURBO. IT BURNS ENERGY.",
              "ENERGY IS SHARED WITH THE DODGE. A DRY TANK MEANS NO WAY OUT." },
            { "SALVAGE",   false, 8.0f, { "LMB", "W", "A", "S", "D" },
              "KILLS AND SALVAGE HEAPS DROP SCRAP. FLY CLOSE AND IT PULLS ITSELF IN.",
              "SCRAP PAYS FOR THE REFIT BAY." },
            { "DODGE",     true,  4.6f, { "SPACE", "W", "A", "S", "D" },
              "SPACE DASHES THE WAY YOU ARE STEERING. THE DASH CANNOT BE HIT.",
              "DODGE LATE, INTO THE RAM: A PERFECT DODGE REFUNDS ENERGY AND RE-ARMS." },
            { "PARRY",     true,  6.0f, { "R" },
              "R PARRIES. A ROUND GOES BACK AT WHOEVER FIRED IT.",
              "BULLET AND SHIP PARRIES RE-ARM AT ONCE. A PARRIED ROCK BECOMES A HOMING ROUND." },
            { "RIFT BOLT", true,  4.4f, { "RMB" },
              "HOLD RIGHT MOUSE TO CHARGE THE RIFT BOLT. PRESS AGAIN TO DETONATE IT.",
              "OPEN SPACE: BURST.   BESIDE A ROCK: HIJACK.   BESIDE A SHIP: OVERLOAD." },
            { "VENT",      true,  6.5f, { "LMB", "E" },
              "AT FULL HEAT, E OPENS THE VENT. STOP THE MARKER INSIDE A ZONE.",
              "BLUE CLEARS THE HEAT. AMBER CLEARS IT AND GRANTS OVERDRIVE: NO HEAT AT ALL." },
        };
        return k;
    }

    class Stage {
    public:
        void attach(sf::RenderWindow* window, sol::state* lua, const enemyarch::EnemyRegistry* reg,
            const zonearch::ZoneState* zone, const sf::Font* font) {
            m_window = window; m_lua = lua; m_reg = reg; m_zoneState = zone; m_font = font;
        }

        /// The player's ship, every frame (MenuSystem::setShip). Used on the next rebuild.
        void setShip(const ship::ShipDesign* d, const ship::Livery* lv) { m_design = d; m_livery = lv; }
        void setSize(sf::Vector2u s) {
            if (s == m_size) return;
            m_size = s;
            if (m_world && (std::abs(static_cast<int>(s.x) - static_cast<int>(m_world->size().x)) > 8
                || std::abs(static_cast<int>(s.y) - static_cast<int>(m_world->size().y)) > 8)) m_dirty = true;
        }
        void invalidate() { m_dirty = true; }

        /// Start (or restart) a lesson.
        void play(int lesson) {
            const int n = static_cast<int>(lessons().size());
            m_lesson = std::clamp(lesson, 0, n - 1);
            m_dirty = true;
        }
        int lesson() const { return m_lesson; }
        float t() const { return m_t; }
        float length() const { return lessons()[m_lesson].length; }

        void step(float dt) {
            if (!m_window || !m_lua || !m_reg) return;
            if (m_dirty || !m_world) { build(); if (!m_world) return; }
            dt = std::clamp(dt, 0.f, 0.05f);
            m_t += dt;
            if (m_t >= length()) { build(); if (!m_world) return; }
            script(dt);
            m_pilot.think(*m_world, dt);
            m_world->step(dt);
            frame(dt);
            holdLights(dt);
        }

        void render() { if (m_world) m_world->render(sf::Color(3, 5, 8)); }
        const sf::Texture* texture() const { return m_world ? m_world->texture() : nullptr; }

        /// Lit flag per Lesson::keys entry.
        std::vector<bool> lit() const {
            const auto& L = lessons()[m_lesson];
            std::vector<bool> out(L.keys.size(), false);
            for (std::size_t i = 0; i < L.keys.size(); ++i) {
                const std::string k = L.keys[i];
                const auto it = m_light.find(k);
                out[i] = it != m_light.end() && it->second > 0.f;
            }
            return out;
        }

    private:
        static float frand() { return std::rand() / static_cast<float>(RAND_MAX); }

        void build() {
            m_dirty = false;
            m_t = 0.f;
            m_light.clear();
            m_ids.clear();
            m_flag = 0;
            m_rift = 0;
            m_lean = { 0.f, 0.f };
            if (!m_world) m_world = std::make_unique<ShadowWorld>();
            ShadowWorld::Setup s;
            s.size = m_size;
            if (m_design) s.design = *m_design;
            if (m_livery) s.livery = *m_livery;
            s.spawn = { 0.f, 0.f };
            s.spawnAngle = 90.f;         // nose to the right: scenes play left to right
            s.hud = true;
            s.god = true;                // a lesson never ends in a death screen
            s.stars = 220;
            s.capacity = 2048;
            if (!m_world->build(*m_window, *m_lua, *m_reg, m_zoneState ? m_zoneState->registry : nullptr,
                m_zoneState ? m_zoneState->current : zonearch::INVALID_ZONE, m_font, s)) {
                m_world.reset();
                return;
            }
            m_pilot.skill = pilot::Skill{};       // a doctrine pilot is the textbook
            m_pilot.skill.reaction = 0.08f;
            m_pilot.skill.aimWobble = 1.f;
            m_pilot.skill.readRam = 1.f;
            m_pilot.skill.readShot = 1.f;
            m_pilot.skill.lateDodge = true;
            m_pilot.reset(*m_lua);
            m_pilot.orders = pilot::Orders{};
            setup();
        }

        // ---- scene set-up: what is in the field when the loop starts ----
        void setup() {
            ShadowWorld& w = *m_world;
            switch (m_lesson) {
            case 0:   // FLIGHT: three rocks as landmarks, nothing to shoot
                w.rock("LARGE", { 520.f, -150.f });
                w.rock("MEDIUM", { -480.f, 170.f });
                w.rock("MEDIUM", { 150.f, 210.f });
                break;
            case 1:   // GUNNERY: a large rock to split, two mediums
                m_ids.push_back(w.rock("LARGE", { 430.f, 0.f }));
                m_ids.push_back(w.rock("MEDIUM", { 380.f, -170.f }));
                m_ids.push_back(w.rock("MEDIUM", { 470.f, 160.f }));
                break;
            case 2:   // TURBO: a line of rocks to fly past
                for (int i = 0; i < 9; ++i)
                    w.rock(i % 3 ? "MEDIUM" : "LARGE", { 500.f + i * 420.f, (i % 2 ? -190.f : 200.f) + frand() * 40.f });
                break;
            case 3:   // SALVAGE: a salvage heap and a lone Wardog
                m_ids.push_back(w.rock("SCRAP_LARGE", { 360.f, -60.f }));
                m_ids.push_back(w.summon("WARDOG", { 560.f, 140.f }));
                break;
            case 4:   // DODGE: one Berserker in its ram band, ram ready
                m_ids.push_back(w.summon("BERSERKER", { 520.f, 0.f }));
                primeEnemy(m_ids.back(), Ram::Ready);
                break;
            case 5:   // PARRY: a Berserker with its ram held back -- its charged
                      // burst is aimed (three rounds, every one parryable, and a
                      // bullet parry re-arms) and its bash is MEANT to be parried
                m_ids.push_back(w.summon("BERSERKER", { 330.f, 0.f }));
                primeEnemy(m_ids.back(), Ram::Off);
                break;
            case 6:   // RIFT BOLT: two rocks in front of a Raider. The bolt goes
                      // off beside the big one -- HIJACK -- and the rocks become
                      // the hunter's rounds.
                m_rift = w.rock("LARGE", { 470.f, -30.f });
                w.rock("MEDIUM", { 500.f, 120.f });
                m_ids.push_back(w.summon("RAIDER", { 680.f, 40.f }));
                primeEnemy(m_ids.back(), Ram::Keep);
                break;
            case 7:   // VENT: rocks to pour fire into, gun already hot
                m_ids.push_back(w.rock("LARGE", { 420.f, -60.f }));
                m_ids.push_back(w.rock("LARGE", { 480.f, 120.f }));
                if (auto* ps = player()) ps->weaponHeat = ps->maxWeaponHeat * 0.72f;
                break;
            default: break;
            }
        }

        enum class Ram { Keep, Ready, Off };

        /// Put a summoned unit straight into the fight (no patrol / alert beat).
        void primeEnemy(uint32_t id, Ram ram) {
            EntityManager& em = m_world->em();
            const std::size_t i = em.getEntityIndex(id);
            if (i == (std::size_t)-1 || i >= em.enemies.size()) return;
            auto& e = em.enemies[i];
            if (ram == Ram::Ready) e.ramCooldown = 0.f;
            if (ram == Ram::Off) e.ramCooldown = 1.0e6f;   // this lesson is about the other attacks
            // Straight into the fight through the AI's own door: a unit hit
            // twice goes to COMBAT on its next think (AISystem updatePerception).
            // A patrol turn would otherwise carry the hunter out of its cone.
            e.timesHit = std::max(e.timesHit, 2);
            e.detectionRadius = std::max(e.detectionRadius, 900.f);
            e.fireTimer = 0.4f;
            // Facing the hunter, so the real vision cone sees it on frame one
            // (the body owns rotation; the transform is overwritten each step).
            const sf::Vector2f d = playerPos() - em.transforms[i].position;
            const float deg = std::atan2(d.x, -d.y) * 180.f / 3.14159265f;
            em.transforms[i].rotation = deg;
            if (b2Body_IsValid(em.physics[i].bodyId))
                b2Body_SetTransform(em.physics[i].bodyId, b2Body_GetPosition(em.physics[i].bodyId),
                    b2MakeRot(deg * 3.14159265f / 180.f));
        }

        PlayerComponent* player() {
            const std::size_t pi = m_world->playerIndex();
            return pi == (std::size_t)-1 ? nullptr : &m_world->em().players[pi];
        }
        sf::Vector2f playerPos() {
            const std::size_t pi = m_world->playerIndex();
            return pi == (std::size_t)-1 ? sf::Vector2f{} : m_world->em().transforms[pi].position;
        }

        // ---- per-frame orders ----
        void script(float) {
            pilot::Orders o;
            const float t = m_t;
            const sf::Vector2f P = playerPos();
            switch (m_lesson) {
            case 0: {   // FLIGHT: a box around the rocks, nose sweeping independently
                static const sf::Vector2f wp[] = { { 330.f, -120.f }, { 330.f, 140.f }, { -330.f, 140.f }, { -330.f, -120.f } };
                o.engage = false; o.fire = false; o.salvage = false; o.rift = false;
                o.dodge = false; o.parry = false; o.turbo = false;
                o.hasMove = true; o.moveTo = wp[static_cast<int>(t / 1.6f) % 4]; o.moveSlack = 50.f;
                const float a = t * 1.1f;
                o.hasAim = true; o.aimAt = P + sf::Vector2f{ std::cos(a), std::sin(a) } * 300.f;
                break;
            }
            case 1:     // GUNNERY: hold position, break rocks
                o.rift = false; o.dodge = false; o.parry = false; o.turbo = false; o.salvage = false;
                o.hasMove = true; o.moveTo = { 0.f, 0.f }; o.moveSlack = 60.f;
                break;
            case 2:     // TURBO
                o.engage = false; o.fire = false; o.rift = false; o.dodge = false; o.parry = false;
                o.hasMove = true; o.moveTo = { 6000.f, 0.f };
                o.hasAim = true; o.aimAt = P + sf::Vector2f{ 400.f, 0.f };
                o.forceTurbo = t > 0.6f;
                break;
            case 3:     // SALVAGE: shoot, then the pilot's idle brain collects
                o.rift = false; o.parry = false; o.turbo = false;
                break;
            case 4:     // DODGE: no shooting, no parry -- only the dash
                o.fire = false; o.parry = false; o.rift = false; o.turbo = false; o.salvage = false;
                o.hasMove = true; o.moveTo = { 0.f, 0.f }; o.moveSlack = 70.f;
                if (!m_ids.empty()) {
                    const std::size_t ei = m_world->em().getEntityIndex(m_ids[0]);
                    if (ei != (std::size_t)-1) { o.hasAim = true; o.aimAt = m_world->em().transforms[ei].position; }
                }
                break;
            case 5:     // PARRY: dead still (a drifting target spoils the Raider's
                        // lead), bat the rounds back, then a rock thrown at us
                o.engage = false; o.fire = false; o.dodge = false; o.rift = false; o.turbo = false; o.salvage = false;
                // Loose station-keeping: only a knockback (a parried bash shoves
                // both ships apart) is flown back; small drift is left alone.
                o.hasMove = true; o.moveTo = { 0.f, 0.f }; o.moveSlack = 130.f;
                if (t > 3.4f && !(m_flag & 1)) {
                    m_flag |= 1;
                    const sf::Vector2f from = P + sf::Vector2f{ -380.f, -300.f };
                    const sf::Vector2f dir = (P - from) / std::sqrt((P - from).x * (P - from).x + (P - from).y * (P - from).y);
                    m_world->rock("MEDIUM", from, dir * 300.f);
                }
                if (!m_ids.empty()) {
                    const std::size_t ei = m_world->em().getEntityIndex(m_ids[0]);
                    if (ei != (std::size_t)-1) { o.hasAim = true; o.aimAt = m_world->em().transforms[ei].position; }
                }
                break;
            case 6: {   // RIFT BOLT: charge at 0.8 s, detonate beside the big rock
                o.engage = false; o.fire = false; o.dodge = false; o.parry = false; o.turbo = false; o.salvage = false;
                o.forceRift = t > 0.8f && t < 1.2f;
                const std::size_t ri = m_world->em().getEntityIndex(m_rift);
                if (ri != (std::size_t)-1) {
                    o.riftTarget = m_rift;
                    o.hasAim = true; o.aimAt = m_world->em().transforms[ri].position;
                }
                else if (!m_ids.empty()) {
                    const std::size_t ei = m_world->em().getEntityIndex(m_ids[0]);
                    if (ei != (std::size_t)-1) { o.hasAim = true; o.aimAt = m_world->em().transforms[ei].position; }
                }
                break;
            }
            case 7:     // VENT: fire into the overheat, answer the QTE on amber
                o.rift = false; o.dodge = false; o.parry = false; o.turbo = false; o.salvage = false;
                o.hasMove = true; o.moveTo = { 0.f, 0.f }; o.moveSlack = 60.f;
                o.fireThroughHeat = true; o.alwaysPerfectVent = true;
                o.forceFire = t > 0.3f;
                if (!m_ids.empty()) {
                    for (uint32_t id : m_ids) {
                        const std::size_t ri = m_world->em().getEntityIndex(id);
                        if (ri != (std::size_t)-1) { o.hasAim = true; o.aimAt = m_world->em().transforms[ri].position; break; }
                    }
                }
                if (!o.hasAim) { o.hasAim = true; o.aimAt = P + sf::Vector2f{ 400.f, 0.f }; }
                break;
            default: break;
            }
            m_pilot.orders = o;
        }

        /// Lean the camera toward the scene's subject (a bolt in flight beats
        /// the enemy), so the lesson's moment stays in the frame.
        void frame(float dt) {
            EntityManager& em = m_world->em();
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return;
            const sf::Vector2f P = em.transforms[pi].position;
            sf::Vector2f subject = P;
            bool have = false;
            const auto& ps = em.players[pi];
            if (ps.riftBoltInFlight) {
                const std::size_t bi = em.getEntityIndex(ps.riftBoltEntityId);
                if (bi != (std::size_t)-1) { subject = em.transforms[bi].position; have = true; }
            }
            for (std::size_t k = 0; !have && k < m_ids.size(); ++k) {
                const std::size_t i = em.getEntityIndex(m_ids[k]);
                if (i == (std::size_t)-1) continue;
                const BodyUserData* ud = bodyUD(em.physics[i].bodyId);
                if (!ud || ud->type != BodyType::Enemy) continue;
                subject = em.transforms[i].position; have = true;
            }
            sf::Vector2f want{ 0.f, 0.f };
            if (have) {
                const sf::Vector2f d = subject - P;
                if (d.x * d.x + d.y * d.y < 1100.f * 1100.f) want = d * 0.45f;
            }
            m_lean += (want - m_lean) * std::min(1.f, dt * 3.f);
            sf::View& v = m_world->view();
            v.setCenter(v.getCenter() + m_lean);
        }

        void holdLights(float dt) {
            for (auto& [k, v] : m_light) v = std::max(0.f, v - dt);
            const pilot::Pressed& p = m_pilot.pressed();
            auto on = [&](const char* k, bool b, float hold) { if (b) m_light[k] = std::max(m_light[k], hold); };
            on("W", p.up, 0.05f); on("S", p.down, 0.05f); on("A", p.left, 0.05f); on("D", p.right, 0.05f);
            on("MOUSE", p.aimMoving, 0.08f);
            on("LMB", p.fire, 0.06f);
            on("SHIFT", p.sprint, 0.05f);
            on("SPACE", p.dash, 0.30f);
            on("R", p.parry, 0.30f);
            on("RMB", p.rift, 0.20f);
            on("E", p.vent, 0.35f);
        }

        sf::RenderWindow* m_window = nullptr;
        sol::state* m_lua = nullptr;
        const enemyarch::EnemyRegistry* m_reg = nullptr;
        const zonearch::ZoneState* m_zoneState = nullptr;
        const sf::Font* m_font = nullptr;
        const ship::ShipDesign* m_design = nullptr;
        const ship::Livery* m_livery = nullptr;
        sf::Vector2u m_size{ 1340, 520 };
        bool m_dirty = true;
        int m_lesson = 0, m_flag = 0;
        float m_t = 0.f;
        sf::Vector2f m_lean{ 0.f, 0.f };
        std::unique_ptr<ShadowWorld> m_world;
        pilot::HunterPilot m_pilot;
        std::vector<uint32_t> m_ids;
        uint32_t m_rift = 0;            ///< what the RIFT BOLT lesson detonates beside
        std::map<std::string, float> m_light;
    };

} // namespace doctrine
