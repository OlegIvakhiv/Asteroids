/**
 * @file DevSystem.hpp
 * @brief The tilde dev menu: cheats, spawning, enemy control, time, camera,
 *        AI overlays and a stats panel.
 *
 * ============================================================================
 * WHY IT IS NOT A NORMAL update()
 * ============================================================================
 * The dev menu has to act at four different points of the frame, and getting
 * any of them wrong breaks something real:
 *
 *   frameBegin()  before logic   tilde, input capture, god-mode mirror
 *   beginPass()   each sim pass  one-shot HP snapshot
 *   endPass()     each sim pass  energy / cooldown / one-shot enforcement
 *   drawWorld()   world view     vision cones, aim lines, selection ring
 *   drawOverlay() screen view    labels, stats, the menu, and ALL actions
 *
 * Actions (spawn, kill, teleport, clear) run from drawOverlay(), i.e. at the
 * END of the frame, after every system has finished iterating. That is the
 * only moment it is safe to create or destroy entities from outside a system:
 * nobody is mid-loop, and nobody is holding an `auto&` into a component
 * vector. PhysicsSystem::cleanup() relies on the same property.
 *
 * ============================================================================
 * ENGINE LIMITS THIS FILE RESPECTS
 * ============================================================================
 *  - RESERVE. Anything created here is refused past dev::ENTITY_BUDGET, and
 *    NO COOLDOWNS floors the fire interval instead of zeroing it.
 *  - TELEPORT goes through b2Body_SetTransform. Writing the transform alone is
 *    overwritten by PhysicsSystem's body -> transform sync the next step.
 *  - KILL ALL sets HP to 0 and lets DamageSystem's death pass do the rest, so
 *    score, death FX and Maniac detonation behave as in real play.
 *  - CLEAR ASTEROIDS destroys directly. HP 0 would fracture every rock into
 *    two or three more, which is the opposite of clearing.
 *  - DESPAWN. PhysicsSystem culls asteroids and enemies past a radius from the
 *    player. A spawn beyond it would vanish next frame, so it is refused with
 *    a message instead of silently doing nothing.
 *  - SELECTION is a persistent entity id, never an index. Swap-and-pop moves
 *    indices under you every time anything dies.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/components.hpp"
#include "utils/DevState.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/TerminalUI.hpp"
#include "systems/EnemySystem.hpp"
#include "systems/AISystem.hpp"
#include "systems/DebugSystem.hpp"
#include <SFML/Graphics.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>

class DevSystem : public ISystem {
public:
    // ========================================================================
    // SETUP
    // ========================================================================

    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_window = ctx.window;
        m_gameView = ctx.gameView;
        m_registry = ctx.enemyRegistry;
        m_dev = ctx.dev;

        m_ui.attach(m_window);
        m_pick = Pick::None;
        m_hpSnap.clear();
        m_toastTimer = 0.f;
    }

    /// Unused: the dev system acts at fixed points of the frame, see the file
    /// header. Kept only to satisfy ISystem.
    void update(float) override {}

    void setFont(sf::Font* font) { m_ui.setFonts(font, nullptr); }

    /// Systems whose public API the menu drives. All three are owned by
    /// SystemManager and outlive this object.
    void setSystems(EnemySystem* enemies, AISystem* ai, DebugSystem* debug) {
        m_enemySys = enemies;
        m_aiSys = ai;
        m_debugSys = debug;
    }

    // ========================================================================
    // FRAME PHASES
    // ========================================================================

    /**
     * @brief Before any logic. Tilde, input capture, god-mode mirror, FPS.
     *
     * Tilde is read as a SCANCODE: the key under Esc, whatever the layout
     * prints on it. On an Italian or Ukrainian layout sf::Keyboard::Key::Grave
     * is somewhere else entirely or nowhere at all.
     */
    void frameBegin(float realDt) {
        if (!m_dev || !m_em) return;

        // ---- FPS, smoothed so the readout is legible ----
        if (realDt > 0.f) {
            const float inst = 1.f / realDt;
            m_fps = (m_fps <= 0.f) ? inst : m_fps + (inst - m_fps) * 0.08f;
            m_frameMs = m_frameMs + (realDt * 1000.f - m_frameMs) * 0.08f;
        }

#ifdef VH_DEV
        const bool grave = sf::Keyboard::isKeyPressed(sf::Keyboard::Scan::Grave);
        if (grave && !m_graveWas) {
            m_dev->menuOpen = !m_dev->menuOpen;
            m_pick = Pick::None;
            if (m_dev->menuOpen) {
                m_ui.primeInput();    // the click that is already down cannot fire
                m_ui.resetReveal();   // CRT open, same as every other terminal
            }
        }
        m_graveWas = grave;
#endif

        m_em->godMode = m_dev->godMode;

        // ---- Input capture ----
        //
        // Mouse: blocked while pointing at the panel or while a pick mode is
        // armed, and KEPT blocked until both buttons are released after that,
        // so the RMB that cancels a pick does not also charge a Rift shot.
        const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
        const sf::Vector2f mp(static_cast<float>(pix.x), static_cast<float>(pix.y));
        const bool overPanel = m_dev->menuOpen && m_panelRect.contains(mp);
        const bool anyButton = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left)
            || sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);

        if (overPanel || (m_dev->menuOpen && m_pick != Pick::None)) m_mouseHold = true;
        else if (!anyButton) m_mouseHold = false;

        InputRegistry::setBlocked(m_dev->freeCam, m_mouseHold);
    }

    /// Start of one simulation pass. Snapshot enemy HP for ONE-SHOT.
    void beginPass() {
        if (!m_dev || !m_em || !m_dev->oneShot) return;
        m_hpSnap.clear();
        forEachEnemy([&](size_t i) {
            m_hpSnap[m_em->transforms[i].entityId] = m_em->healths[i].currentHp;
            });
    }

    /**
     * @brief End of one simulation pass. Re-assert every running cheat.
     *
     * Enforcing AFTER the logic systems, rather than patching each of them,
     * means WeaponSystem, InputSystem and DamageSystem stay untouched. The
     * value a cheat writes here is what the NEXT pass starts from.
     *
     * ONE-SHOT: there is no single funnel for damage TO enemies (about a dozen
     * sites write currentHp directly), so instead of multiplying damage we
     * look at the result: any enemy that lost HP this pass loses all of it.
     * DamageSystem's death pass collects it on the next pass. Side effect:
     * an asteroid bump or friendly rocket also one-shots. For a test cheat,
     * that is fine.
     */
    void endPass() {
        if (!m_dev || !m_em) return;

        const size_t p = m_em->getEntityIndex(m_playerEntityId);
        if (p != (size_t)-1 && p < m_em->players.size()) {
            auto& ps = m_em->players[p];

            if (m_dev->infiniteEnergy) {
                ps.energyDrive = ps.maxEnergyDrive;
                ps.overheatTimer = 0.f;
                ps.weaponHeat = 0.f;
                ps.heatCoolDelay = 0.f;
                ps.weaponOverheated = false;   // VentQTESystem closes an open QTE on this edge
            }

            if (m_dev->noCooldowns) {
                ps.dashCooldown = 0.f;
                ps.parryCooldown = 0.f;
                ps.riftCooldown = 0.f;
                if (m_em->transforms.size() < dev::ENTITY_BUDGET)
                    ps.shootTimer = std::min(ps.shootTimer, dev::NO_CD_FIRE_INTERVAL);
            }
        }

        if (m_dev->oneShot && !m_hpSnap.empty()) {
            forEachEnemy([&](size_t i) {
                auto it = m_hpSnap.find(m_em->transforms[i].entityId);
                if (it == m_hpSnap.end()) return;
                float& hp = m_em->healths[i].currentHp;
                if (hp > 0.f && hp < it->second - 0.001f) hp = 0.f;
                });
        }
    }

    /// World-space debug geometry. Call with the world view installed.
    void drawWorld() {
        if (!m_dev || !m_em || !m_window) return;
        if (m_dev->showVisionCones) drawVisionCones();
        if (m_dev->showAimLines)    drawAimLines();
        drawSelection();
    }

    /**
     * @brief Screen overlay, menu, and every action. Call LAST in the frame.
     */
    void drawOverlay(float realDt) {
        if (!m_dev || !m_em || !m_window || !m_ui.ready()) return;

        if (m_dev->aiDisabled.size() > 0) pruneStaleIds();

        m_ui.begin(realDt);
        if (m_toastTimer > 0.f) m_toastTimer -= realDt;

        if (m_dev->showAILabels) drawAILabels();
        if (m_dev->showStats)    drawStats();
        drawCheatBadge();

        if (!m_dev->menuOpen) {
            m_panelRect = {};
            return;
        }

        drawMenu();
        handleWorldPick();
        drawPickHint();
    }

    /// SystemManager reports back after servicing requestReloadScripts, so
    /// the message says what actually happened rather than what was asked.
    void reportReload(bool ok) {
        toast(ok ? "SCRIPTS RELOADED" : "RELOAD FAILED -- SEE CONSOLE", !ok);
    }

private:
    // ========================================================================
    // TYPES & TABLES
    // ========================================================================

    enum class Tab { Spawn, Player, Enemy, World, Time, Camera, Visual };
    enum class Where { Cursor, NearPlayer, Random };
    enum class Pick { None, SpawnEnemy, SpawnAsteroid, TeleportPlayer, SelectEnemy, TeleportEnemy };

    static constexpr const char* AST_TYPES[] = { "SMALL", "MEDIUM", "LARGE", "MAGMATIC" };
    static constexpr int   AST_TYPE_COUNT = 4;
    static constexpr int   COUNTS[] = { 1, 3, 5, 10 };
    static constexpr int   COUNT_N = 4;
    static constexpr float SPEEDS[] = { 0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f };
    static constexpr int   SPEED_N = 8;
    static constexpr int   SPEED_ONE = 3;
    static constexpr float SCALES[] = { 0.5f, 0.75f, 1.f, 1.25f, 1.5f, 2.f };
    static constexpr int   SCALE_N = 6;
    static constexpr int   SCALE_ONE = 2;

    static const char* whereName(Where w) {
        switch (w) {
        case Where::Cursor:     return "AT CURSOR";
        case Where::NearPlayer: return "NEAR PLAYER";
        default:                return "RANDOM";
        }
    }

    static const char* stateName(EnemyState s) {
        switch (s) {
        case EnemyState::PATROL: return "PATROL";
        case EnemyState::ALERT:  return "ALERT";
        default:                 return "COMBAT";
        }
    }

    static sf::Color stateColor(EnemyState s) {
        switch (s) {
        case EnemyState::PATROL: return tui::CYAN_MID;
        case EnemyState::ALERT:  return tui::AMBER;
        default:                 return tui::RED;
        }
    }

    static const char* maneuverName(Maneuver m) {
        switch (m) {
        case Maneuver::APPROACH:   return "APPROACH";
        case Maneuver::STRAFE:     return "STRAFE";
        case Maneuver::FALLBACK:   return "FALLBACK";
        case Maneuver::ATTACK_RUN: return "ATTACK RUN";
        case Maneuver::REPOSITION: return "REPOSITION";
        default:                   return "CIRCLE";
        }
    }

    /// The committed attack currently overriding the manoeuvre, if any. These
    /// own the ship while active, so they are what the label should say.
    static std::string commitName(const EnemyComponent& ec) {
        static const char* PH[] = { "", "WINDUP", "RUN", "RECOVER" };
        if (ec.frenzyState != FrenzyState::None) {
            static const char* F[] = { "", "IGNITE", "CHARGE", "THROWN" };
            return std::string("FRENZY ") + F[static_cast<int>(ec.frenzyState)];
        }
        if (ec.ramState != RamState::None) {
            static const char* R[] = { "", "WINDUP", "CHARGE", "RECOVER" };
            return std::string("RAM ") + R[static_cast<int>(ec.ramState)];
        }
        if (ec.bashState != BashState::None) {
            static const char* B[] = { "", "WINDUP", "LUNGE", "RECOVER" };
            return std::string("BASH ") + B[static_cast<int>(ec.bashState)];
        }
        if (ec.mineRunState != MineRunState::None)
            return std::string("MINE RUN ") + PH[static_cast<int>(ec.mineRunState)];
        if (ec.stormActive)                return "BULLET STORM";
        if (ec.stormRecoverTimer > 0.f)    return "STORM DIZZY";
        if (ec.staggerTimer > 0.f)         return "STAGGERED";
        if (ec.telegraphActive)            return "TELEGRAPH";
        return "";
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    template <class F>
    void forEachEnemy(F&& fn) {
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (ud && ud->type == BodyType::Enemy) fn(i);
        }
    }

    BodyType typeAt(size_t i) const {
        BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
        return ud ? ud->type : BodyType::Asteroid;
    }

    size_t playerIdx() const { return m_em->getEntityIndex(m_playerEntityId); }

    /// Where "near player" and "random" measure from. Falls back to the camera
    /// when the player is dead, so spawning still works post-mortem.
    sf::Vector2f anchor() const {
        const size_t p = playerIdx();
        if (p != (size_t)-1) return m_em->transforms[p].position;
        return m_gameView ? m_gameView->getCenter() : sf::Vector2f(0.f, 0.f);
    }

    sf::Vector2f mouseWorld() const {
        const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
        return m_window->mapPixelToCoords(pix, *m_gameView);
    }

    sf::Vector2f worldToScreen(sf::Vector2f w) const {
        const sf::Vector2i p = m_window->mapCoordsToPixel(w, *m_gameView);
        return { static_cast<float>(p.x), static_cast<float>(p.y) };
    }

    static float len(sf::Vector2f v) { return std::sqrt(v.x * v.x + v.y * v.y); }

    static sf::Vector2f ring(float radius) {
        const float a = (rand() % 3600) * 0.1f * 3.14159f / 180.f;
        return { std::cos(a) * radius, std::sin(a) * radius };
    }

    void toast(const std::string& msg, bool bad = false) {
        m_toast = msg;
        m_toastBad = bad;
        m_toastTimer = 2.5f;
    }

    /// PhysicsSystem culls past these radii (Lua gives METRES, from the player).
    float despawnRadiusPx(bool enemy) const {
        if (!m_lua) return 1e9f;
        const float m = enemy
            ? (*m_lua)["enemy_config"]["despawn_radius"].get_or(250.f)
            : (*m_lua)["spawn_settings"]["despawn_radius"].get_or(100.f);
        return m * SCALE;
    }

    /// Refuse anything that would be culled on the next frame. With no player
    /// there is no culling (cleanup keys off the player), so anything goes.
    bool insideDespawn(sf::Vector2f pos, bool enemy) {
        const size_t p = playerIdx();
        if (p == (size_t)-1) return true;
        if (len(pos - m_em->transforms[p].position) <= despawnRadiusPx(enemy) * 0.95f) return true;
        toast("BEYOND DESPAWN RADIUS -- IT WOULD VANISH", true);
        return false;
    }

    bool budgetFor(size_t count) {
        if (m_em->transforms.size() + count <= dev::ENTITY_BUDGET) return true;
        char b[96];
        std::snprintf(b, sizeof(b), "ENTITY BUDGET %zu REACHED", dev::ENTITY_BUDGET);
        toast(b, true);
        return false;
    }

    /// Drop per-enemy "AI off" entries for units that no longer exist, so the
    /// set cannot grow for a whole session of spawn-and-kill.
    void pruneStaleIds() {
        for (auto it = m_dev->aiDisabled.begin(); it != m_dev->aiDisabled.end(); ) {
            it = (m_em->getEntityIndex(*it) == (size_t)-1) ? m_dev->aiDisabled.erase(it) : std::next(it);
        }
        if (m_dev->selectedEnemy && m_em->getEntityIndex(m_dev->selectedEnemy) == (size_t)-1)
            m_dev->selectedEnemy = 0;
    }

    // ========================================================================
    // ACTIONS -- only ever called from drawOverlay(), end of frame
    // ========================================================================

    sf::Vector2f resolveWhere(Where w, sf::Vector2f cursor) const {
        switch (w) {
        case Where::Cursor:     return cursor;
        case Where::NearPlayer: return anchor() + ring(350.f);
        default:                return anchor() + ring(700.f + static_cast<float>(rand() % 400));
        }
    }

    void spawnEnemies(sf::Vector2f at) {
        if (!m_enemySys || !m_registry || m_registry->empty()) return;
        const auto& all = m_registry->all();
        m_archIdx = std::clamp(m_archIdx, 0, static_cast<int>(all.size()) - 1);
        const int n = COUNTS[m_countIdx];
        if (!budgetFor(static_cast<size_t>(n))) return;

        const sf::Vector2f base = resolveWhere(m_enemyWhere, at);
        if (!insideDespawn(base, true)) return;

        const std::string& key = all[static_cast<size_t>(m_archIdx)].key;
        int made = 0;
        for (int k = 0; k < n; ++k) {
            // Spread a group on a small ring. Stacking them on one point makes
            // Box2D resolve the overlap as an explosion.
            const sf::Vector2f pos = (k == 0) ? base : base + ring(90.f + 25.f * k);
            if (m_enemySys->summon(key, pos) != 0) ++made;
        }
        char b[96];
        std::snprintf(b, sizeof(b), "SPAWNED %d x %s", made, all[static_cast<size_t>(m_archIdx)].display.c_str());
        toast(b);
    }

    void spawnAsteroid(sf::Vector2f at) {
        if (!m_enemySys || !budgetFor(1)) return;
        const sf::Vector2f pos = resolveWhere(m_astWhere, at);
        if (!insideDespawn(pos, false)) return;

        // Cursor / near: a stationary target, which is what you want to test
        // a parry or a Rift on. Random: inbound like the director's rocks.
        // Asteroid velocity is in METRES/s (it goes straight to Box2D).
        sf::Vector2f vel(0.f, 0.f);
        const char* key = AST_TYPES[m_astTypeIdx];
        if (m_astWhere == Where::Random && m_lua) {
            sol::optional<sol::table> sr = (*m_lua)["asteroid_types"][key]["speed_range"];
            float speed = 6.f;
            if (sr) speed = 0.5f * ((*sr)[1].get_or(6.f) + (*sr)[2].get_or(6.f));
            const sf::Vector2f d = anchor() - pos;
            const float l = std::max(1.f, len(d));
            vel = d / l * speed;
        }
        if (m_enemySys->spawnAsteroid(key, pos, vel) != 0) toast(std::string("SPAWNED ") + key);
    }

    void teleportEntity(uint32_t id, sf::Vector2f pos) {
        const size_t i = m_em->getEntityIndex(id);
        if (i == (size_t)-1) return;
        const b2BodyId body = m_em->physics[i].bodyId;
        if (!b2Body_IsValid(body)) return;
        b2Body_SetTransform(body, { pos.x / SCALE, pos.y / SCALE }, b2Body_GetRotation(body));
        b2Body_SetLinearVelocity(body, { 0.f, 0.f });
        b2Body_SetAngularVelocity(body, 0.f);
        m_em->transforms[i].position = pos;   // so this frame's draw agrees
    }

    void killAllEnemies() {
        int n = 0;
        forEachEnemy([&](size_t i) { m_em->healths[i].currentHp = 0.f; ++n; });
        char b[64];
        std::snprintf(b, sizeof(b), "KILLED %d -- COLLECTED NEXT STEP", n);
        toast(b);
    }

    void clearAsteroids() {
        std::vector<size_t> idx;
        for (size_t i = 0; i < m_em->physics.size(); ++i)
            if (i != playerIdx() && typeAt(i) == BodyType::Asteroid) idx.push_back(i);
        // Reverse order: swap-and-pop only ever moves the LAST element down,
        // so destroying high indices first leaves every lower one valid.
        for (size_t k = idx.size(); k-- > 0; ) m_em->destroyEntity(idx[k]);
        char b[64];
        std::snprintf(b, sizeof(b), "CLEARED %zu ASTEROIDS", idx.size());
        toast(b);
    }

    void selectAt(sf::Vector2f world) {
        uint32_t best = 0;
        float bestD = 140.f;
        forEachEnemy([&](size_t i) {
            const float d = len(m_em->transforms[i].position - world);
            if (d < bestD) { bestD = d; best = m_em->transforms[i].entityId; }
            });
        m_dev->selectedEnemy = best;
        toast(best ? "TARGET SELECTED" : "NO ENEMY THERE", best == 0);
    }

    // ========================================================================
    // WORLD PICK
    // ========================================================================

    void handleWorldPick() {
        if (m_pick == Pick::None) return;

        if (m_ui.rightEdge()) { m_pick = Pick::None; return; }

        const sf::Vector2f mp = m_ui.mouse();
        if (m_panelRect.contains(mp)) return;          // clicks on the panel are the panel's
        if (!m_ui.leftEdge()) return;                  // no click, or a widget already took it
        m_ui.consumeClick();

        const sf::Vector2f w = mouseWorld();
        switch (m_pick) {
        case Pick::SpawnEnemy:    spawnEnemies(w); break;          // stays armed: place again
        case Pick::SpawnAsteroid: spawnAsteroid(w); break;         // stays armed
        case Pick::TeleportPlayer:
            teleportEntity(m_playerEntityId, w);
            m_pick = Pick::None;
            break;
        case Pick::SelectEnemy:
            selectAt(w);
            m_pick = Pick::None;
            break;
        case Pick::TeleportEnemy:
            if (m_dev->selectedEnemy && insideDespawn(w, true)) teleportEntity(m_dev->selectedEnemy, w);
            m_pick = Pick::None;
            break;
        default: break;
        }
    }

    void drawPickHint() {
        if (m_pick == Pick::None) return;
        std::string what;
        switch (m_pick) {
        case Pick::SpawnEnemy: {
            const auto& all = m_registry->all();
            const int i = std::clamp(m_archIdx, 0, static_cast<int>(all.size()) - 1);
            what = "PLACE " + all[static_cast<size_t>(i)].display + " x" + std::to_string(COUNTS[m_countIdx]);
            break;
        }
        case Pick::SpawnAsteroid:  what = std::string("PLACE ") + AST_TYPES[m_astTypeIdx]; break;
        case Pick::TeleportPlayer: what = "TELEPORT SHIP"; break;
        case Pick::SelectEnemy:    what = "SELECT ENEMY"; break;
        case Pick::TeleportEnemy:  what = "TELEPORT TARGET"; break;
        default: break;
        }
        const sf::Vector2f mp = m_ui.mouse();
        m_ui.cross(mp, 9.f, tui::AMBER);
        m_ui.text({ mp.x + 16.f, mp.y + 10.f }, "LMB " + what + "   RMB CANCEL", 12, tui::AMBER);
    }

    // ========================================================================
    // MENU
    // ========================================================================

    /// A full-width on/off row. Amber when on.
    bool toggle(float& y, const std::string& label, bool& value) {
        const tui::Rect r{ m_cx, y, m_cw, 26.f };
        y += 31.f;
        if (m_ui.button(r, label + (value ? "  :  ON" : "  :  OFF"), value, false, false, 14)) {
            value = !value;
            return true;
        }
        return false;
    }

    bool action(float& y, const std::string& label, bool disabled = false) {
        const tui::Rect r{ m_cx, y, m_cw, 26.f };
        y += 31.f;
        return m_ui.button(r, label, false, disabled, false, 14);
    }

    /// "< VALUE >" row. Returns -1, 0 or +1.
    int stepper(float& y, const std::string& caption, const std::string& value) {
        m_ui.text({ m_cx, y + 5.f }, caption, 12, tui::TEXT_DIM);
        const float bx = m_cx + 92.f;
        const float bw = m_cw - 92.f;
        int d = 0;
        if (m_ui.iconButton({ bx, y, 26.f, 26.f }, "<", tui::CYAN_MID, 14)) d = -1;
        if (m_ui.iconButton({ bx + bw - 26.f, y, 26.f, 26.f }, ">", tui::CYAN_MID, 14)) d = +1;
        const float tw = m_ui.textWidth(value, 14);
        m_ui.text({ bx + bw * 0.5f - tw * 0.5f, y + 4.f }, value, 14, tui::CYAN);
        y += 31.f;
        return d;
    }

    void heading(float& y, const std::string& text) {
        y += 4.f;
        m_ui.text({ m_cx, y }, text, 12, tui::CYAN_MID, true, 2.f);
        m_ui.hline(m_cx, y + 18.f, m_cw, tui::CYAN_LOW);
        y += 26.f;
    }

    void line(float& y, const std::string& text, sf::Color c = tui::TEXT) {
        m_ui.text({ m_cx, y }, text, 12, c);
        y += 18.f;
    }

    static int wrap(int v, int d, int n) { return ((v + d) % n + n) % n; }

    void drawMenu() {
        const sf::Vector2u ws = m_window->getSize();
        const float H = std::min(660.f, static_cast<float>(ws.y) - 80.f);
        m_panelRect = { 18.f, 56.f, 400.f, H };

        if (!m_ui.panel(m_panelRect, "DEV", 0.f, tui::Chrome::Heavy, tui::AMBER)) return;

        m_cx = m_panelRect.x + 16.f;
        m_cw = m_panelRect.w - 32.f;
        float y = m_panelRect.y + 22.f;

        // ---- Tabs, two rows ----
        static const char* NAMES[] = { "SPAWN", "PLAYER", "ENEMY", "WORLD", "TIME", "CAMERA", "VISUAL" };
        const int perRow = 4;
        const float tw = (m_cw - (perRow - 1) * 6.f) / perRow;
        for (int t = 0; t < 7; ++t) {
            const int row = t / perRow, col = t % perRow;
            const tui::Rect r{ m_cx + col * (tw + 6.f), y + row * 30.f, tw, 24.f };
            if (m_ui.button(r, NAMES[t], static_cast<int>(m_tab) == t, false, false, 13)) {
                m_tab = static_cast<Tab>(t);
                m_pick = Pick::None;
            }
        }
        y += 70.f;

        switch (m_tab) {
        case Tab::Spawn:  tabSpawn(y);  break;
        case Tab::Player: tabPlayer(y); break;
        case Tab::Enemy:  tabEnemy(y);  break;
        case Tab::World:  tabWorld(y);  break;
        case Tab::Time:   tabTime(y);   break;
        case Tab::Camera: tabCamera(y); break;
        case Tab::Visual: tabVisual(y); break;
        }

        // ---- Toast + footer ----
        const float fy = m_panelRect.bottom() - 46.f;
        if (m_toastTimer > 0.f) {
            const auto a = static_cast<std::uint8_t>(255.f * std::clamp(m_toastTimer, 0.f, 1.f));
            m_ui.text({ m_cx, fy }, m_toast, 12, tui::UI::withAlpha(m_toastBad ? tui::RED : tui::GREEN, a));
        }
        m_ui.text({ m_cx, m_panelRect.bottom() - 24.f }, "~ CLOSE    F3 SHAPES    F5 RELOAD", 11, tui::TEXT_DIM);
    }

    // ---- SPAWN ----
    void tabSpawn(float& y) {
        heading(y, "ENEMY");
        if (!m_registry || m_registry->empty()) {
            line(y, "NO ARCHETYPES LOADED", tui::RED);
        }
        else {
            const auto& all = m_registry->all();
            const int n = static_cast<int>(all.size());
            m_archIdx = std::clamp(m_archIdx, 0, n - 1);
            const auto& a = all[static_cast<size_t>(m_archIdx)];
            std::string name = a.display + (a.summonOnly ? " *" : "");
            if (int d = stepper(y, "UNIT", name)) m_archIdx = wrap(m_archIdx, d, n);
            if (int d = stepper(y, "COUNT", std::to_string(COUNTS[m_countIdx]))) m_countIdx = wrap(m_countIdx, d, COUNT_N);
            if (int d = stepper(y, "WHERE", whereName(m_enemyWhere)))
                m_enemyWhere = static_cast<Where>(wrap(static_cast<int>(m_enemyWhere), d, 3));

            const bool armed = m_pick == Pick::SpawnEnemy;
            const tui::Rect r{ m_cx, y, m_cw, 26.f };
            y += 31.f;
            if (m_ui.button(r, armed ? "PLACING... (RMB STOP)" : "SPAWN ENEMY", armed, false, false, 14)) {
                if (m_enemyWhere == Where::Cursor) m_pick = armed ? Pick::None : Pick::SpawnEnemy;
                else spawnEnemies({});
            }
            if (a.summonOnly) line(y, "* SUMMON-ONLY UNIT", tui::TEXT_DIM);
        }

        heading(y, "ASTEROID");
        if (int d = stepper(y, "TYPE", AST_TYPES[m_astTypeIdx])) m_astTypeIdx = wrap(m_astTypeIdx, d, AST_TYPE_COUNT);
        if (int d = stepper(y, "WHERE", whereName(m_astWhere)))
            m_astWhere = static_cast<Where>(wrap(static_cast<int>(m_astWhere), d, 3));
        {
            const bool armed = m_pick == Pick::SpawnAsteroid;
            const tui::Rect r{ m_cx, y, m_cw, 26.f };
            y += 31.f;
            if (m_ui.button(r, armed ? "PLACING... (RMB STOP)" : "SPAWN ASTEROID", armed, false, false, 14)) {
                if (m_astWhere == Where::Cursor) m_pick = armed ? Pick::None : Pick::SpawnAsteroid;
                else spawnAsteroid({});
            }
        }

        y += 6.f;
        char b[96];
        std::snprintf(b, sizeof(b), "ENTITIES %zu / %zu BUDGET", m_em->transforms.size(), dev::ENTITY_BUDGET);
        line(y, b, m_em->transforms.size() > dev::ENTITY_BUDGET * 9 / 10 ? tui::RED : tui::TEXT_DIM);
    }

    // ---- PLAYER ----
    void tabPlayer(float& y) {
        heading(y, "CHEATS");
        toggle(y, "GOD MODE", m_dev->godMode);
        toggle(y, "INFINITE ENERGY + HEAT", m_dev->infiniteEnergy);
        toggle(y, "NO COOLDOWNS", m_dev->noCooldowns);
        toggle(y, "ONE-SHOT ENEMIES", m_dev->oneShot);

        heading(y, "SHIP");
        const size_t p = playerIdx();
        const bool alive = p != (size_t)-1;
        if (action(y, "RESTORE HULL", !alive) && alive) {
            m_em->healths[p].currentHp = m_em->healths[p].maxHp;
            toast("HULL RESTORED");
        }
        {
            const bool armed = m_pick == Pick::TeleportPlayer;
            const tui::Rect r{ m_cx, y, m_cw, 26.f };
            y += 31.f;
            if (m_ui.button(r, armed ? "CLICK DESTINATION..." : "TELEPORT (CLICK)", armed, !alive, false, 14))
                m_pick = armed ? Pick::None : Pick::TeleportPlayer;
        }
        if (!alive) line(y, "SHIP DESTROYED", tui::RED);

        y += 6.f;
        line(y, "NO COOLDOWNS FLOORS FIRE AT 20/S", tui::TEXT_DIM);
        line(y, "ONE-SHOT: ANY HP LOSS IS FATAL", tui::TEXT_DIM);
    }

    // ---- ENEMY ----
    void tabEnemy(float& y) {
        heading(y, "ALL");
        toggle(y, "FREEZE ALL AI", m_dev->freezeAllAI);
        if (action(y, "KILL ALL ENEMIES")) killAllEnemies();

        heading(y, "TARGET");
        {
            const bool armed = m_pick == Pick::SelectEnemy;
            const tui::Rect r{ m_cx, y, m_cw, 26.f };
            y += 31.f;
            if (m_ui.button(r, armed ? "CLICK AN ENEMY..." : "SELECT (CLICK)", armed, false, false, 14))
                m_pick = armed ? Pick::None : Pick::SelectEnemy;
        }

        const uint32_t id = m_dev->selectedEnemy;
        const size_t i = id ? m_em->getEntityIndex(id) : (size_t)-1;
        if (i == (size_t)-1) {
            line(y, "NONE SELECTED", tui::TEXT_DIM);
            return;
        }

        const auto& ec = m_em->enemies[i];
        const auto& hp = m_em->healths[i];
        const auto& def = m_registry->resolve(ec.archetype);
        char b[128];
        std::snprintf(b, sizeof(b), "%s  #%u", def.display.c_str(), id);
        line(y, b, tui::CYAN);
        std::snprintf(b, sizeof(b), "HP %.0f / %.0f   %s", hp.currentHp, hp.maxHp, stateName(ec.visualState));
        line(y, b);

        if (action(y, "DAMAGE 25%")) {
            m_em->healths[i].currentHp -= hp.maxHp * 0.25f;
            m_em->enemies[i].hitFlashTimer = 0.12f;   // reads as a hit to the AI, as a real shot would
        }
        if (action(y, "KILL")) m_em->healths[i].currentHp = 0.f;
        {
            const bool armed = m_pick == Pick::TeleportEnemy;
            const tui::Rect r{ m_cx, y, m_cw, 26.f };
            y += 31.f;
            if (m_ui.button(r, armed ? "CLICK DESTINATION..." : "TELEPORT (CLICK)", armed, false, false, 14))
                m_pick = armed ? Pick::None : Pick::TeleportEnemy;
        }
        const bool off = m_dev->aiDisabled.count(id) != 0;
        if (action(y, off ? "AI : OFF  (ENABLE)" : "AI : ON  (DISABLE)")) {
            if (off) m_dev->aiDisabled.erase(id);
            else     m_dev->aiDisabled.insert(id);
        }
        if (action(y, "DESELECT")) m_dev->selectedEnemy = 0;
    }

    // ---- WORLD ----
    void tabWorld(float& y) {
        heading(y, "SPAWNERS");
        toggle(y, "ENEMY DIRECTOR", m_dev->directorOn);
        toggle(y, "ASTEROID SPAWNER", m_dev->asteroidSpawnerOn);

        heading(y, "CLEAN UP");
        if (action(y, "CLEAR ASTEROIDS")) clearAsteroids();
        if (action(y, "KILL ALL ENEMIES")) killAllEnemies();

        heading(y, "SCRIPTS");
        if (action(y, "RELOAD LUA  (F5)")) m_dev->requestReloadScripts = true;
    }

    // ---- TIME ----
    void tabTime(float& y) {
        heading(y, "CLOCK");
        if (toggle(y, "PAUSE SIMULATION", m_dev->paused)) m_dev->stepFrames = 0;
        if (action(y, "STEP ONE FRAME", !m_dev->paused)) ++m_dev->stepFrames;

        int si = SPEED_ONE;
        for (int k = 0; k < SPEED_N; ++k) if (std::abs(SPEEDS[k] - m_dev->timeScale) < 0.01f) si = k;
        char b[32];
        std::snprintf(b, sizeof(b), "x%.2f", SPEEDS[si]);
        if (int d = stepper(y, "SPEED", b)) {
            si = std::clamp(si + d, 0, SPEED_N - 1);   // clamp, not wrap: 4x -> 0.25x in one click is a trap
            m_dev->timeScale = SPEEDS[si];
        }
        if (action(y, "RESET SPEED")) m_dev->timeScale = 1.f;

        y += 6.f;
        line(y, "FAST = MORE STEPS, NOT BIGGER ONES", tui::TEXT_DIM);
        line(y, "ESC STILL OPENS PAUSE MENU", tui::TEXT_DIM);
    }

    // ---- CAMERA ----
    void tabCamera(float& y) {
        heading(y, "VIEW");
        toggle(y, "FREE CAMERA", m_dev->freeCam);

        int si = SCALE_ONE;
        for (int k = 0; k < SCALE_N; ++k) if (std::abs(SCALES[k] - m_dev->viewScale) < 0.01f) si = k;
        char b[32];
        std::snprintf(b, sizeof(b), "x%.2f", SCALES[si]);
        if (int d = stepper(y, "VIEW SCALE", b)) m_dev->viewScale = SCALES[std::clamp(si + d, 0, SCALE_N - 1)];

        if (action(y, "RESET CAMERA")) {
            m_dev->freeCam = false;
            m_dev->viewScale = 1.f;
        }

        y += 6.f;
        line(y, "FREE CAM: WASD MOVE, LSHIFT FAST", tui::TEXT_DIM);
        line(y, "SHIP IGNORES KEYS WHILE ON", tui::TEXT_DIM);
        line(y, "VIEW SCALE >1 SHOWS MORE WORLD", tui::TEXT_DIM);
    }

    // ---- VISUAL ----
    void tabVisual(float& y) {
        heading(y, "OVERLAYS");
        bool shapes = m_debugSys && m_debugSys->isEnabled();
        if (toggle(y, "PHYSICS SHAPES  (F3)", shapes) && m_debugSys) m_debugSys->toggle();
        toggle(y, "AI LABELS", m_dev->showAILabels);
        toggle(y, "VISION CONES", m_dev->showVisionCones);
        toggle(y, "AIM LINES", m_dev->showAimLines);
        toggle(y, "STATS PANEL", m_dev->showStats);

        y += 6.f;
        line(y, "CONE: RED = SEES YOU NOW", tui::TEXT_DIM);
        line(y, "RING: PROXIMITY SENSE", tui::TEXT_DIM);
    }

    // ========================================================================
    // WORLD OVERLAYS
    // ========================================================================

    void drawVisionCones() {
        const size_t p = playerIdx();
        const bool alive = p != (size_t)-1;
        const sf::Vector2f pp = alive ? m_em->transforms[p].position : sf::Vector2f();

        forEachEnemy([&](size_t i) {
            const auto& tf = m_em->transforms[i];
            const auto& ec = m_em->enemies[i];
            sol::table cfg = m_registry->resolve(ec.archetype).config;

            // Same numbers and the same test as AISystem::canSee. If that
            // function changes, this must follow, or the overlay lies.
            const float range = cfg["vision_range"].get_or(620.f);
            const float fov = cfg["vision_fov"].get_or(110.f);
            const float prox = cfg["proximity_sense"].get_or(150.f);
            float half = fov * 0.5f;
            if (ec.visualState != EnemyState::PATROL) half *= cfg["vision_fov_alert_mult"].get_or(1.45f);
            half = std::min(half, 175.f);

            const float r = tf.rotation * 3.14159f / 180.f;
            const sf::Vector2f fwd(std::sin(r), -std::cos(r));

            bool sees = false;
            if (alive) {
                const sf::Vector2f d = pp - tf.position;
                const float dist = len(d);
                if (dist < prox) sees = true;
                else if (dist <= range && dist > 0.01f) {
                    const float dp = (fwd.x * d.x + fwd.y * d.y) / dist;
                    sees = dp >= std::cos(half * 3.14159f / 180.f);
                }
            }

            const bool frozen = m_dev->isAIFrozen(tf.entityId);
            sf::Color c = sees ? tui::RED : stateColor(ec.visualState);
            if (frozen) c = tui::TEXT_DEAD;

            // Cone fill
            const int SEG = 24;
            sf::VertexArray fan(sf::PrimitiveType::TriangleFan, SEG + 2);
            fan[0] = sf::Vertex{ tf.position, tui::UI::withAlpha(c, 34) };
            const float h = half * 3.14159f / 180.f;
            for (int k = 0; k <= SEG; ++k) {
                const float t = -h + 2.f * h * k / SEG;
                const sf::Vector2f dir(fwd.x * std::cos(t) - fwd.y * std::sin(t),
                    fwd.x * std::sin(t) + fwd.y * std::cos(t));
                fan[static_cast<size_t>(k) + 1] = sf::Vertex{ tf.position + dir * range, tui::UI::withAlpha(c, 10) };
            }
            m_window->draw(fan);

            // Edges
            sf::VertexArray edges(sf::PrimitiveType::Lines, 4);
            edges[0] = sf::Vertex{ tf.position, tui::UI::withAlpha(c, 160) };
            edges[1] = sf::Vertex{ fan[1].position, tui::UI::withAlpha(c, 60) };
            edges[2] = sf::Vertex{ tf.position, tui::UI::withAlpha(c, 160) };
            edges[3] = sf::Vertex{ fan[SEG + 1].position, tui::UI::withAlpha(c, 60) };
            m_window->draw(edges);

            // Proximity ring
            sf::CircleShape ringShape(prox, 40);
            ringShape.setOrigin({ prox, prox });
            ringShape.setPosition(tf.position);
            ringShape.setFillColor(sf::Color::Transparent);
            ringShape.setOutlineThickness(1.f);
            ringShape.setOutlineColor(tui::UI::withAlpha(c, 70));
            m_window->draw(ringShape);
            });
    }

    void drawAimLines() {
        forEachEnemy([&](size_t i) {
            const auto& tf = m_em->transforms[i];
            const auto& ec = m_em->enemies[i];

            // Locked telegraph: where the shot WILL go, not where he looks.
            if (ec.telegraphActive) {
                sf::VertexArray l(sf::PrimitiveType::Lines, 2);
                l[0] = sf::Vertex{ tf.position, tui::UI::withAlpha(tui::AMBER, 220) };
                l[1] = sf::Vertex{ tf.position + ec.telegraphDir * 900.f, tui::UI::withAlpha(tui::AMBER, 0) };
                m_window->draw(l);
            }

            // Memory: where he thinks you are. Diverges from you during ALERT
            // search -- which is exactly the thing worth watching.
            const AIState* ai = m_aiSys ? m_aiSys->brainOf(tf.entityId) : nullptr;
            if (ai && ai->hasSeenPlayer && ec.visualState != EnemyState::PATROL) {
                sf::VertexArray l(sf::PrimitiveType::Lines, 2);
                l[0] = sf::Vertex{ tf.position, tui::UI::withAlpha(tui::TEXT_DIM, 140) };
                l[1] = sf::Vertex{ ai->lastKnownPlayerPos, tui::UI::withAlpha(tui::TEXT_DIM, 140) };
                m_window->draw(l);
                crossAt(ai->lastKnownPlayerPos, 10.f, tui::TEXT_DIM);
            }
            });
    }

    void drawSelection() {
        const uint32_t id = m_dev->selectedEnemy;
        const size_t i = id ? m_em->getEntityIndex(id) : (size_t)-1;
        if (i == (size_t)-1) return;
        m_pulse += 0.08f;
        const float r = 56.f + 5.f * std::sin(m_pulse);
        sf::CircleShape c(r, 40);
        c.setOrigin({ r, r });
        c.setPosition(m_em->transforms[i].position);
        c.setFillColor(sf::Color::Transparent);
        c.setOutlineThickness(2.f);
        c.setOutlineColor(tui::AMBER);
        m_window->draw(c);
    }

    void crossAt(sf::Vector2f p, float s, sf::Color c) {
        sf::VertexArray v(sf::PrimitiveType::Lines, 4);
        v[0] = sf::Vertex{ { p.x - s, p.y }, c };
        v[1] = sf::Vertex{ { p.x + s, p.y }, c };
        v[2] = sf::Vertex{ { p.x, p.y - s }, c };
        v[3] = sf::Vertex{ { p.x, p.y + s }, c };
        m_window->draw(v);
    }

    // ========================================================================
    // SCREEN OVERLAYS
    // ========================================================================

    /// Labels are drawn in SCREEN space at the projected position, so they
    /// stay crisp and readable at any view scale and ignore camera shake.
    void drawAILabels() {
        const sf::Vector2u ws = m_window->getSize();
        forEachEnemy([&](size_t i) {
            const auto& tf = m_em->transforms[i];
            const sf::Vector2f s = worldToScreen(tf.position);
            if (s.x < -100.f || s.y < -100.f || s.x > ws.x + 100.f || s.y > ws.y + 100.f) return;

            const auto& ec = m_em->enemies[i];
            const auto& hp = m_em->healths[i];
            const auto& def = m_registry->resolve(ec.archetype);
            const AIState* ai = m_aiSys ? m_aiSys->brainOf(tf.entityId) : nullptr;
            const bool frozen = m_dev->isAIFrozen(tf.entityId);

            float y = s.y - 78.f;
            const float x = s.x - 60.f;

            char b[96];
            std::snprintf(b, sizeof(b), "%s #%u%s", def.display.c_str(), tf.entityId, frozen ? "  AI OFF" : "");
            m_ui.text({ x, y }, b, 11, frozen ? tui::RED : tui::TEXT);
            y += 14.f;

            const std::string commit = commitName(ec);
            std::string st = stateName(ec.visualState);
            if (!commit.empty())                                   st += " / " + commit;
            else if (ai && ec.visualState == EnemyState::COMBAT)   st += std::string(" / ") + maneuverName(ai->maneuver);
            m_ui.text({ x, y }, st, 11, stateColor(ec.visualState));
            y += 15.f;

            // Suspicion bar -- the gate between PATROL and ALERT
            const float sus = ai ? std::clamp(ai->suspicion, 0.f, 1.f) : 0.f;
            m_ui.fill({ x, y, 60.f, 4.f }, tui::CYAN_LOW);
            m_ui.fill({ x, y, 60.f * sus, 4.f }, tui::AMBER);
            std::snprintf(b, sizeof(b), "HP %.0f/%.0f", hp.currentHp, hp.maxHp);
            m_ui.text({ x + 66.f, y - 6.f }, b, 10, tui::TEXT_DIM);
            });
    }

    void drawStats() {
        const sf::Vector2u ws = m_window->getSize();
        const tui::Rect r{ static_cast<float>(ws.x) - 330.f, 56.f, 312.f, 332.f };
        if (!m_ui.panel(r, "STATS", 0.f, tui::Chrome::Hairline, tui::CYAN_MID)) return;

        float y = r.y + 16.f;
        const float x = r.x + 12.f;
        char b[128];
        auto row = [&](const char* s, sf::Color c = tui::TEXT) { m_ui.text({ x, y }, s, 12, c); y += 18.f; };

        std::snprintf(b, sizeof(b), "FPS %.0f   (%.1f ms)", m_fps, m_frameMs);
        row(b, m_fps < 50.f ? tui::AMBER : tui::TEXT);

        std::snprintf(b, sizeof(b), "SIM x%.2f%s   HITSTOP x%.2f", m_dev->timeScale,
            m_dev->paused ? " PAUSED" : "", m_em->timeScale);
        row(b, m_dev->paused ? tui::AMBER : tui::TEXT);
        y += 4.f;

        // ---- Census ----
        int enemies = 0, asteroids = 0, bullets = 0;
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            switch (typeAt(i)) {
            case BodyType::Enemy:    ++enemies; break;
            case BodyType::Asteroid: ++asteroids; break;
            case BodyType::Bullet:   ++bullets; break;
            default: break;
            }
        }

        // ---- The reserve: the memory number that can actually crash us ----
        const size_t n = m_em->transforms.size();
        const size_t cap = m_em->transforms.capacity();
        const bool blown = cap > dev::ENTITY_RESERVE;
        std::snprintf(b, sizeof(b), "ENTITIES %zu / %zu", n, dev::ENTITY_RESERVE);
        row(b, blown ? tui::RED : tui::TEXT);
        const float bw = r.w - 24.f;
        m_ui.fill({ x, y, bw, 5.f }, tui::CYAN_LOW);
        m_ui.fill({ x, y, bw * std::min(1.f, float(n) / dev::ENTITY_RESERVE), 5.f },
            n > dev::ENTITY_BUDGET ? tui::RED : tui::CYAN);
        m_ui.vline(x + bw * float(dev::ENTITY_BUDGET) / dev::ENTITY_RESERVE, y - 2.f, 9.f, tui::AMBER);
        y += 12.f;
        if (blown) row("RESERVE BLOWN -- VECTORS REALLOCATED", tui::RED);

        std::snprintf(b, sizeof(b), "ENEMY %d  ROCK %d  SHOT %d", enemies, asteroids, bullets);
        row(b, tui::TEXT_DIM);
        std::snprintf(b, sizeof(b), "PARTICLES %zu / %zu", m_em->particles.size(), m_em->particles.capacity());
        row(b, tui::TEXT_DIM);
        std::snprintf(b, sizeof(b), "DEBRIS %zu", m_em->debris.size());
        row(b, tui::TEXT_DIM);
        y += 4.f;

        // ---- Player ----
        const size_t p = playerIdx();
        if (p == (size_t)-1) {
            row("PLAYER  DESTROYED", tui::RED);
        }
        else {
            const auto& ps = m_em->players[p];
            const auto& hp = m_em->healths[p];
            const auto& pos = m_em->transforms[p].position;
            std::snprintf(b, sizeof(b), "HULL %.0f/%.0f  EN %.0f  HEAT %.0f", hp.currentHp, hp.maxHp,
                ps.energyDrive, ps.weaponHeat);
            row(b);
            std::snprintf(b, sizeof(b), "POS %.0f, %.0f", pos.x, pos.y);
            row(b, tui::TEXT_DIM);

            // ---- Nearest enemy ----
            float best = 1e30f;
            size_t bi = (size_t)-1;
            forEachEnemy([&](size_t i) {
                const float d = len(m_em->transforms[i].position - pos);
                if (d < best) { best = d; bi = i; }
                });
            y += 4.f;
            if (bi == (size_t)-1) {
                row("NEAREST  --", tui::TEXT_DIM);
            }
            else {
                const auto& ec = m_em->enemies[bi];
                std::snprintf(b, sizeof(b), "NEAREST %s  %.0fpx",
                    m_registry->resolve(ec.archetype).display.c_str(), best);
                row(b, stateColor(ec.visualState));
                std::snprintf(b, sizeof(b), "  HP %.0f/%.0f  %s", m_em->healths[bi].currentHp,
                    m_em->healths[bi].maxHp, stateName(ec.visualState));
                row(b, stateColor(ec.visualState));
            }
        }
    }

    /// Always-on reminder of what is rigged, menu open or not. The worst dev
    /// bug is the one where you tune feel for an hour with god mode still on.
    void drawCheatBadge() {
        std::string s;
        auto add = [&](bool on, const char* t) { if (on) { if (!s.empty()) s += "  "; s += t; } };
        add(m_dev->godMode, "GOD");
        add(m_dev->infiniteEnergy, "INF-EN");
        add(m_dev->noCooldowns, "NO-CD");
        add(m_dev->oneShot, "ONE-SHOT");
        add(m_dev->freezeAllAI, "AI-FROZEN");
        add(!m_dev->directorOn, "NO-DIRECTOR");
        add(!m_dev->asteroidSpawnerOn, "NO-ROCKS");
        add(m_dev->paused, "PAUSED");
        add(m_dev->freeCam, "FREE-CAM");
        char b[24];
        if (std::abs(m_dev->timeScale - 1.f) > 0.01f) { std::snprintf(b, sizeof(b), "x%.2f", m_dev->timeScale); add(true, b); }
        if (s.empty()) return;
        s = "DEV  " + s;
        const float w = static_cast<float>(m_window->getSize().x);
        const float tw = m_ui.textWidth(s, 12);
        m_ui.text({ w * 0.5f - tw * 0.5f, 10.f }, s, 12, tui::AMBER);
    }

    // ========================================================================
    // MEMBERS
    // ========================================================================

    EntityManager* m_em = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    sf::RenderWindow* m_window = nullptr;
    sf::View* m_gameView = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    DevState* m_dev = nullptr;

    EnemySystem* m_enemySys = nullptr;
    AISystem* m_aiSys = nullptr;
    DebugSystem* m_debugSys = nullptr;

    tui::UI m_ui;
    tui::Rect m_panelRect;
    float m_cx = 0.f, m_cw = 0.f;   ///< content column of the open panel

    Tab   m_tab = Tab::Spawn;
    Pick  m_pick = Pick::None;
    int   m_archIdx = 0;
    int   m_countIdx = 0;
    Where m_enemyWhere = Where::Cursor;
    int   m_astTypeIdx = 2;         ///< LARGE: the one that shows off the cascade
    Where m_astWhere = Where::Cursor;

    bool m_graveWas = false;
    bool m_mouseHold = false;

    std::unordered_map<uint32_t, float> m_hpSnap;   ///< one-shot baseline, per pass

    std::string m_toast;
    bool  m_toastBad = false;
    float m_toastTimer = 0.f;

    float m_fps = 0.f;
    float m_frameMs = 0.f;
    float m_pulse = 0.f;
};