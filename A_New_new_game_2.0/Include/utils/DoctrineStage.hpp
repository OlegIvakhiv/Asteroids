/**
 * @file DoctrineStage.hpp
 * @brief FIELD DOCTRINE: sixteen directed scenes of real gameplay.
 *
 * 1.0 ran the doctrine as free fights: an AI pilot reacting to an AI enemy.
 * Playtest: "sometimes it messes up and does not do what it should -- one
 * normally recorded visualisation is better than a dynamic one every loop".
 * 2.0 DIRECTS every scene, like a film:
 *
 *   - THE HUNTER is flown from a timeline, through the real input path (a
 *     VirtualPad -- the same keys a player presses). No pilot brain.
 *   - THE ENEMY is puppeteered through EnemyComponent::Direction: it does
 *     only what the scene cues (one bash, one ram, one shot, perfectly
 *     aimed) and can be held on its mark. The attack itself -- tell, lunge,
 *     damage, stagger -- is the game's own code.
 *   - THE CLOCK is fixed: one 1/60 s logic pass per tick, a fixed RNG seed
 *     at the top of every loop, the same set-up every loop. The same scene
 *     therefore plays out identically each time, like a recording, while
 *     still being YOUR ship and paints and still the current mechanics.
 *   - THE CAMERA fits the scene: it frames the hunter and the scene's
 *     subjects (zooming out as needed), so nothing important happens off the
 *     edge of the small reel window. The one exception is drawn as an
 *     exception -- the hijacked rock gets an INSET window following it to
 *     a target far outside the frame.
 *   - THE PAGE is annotated: captions, callouts pointing at the real HUD
 *     bars (HudSystem::layout()), outlines, freeze frames on the moment that
 *     matters (the amber QTE zone), and hard cuts between takes.
 *
 * Scenes: BASIC  movement, HUD, gunnery, turbo, salvage, dodge
 *         ADVANCED  parry (objects / rounds / melee), perfect dodge,
 *         Rift Bolt (burst / overload / hijack / direct hit), vent, poise.
 *
 * @author Oleg Ivakhiv
 * @version 2.0 (directed scenes)
 */

#pragma once

#include "utils/ShadowWorld.hpp"
#include "utils/HunterPilot.hpp"      // pilot::Binds
#include "utils/UiPalette.hpp"
#include <memory>
#include <vector>
#include <map>
#include <string>
#include <cstdlib>
#include <cmath>
#include <algorithm>

namespace doctrine {

    struct Lesson {
        const char* name;
        bool        advanced;
        float       length;                 ///< loop length, seconds of scene time
        std::vector<const char*> keys;      ///< keycaps under the scene, in order
        const char* line1;                  ///< what to press
        const char* line2;                  ///< why it matters
    };

    enum Scene : int {
        MOVEMENT = 0, HUD, GUNNERY, TURBO, SALVAGE, DODGE,
        PARRY_OBJECTS, PARRY_ROUNDS, PARRY_MELEE, PERFECT_DODGE,
        RIFT_BURST, RIFT_OVERLOAD, RIFT_HIJACK, RIFT_HIT, VENT, POISE, SCENE_COUNT
    };

    inline const std::vector<Lesson>& lessons() {
        static const std::vector<Lesson> k = {
            { "MOVEMENT", false, 10.0f, { "W", "A", "S", "D", "MOUSE" },
              "W A S D PUSH YOU ACROSS THE SCREEN. THE NOSE FOLLOWS THE MOUSE.",
              "FULL THRUST ALONG THE NOSE, LESS ACROSS IT, LEAST BACKWARDS. NOTHING STOPS YOU BUT THRUST." },
            { "HUD", false, 12.0f, { "SPACE", "SHIFT", "RMB", "LMB" },
              "HULL, POISE, ENERGY, RIFT BOLT, HEAT -- TOP LEFT, ALWAYS.",
              "READ THEM IN THE CORNER OF YOUR EYE: SEGMENTS COUNT FASTER THAN NUMBERS." },
            { "GUNNERY", false, 12.0f, { "LMB", "RMB", "MOUSE" },
              "LEFT MOUSE: MAIN GUN.  RIGHT MOUSE (HOLD): RIFT BOLT.",
              "EVERY SHOT ADDS HEAT. IGNORE THE VENT AND THE GUN LOCKS UNTIL IT COOLS." },
            { "TURBO", false, 6.0f, { "D", "SHIFT" },
              "HOLD LEFT SHIFT FOR TURBO. IT BURNS ENERGY FAST.",
              "ENERGY IS SHARED WITH THE DODGE. A DRY TANK MEANS NO WAY OUT." },
            { "SALVAGE", false, 7.0f, { "D", "W", "S" },
              "FLY THROUGH SCRAP AND IT IS YOURS. THE COUNTER SITS UNDER THE HEAT BAR.",
              "KILLS, WRECKS AND SALVAGE HEAPS DROP IT. THE REFIT BAY SPENDS IT." },
            { "DODGE", false, 7.0f, { "SPACE", "A" },
              "SPACE DASHES THE WAY YOU ARE STEERING. A DASH OUT OF REACH SIMPLY MISSES THE HIT.",
              "IT COSTS ENERGY AND HAS A SHORT RECOVERY. EARLY IS SAFE; LATE IS PERFECT (ADVANCED)." },
            { "PARRY // OBJECTS", true, 9.0f, { "R", "MOUSE" },
              "R PARRIES ANYTHING THAT FLIES AT YOU.",
              "SMALL AND MEDIUM ROCKS SHATTER. LARGE AND EXPLOSIVE ONES ARE BATTED AWAY." },
            { "PARRY // ROUNDS", true, 6.0f, { "R" },
              "TIME R TO THE ROUND, NOT TO THE SHOT.",
              "A PARRIED ROUND FLIES BACK AT WHOEVER FIRED IT. BULLET PARRIES RE-ARM AT ONCE." },
            { "PARRY // MELEE", true, 6.5f, { "R" },
              "THE CYAN CRESCENT IS A BASH: THE ONE MELEE ATTACK MEANT TO BE PARRIED.",
              "PARRY THE LUNGE AND THE ATTACKER IS THROWN OFF BALANCE." },
            { "PERFECT DODGE", true, 7.0f, { "SPACE", "W" },
              "AN AMBER LANE IS A RAM. IT CANNOT BE PARRIED -- DODGE INTO IT, LATE.",
              "PERFECT DODGE: NO DAMAGE, ENERGY REFUNDED, THE DASH RE-ARMS AT ONCE." },
            { "RIFT // BURST", true, 5.0f, { "RMB" },
              "HOLD RIGHT MOUSE TO FIRE. RIGHT MOUSE AGAIN: THE BOLT BURSTS WHERE IT IS.",
              "THE BURST CLEARS ENEMY ROUNDS AROUND IT." },
            { "RIFT // OVERLOAD", true, 5.0f, { "RMB" },
              "DETONATE THE BOLT BESIDE A SHIP.",
              "OVERLOAD: MASSIVE DAMAGE, AND THE SHIP IS STUNNED." },
            { "RIFT // HIJACK", true, 5.5f, { "RMB" },
              "DETONATE THE BOLT BESIDE A ROCK.",
              "HIJACK: THE ROCK BECOMES YOUR MISSILE AND HUNTS THE NEAREST HOSTILE." },
            { "RIFT // DIRECT HIT", true, 4.0f, { "RMB" },
              "OR JUST LET THE BOLT LAND.",
              "A DIRECT HIT: HEAVY DAMAGE AND A STUN-LOCK." },
            { "VENT", true, 10.0f, { "LMB", "E" },
              "AT FULL HEAT THE VENT OPENS. PRESS E WITH THE MARKER IN A ZONE.",
              "BLUE CLEARS THE HEAT. AMBER CLEARS IT AND GRANTS OVERDRIVE: NO HEAT AT ALL." },
            { "POISE", true, 8.0f, { "MOUSE" },
              "POISE IS HOW MUCH PUNISHMENT YOU TAKE BEFORE YOU LOSE CONTROL.",
              "BROKEN POISE = STAGGER. NO CONTROL UNTIL YOU RECOVER. IT REFILLS WHEN THE HITS STOP." },
        };
        return k;
    }

    class Stage {
    public:
        static constexpr float DT = 1.f / 60.f;

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

        void play(int lesson) {
            const int n = static_cast<int>(lessons().size());
            m_lesson = std::clamp(lesson, 0, n - 1);
            m_dirty = true;
        }
        int lesson() const { return m_lesson; }
        float t() const { return m_wt; }
        float length() const { return lessons()[m_lesson].length; }

        /// Real time in; fixed 1/60 s ticks out.
        void step(float dt) {
            if (!m_window || !m_lua || !m_reg) return;
            if (m_dirty || !m_world) { build(); if (!m_world) return; }
            m_acc += std::clamp(dt, 0.f, 0.1f);
            int guard = 0;
            while (m_acc >= DT && guard++ < 6) {
                m_acc -= DT;
                tick();
                if (!m_world) return;
            }
        }

        void render() {
            if (!m_world) return;
            m_world->render(sf::Color(3, 5, 8), [this](sf::RenderTexture& t) { overlay(t); });
        }
        const sf::Texture* texture() const { return m_world ? m_world->texture() : nullptr; }

        /// Lit flag per Lesson::keys entry.
        std::vector<bool> lit() const {
            const auto& L = lessons()[m_lesson];
            std::vector<bool> out(L.keys.size(), false);
            for (std::size_t i = 0; i < L.keys.size(); ++i) {
                const auto it = m_light.find(L.keys[i]);
                out[i] = it != m_light.end() && it->second > 0.f;
            }
            return out;
        }

    private:
        // ==================================================================
        // TYPES
        // ==================================================================
        struct Hands {
            bool up = false, down = false, left = false, right = false;
            bool fire = false, sprint = false, dash = false, parry = false, rift = false, vent = false;
            bool aimSet = false;
            sf::Vector2f aim;
        };
        struct Callout {
            sf::FloatRect rect;          ///< texture px (HUD) -- or a world point, below
            bool world = false;
            sf::Vector2f wpos;
            float wr = 40.f;             ///< world callout radius
            std::string label, sub;
            sf::Color color = ui::AMBER;
        };
        struct Mark {                    ///< where the scene put its actors, for cuts
            uint32_t id;
            sf::Vector2f pos;
        };

        // ==================================================================
        // BUILD
        // ==================================================================
        void build() {
            m_dirty = false;
            m_wt = m_prevWt = 0.f;
            m_acc = 0.f;
            m_freeze = 0.f;
            m_cut = 0.f;
            m_light.clear();
            m_ids.clear();
            m_hold.clear();
            m_focus.clear();
            m_focusPts.clear();
            m_marks.clear();
            m_boltBest = 1e9f;
            m_expiredAt = -1.f;
            m_insetCamInit = false;
            m_rock = m_far = 0;
            m_flags = 0;
            m_insetOn = false;
            m_insetId = 0;
            m_ventNow = false;
            m_hands = Hands{};
            m_cam = { 0.f, 0.f };
            m_camZoom = 1.f;
            m_camInit = false;
            m_lastRot = 0.f;
            m_turn = 0.f;
            m_brokeAt = -1.f;
            m_peak[0] = m_peak[1] = m_peak[2] = 0.f;
            m_burstAt = -1.f; m_erased = 0;
            m_throws = 0; m_wornAt = -1.f; m_killAt = -1.f;
            m_spent.clear();
            m_guided.clear();
            m_targetHp0 = 0.f;
            // Same dice every loop: this is what makes a directed scene play
            // back like a recording.
            std::srand(7919u + static_cast<unsigned>(m_lesson) * 104729u);
            m_ticks = 0;

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
            m_binds.load(*m_lua);
            setup();
        }

        // ==================================================================
        // ONE FIXED TICK
        // ==================================================================
        void tick() {
            // The game's systems roll std::rand(), and so does everything
            // else in the program (menus, UI glitches, particles at draw time)
            // -- a different number of times per tick on a 60 Hz and a 144 Hz
            // screen. So the dice are re-seeded from the scene clock on every
            // tick: what a scene rolls depends on the tick, never on how many
            // frames were drawn in between. The outside sequence is handed
            // back afterwards, still moving.
            struct Dice {
                unsigned outer;
                Dice(unsigned seed) : outer(static_cast<unsigned>(std::rand())) { std::srand(seed); }
                ~Dice() { std::srand(outer); }
            } dice(7919u + static_cast<unsigned>(m_lesson) * 104729u + m_ticks++ * 2654435761u);
            if (m_cut > 0.f) m_cut = std::max(0.f, m_cut - DT);
            if (m_freeze > 0.f) {                    // the world holds its breath
                m_freeze = std::max(0.f, m_freeze - DT);
                decayLights();
                return;
            }
            m_prevWt = m_wt;
            m_wt += DT;
            if (m_wt >= length()) { build(); return; }

            m_hands = Hands{};
            m_hands.aimSet = true;
            m_hands.aim = P() + sf::Vector2f{ 400.f, 0.f };
            direct();
            if (m_freeze > 0.f) { decayLights(); return; }   // the scene froze on this tick
            writePad();
            guideRocks();
            m_world->step(DT);
            holdMarks();
            tether();
            frameCamera();
            measure();
            decayLights();
            lightKeys();
        }

        // ==================================================================
        // SCENE SET-UP
        // ==================================================================
        void setup() {
            ShadowWorld& w = *m_world;
            switch (m_lesson) {
            case MOVEMENT:
                w.rock("LARGE", { 560.f, -190.f });
                w.rock("MEDIUM", { -470.f, 180.f });
                w.rock("MEDIUM", { 240.f, 230.f });
                w.rock("SMALL", { -260.f, -220.f });
                w.rock("SMALL", { 760.f, 120.f });
                break;
            case HUD:
                w.rock("LARGE", { 470.f, 0.f });
                break;
            case GUNNERY:
                m_ids.push_back(w.rock("LARGE", { 430.f, -40.f }));
                m_ids.push_back(w.rock("MEDIUM", { 460.f, 150.f }));
                m_ids.push_back(w.rock("LARGE", { 560.f, -190.f }));
                m_ids.push_back(w.rock("LARGE", { 640.f, 90.f }));
                for (uint32_t id : m_ids) m_focus.push_back(id);
                break;
            case TURBO:
                for (int i = 0; i < 12; ++i)
                    w.rock(i % 3 ? "MEDIUM" : "LARGE", { 520.f + i * 430.f, (i % 2 ? -200.f : 190.f) });
                break;
            case SALVAGE: {
                EntityManager& em = w.em();
                for (int i = 0; i < 18; ++i) {
                    ScrapPickup p;
                    p.position = { 380.f + (i % 6) * 26.f, -40.f + (i / 6) * 34.f + ((i % 2) ? 8.f : 0.f) };
                    p.velocity = { 0.f, 0.f };
                    p.rotation = static_cast<float>(i * 37 % 360);
                    p.spin = (i % 2 ? 40.f : -55.f);
                    p.size = 5.f;
                    p.value = 3;
                    em.scrapPickups.push_back(p);
                }
                m_focusPts.push_back({ 450.f, 0.f });
                break;
            }
            case DODGE:
                enemy("BERSERKER", { 235.f, 0.f }, true, true, true, false);
                break;
            case PARRY_OBJECTS: {
                // The backstop: both batted rocks land on it, so it has to
                // live through the first one for the magma blast to read.
                toughen(enemy("RAIDER", { 620.f, 0.f }, true, true, true, false), 2500.f);
                break;
            }
            case PARRY_ROUNDS:
                enemy("RAIDER", { 470.f, 0.f }, true, true, true, true);
                break;
            case PARRY_MELEE:
                enemy("BERSERKER", { 175.f, 0.f }, true, true, true, false);
                break;
            case PERFECT_DODGE:
                enemy("BERSERKER", { 560.f, 0.f }, true, true, true, false);
                break;
            case RIFT_BURST:
                enemy("RAIDER", { 700.f, -70.f }, true, true, true, true);
                enemy("RAIDER", { 720.f, 80.f }, true, true, true, true);
                break;
            case RIFT_OVERLOAD:
                // Sturdy enough to live through it: the stun is the lesson.
                toughen(enemy("RAIDER", { 540.f, 0.f }, true, true, true, false), 900.f);
                break;
            case RIFT_HIJACK: {
                m_rock = w.rock("LARGE", { 420.f, 30.f });
                m_focus.push_back(m_rock);
                // Far outside the reel's frame on purpose: the inset follows.
                const uint32_t r = enemy("RAIDER", { 1250.f, -430.f }, true, true, true, false, false);
                m_far = r;
                break;
            }
            case RIFT_HIT:
                enemy("BERSERKER", { 540.f, 0.f }, true, true, true, false);
                break;
            case VENT:
                m_ids.push_back(w.rock("LARGE", { 430.f, -60.f }));
                m_ids.push_back(w.rock("LARGE", { 480.f, 110.f }));
                m_ids.push_back(w.rock("LARGE", { 640.f, 10.f }));
                for (uint32_t id : m_ids) m_focus.push_back(id);
                if (auto* ps = pc()) ps->weaponHeat = ps->maxWeaponHeat * 0.78f;
                break;
            case POISE:
                enemy("BERSERKER", { 185.f, 0.f }, true, true, true, false);
                break;
            default: break;
            }
            m_marks.push_back({ m_world->playerId(), { 0.f, 0.f } });
        }

        /// A directed enemy: on its mark, facing the hunter, in COMBAT, and
        /// only ever doing what the scene cues.
        uint32_t enemy(const char* key, sf::Vector2f pos, bool hold, bool noRam, bool noBash,
            bool perfectAim, bool focus = true) {
            const uint32_t id = m_world->summon(key, pos);
            EntityManager& em = m_world->em();
            const std::size_t i = em.getEntityIndex(id);
            if (i == (std::size_t)-1 || i >= em.enemies.size()) return 0;
            auto& e = em.enemies[i];
            e.direct.on = true;
            e.direct.noRam = noRam;
            e.direct.noBash = noBash;
            e.direct.noShoot = true;
            e.direct.perfectAim = perfectAim;
            e.timesHit = std::max(e.timesHit, 2);       // straight to COMBAT (AISystem perception)
            e.detectionRadius = std::max(e.detectionRadius, 2000.f);
            face(i, P());
            m_ids.push_back(id);
            if (hold) m_hold.push_back(id);
            if (focus) m_focus.push_back(id);
            m_marks.push_back({ id, pos });
            return id;
        }

        void toughen(uint32_t id, float hp) {
            if (const std::size_t i = idx(id); i != (std::size_t)-1) {
                auto& h = m_world->em().healths[i];
                h.maxHp = h.currentHp = std::max(h.maxHp, hp);
            }
        }

        void face(std::size_t i, sf::Vector2f at) {
            EntityManager& em = m_world->em();
            const sf::Vector2f d = at - em.transforms[i].position;
            const float deg = std::atan2(d.x, -d.y) * 180.f / 3.14159265f;
            em.transforms[i].rotation = deg;
            if (b2Body_IsValid(em.physics[i].bodyId))
                b2Body_SetTransform(em.physics[i].bodyId, b2Body_GetPosition(em.physics[i].bodyId),
                    b2MakeRot(deg * 3.14159265f / 180.f));
        }

        // ==================================================================
        // THE DIRECTOR: one call per tick, scene clock m_wt
        // ==================================================================
        void direct() {
            switch (m_lesson) {
            case MOVEMENT:      sceneMovement(); break;
            case HUD:           sceneHud(); break;
            case GUNNERY:       sceneGunnery(); break;
            case TURBO:         sceneTurbo(); break;
            case SALVAGE:       sceneSalvage(); break;
            case DODGE:         sceneDodge(); break;
            case PARRY_OBJECTS: sceneParryObjects(); break;
            case PARRY_ROUNDS:  sceneParryRounds(); break;
            case PARRY_MELEE:   sceneParryMelee(); break;
            case PERFECT_DODGE: scenePerfectDodge(); break;
            case RIFT_BURST:    sceneRiftBurst(); break;
            case RIFT_OVERLOAD: sceneRiftOverload(); break;
            case RIFT_HIJACK:   sceneRiftHijack(); break;
            case RIFT_HIT:      sceneRiftHit(); break;
            case VENT:          sceneVent(); break;
            case POISE:         scenePoise(); break;
            default: break;
            }
        }

        // ---- BASIC ---------------------------------------------------------

        void sceneMovement() {
            const float t = m_wt;
            if (at(2.6f) || at(5.0f) || at(7.4f)) cut();
            if (t < 2.6f)      { move({ 1.f, 0.f }); }              // forward: D, along the nose
            else if (t < 5.0f) { move({ 0.f, -1.f }); }             // sideways: W, across the nose
            else if (t < 7.4f) { move({ -1.f, 0.f }); }             // backwards: A
            else if (t < 8.0f) { move({ 1.f, 0.f }); }              // a push, then let go
            if (t >= 7.4f) {
                const float a = (t - 7.4f) * 1.6f;
                m_hands.aim = P() + sf::Vector2f{ std::cos(a), std::sin(a) } * 400.f;
            }
        }

        void sceneHud() {
            const float t = m_wt;
            auto* ps = pc();
            if (!ps) return;
            auto& hp = m_world->em().healths[m_world->playerIndex()];
            if (at(0.6f)) { hp.currentHp = std::max(1.f, hp.currentHp - hp.maxHp * 0.35f); }
            if (at(3.0f)) { ps->poise = ps->poiseMax * 0.35f; ps->poiseHitFlash = 0.3f; ps->poiseRegenTimer = ps->poiseDelay; }
            if (in(5.2f, 5.25f)) { move({ 0.f, -1.f }); m_hands.dash = true; }
            if (in(6.0f, 7.0f)) { move({ 1.f, 0.f }); m_hands.sprint = true; }
            if (in(7.0f, 7.6f)) { move({ -1.f, 0.f }); }            // drift back
            if (m_wt >= 7.6f) flyTo({ 0.f, 0.f }, 60.f);            // settle for the last two bars
            if (in(7.7f, 8.4f)) m_hands.rift = true;
            if (in(9.8f, 11.8f)) m_hands.fire = true;
            m_hands.aim = { 470.f, 0.f };
        }

        void sceneGunnery() {
            const float t = m_wt;
            auto* ps = pc();
            if (!ps) return;
            m_hands.aim = nearestAlive(m_ids, P() + sf::Vector2f{ 500.f, 0.f });
            if (in(0.4f, 2.4f)) m_hands.fire = true;
            if (in(2.7f, 3.4f)) m_hands.rift = true;               // charge; it fires itself when full
            if (at(4.2f)) ps->weaponHeat = std::max(ps->weaponHeat, ps->maxWeaponHeat * 0.62f);
            // ...and keeps the trigger down -- through the lockout, then lets go
            // (once the gun unlocks it would only start the cycle again).
            if (t >= 4.2f && (!(m_flags & 2) || ps->weaponOverheated)) m_hands.fire = true;
            if (ps->qteActive && !(m_flags & 1)) m_flags |= 1;      // the vent opened: and is ignored
            if ((m_flags & 1) && !ps->qteActive && !(m_flags & 2)) { m_flags |= 2; m_expiredAt = t; }
        }

        void sceneTurbo() {
            move({ 1.f, 0.f });
            if (m_wt > 0.6f) m_hands.sprint = true;
            m_hands.aim = P() + sf::Vector2f{ 400.f, 0.f };
        }

        void sceneSalvage() {
            flyTo({ 470.f, 0.f }, 30.f);
            m_hands.aim = P() + sf::Vector2f{ 400.f, 0.f };
        }

        void sceneDodge() {
            const std::size_t ei = idx(firstEnemy());
            if (at(3.2f)) cut();
            if (at(0.8f) || at(4.0f)) cue(firstEnemy(), EnemyComponent::Cue::Bash);
            if (ei != (std::size_t)-1) {
                const auto& e = m_world->em().enemies[ei];
                m_hands.aim = m_world->em().transforms[ei].position;
                // Early and away: out of reach before the lunge starts.
                // Straight across the lunge's line, as it locks on: the lunge
                // commits to where the hunter WAS and hits empty space.
                if (e.bashState == BashState::Windup && e.bashTimer < 0.07f && !(m_flags & 4)) {
                    m_flags |= 4; move({ 0.f, -1.f }); m_hands.dash = true;
                }
                if (e.bashState == BashState::None) m_flags &= ~4u;
            }
            if (!m_hands.dash && m_wt > 1.3f) brake();
        }

        // ---- ADVANCED ------------------------------------------------------

        void sceneParryObjects() {
            const float t = m_wt;
            const sf::Vector2f target = enemyPos(firstEnemy(), { 620.f, 0.f });
            m_hands.aim = target;
            // Four throws: each arrives about 1 s after it is thrown, guided
            // onto the hunter (guideRocks) so it cannot sail past.
            if (at(0.4f)) throwRock("SMALL", { -0.6f, -1.f }, 330.f, 340.f);
            if (at(2.4f)) throwRock("MEDIUM", { -1.f, -0.5f }, 330.f, 340.f);
            // A batted rock leaves along hunter -> rock (DamageSystem: straight,
            // no homing). So these two come in exactly down the Raider's line
            // and go back out exactly AT it -- wherever the first one knocked it.
            // tether() holds the hunter still on that line.
            if (at(4.5f)) throwRock("LARGE", target - P(), 360.f, 320.f);
            if (at(6.6f)) throwRock("MAGMATIC", target - P(), 360.f, 320.f);
            if (parryIncomingRock(0.11f)) m_hands.parry = true;
            (void)t;
        }

        void sceneParryRounds() {
            const uint32_t r = firstEnemy();
            m_hands.aim = enemyPos(r, { 470.f, 0.f });
            if (at(0.6f) || at(3.2f)) cue(r, EnemyComponent::Cue::Shoot);
            if (parryIncomingRound(0.10f)) m_hands.parry = true;
        }

        void sceneParryMelee() {
            const uint32_t b = firstEnemy();
            if (at(3.3f)) cut();
            if (at(0.8f) || at(4.0f)) cue(b, EnemyComponent::Cue::Bash);
            const std::size_t ei = idx(b);
            if (ei != (std::size_t)-1) {
                const auto& e = m_world->em().enemies[ei];
                m_hands.aim = m_world->em().transforms[ei].position;
                if ((e.bashState == BashState::Lunge || (e.bashState == BashState::Windup && e.bashTimer < 0.03f))
                    && !(m_flags & 4)) { m_flags |= 4; m_hands.parry = true; }
                if (e.bashState == BashState::None) m_flags &= ~4u;
            }
        }

        void scenePerfectDodge() {
            const uint32_t b = firstEnemy();
            if (at(3.4f)) cut();
            if (at(0.6f) || at(3.8f)) cue(b, EnemyComponent::Cue::Ram);
            const std::size_t ei = idx(b);
            if (ei == (std::size_t)-1) return;
            const auto& em = m_world->em();
            const auto& e = em.enemies[ei];
            m_hands.aim = em.transforms[ei].position;
            if (e.ramState == RamState::Charge && !(m_flags & 4)) {
                // INTO it, at the last instant: the dodge's i-frames carry the
                // hunter through the charging hull (light / medium phase through
                // it; a heavy shoulder-bashes it) -- that is the perfect dodge.
                const sf::Vector2f E = em.transforms[ei].position;
                const sf::Vector2f dir = norm(e.ramDir);
                const sf::Vector2f rel = P() - E;
                const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[ei].bodyId);
                const float sp = std::max(300.f, std::sqrt(v.x * v.x + v.y * v.y) * SCALE);
                const float rad = m_reg->byId(e.archetype) ? m_reg->byId(e.archetype)->radius : 50.f;
                const float tti = (dot(rel, dir) - rad - 30.f) / sp;
                if (tti < 0.10f) { m_flags |= 4; move(E - P()); m_hands.dash = true; }
            }
            if (e.ramState == RamState::None) m_flags &= ~4u;
        }

        void sceneRiftBurst() {
            m_hands.aim = { 700.f, 5.f };
            if (at(0.25f)) { for (uint32_t id : m_ids) cue(id, EnemyComponent::Cue::Shoot); }
            if (in(0.35f, 1.0f)) m_hands.rift = true;
            auto* ps = pc();
            if (ps && ps->riftBoltInFlight && !(m_flags & 8)) {
                const sf::Vector2f B = boltPos();
                const float R = (*m_lua)["rift_burst_radius"].get_or(120.f);
                int inBlast = 0; float nearest = 1e9f;
                const auto& em = m_world->em();
                for (std::size_t i = 0; i < em.bullets.size() && i < em.transforms.size(); ++i) {
                    const auto& b = em.bullets[i];
                    if (!b.isActive || !b.isEnemyBullet || b.markedForDestroy) continue;
                    const float d = len(em.transforms[i].position - B);
                    nearest = std::min(nearest, d);
                    if (d < R * 0.9f) ++inBlast;
                }
                // In the thick of the volley -- or, failing that, before it gets past.
                if (inBlast >= 2 || nearest < 45.f || len(B - P()) > 560.f) {
                    m_flags |= 8; m_hands.rift = true;
                    m_burstAt = m_wt; m_erased = inBlast; m_burstPos = B;
                }
            }
        }

        void sceneRiftOverload() {
            const uint32_t r = firstEnemy();
            const sf::Vector2f E = enemyPos(r, { 540.f, 0.f });
            // Past it, not into it: clear of the hull, inside the 40 px reach.
            float rad = 30.f;
            if (const std::size_t ei = idx(r); ei != (std::size_t)-1 && ei < m_world->em().enemies.size())
                if (const auto* def = m_reg->byId(m_world->em().enemies[ei].archetype)) rad = def->radius;
            const float skim = std::clamp(rad + 20.f, 40.f, 56.f);
            if (!(m_flags & 8)) m_hands.aim = E + sf::Vector2f{ 0.f, -skim };
            noteTargetHp(r);
            if (in(0.3f, 0.95f)) m_hands.rift = true;
            detonateAtClosestPass(E, 58.f);
        }

        void sceneRiftHijack() {
            const sf::Vector2f R = rockPos(m_rock, { 420.f, 30.f });
            // Skim it: just clear of the rock's edge (the bolt must not touch it),
            // and inside the hijack reach (WeaponSystem::detonate: 40 px of surface).
            float rr = 45.f;
            if (const std::size_t ri = idx(m_rock); ri != (std::size_t)-1) rr = m_world->em().healths[ri].visualRadius;
            const float skim = std::clamp(rr + 22.f, 50.f, 78.f);
            if (!(m_flags & 8)) m_hands.aim = R + sf::Vector2f{ 0.f, -skim };
            else m_hands.aim = P() + sf::Vector2f{ 400.f, 0.f };
            if (in(0.3f, 0.95f)) m_hands.rift = true;
            if (detonateAtClosestPass(R, 82.f)) {
                m_insetOn = true; m_insetId = m_rock; m_insetAt = m_wt;
                m_focus.clear();               // the main frame stays on the hunter; the inset follows the rock
            }
            // Once the rock is gone (it struck), the inset rests on the target --
            // and once that is gone too, it holds on the wreck a moment, then closes.
            if (m_insetOn && idx(m_insetId) == (std::size_t)-1) m_insetId = m_far;
            if (m_insetOn && m_killAt < 0.f && idx(m_rock) == (std::size_t)-1 && idx(m_far) == (std::size_t)-1) m_killAt = m_wt;
            if (m_killAt >= 0.f && m_wt > m_killAt + 1.5f) m_insetOn = false;
        }

        void sceneRiftHit() {
            const uint32_t b = firstEnemy();
            m_hands.aim = enemyPos(b, { 540.f, 0.f });
            noteTargetHp(b);
            if (in(0.3f, 0.95f)) m_hands.rift = true;               // and never detonated
        }

        void sceneVent() {
            auto* ps = pc();
            if (!ps) return;
            m_hands.aim = nearestAlive(m_ids, P() + sf::Vector2f{ 500.f, 0.f });
            if (m_wt > 0.3f) m_hands.fire = true;
            if (m_ventNow) { m_ventNow = false; m_hands.vent = true; m_flags |= 32; return; }
            // Freeze on the moment the marker sits in the amber.
            if (ps->qteActive && !(m_flags & 16)) {
                const float d = std::fabs(ps->qtePos - ps->qteGoodCenter);
                if (d < ps->qtePerfectHalf * 0.35f) {
                    m_flags |= 16;
                    m_freeze = 2.4f;
                    m_ventNow = true;      // pressed on the first tick after the freeze
                }
            }
        }

        void scenePoise() {
            const float t = m_wt;
            auto* ps = pc();
            if (!ps) return;
            const uint32_t b = firstEnemy();
            m_hands.aim = enemyPos(b, { 185.f, 0.f });
            const bool broken = ps->staggerTimer > 0.f || ps->staggerRecoverTimer > 0.f;
            if (broken && m_brokeAt < 0.f) m_brokeAt = t;
            const float frac = ps->poiseMax > 0.f ? ps->poise / ps->poiseMax : 0.f;
            // 1) Wear it down: rocks. A rock only bites above 12 m/s closing
            //    speed (DamageSystem), and each hit buys ~1 s of i-frames, so:
            //    fast, and a second apart. A light hull may break right here.
            if (m_brokeAt < 0.f && !(m_flags & 64)) {
                if (m_throws < 4 && t > 0.3f && std::fmod(t - 0.3f, 1.1f) < DT) {
                    const float a = 0.9f + 0.7f * static_cast<float>(m_throws % 3);
                    ++m_throws;
                    m_spent.push_back(throwRock("MEDIUM", { -std::cos(a), -std::sin(a) * ((m_throws % 2) ? 1.f : -1.f) }, 280.f, 640.f, false));
                }
                const bool spent = m_throws >= 4 && t > 0.3f + 3.f * 1.1f + 0.8f;
                if ((frac < 0.5f && t > 0.8f) || spent) { m_flags |= 64; m_wornAt = t; }
            }
            // A rock that has landed its hit is sent on its way, so it cannot
            // sit beside the hunter and block the next one.
            for (uint32_t& id : m_spent) {
                const std::size_t ri = idx(id);
                if (!id || ri == (std::size_t)-1) { id = 0; continue; }
                const b2BodyId rb = m_world->em().physics[ri].bodyId;
                const sf::Vector2f rel = m_world->em().transforms[ri].position - P();
                const b2Vec2 rv = b2Body_GetLinearVelocity(rb);
                if (len(rel) < 130.f && dot(rel, { rv.x, rv.y }) > 0.f) {
                    const sf::Vector2f out = norm(rel) * (650.f / SCALE);
                    b2Body_SetLinearVelocity(rb, { out.x, out.y });
                    id = 0;
                }
            }
            // 2) Then the heavy hit, on a worn bar (once the i-frames are over):
            //    the bash breaks it. A heavy hull may need another.
            if (m_brokeAt < 0.f && (m_flags & 64) && t > m_wornAt + 0.9f && std::fmod(t - m_wornAt - 0.9f, 1.3f) < DT)
                cue(b, EnemyComponent::Cue::Bash);
            // Back on the mark once control returns, so the refill plays in frame.
            // Sit still for the rocks; once it has tumbled, fly back to the mark.
            if (!broken && m_brokeAt >= 0.f) flyTo({ 0.f, 0.f }, 40.f);   // (before the break, tether() holds it)
        }

        // ==================================================================
        // DIRECTOR HELPERS
        // ==================================================================
        bool at(float s) const { return m_prevWt < s && m_wt >= s; }
        bool in(float a, float b) const { return m_wt >= a && m_wt < b; }

        static float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }
        static float dot(sf::Vector2f a, sf::Vector2f b) { return a.x * b.x + a.y * b.y; }
        static sf::Vector2f norm(sf::Vector2f v) { const float L = len(v); return L > 1e-4f ? v / L : sf::Vector2f{ 1.f, 0.f }; }

        std::size_t idx(uint32_t id) const { return id ? m_world->em().getEntityIndex(id) : (std::size_t)-1; }
        PlayerComponent* pc() {
            const std::size_t pi = m_world->playerIndex();
            return pi == (std::size_t)-1 ? nullptr : &m_world->em().players[pi];
        }
        sf::Vector2f P() const {
            const std::size_t pi = m_world->playerIndex();
            return pi == (std::size_t)-1 ? sf::Vector2f{} : m_world->em().transforms[pi].position;
        }
        uint32_t firstEnemy() const {
            for (uint32_t id : m_ids) {
                const std::size_t i = idx(id);
                if (i == (std::size_t)-1 || i >= m_world->em().enemies.size()) continue;
                const BodyUserData* ud = bodyUD(m_world->em().physics[i].bodyId);
                if (ud && ud->type == BodyType::Enemy) return id;
            }
            return 0;
        }
        sf::Vector2f enemyPos(uint32_t id, sf::Vector2f fallback) const {
            const std::size_t i = idx(id);
            return i == (std::size_t)-1 ? fallback : m_world->em().transforms[i].position;
        }
        sf::Vector2f rockPos(uint32_t id, sf::Vector2f fallback) const { return enemyPos(id, fallback); }
        bool stunned(uint32_t id) const {
            const std::size_t i = idx(id);
            if (i == (std::size_t)-1 || i >= m_world->em().enemies.size()) return false;
            const auto& e = m_world->em().enemies[i];
            return m_world->em().healths[i].stunTimer > 0.f || e.staggerTimer > 0.f;
        }
        sf::Vector2f nearestAlive(const std::vector<uint32_t>& ids, sf::Vector2f fallback) const {
            float best = 1e9f; sf::Vector2f out = fallback;
            for (uint32_t id : ids) {
                const std::size_t i = idx(id);
                if (i == (std::size_t)-1) continue;
                const float d = len(m_world->em().transforms[i].position - P());
                if (d < best) { best = d; out = m_world->em().transforms[i].position; }
            }
            return out;
        }
        sf::Vector2f boltPos() {
            auto* ps = pc();
            if (!ps || !ps->riftBoltInFlight) return P();
            const std::size_t bi = idx(ps->riftBoltEntityId);
            return bi == (std::size_t)-1 ? P() : m_world->em().transforms[bi].position;
        }

        void noteTargetHp(uint32_t id) {
            const std::size_t i = idx(id);
            if (i != (std::size_t)-1 && m_targetHp0 <= 0.f) m_targetHp0 = m_world->em().healths[i].maxHp;
        }
        /// "TARGET HULL" / "STUN" rows for the rift lessons.
        void targetReadout(uint32_t id, std::vector<std::pair<std::string, std::string>>& out) const {
            const std::size_t i = idx(id);
            char b[32];
            if (i == (std::size_t)-1) { out.push_back({ "TARGET", "DESTROYED" }); return; }
            const auto& h = m_world->em().healths[i];
            std::snprintf(b, sizeof b, "%3.0f%%", 100.f * std::max(0.f, h.currentHp) / std::max(1.f, h.maxHp));
            out.push_back({ "TARGET HULL", b });
            const float st = std::max(h.stunTimer, i < m_world->em().enemies.size() ? m_world->em().enemies[i].staggerTimer : 0.f);
            std::snprintf(b, sizeof b, st > 0.f ? "%3.1f S" : "--", st);
            out.push_back({ "STUN", b });
        }

        void cue(uint32_t id, EnemyComponent::Cue c) {
            const std::size_t i = idx(id);
            if (i == (std::size_t)-1 || i >= m_world->em().enemies.size()) return;
            m_world->em().enemies[i].direct.cue = c;
        }

        /// 8 directions, like a keyboard.
        void move(sf::Vector2f d) {
            const float L = len(d);
            if (L < 0.05f) return;
            d /= L;
            const float t = 0.38f;
            m_hands.up = d.y < -t; m_hands.down = d.y > t;
            m_hands.left = d.x < -t; m_hands.right = d.x > t;
        }
        /// Fly to a point and stop there (thrust against the drift when close).
        void flyTo(sf::Vector2f goal, float slack) {
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return;
            const b2Vec2 bv = b2Body_GetLinearVelocity(m_world->em().physics[pi].bodyId);
            const sf::Vector2f V{ bv.x * SCALE, bv.y * SCALE };
            const sf::Vector2f d = goal - P();
            const float L = len(d);
            // Aim for where the drift will carry us, so it arrives instead of orbiting.
            const sf::Vector2f want = d - V * 0.45f;
            if (L > slack || len(V) > 70.f) move(want);
        }

        /// Kill the drift: thrust against it until the ship sits still.
        void brake() {
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return;
            const b2Vec2 bv = b2Body_GetLinearVelocity(m_world->em().physics[pi].bodyId);
            const sf::Vector2f V{ bv.x * SCALE, bv.y * SCALE };
            if (len(V) > 45.f) move(-V);
        }

        /// Throw a rock at the hunter from `fromDir` (unit-ish), `dist` px out.
        uint32_t throwRock(const char* type, sf::Vector2f fromDir, float dist, float speed, bool focus = true) {
            const sf::Vector2f u = norm(fromDir);
            const sf::Vector2f from = P() + u * dist;
            // Lead a drifting hunter: aim where it will be when the rock arrives.
            sf::Vector2f V{};
            if (const std::size_t pi = m_world->playerIndex(); pi != (std::size_t)-1) {
                const b2Vec2 bv = b2Body_GetLinearVelocity(m_world->em().physics[pi].bodyId);
                V = { bv.x * SCALE, bv.y * SCALE };
            }
            const sf::Vector2f hit = P() + V * (dist / std::max(1.f, speed));
            const uint32_t id = m_world->rock(type, from, norm(hit - from) * speed);
            if (id && focus) m_focus.push_back(id);
            if (id) m_guided.push_back({ id, speed });
            return id;
        }

        /// Thrown rocks fly AT the hunter, not at where it was: re-aim every
        /// tick until the last stretch, then let physics take the contact.
        /// A batted rock is the player's, and is never touched.
        void guideRocks() {
            EntityManager& em = m_world->em();
            for (auto& g : m_guided) {
                const std::size_t i = idx(g.first);
                if (!g.first || i == (std::size_t)-1 || em.healths[i].wasParryLaunched) { g.first = 0; continue; }
                const sf::Vector2f rel = P() - em.transforms[i].position;
                if (len(rel) < std::max(10.f, em.healths[i].visualRadius) + 75.f) { g.first = 0; continue; }
                const sf::Vector2f v = norm(rel) * (g.second / SCALE);
                b2Body_SetLinearVelocity(em.physics[i].bodyId, { v.x, v.y });
            }
            m_guided.erase(std::remove_if(m_guided.begin(), m_guided.end(),
                [](const std::pair<uint32_t, float>& g) { return g.first == 0; }), m_guided.end());
        }

        bool parryIncomingRock(float lead) {
            const auto& em = m_world->em();
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return false;
            const auto& ps = em.players[pi];
            if (ps.parryCooldown > 0.f || ps.parryTimer > 0.f) return false;
            const b2Vec2 pv = b2Body_GetLinearVelocity(em.physics[pi].bodyId);
            const sf::Vector2f V{ pv.x * SCALE, pv.y * SCALE };
            for (std::size_t i = 0; i < em.physics.size(); ++i) {
                if (i == pi || !b2Body_IsValid(em.physics[i].bodyId)) continue;
                const BodyUserData* ud = bodyUD(em.physics[i].bodyId);
                if (!ud || ud->type != BodyType::Asteroid) continue;
                const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[i].bodyId);
                const sf::Vector2f rel = em.transforms[i].position - P();
                const sf::Vector2f rv = sf::Vector2f{ v.x * SCALE, v.y * SCALE } - V;
                if (dot(rel, rv) >= 0.f) continue;
                const float r = std::max(10.f, em.healths[i].visualRadius);
                const float gap = len(rel) - r - 30.f;
                if (gap / std::max(1.f, len(rv)) < lead) return true;
            }
            return false;
        }

        bool parryIncomingRound(float lead) {
            const auto& em = m_world->em();
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return false;
            const auto& ps = em.players[pi];
            if (ps.parryCooldown > 0.f || ps.parryTimer > 0.f) return false;
            for (std::size_t i = 0; i < em.bullets.size() && i < em.physics.size(); ++i) {
                const auto& b = em.bullets[i];
                if (!b.isActive || !b.isEnemyBullet || b.markedForDestroy) continue;
                if (!b2Body_IsValid(em.physics[i].bodyId)) continue;
                const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[i].bodyId);
                const sf::Vector2f bv{ v.x * SCALE, v.y * SCALE };
                const sf::Vector2f rel = em.transforms[i].position - P();
                if (dot(rel, bv) >= 0.f) continue;
                if ((len(rel) - 30.f) / std::max(1.f, len(bv)) < lead) return true;
            }
            return false;
        }

        /// Press the Rift key once, on the tick the bolt makes its closest pass
        /// to `at`. WeaponSystem reads the bolt AFTER this tick's physics step,
        /// so judge where it will be then (one tick on), not where it is now --
        /// at ~1100 px/s a tick late is 18 px, enough to miss the 40 px reach.
        bool detonateAtClosestPass(sf::Vector2f at, float within) {
            auto* ps = pc();
            if (!ps || !ps->riftBoltInFlight || (m_flags & 8)) return false;
            const std::size_t bi = idx(ps->riftBoltEntityId);
            if (bi == (std::size_t)-1) return false;
            const b2Vec2 bv = b2Body_GetLinearVelocity(m_world->em().physics[bi].bodyId);
            const sf::Vector2f V{ bv.x * SCALE, bv.y * SCALE };
            const sf::Vector2f next = m_world->em().transforms[bi].position + V * DT;
            const float along = dot(at - next, norm(V));      // > 0: still closing
            if (along < 0.5f * len(V) * DT && len(next - at) < within) {
                m_flags |= 8; m_hands.rift = true; return true;
            }
            return false;
        }

        /// A hard cut between takes: everyone back on their marks, at rest.
        void cut() {
            EntityManager& em = m_world->em();
            for (const auto& m : m_marks) {
                const std::size_t i = idx(m.id);
                if (i == (std::size_t)-1) continue;
                const b2BodyId b = em.physics[i].bodyId;
                if (!b2Body_IsValid(b)) continue;
                b2Body_SetTransform(b, { m.pos.x / SCALE, m.pos.y / SCALE }, b2Body_GetRotation(b));
                b2Body_SetLinearVelocity(b, { 0.f, 0.f });
                b2Body_SetAngularVelocity(b, 0.f);
                em.transforms[i].position = m.pos;
                if (i < em.enemies.size() && m.id != m_world->playerId()) face(i, { 0.f, 0.f });
            }
            if (const std::size_t pi = m_world->playerIndex(); pi != (std::size_t)-1) {
                em.transforms[pi].rotation = 90.f;
                b2Body_SetTransform(em.physics[pi].bodyId, b2Body_GetPosition(em.physics[pi].bodyId),
                    b2MakeRot(90.f * 3.14159265f / 180.f));
            }
            m_flags &= ~4u;
            m_cut = 0.2f;
            m_camInit = false;
        }

        /// The hunter as a target dummy (PARRY // OBJECTS, and POISE until it
        /// breaks): a hit or a parry shoves it, then it eases back onto its
        /// mark -- the lesson is the rock, not the flight.
        void tether() {
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return;
            const b2BodyId b = m_world->em().physics[pi].bodyId;
            if ((m_lesson == POISE && m_brokeAt < 0.f) || m_lesson == PARRY_OBJECTS) {
                const auto& ps = m_world->em().players[pi];
                if (ps.staggerTimer > 0.f || ps.staggerRecoverTimer > 0.f) return;
                const b2Vec2 v0 = b2Body_GetLinearVelocity(b);
                const sf::Vector2f V{ v0.x * SCALE, v0.y * SCALE };
                sf::Vector2f home = -P() * 2.5f;
                if (len(home) > 160.f) home = norm(home) * 160.f;
                const sf::Vector2f v = V + (home - V) * 0.12f;     // a shove decays into the glide home
                b2Body_SetLinearVelocity(b, { v.x / SCALE, v.y / SCALE });
                return;
            }
        }

        /// Directed enemies stay on their marks unless an attack is moving them.
        void holdMarks() {
            EntityManager& em = m_world->em();
            for (uint32_t id : m_hold) {
                const std::size_t i = idx(id);
                if (i == (std::size_t)-1 || i >= em.enemies.size()) continue;
                const auto& e = em.enemies[i];
                const bool attacking = e.bashState == BashState::Lunge || e.bashState == BashState::Recover
                    || e.ramState == RamState::Charge || e.ramState == RamState::Recover;
                const bool reeling = e.staggerTimer > 0.f || e.staggerRecoverTimer > 0.f;
                if (attacking || reeling) continue;
                if (!b2Body_IsValid(em.physics[i].bodyId)) continue;
                // Glide back to the mark (a whiffed lunge or a shove left it elsewhere).
                sf::Vector2f home = em.transforms[i].position;
                for (const auto& m : m_marks) if (m.id == id) home = m.pos;
                sf::Vector2f v = (home - em.transforms[i].position) * 3.f;
                if (len(v) > 280.f) v = norm(v) * 280.f;
                if (len(home - em.transforms[i].position) < 3.f) v = { 0.f, 0.f };
                b2Body_SetLinearVelocity(em.physics[i].bodyId, { v.x / SCALE, v.y / SCALE });
            }
        }

        void writePad() {
            VirtualPad& pad = m_world->pad();
            pad.clear();
            pad.set(m_binds.up, m_hands.up);
            pad.set(m_binds.down, m_hands.down);
            pad.set(m_binds.left, m_hands.left);
            pad.set(m_binds.right, m_hands.right);
            pad.set(m_binds.fire, m_hands.fire);
            pad.set(m_binds.sprint, m_hands.sprint);
            pad.set(m_binds.dash, m_hands.dash);
            pad.set(m_binds.parry, m_hands.parry);
            pad.set(m_binds.rift, m_hands.rift);
            pad.set(m_binds.vent, m_hands.vent);
            pad.aimSet = true;
            pad.aim = m_hands.aim;
        }

        // ==================================================================
        // CAMERA: fit the hunter and the scene's subjects into the frame
        // ==================================================================
        void frameCamera() {
            const sf::Vector2f base(static_cast<float>(m_size.x), static_cast<float>(m_size.y));
            sf::Vector2f lo = P(), hi = P();
            auto add = [&](sf::Vector2f p) { lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); };
            for (uint32_t id : m_focus) {
                const std::size_t i = idx(id);
                if (i == (std::size_t)-1) continue;
                const sf::Vector2f p = m_world->em().transforms[i].position;
                // A batted rock is followed only until it leaves the hunter's
                // neighbourhood; the frame does not chase it into the dark.
                if (m_world->em().healths[i].wasParryLaunched && len(p - P()) > 520.f) continue;
                if (len(p - P()) < 1600.f) add(p);
            }
            for (const auto& p : m_focusPts) add(p);
            if (auto* ps = pc(); ps && ps->riftBoltInFlight && m_lesson != RIFT_HIJACK) add(boltPos());
            const float pad = 170.f;
            const sf::Vector2f need{ (hi.x - lo.x) + pad * 2.f, (hi.y - lo.y) + pad * 2.f };
            float zoom = std::max({ 1.f, need.x / base.x, need.y / base.y });
            zoom = std::min(zoom, 2.2f);
            const sf::Vector2f c = (lo + hi) * 0.5f;
            if (!m_camInit) { m_cam = c; m_camZoom = zoom; m_camInit = true; }
            const float k = 1.f - std::exp(-4.f * DT);
            m_cam += (c - m_cam) * k;
            m_camZoom += (zoom - m_camZoom) * k;
            sf::View& v = m_world->view();
            v.setCenter(m_cam + m_world->shakeOffset());
            v.setSize(base * m_camZoom);
        }

        // ==================================================================
        // MEASURE + KEY LIGHTS
        // ==================================================================
        void measure() {
            const std::size_t pi = m_world->playerIndex();
            if (pi == (std::size_t)-1) return;
            const float r = m_world->em().transforms[pi].rotation;
            float d = r - m_lastRot;
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
            m_turn += (std::fabs(d) / DT - m_turn) * 0.15f;
            m_lastRot = r;
            if (m_lesson == MOVEMENT) {
                const int take = m_wt < 2.6f ? 0 : m_wt < 5.0f ? 1 : m_wt < 7.4f ? 2 : -1;
                if (take >= 0) {
                    const b2Vec2 v = b2Body_GetLinearVelocity(m_world->em().physics[pi].bodyId);
                    m_peak[take] = std::max(m_peak[take], std::sqrt(v.x * v.x + v.y * v.y) * SCALE);
                }
            }
        }

        void decayLights() { for (auto& [k, v] : m_light) v = std::max(0.f, v - DT); }

        void lightKeys() {
            auto on = [&](const char* k, bool b, float hold) { if (b) m_light[k] = std::max(m_light[k], hold); };
            on("W", m_hands.up, 0.05f); on("S", m_hands.down, 0.05f);
            on("A", m_hands.left, 0.05f); on("D", m_hands.right, 0.05f);
            const bool aimMoving = len(m_hands.aim - m_lastAim) > 1.5f;
            m_lastAim = m_hands.aim;
            on("MOUSE", aimMoving, 0.08f);
            on("LMB", m_hands.fire, 0.06f);
            on("SHIFT", m_hands.sprint, 0.05f);
            on("SPACE", m_hands.dash, 0.35f);
            on("R", m_hands.parry, 0.35f);
            on("RMB", m_hands.rift, 0.22f);
            on("E", m_hands.vent, 0.45f);
        }

        // ==================================================================
        // ANNOTATION: what the page says, frame by frame
        // ==================================================================
        void annotate(std::vector<Callout>& co, std::string& title, std::string& sub,
            std::vector<std::pair<std::string, std::string>>& readout) {
            const float t = m_wt;
            const auto& L = m_world->hudLayout();
            auto hudCall = [&](const sf::FloatRect& r, const char* label, const char* s, sf::Color c = ui::AMBER) {
                Callout k; k.rect = r; k.label = label; k.sub = s; k.color = c; co.push_back(k);
            };
            auto worldCall = [&](uint32_t id, float rad, const char* label, const char* s, sf::Color c = ui::AMBER) {
                const std::size_t i = idx(id);
                if (i == (std::size_t)-1) return;
                Callout k; k.world = true; k.wpos = m_world->em().transforms[i].position; k.wr = rad;
                k.label = label; k.sub = s; k.color = c; co.push_back(k);
            };
            auto cap = [&](const char* a, const char* b) { title = a; sub = b; };
            PlayerComponent* ps = pc();
            const EnemyComponent* e0 = nullptr;
            if (const std::size_t ei = idx(firstEnemy()); ei != (std::size_t)-1) e0 = &m_world->em().enemies[ei];

            switch (m_lesson) {
            case MOVEMENT: {
                if (t < 2.6f) cap("FORWARD -- ALONG THE NOSE", "THE MAIN DRIVES PUSH HARDEST THIS WAY. NOSE RIGHT: D IS FORWARD.");
                else if (t < 5.0f) cap("SIDEWAYS -- ACROSS THE NOSE", "ONLY THE SIDE THRUSTERS PUSH THIS WAY. COMPARE THE TOP SPEEDS, TOP RIGHT.");
                else if (t < 7.4f) cap("BACKWARDS -- AGAINST THE NOSE", "SLOWER THAN FORWARD TOO. HOW MUCH, SIDEWAYS AND BACK, IS SET BY THE HULL'S DRIVES.");
                else cap("LET GO -- YOU KEEP DRIFTING", "NO BRAKES IN SPACE. THE NOSE TURNS AT THE HULL'S OWN RATE, HEAVY TURNS SLOWER.");
                const std::size_t pi = m_world->playerIndex();
                if (pi != (std::size_t)-1) {
                    const b2Vec2 v = b2Body_GetLinearVelocity(m_world->em().physics[pi].bodyId);
                    char b[32];
                    std::snprintf(b, sizeof b, "%4.0f PX/S", std::sqrt(v.x * v.x + v.y * v.y) * SCALE);
                    readout.push_back({ "SPEED", b });
                    std::snprintf(b, sizeof b, "%4.0f DEG/S", m_turn);
                    readout.push_back({ "TURN", b });
                    const char* names[3] = { "TOP FORWARD", "TOP SIDEWAYS", "TOP BACKWARDS" };
                    for (int k = 0; k < 3; ++k) {
                        if (m_peak[k] <= 0.f) continue;
                        std::snprintf(b, sizeof b, "%4.0f PX/S", m_peak[k]);
                        readout.push_back({ names[k], b });
                    }
                }
                break;
            }
            case HUD:
                if (t < 2.4f) hudCall(L.hull, "HULL", "RED. EMPTY AND YOU ARE DEAD. THE PALE TAIL IS WHAT YOU JUST LOST.", ui::RED);
                else if (t < 4.8f) hudCall(L.poise, "POISE", "ORANGE. HITS WEAR IT DOWN. AT ZERO THE NEXT HEAVY HIT STAGGERS YOU.", ui::AMBER_HOT);
                else if (t < 7.4f) hudCall(L.energy, "ENERGY", "BLUE. TURBO, DODGE AND THE RIFT BOLT DRAW FROM IT. IT REFILLS BY ITSELF.", ui::CYAN);
                else if (t < 9.6f) hudCall(L.rift, "RIFT BOLT", "PURPLE. EMPTIES ON EVERY BOLT, FILLS AS IT RECHARGES. FULL AND PULSING = READY.", ui::VIOLET);
                else hudCall(L.heat, "HEAT", "SEGMENTED. EVERY SHOT ADDS HEAT, IT COOLS WHEN YOU STOP. FULL = THE GUN LOCKS.", ui::HAZARD);
                break;
            case GUNNERY:
                if (t < 2.6f) cap("LEFT MOUSE -- MAIN GUN", "FAST PLASMA. AIM WITH THE MOUSE: THE NOSE FOLLOWS IT.");
                else if (t < 4.2f) cap("RIGHT MOUSE -- RIFT BOLT", "HOLD TO CHARGE; IT FIRES ITSELF WHEN FULL. HEAVY, AND SLOW TO RECHARGE.");
                else if (!(m_flags & 1)) { cap("HEAT", "EVERY SHOT ADDS HEAT. KEEP THE TRIGGER DOWN AND WATCH IT CLIMB."); hudCall(L.heat, "HEAT", "", ui::HAZARD); }
                else if (!(m_flags & 2)) { cap("OVERHEAT -- THE VENT OPENS", "A FEW SECONDS TO VENT (SEE ADVANCED: VENT). HERE IT IS IGNORED..."); hudCall(L.heat, "VENT QTE", "PRESS E IN A ZONE -- OR DON'T", ui::CYAN); }
                else if (ps && ps->weaponOverheated) { cap("...AND THE GUN LOCKS", "NO SHOOTING UNTIL THE BAR DRAINS. THE TRIGGER IS STILL DOWN; NOTHING COMES OUT."); hudCall(L.heat, "LOCKED", "COOLING", ui::HAZARD); }
                else { cap("UNLOCKED -- AFTER THE WHOLE BAR DRAINED", "THAT WAS THE PRICE OF IGNORING THE VENT. ADVANCED: VENT SHOWS THE WAY OUT."); hudCall(L.heat, "HEAT", "", ui::HAZARD); }
                break;
            case TURBO:
                if (ps && ps->overheatTimer > 0.f) { cap("DRY TANK -- ENGINES OVERHEAT", "NO TURBO AND NO DODGE UNTIL THEY RECOVER. NEVER RUN IT DRY IN A FIGHT."); hudCall(L.energy, "ENERGY", "EMPTY", ui::HAZARD); }
                else { cap("LEFT SHIFT -- TURBO", "FULL BURN ALONG THE NOSE. THE ENERGY BAR DRAINS WHILE YOU HOLD IT."); hudCall(L.energy, "ENERGY", "DRAINING", ui::CYAN); }
                break;
            case SALVAGE:
                hudCall(L.scrap, "SCRAP", "YOUR MONEY. EVERY CUBE YOU FLY THROUGH COUNTS UP HERE.", ui::AMBER);
                if (t < 3.6f) cap("FLY THROUGH IT", "LOOSE SCRAP PULLS ITSELF IN ONCE YOU ARE CLOSE.");
                else cap("WHERE IT COMES FROM -- WHAT IT BUYS", "KILLS, WRECKS AND SALVAGE HEAPS DROP IT. THE REFIT BAY TURNS IT INTO PARTS AND REFITS.");
                break;
            case DODGE:
                if (e0 && e0->bashState == BashState::Windup) worldCall(firstEnemy(), 70.f, "CYAN CRESCENT", "A BASH IS COMING", ui::CYAN);
                cap("SPACE -- DASH OUT OF REACH", "THE DASH GOES WHERE YOU STEER (HERE: W). OFF THE LUNGE'S LINE AS IT COMMITS: IT HITS NOTHING.");
                if (ps && ps->dashCooldown > 0.f) hudCall(L.energy, "ENERGY", "EVERY DASH COSTS SOME", ui::CYAN);
                break;
            case PARRY_OBJECTS:
                if (t < 4.4f) cap("SMALL AND MEDIUM ROCKS -- SHATTERED", "R THE MOMENT IT REACHES YOU. NO DAMAGE, AND THE ROCK IS GONE.");
                else cap("LARGE AND EXPLOSIVE ROCKS -- BATTED AWAY", "THEY FLY OFF AS YOUR ROUND. A MAGMA ROCK GOES OFF WHERE IT LANDS.");
                break;
            case PARRY_ROUNDS:
                if (e0 && e0->telegraphActive) worldCall(firstEnemy(), 60.f, "NOSE LIT", "A SHOT IS COMING", ui::AMBER);
                cap("R -- PARRY THE ROUND", "TIME IT TO THE ROUND ARRIVING, NOT TO THE SHOT. IT FLIES BACK AT THE SHOOTER.");
                break;
            case PARRY_MELEE:
                if (e0 && e0->bashState == BashState::Windup) worldCall(firstEnemy(), 70.f, "CYAN CRESCENT", "PARRY THIS", ui::CYAN);
                cap("R -- PARRY THE BASH", "PARRY ON THE LUNGE. THE ATTACKER IS THROWN OFF BALANCE, OPEN TO PUNISHMENT.");
                break;
            case PERFECT_DODGE:
                if (e0 && e0->ramState == RamState::Windup) worldCall(firstEnemy(), 80.f, "AMBER LANE", "A RAM. CANNOT BE PARRIED", ui::AMBER_HOT);
                cap("DODGE INTO IT -- AT THE LAST INSTANT", "THE DASH'S I-FRAMES EAT THE RAM: NO DAMAGE, ENERGY BACK, THE DASH RE-ARMS AT ONCE.");
                if (ps && ps->dodgeVerdictFlash > 0.f) hudCall(L.energy, "PERFECT DODGE", "ENERGY REFUNDED", ui::CYAN);
                break;
            case RIFT_BURST:
                if (ps && ps->riftCharging) cap("HOLD RIGHT MOUSE -- CHARGE", "IT FIRES ITSELF WHEN THE CHARGE IS FULL.");
                else if (!(m_flags & 8)) cap("RIGHT MOUSE AGAIN...", "...AND THE BOLT BURSTS WHERE IT IS.");
                else {
                    cap("BURST", "THE BLAST ERASES ENEMY ROUNDS AROUND IT. OPEN SPACE: A SHIELD YOU THROW.");
                    if (m_wt < m_burstAt + 2.2f) {
                        Callout k; k.world = true; k.wpos = m_burstPos;
                        k.wr = (*m_lua)["rift_burst_radius"].get_or(120.f);
                        k.label = std::to_string(m_erased) + (m_erased == 1 ? " ROUND ERASED" : " ROUNDS ERASED");
                        k.sub = "INSIDE THE BLAST"; k.color = ui::VIOLET; co.push_back(k);
                    }
                }
                break;
            case RIFT_OVERLOAD:
                if (!(m_flags & 8)) cap("FIRE PAST THE SHIP...", "CLOSE BESIDE IT, NOT INTO IT.");
                else { cap("OVERLOAD", "DETONATED BESIDE A HULL: MASSIVE DAMAGE, AND THE SHIP IS STUNNED."); if (stunned(firstEnemy())) worldCall(firstEnemy(), 70.f, "STUNNED", "", ui::VIOLET); }
                targetReadout(m_ids.empty() ? 0 : m_ids[0], readout);
                break;
            case RIFT_HIJACK:
                if (!(m_flags & 8)) cap("FIRE PAST THE ROCK...", "DETONATE BESIDE IT.");
                else if (m_killAt < 0.f) cap("HIJACK", "THE ROCK IS YOURS NOW: IT HUNTS THE NEAREST HOSTILE -- EVEN ONE YOU CANNOT SEE.");
                else cap("A KILL OUTSIDE THE FRAME", "YOU NEVER SAW IT. THE ROCK DID: ONE BOLT, ONE CLICK, ONE KILL.");
                break;
            case RIFT_HIT:
                if (e0 && stunned(firstEnemy())) worldCall(firstEnemy(), 70.f, "STUN-LOCKED", "", ui::VIOLET);
                cap("DIRECT HIT", "NO SECOND CLICK: LET THE BOLT LAND. HEAVY DAMAGE AND A STUN-LOCK.");
                targetReadout(m_ids.empty() ? 0 : m_ids[0], readout);
                break;
            case VENT:
                if (m_freeze > 0.f) {
                    cap("NOW -- THE MARKER IS IN THE AMBER", "PRESS E. BLUE CLEARS THE HEAT. AMBER CLEARS IT AND GRANTS OVERDRIVE.");
                    hudCall(L.heat, "AMBER ZONE", "PRESS E HERE", ui::AMBER_HOT);
                }
                else if (ps && ps->overdriveTimer > 0.f) { cap("OVERDRIVE", "NO HEAT AT ALL FOR A FEW SECONDS. KEEP FIRING."); hudCall(L.heat, "COOLANT LOCK", "NO HEAT", ui::AMBER_HOT); }
                else if (ps && ps->qteActive) { cap("THE VENT IS OPEN", "A MARKER SWEEPS THE BAR. WAIT FOR THE ZONE..."); hudCall(L.heat, "VENT", "", ui::CYAN); }
                else { cap("FIRE INTO THE OVERHEAT", "AT FULL HEAT THE VENT OPENS INSTEAD OF A PLAIN LOCKOUT."); hudCall(L.heat, "HEAT", "", ui::HAZARD); }
                break;
            case POISE: {
                hudCall(L.poise, "POISE", "", ui::AMBER_HOT);
                if (ps) {
                    char b[32];
                    std::snprintf(b, sizeof b, "%3.0f%%", 100.f * std::max(0.f, ps->poise) / std::max(1.f, ps->poiseMax));
                    readout.push_back({ "POISE", b });
                    const float st = ps->staggerTimer > 0.f ? ps->staggerTimer : ps->staggerRecoverTimer;
                    std::snprintf(b, sizeof b, st > 0.f ? "%3.1f S" : "--", st);
                    readout.push_back({ ps->staggerTimer > 0.f ? "NO CONTROL" : "RECOVERING", b });
                    if (ps->staggerTimer > 0.f) {
                        Callout k; k.world = true; k.wpos = P(); k.wr = 60.f;
                        k.label = "STUN-LOCKED"; k.sub = "TUMBLING -- NO CONTROL"; k.color = ui::HAZARD; co.push_back(k);
                    }
                }
                if (m_brokeAt < 0.f && !(m_flags & 64)) cap("EVERY HIT WEARS POISE DOWN", "ROCKS, BASHES AND RAMS TAKE A BITE. CHIP FIRE SOFTENS IT, BUT CAN NEVER BREAK IT ALONE.");
                else if (m_brokeAt < 0.f) cap("A HEAVY HIT BREAKS IT", "THE LOWER IT IS, THE CLOSER THE NEXT BIG HIT IS TO A STAGGER.");
                else if (ps && ps->staggerTimer > 0.f) cap("BROKEN -- STUN-LOCKED", "THE SHIP TUMBLES: NO CONTROL AT ALL. ONLY A DODGE BREAKS OUT EARLY.");
                else if (ps && ps->staggerRecoverTimer > 0.f) cap("RECOVERING", "THE AIM COMES BACK FIRST, THEN THE DRIVES.");
                else cap("THE BAR IS FULL AGAIN", "A BREAK REFILLS IT -- THE TUMBLE WAS THE PRICE. BETWEEN HITS IT REFILLS AFTER A QUIET SPELL.");
                break;
            }
            default: break;
            }
        }

        // ==================================================================
        // OVERLAY DRAWING (texture px)
        // ==================================================================
        void overlay(sf::RenderTexture& t) {
            std::vector<Callout> co;
            std::string title, sub;
            std::vector<std::pair<std::string, std::string>> readout;
            annotate(co, title, sub, readout);
            const sf::Vector2f S(static_cast<float>(t.getSize().x), static_cast<float>(t.getSize().y));
            const float pulse = 0.65f + 0.35f * std::sin(m_wt * 9.f);

            // ---- inset window (the exception to "everything in frame") ----
            if (m_insetOn) drawInset(t, S);

            // ---- callouts ----
            for (const auto& c : co) {
                sf::FloatRect r = c.rect;
                if (c.world) {
                    const sf::Vector2i px = t.mapCoordsToPixel(c.wpos, m_world->worldView());
                    const float rr = c.wr / std::max(0.5f, m_camZoom);
                    r = { { px.x - rr, px.y - rr }, { rr * 2.f, rr * 2.f } };
                }
                if (r.size.x <= 0.f) continue;
                outline(t, r, ui::alpha(c.color, pulse));
                const float lx = r.position.x + r.size.x + 46.f;
                const float ly = c.world ? r.position.y - 30.f : r.position.y - 4.f;
                line(t, { r.position.x + r.size.x + 4.f, r.position.y + r.size.y * 0.5f }, { lx - 6.f, ly + 9.f }, ui::alpha(c.color, 0.9f));
                const float w = std::max(textW(c.label, 16, 2.f), textW(c.sub, 12, 1.4f)) + 22.f;
                const float h = c.sub.empty() ? 26.f : 44.f;
                ui::fill(t, lx - 6.f, ly - 2.f, w, h, ui::alpha(ui::VOID_BG, 0.86f));
                ui::fill(t, lx - 6.f, ly - 2.f, 3.f, h, c.color);
                text(t, c.label, lx + 4.f, ly + 1.f, 16, c.color, 2.f);
                if (!c.sub.empty()) text(t, c.sub, lx + 4.f, ly + 22.f, 12, ui::TEXT, 1.4f);
            }

            // ---- readout (top right) ----
            if (!readout.empty()) {
                const float x = S.x - 250.f, y = 20.f;
                ui::fill(t, x, y, 230.f, 12.f + 26.f * readout.size(), ui::alpha(ui::VOID_BG, 0.86f));
                ui::brackets(t, x, y, 230.f, 12.f + 26.f * readout.size(), ui::CYAN_MID, 8.f);
                for (std::size_t i = 0; i < readout.size(); ++i) {
                    text(t, readout[i].first, x + 12.f, y + 9.f + 26.f * i, 12, ui::TEXT_DIM, 1.8f);
                    text(t, readout[i].second, x + 218.f - textW(readout[i].second, 16, 1.4f), y + 6.f + 26.f * i, 16, ui::CYAN, 1.4f);
                }
            }

            // ---- caption (bottom centre) ----
            if (!title.empty()) {
                const float w = std::max(textW(title, 18, 2.2f), textW(sub, 12, 1.4f)) + 40.f;
                const float x = (S.x - w) * 0.5f, y = S.y - 70.f;
                ui::fill(t, x, y, w, 54.f, ui::alpha(ui::VOID_BG, 0.86f));
                ui::brackets(t, x, y, w, 54.f, ui::AMBER, 9.f);
                text(t, title, x + 20.f, y + 8.f, 18, ui::AMBER, 2.2f);
                text(t, sub, x + 20.f, y + 32.f, 12, ui::TEXT, 1.4f);
            }

            // ---- freeze chip / cut flash ----
            if (m_freeze > 0.f) {
                const std::string s = "PAUSED  //  " + std::to_string(static_cast<int>(std::ceil(m_freeze)));
                const float w = textW(s, 14, 2.2f) + 24.f;
                ui::fill(t, (S.x - w) * 0.5f, 14.f, w, 26.f, ui::AMBER);
                text(t, s, (S.x - w) * 0.5f + 12.f, 18.f, 14, ui::INK, 2.2f);
            }
            if (m_cut > 0.f) ui::fill(t, 0.f, 0.f, S.x, S.y, sf::Color(2, 3, 5, static_cast<uint8_t>(255.f * std::clamp(m_cut / 0.2f, 0.f, 1.f))));
        }

        void drawInset(sf::RenderTexture& t, sf::Vector2f S) {
            sf::Vector2f c = enemyPos(m_insetId, m_insetCam);
            if (!m_insetCamInit) { m_insetCam = c; m_insetCamInit = true; }
            m_insetCam += (c - m_insetCam) * 0.2f;
            const sf::FloatRect box{ { S.x - 470.f, 54.f }, { 450.f, 260.f } };
            ui::fill(t, box.position.x - 2.f, box.position.y - 2.f, box.size.x + 4.f, box.size.y + 4.f, ui::alpha(ui::AMBER, 0.55f));
            ui::fill(t, box.position.x, box.position.y, box.size.x, box.size.y, sf::Color(9, 13, 20));
            sf::View v(m_insetCam, { 900.f, 520.f });
            v.setViewport(sf::FloatRect({ box.position.x / S.x, box.position.y / S.y }, { box.size.x / S.x, box.size.y / S.y }));
            // A fixed world grid of faint marks: the window follows the rock,
            // so without something nailed to space it would look parked.
            {
                const float g = 110.f;
                sf::VertexArray marks(sf::PrimitiveType::Lines);
                const sf::Color mc(70, 95, 120, 150);
                for (float gx = std::floor((m_insetCam.x - 500.f) / g) * g; gx < m_insetCam.x + 500.f; gx += g)
                    for (float gy = std::floor((m_insetCam.y - 300.f) / g) * g; gy < m_insetCam.y + 300.f; gy += g) {
                        marks.append({ { gx - 4.f, gy }, mc }); marks.append({ { gx + 4.f, gy }, mc });
                        marks.append({ { gx, gy - 4.f }, mc }); marks.append({ { gx, gy + 4.f }, mc });
                    }
                t.setView(v);
                t.draw(marks);
                t.setView(t.getDefaultView());
            }
            m_world->drawWorld(v);
            ui::brackets(t, box.position.x, box.position.y, box.size.x, box.size.y, ui::AMBER, 14.f);
            ui::fill(t, box.position.x, box.position.y - 22.f, textW("INSET // THE HIJACKED ROCK", 12, 2.f) + 18.f, 20.f, ui::AMBER);
            text(t, "INSET // THE HIJACKED ROCK", box.position.x + 9.f, box.position.y - 19.f, 12, ui::INK, 2.f);
        }

        // ---- tiny draw kit (texture px) ----
        float textW(const std::string& s, unsigned size, float sp) const {
            if (!m_font || s.empty()) return 0.f;
            sf::Text tx(*m_font, s, size);
            tx.setLetterSpacing(sp);
            return tx.getLocalBounds().size.x;
        }
        void text(sf::RenderTarget& t, const std::string& s, float x, float y, unsigned size, sf::Color c, float sp) const {
            if (!m_font || s.empty()) return;
            sf::Text tx(*m_font, s, size);
            tx.setLetterSpacing(sp);
            tx.setFillColor(c);
            tx.setPosition({ std::round(x), std::round(y) });
            t.draw(tx);
        }
        static void line(sf::RenderTarget& t, sf::Vector2f a, sf::Vector2f b, sf::Color c) {
            const sf::Vertex v[2] = { { a, c }, { b, c } };
            t.draw(v, 2, sf::PrimitiveType::Lines);
        }
        static void outline(sf::RenderTarget& t, sf::FloatRect r, sf::Color c) {
            const float x = r.position.x - 5.f, y = r.position.y - 5.f, w = r.size.x + 10.f, h = r.size.y + 10.f;
            ui::fill(t, x, y, w, 2.f, c); ui::fill(t, x, y + h - 2.f, w, 2.f, c);
            ui::fill(t, x, y, 2.f, h, c); ui::fill(t, x + w - 2.f, y, 2.f, h, c);
        }

        // ==================================================================
        // STATE
        // ==================================================================
        sf::RenderWindow* m_window = nullptr;
        sol::state* m_lua = nullptr;
        const enemyarch::EnemyRegistry* m_reg = nullptr;
        const zonearch::ZoneState* m_zoneState = nullptr;
        const sf::Font* m_font = nullptr;
        const ship::ShipDesign* m_design = nullptr;
        const ship::Livery* m_livery = nullptr;
        sf::Vector2u m_size{ 1340, 520 };
        bool m_dirty = true;
        int m_lesson = 0;
        std::unique_ptr<ShadowWorld> m_world;
        pilot::Binds m_binds;

        float m_wt = 0.f, m_prevWt = 0.f, m_acc = 0.f;
        float m_freeze = 0.f, m_cut = 0.f;
        unsigned m_flags = 0;
        unsigned m_ticks = 0;                  ///< ticks since the loop began: the per-tick seed
        Hands m_hands;
        sf::Vector2f m_lastAim;
        std::vector<uint32_t> m_ids, m_hold, m_focus;
        std::vector<sf::Vector2f> m_focusPts;
        std::vector<Mark> m_marks;
        uint32_t m_rock = 0, m_far = 0;
        float m_boltBest = 1e9f;
        bool m_ventNow = false;
        float m_expiredAt = -1.f, m_brokeAt = -1.f;
        bool m_insetOn = false, m_insetCamInit = false;
        uint32_t m_insetId = 0;
        float m_insetAt = 0.f;
        sf::Vector2f m_insetCam;
        sf::Vector2f m_cam;
        float m_camZoom = 1.f;
        bool m_camInit = false;
        float m_lastRot = 0.f, m_turn = 0.f;
        float m_peak[3] = { 0.f, 0.f, 0.f };   ///< MOVEMENT: top speed per take
        float m_burstAt = -1.f;                ///< RIFT // BURST: when it went off
        int m_throws = 0;                      ///< POISE: rocks thrown so far
        float m_killAt = -1.f;                 ///< RIFT // HIJACK: when the rock struck home
        float m_wornAt = -1.f;                 ///< POISE: when the bar fell under half
        std::vector<uint32_t> m_spent;         ///< POISE: thrown rocks still to be cleared
        std::vector<std::pair<uint32_t, float>> m_guided;   ///< thrown rocks still homing on the hunter (id, px/s)
        int m_erased = 0;                      ///< ...and how many rounds it took with it
        sf::Vector2f m_burstPos;
        float m_targetHp0 = 0.f;               ///< RIFT: the target's hull before the bolt
        std::map<std::string, float> m_light;
    };

} // namespace doctrine
