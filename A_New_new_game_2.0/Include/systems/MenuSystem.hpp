/**
 * @file MenuSystem.hpp
 * @brief The terminal: boot, main menu, codex, field doctrine, pause, game over.
 *
 * Purely screen-space UI. Owns its own pages, selection and transitions. Does
 * NOT own the GameState -- SystemManager does, calls setState() so this system
 * knows which screen to draw, and polls takeClickAction() for what to do next.
 *
 * ============================================================================
 * 4.0: THE TERMINAL REDESIGN
 * ============================================================================
 * Playtest: "a lot of space that does nothing", decorative tabs, a feed that
 * was a starfield, and screens that snapped from one to the next. Now:
 *
 *  - DESIGN SPACE. Everything is authored on a 1920x1080 canvas, letterboxed
 *    into the window by tui::UI (setDesignSize). One layout, every window.
 *
 *  - EVERY PANEL EARNS ITS SPACE. Tactical feed (a live fight, utils/LiveFeed),
 *    command list with grouped routes and a status per row, hunter dossier
 *    (YOUR refit-bay ship and its numbers), contract ledger (real run history),
 *    sector + void corruption, the intercept stream, and a brief that retypes
 *    itself for whatever is selected.
 *
 *  - PAGES OPEN AND CLOSE ON THE CENTRE LINE. Opening: a dot, a hot line, then
 *    the picture opens up and down from it (the old CRT reveal). Closing is
 *    the exact reverse, panels folding back in the reverse order they opened.
 *    A route is only taken once its page has folded away: confirmSelection()
 *    starts the close, and the MenuAction is handed to SystemManager when it
 *    finishes (takeClickAction). Leaving for flight or quitting also switches
 *    the tube off -- the whole screen squeezes to a line and a dot -- and
 *    flight comes back on the same way (overlay()).
 *
 *  - THE BREADCRUMB IS THE NAVIGATION. TERMINAL://HUNT/COMMAND deletes itself
 *    on the way out and types the next address on the way in. It replaces the
 *    CAMPAIGN / HUNT / ARSENAL / CODEX tabs, which looked clickable and were
 *    not.
 *
 *  - CODEX is real: every hostile in enemy.lua and every object category in
 *    asteroids.lua, opened the first time you SEE one in a contract, with the
 *    field notes from scripts/codex.lua and your kill count. CAMPAIGN and
 *    ARSENAL are on the list, SEALED, with a brief saying what they will be.
 *
 *  - FIELD DOCTRINE is eight looping scenes (utils/DoctrineStage): flight,
 *    gunnery, turbo, salvage; dodge, parry, rift bolt, vent. Flown in your
 *    ship and paints, the keycaps lighting as the pilot presses them.
 *
 * 4.1: THE FEED AND THE DOCTRINE ARE THE REAL GAME
 *   Playtest: "we have all the code to load the main game -- make the feed a
 *   simulation with the AI flying player ships, and the doctrine real
 *   gameplay". Both now run on a ShadowWorld (utils/ShadowWorld): a hidden
 *   copy of the game with every gameplay system, rendered into a texture
 *   this screen draws as a picture. The hunter is flown by HunterPilot
 *   through a virtual keyboard, so every dodge, parry, Rift Bolt and vent in
 *   them is the real mechanic. They only step while their page is showing.
 *   F5 (reloadText) restarts both: their worlds hold handles into Lua tables
 *   the reload replaces.
 *
 * DESIGN CONTRACT (unchanged, read before editing the layout):
 *   - This screen is a MACHINE. Orthogonal, hard edges, flat fills, hairline
 *     strokes. The single skewed element is the selection bar.
 *   - PANELS ARE WINDOWS. Moving content renders through beginClip()/endClip()
 *     in the panel's own coordinates and is hard-clipped by the GPU.
 *   - Greys are DARK and accents are BRIGHT. Dark future, not hologram.
 *   - The void corruption caution is the ONLY element allowed to shout.
 *
 * Pause and game over keep a single centred command panel for now -- they get
 * their own pass with the HUD.
 *
 * @author Oleg Ivakhiv
 * @version 4.1 (live feed + live doctrine)
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/TerminalUI.hpp"
#include "utils/UiPalette.hpp"
#include "utils/TermDraw.hpp"
#include "utils/LiveFeed.hpp"
#include "utils/DoctrineStage.hpp"
#include "utils/HunterRecord.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/ShipLivery.hpp"
#include "core/EnemyArchetypes.hpp"
#include <SFML/Graphics.hpp>
#include <sol/sol.hpp>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <cctype>
#include <type_traits>

class MenuSystem : public ISystem {
public:
    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    /// Runs at startup AND on every restart(), so everything here is either
    /// idempotent or guarded -- a restart must not re-run the boot or wipe
    /// the intercept stream.
    void init(const SystemContext& ctx) override {
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_reg = ctx.enemyRegistry;
        m_ui.attach(ctx.window);
        m_ui.setDesignSize({ DW, DH });
        m_zoneState = ctx.zone;
        m_feed.attach(ctx.window, ctx.lua, m_reg, m_zoneState, m_font);
        m_stage.attach(ctx.window, ctx.lua, m_reg, m_zoneState, m_font);
        if (!m_seeded) {
            m_seeded = true;
            for (int i = 0; i < 70; ++i) pushWallLine();
            loadCodexText();
        }
    }

    void setFont(sf::Font* font) {
        m_font = font;
        m_ui.setFonts(m_font, m_mono);
        // The doctrine HUD prints its verdicts with it.
        if (m_window && m_lua) {
            m_feed.attach(m_window, m_lua, m_reg, m_zoneState, m_font);
            m_stage.attach(m_window, m_lua, m_reg, m_zoneState, m_font);
        }
    }
    void setMonoFont(sf::Font* font) { m_mono = font; m_ui.setFonts(m_font, m_mono); }

    void update(float dt) override {
        if (!m_window) return;
        m_ui.begin(dt);
        tick(dt);
        draw();
    }

    /**
     * @brief The action a finished transition (or a click) produced, once.
     *
     * Keyboard confirms no longer act immediately: confirmSelection() starts
     * the close, and the action surfaces HERE when the page has folded away.
     * SystemManager polls this every menu frame.
     */
    MenuAction takeClickAction() {
        const MenuAction a = m_clickAction;
        m_clickAction = MenuAction::None;
        return a;
    }

    // ========================================================================
    // STATE
    // ========================================================================

    void setState(GameState state, int score = 0) {
        m_score = score;
        if (state == m_lastState) return;
        const GameState prev = m_lastState;
        m_lastState = state;
        m_clickAction = MenuAction::None;
        m_ui.primeInput();

        switch (state) {
        case GameState::MainMenu:
            // Boot only on a COLD entry. Coming back from a dead run snaps in:
            // the machine is already awake.
            if (prev == static_cast<GameState>(-1)) beginBoot();
            else openPage(m_returnToCodex ? Page::Codex : Page::Main);
            m_returnToCodex = false;
            break;
        case GameState::Tutorial: openPage(Page::Doctrine); break;
        case GameState::Paused:   openPage(Page::Pause); break;
        case GameState::GameOver: openPage(Page::GameOver); break;
        default:
            m_page = Page::None;
            m_closing = false;
            break;
        }
    }

    /// W / S. Routed to whichever list the current page has.
    void moveSelection(int dir) {
        if (busy() || dir == 0) return;
        if (m_page == Page::Codex) { stepCodex(dir); return; }
        if (m_page == Page::Doctrine) { stepReel(dir); return; }
        auto& rows = currentRows();
        if (rows.empty()) return;
        const int n = static_cast<int>(rows.size());
        int& sel = currentSel();
        sel = ((sel + dir) % n + n) % n;
        m_selT = m_time;
    }

    /// ENTER. Starts the route; the action itself arrives via takeClickAction().
    MenuAction confirmSelection() {
        if (busy()) return MenuAction::None;
        if (m_page == Page::Codex || m_page == Page::Doctrine) return MenuAction::None;
        auto& rows = currentRows();
        if (rows.empty()) return MenuAction::None;
        activate(rows[currentSel()]);
        return MenuAction::None;
    }

    /// The doctrine page's W / S (game.cpp routes Tutorial input here).
    void scrollTutorial(int direction) {
        if (m_page != Page::Doctrine || busy()) return;
        stepReel(direction > 0 ? -1 : 1);
    }

    // ========================================================================
    // BOOT
    // ========================================================================

    bool isBooting() const { return m_page == Page::Boot; }

    /// Any keypress lands here, every frame it is held -- game.cpp polls
    /// levels -- so it acts on the first call and then waits for a gap.
    void skipBoot() {
        if (m_page != Page::Boot) return;
        if (m_time - m_lastSkip < 0.3f) { m_lastSkip = m_time; return; }
        m_lastSkip = m_time;
        if (m_bootT < BOOT_SLAB_T + 0.42f) m_bootT = BOOT_SLAB_T + 0.42f;   // straight to the greeting
        else if (m_slabClose < 0.f) m_slabClose = 0.f;                       // fold it, open the terminal
    }

    // ========================================================================
    // EXTERNAL DATA
    // ========================================================================

    void setHunterLosses(int losses) { m_hunterLosses = std::max(0, losses); }
    void setFeedContacts(int) {}
    void setRecord(const record::HunterRecord* r) { m_record = r; }
    void setRunSeconds(float s) { m_runSeconds = s; }

    /// The ship in the refit bay, every frame. The mesh is rebuilt only when
    /// the outline actually changes.
    void setShip(const ship::ShipDesign* d, const ship::Livery* lv, const std::string& name) {
        m_design = d; m_livery = lv; m_shipName = name;
        if (!d) return;
        const auto& o = d->renderOutline();
        if (o != m_meshSource) {
            m_meshSource = o;
            m_mesh = tdraw::meshFromDesign(*d, 2.4f);
        }
        m_stage.setShip(d, lv);
    }

    /// After F5: scripts/codex.lua has been re-run.
    void reloadText() {
        loadCodexText();
        // Archetype ids and Lua tables were just rebuilt: both hidden worlds
        // hold handles into the old ones. Start them again.
        m_feed.invalidate();
        m_stage.invalidate();
    }

    /// Switch the tube on over the first frames of flight. SystemManager
    /// calls this after the HUD while Playing; a no-op unless armed.
    void overlay(float dt) {
        if (m_tubeOn < 0.f || !m_window) return;
        m_tubeOn += std::clamp(dt, 0.f, 0.05f);
        const float v = std::clamp(m_tubeOn / TUBE_DUR, 0.f, 1.f);
        m_window->setView(m_ui.windowView());
        tube(v, m_ui.windowSize());
        if (v >= 1.f) m_tubeOn = -1.f;
    }

private:
    // ========================================================================
    // TYPES
    // ========================================================================

    enum class Page { None, Boot, Main, Codex, Doctrine, Pause, GameOver };
    using Rect = tui::Rect;
    using Chrome = tui::Chrome;

    struct Row {
        std::string id;
        std::string group;
        std::string label;
        MenuAction  action = MenuAction::None;
        bool        locked = false;
        bool        tube = false;            ///< switch the screen off on the way out
        Page        page = Page::None;       ///< an internal page instead of an action
    };

    struct Panel {
        Rect        r;
        std::string label;
        std::string code;
        Chrome      chrome = Chrome::Hairline;
        sf::Color   accent = ui::CYAN;
        float       delay = 0.f;
        void (MenuSystem::* draw)(const Rect&) = nullptr;
    };

    struct CodexText {
        std::string cls, counter;
        float threat = 0.f;
        std::vector<std::string> behaviour;
        bool valid = false;
    };

    struct CodexEntry {
        std::string key, name, group;
        bool hostile = false;
    };

    struct WallLine { std::string text; bool signal = false; };

    // ========================================================================
    // CONSTANTS
    // ========================================================================

    static constexpr float DW = 1920.f, DH = 1080.f;
    static constexpr float OPEN_DUR = 0.34f;
    static constexpr float CLOSE_DUR = 0.26f;
    static constexpr float CLOSE_STAGGER = 0.45f;
    static constexpr float CONFIRM_BLINK = 0.18f;
    static constexpr float TUBE_DUR = 0.42f;
    static constexpr float SKEW = 14.f;
    static constexpr float BOOT_LOG_T0 = 1.25f, BOOT_LOG_DT = 0.2f, BOOT_SLAB_T = 3.9f;

    // ========================================================================
    // TRANSITIONS
    // ========================================================================

    struct Next {
        enum Kind { None, Page, Action } kind = None;
        MenuSystem::Page page = MenuSystem::Page::None;
        MenuAction action = MenuAction::None;
        bool tube = false;
    };

    bool busy() const { return m_closing || m_page == Page::Boot || m_page == Page::None || m_ui.glossaryOpen(); }

    void openPage(Page p) {
        m_page = p;
        m_openT = 0.f;
        m_closing = false;
        m_tubeOff = -1.f;
        m_selT = m_time;
        m_pathT = m_time;
        if (p == Page::Doctrine) m_stage.play(m_reel);
        m_mouseArm = true;
        if (p == Page::Codex) buildCodexList();
        m_ui.primeInput();
    }

    void startClose(const Next& n) {
        m_next = n;
        m_closing = true;
        m_closeT = -CONFIRM_BLINK;   // the selection blinks before anything folds
        m_tubeOff = -1.f;
        m_pathFrom = pathOf(m_page);
    }

    void activate(const Row& r) {
        if (r.locked) { m_denyT = m_time; return; }
        Next n;
        if (r.page != Page::None) { n.kind = Next::Page; n.page = r.page; }
        else { n.kind = Next::Action; n.action = r.action; n.tube = r.tube; }
        if (n.kind == Next::Action && n.action == MenuAction::None) return;
        startClose(n);
    }

    float maxDelay() const {
        float m = 0.f;
        for (const auto& p : panelsOf(m_page)) m = std::max(m, p.delay);
        return m;
    }

    float closeTotal() const { return maxDelay() * CLOSE_STAGGER + CLOSE_DUR + 0.06f; }

    /// 0 = shut, 1 = fully open. The same curve both ways.
    float panelP(const Panel& p) const {
        if (m_closing) {
            const float t = m_closeT - (maxDelay() - p.delay) * CLOSE_STAGGER;
            return 1.f - std::clamp(t / CLOSE_DUR, 0.f, 1.f);
        }
        return std::clamp((m_openT - p.delay) / OPEN_DUR, 0.f, 1.f);
    }

    void finishClose() {
        const Next n = m_next;
        m_closing = false;
        m_next = Next{};
        if (n.kind == Next::Page) { openPage(n.page); return; }
        if (n.kind == Next::Action) {
            m_clickAction = n.action;
            if (n.action == MenuAction::StartGame || n.action == MenuAction::RestartGame) m_tubeOn = 0.f;
            // Hold the panels shut until SystemManager moves the state on.
            m_page = Page::None;
        }
    }

    // ========================================================================
    // PER-FRAME
    // ========================================================================

    void tick(float dt) {
        dt = std::clamp(dt, 0.f, 0.1f);
        m_time += dt;
        m_casualties += dt * (2.6f + frand() * 3.4f);
        tickWall(dt);

        // The hidden worlds only run while someone can see them.
        if (m_page == Page::Main || m_page == Page::Boot || m_page == Page::Codex) m_feed.step(dt);
        if (m_page == Page::Doctrine) m_stage.step(dt);

        // Hover only steers the selection once the mouse has actually moved
        // on this page. Otherwise a pointer parked mid-screen from aiming in
        // flight grabs whichever pause row happens to sit under it.
        if (m_mouseArm) { m_mouseAnchor = m_ui.mouse(); m_mouseArm = false; m_mouseLive = false; }
        else if (!m_mouseLive) {
            const sf::Vector2f d = m_ui.mouse() - m_mouseAnchor;
            if (d.x * d.x + d.y * d.y > 36.f) m_mouseLive = true;
        }

        if (m_page == Page::Boot) {
            m_bootT += dt;
            if (m_ui.leftEdge()) { m_ui.consumeClick(); m_lastSkip = -1.f; skipBoot(); }
            if (m_slabClose >= 0.f) {
                m_slabClose += dt;
                if (m_slabClose > CLOSE_DUR + 0.05f) { m_slabClose = -1.f; openPage(Page::Main); }
            }
            return;
        }

        if (m_closing) {
            m_closeT += dt;
            if (m_closeT >= closeTotal()) {
                if (m_next.tube) {
                    if (m_tubeOff < 0.f) m_tubeOff = 0.f;
                    m_tubeOff += dt;
                    if (m_tubeOff >= TUBE_DUR) finishClose();
                }
                else finishClose();
            }
        }
        else m_openT += dt;

        if (!busy() && m_openT > 0.25f && !m_ui.glossaryOpen()) {
            if (m_page == Page::Codex && m_ui.keyEdge(sf::Keyboard::Key::Escape)) {
                Next n; n.kind = Next::Page; n.page = Page::Main; startClose(n);
            }
            else if (m_page == Page::Doctrine) {
                if (m_ui.keyEdge(sf::Keyboard::Key::Escape)) {
                    Next n; n.kind = Next::Action; n.action = MenuAction::BackToMenu; startClose(n);
                }
                if (m_ui.keyEdge(sf::Keyboard::Key::A) || m_ui.keyEdge(sf::Keyboard::Key::Left)) jumpReelGroup(false);
                if (m_ui.keyEdge(sf::Keyboard::Key::D) || m_ui.keyEdge(sf::Keyboard::Key::Right)) jumpReelGroup(true);
            }
        }
    }

    // ========================================================================
    // ROWS
    // ========================================================================

    std::vector<Row>& currentRows() {
        if (m_page == Page::Pause) return m_pauseRows;
        if (m_page == Page::GameOver) return m_overRows;
        return m_mainRows;
    }
    int& currentSel() {
        if (m_page == Page::Pause) return m_pauseSel;
        if (m_page == Page::GameOver) return m_overSel;
        return m_mainSel;
    }

    std::vector<Row> m_mainRows = {
        { "hunt",  "CONTRACTS", "ENGAGE THE HUNT",  MenuAction::StartGame,    false, true },
        { "camp",  "CONTRACTS", "CAMPAIGN",         MenuAction::None,         true },
        { "arse",  "ARCHIVES",  "ARSENAL",          MenuAction::None,         true },
        { "codx",  "ARCHIVES",  "CODEX",            MenuAction::None,         false, false, Page::Codex },
        { "doct",  "ARCHIVES",  "FIELD DOCTRINE",   MenuAction::ShowTutorial, false },
        { "refit", "HANGAR",    "REFIT BAY",        MenuAction::ShowRefit,    false },
        { "rites", "SYSTEM",    "MACHINE RITES",    MenuAction::None,         true },
        { "leave", "SYSTEM",    "LEAVE THE SYSTEM", MenuAction::QuitGame,     false, true },
    };
    std::vector<Row> m_pauseRows = {
        { "resume",  "", "RESUME CONTRACT",     MenuAction::ResumeGame },
        { "doct",    "", "FIELD DOCTRINE",      MenuAction::ShowTutorial },
        { "restart", "", "PURGE AND RESTART",   MenuAction::RestartGame },
        { "abandon", "", "ABANDON TO TERMINAL", MenuAction::BackToMenu },
        { "leave",   "", "LEAVE THE SYSTEM",    MenuAction::QuitGame, false, true },
    };
    std::vector<Row> m_overRows = {
        { "again",  "", "RE-ENGAGE",          MenuAction::RestartGame },
        { "return", "", "RETURN TO TERMINAL", MenuAction::BackToMenu },
        { "leave",  "", "LEAVE THE SYSTEM",   MenuAction::QuitGame, false, true },
    };

    // ========================================================================
    // PANELS
    // ========================================================================

    std::vector<Panel> panelsOf(Page p) const {
        switch (p) {
        case Page::Main: return {
            { { 38.f, 116.f, 900.f, 556.f },  "TACTICAL FEED // CAMERA", "LIVE",     Chrome::Brackets, ui::CYAN, 0.00f, &MenuSystem::drawFeed },
            { { 976.f, 116.f, 546.f, 600.f }, "COMMAND",                 "8 ROUTES", Chrome::Heavy,    ui::CYAN, 0.10f, &MenuSystem::drawCommand },
            { { 1560.f, 116.f, 322.f, 472.f },"HUNTER DOSSIER",          "",         Chrome::Brackets, ui::CYAN, 0.18f, &MenuSystem::drawDossier },
            { { 38.f, 708.f, 428.f, 252.f },  "CONTRACT LEDGER",         "",         Chrome::Hairline, ui::CYAN, 0.26f, &MenuSystem::drawLedger },
            { { 496.f, 708.f, 442.f, 252.f }, "SECTOR // DERANT RIM",    "",         Chrome::Hairline, ui::CYAN, 0.32f, &MenuSystem::drawSector },
            { { 1560.f, 624.f, 322.f, 336.f },"INTERCEPT",               "BAND 7",   Chrome::Hairline, ui::CYAN, 0.38f, &MenuSystem::drawIntercept },
            { { 976.f, 752.f, 546.f, 208.f }, "BRIEF",                   "",         Chrome::Hairline, ui::CYAN, 0.46f, &MenuSystem::drawBrief },
        };
        case Page::Codex: return {
            { { 38.f, 116.f, 520.f, 844.f },  "CODEX // INDEX", "",      Chrome::Heavy,    ui::CYAN, 0.00f, &MenuSystem::drawCodexIndex },
            { { 596.f, 116.f, 700.f, 844.f }, "SPECIMEN",       "",      Chrome::Brackets, ui::CYAN, 0.10f, &MenuSystem::drawCodexSpecimen },
            { { 1334.f, 116.f, 548.f, 844.f },"ENTRY",          "",      Chrome::Hairline, ui::CYAN, 0.20f, &MenuSystem::drawCodexEntry },
        };
        case Page::Doctrine: return {
            { { 38.f, 116.f, 420.f, 844.f },  "DOCTRINE // REELS", "8 REELS", Chrome::Heavy,    ui::CYAN, 0.00f, &MenuSystem::drawReelList },
            { { 496.f, 116.f, 1386.f, 844.f },"LESSON",            "LIVE",    Chrome::Brackets, ui::CYAN, 0.10f, &MenuSystem::drawReel },
        };
        case Page::Pause: return {
            { { 660.f, 290.f, 600.f, 440.f }, "HOLD", "RUN SUSPENDED", Chrome::Heavy, ui::AMBER, 0.00f, &MenuSystem::drawPause },
        };
        case Page::GameOver: return {
            { { 640.f, 250.f, 640.f, 520.f }, "HUNTER LOST", "BEACON SILENT", Chrome::Heavy, ui::RED, 0.00f, &MenuSystem::drawGameOver },
        };
        default: return {};
        }
    }

    static std::string pathOf(Page p) {
        switch (p) {
        case Page::Main:     return "TERMINAL://HUNT/COMMAND";
        case Page::Codex:    return "TERMINAL://ARCHIVE/CODEX";
        case Page::Doctrine: return "TERMINAL://ARCHIVE/DOCTRINE";
        case Page::Pause:    return "FIELD://CONTRACT/HOLD";
        case Page::GameOver: return "FIELD://CONTRACT/HUNTER_LOST";
        default:             return "";
        }
    }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw() {
        if (!m_font) return;
        const bool overWorld = (m_lastState == GameState::Paused || m_lastState == GameState::GameOver);

        // Whole window first, bars included, so the letterbox is ground too.
        m_window->setView(m_ui.windowView());
        {
            const sf::Vector2f ws = m_ui.windowSize();
            sf::RectangleShape bg(ws);
            bg.setFillColor(overWorld ? sf::Color(2, 3, 5, 214) : ui::VOID_BG);
            m_window->draw(bg);
        }
        m_window->setView(m_ui.uiView());

        if (m_page == Page::Boot) { drawBoot(); return; }
        if (m_page == Page::Codex) buildCodexList();

        if (!overWorld) {
            // A faint dot grid: the empty gutters still read as a surface.
            m_b.clear();
            for (float y = 120.f; y < 980.f; y += 40.f)
                for (float x = 40.f; x < DW - 30.f; x += 40.f) m_b.rect(x, y, 1.f, 1.f, tdraw::alpha(ui::CYAN_LOW, 0.5f));
            m_b.draw(*m_window);
        }

        drawHeader();
        drawFooter();

        if (m_page != Page::None) {
            for (const auto& p : panelsOf(m_page)) drawPanel(p, panelP(p));
        }

        drawGlossary();

        if (m_tubeOff >= 0.f) {
            m_window->setView(m_ui.windowView());
            tube(1.f - std::clamp(m_tubeOff / TUBE_DUR, 0.f, 1.f), m_ui.windowSize());
            m_window->setView(m_ui.uiView());
        }
        scan(0.f, 0.f, DW, DH, 26);
    }

    // ---- panel chrome -----------------------------------------------------

    void drawPanel(const Panel& pn, float p) {
        if (p <= 0.f) return;
        const Rect& r = pn.r;
        const float wf = std::clamp(p / 0.34f, 0.06f, 1.f);
        const float hf = std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
        const float dw = r.w * wf, dh = std::max(2.f, r.h * hf);
        const Rect d{ r.cx() - dw * 0.5f, r.cy() - dh * 0.5f, dw, dh };
        const sf::Color acc = pn.accent;

        m_b.clear();
        m_b.rect(d.x, d.y, d.w, d.h, ui::PANEL_BG);
        if (p < 0.98f) {
            const float a = std::clamp((1.f - p) * 2.2f, 0.f, 1.f);
            m_b.rect(d.x, d.cy() - 1.f, d.w, 2.f, tdraw::alpha(acc, 0.92f * a));
            if (p < 0.08f) m_b.rect(d.cx() - 4.f, d.cy() - 2.f, 8.f, 4.f, tdraw::alpha(ui::TEXT, a));
        }
        const float L = std::min(22.f, std::min(d.w, d.h) * 0.25f);
        switch (pn.chrome) {
        case Chrome::Heavy: {
            const sf::Color c = tdraw::alpha(acc, 0.82f);
            m_b.rect(d.x, d.y, d.w, 2.f, c); m_b.rect(d.x, d.bottom() - 2.f, d.w, 2.f, c);
            m_b.rect(d.x, d.y, 1.f, d.h, c); m_b.rect(d.right() - 1.f, d.y, 1.f, d.h, c);
            break;
        }
        case Chrome::Hairline: {
            const sf::Color c = tdraw::alpha(acc, 0.5f);
            m_b.rect(d.x, d.y, d.w, 1.f, c); m_b.rect(d.x, d.bottom() - 1.f, d.w, 1.f, c);
            m_b.rect(d.x, d.y, 1.f, d.h, c); m_b.rect(d.right() - 1.f, d.y, 1.f, d.h, c);
            bracketsB(d, tdraw::alpha(acc, 0.85f), std::min(10.f, L));
            break;
        }
        default: {
            const sf::Color c = tdraw::alpha(ui::CYAN_LOW, 0.6f);
            m_b.rect(d.x, d.y, d.w, 1.f, c); m_b.rect(d.x, d.bottom() - 1.f, d.w, 1.f, c);
            m_b.rect(d.x, d.y, 1.f, d.h, c); m_b.rect(d.right() - 1.f, d.y, 1.f, d.h, c);
            bracketsB(d, tdraw::alpha(acc, 0.75f), L);
            break;
        }
        }
        m_b.draw(*m_window);
        if (p < 1.f) return;

        // Title on the border; a gap punched behind it so the stroke never
        // runs through the glyphs.
        if (!pn.label.empty()) {
            if (pn.chrome == Chrome::Heavy) {
                const float w = tw(pn.label, 14, 1.8f) + 14.f;
                fill({ r.x + 8.f, r.y - 10.f, w, 20.f }, acc);
                txt(pn.label, r.x + 15.f, r.y - 9.f, 14, ui::INK, 1.8f);
            }
            else {
                const float w = tw(pn.label, 14, 1.8f) + 12.f;
                fill({ r.x + 10.f, r.y - 2.f, w, 4.f }, ui::VOID_BG);
                txt(pn.label, r.x + 16.f, r.y - 10.f, 14, acc, 1.8f);
            }
        }
        const std::string code = pn.code.empty() ? std::string() : pn.code;
        if (!code.empty()) {
            const float w = tw(code, 12, 1.4f) + 12.f;
            fill({ r.right() - 16.f - w, r.y - 2.f, w, 4.f }, ui::VOID_BG);
            txtR(code, r.right() - 22.f, r.y - 8.f, 12, ui::TEXT_DIM);
        }

        // A 60 ms repaint flicker as the content arrives: a terminal redraw.
        const float age = m_openT - pn.delay - OPEN_DUR;
        if (!m_closing && age < 0.06f && static_cast<int>(age * 100.f) % 2 == 1) return;

        m_ui.beginClip(r);
        (this->*pn.draw)(Rect{ 0.f, 0.f, r.w, r.h });
        m_ui.endClip();
    }

    // ---- header / footer --------------------------------------------------

    void drawHeader() {
        const bool over = m_lastState == GameState::GameOver;
        txt("VOID HUNTER", 38.f, 22.f, 34, over ? ui::RED : ui::CYAN, 2.2f);
        txt("HUNTER-NET TERMLINK  //  NODE DR-07", 40.f, 66.f, 12, ui::TEXT_DIM, 1.8f);

        // Breadcrumb: deletes itself on the way out, types the next address in.
        std::string path = m_closing ? m_pathFrom : pathOf(m_page);
        std::size_t n = path.size();
        if (m_closing) {
            const float t = std::max(0.f, m_closeT);
            n = static_cast<std::size_t>(std::max(0.f, static_cast<float>(path.size()) - t * 120.f));
        }
        else n = static_cast<std::size_t>(std::clamp((m_time - m_pathT) * 70.f, 0.f, static_cast<float>(path.size())));
        const std::string shown = path.substr(0, n);
        txt(">", 640.f, 32.f, 18, ui::AMBER);
        txt(shown, 662.f, 32.f, 18, ui::TEXT);
        if (std::fmod(m_time, 0.66f) < 0.33f) fill({ 666.f + tw(shown, 18, 1.4f), 36.f, 11.f, 18.f }, ui::CYAN);

        // Real wall clock: the one thing on this screen that is exactly true.
        const std::time_t now = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        char clock[48];
        std::snprintf(clock, sizeof clock, "CYCLE %04d.%d   %02d:%02d:%02d",
            m_record ? 400 + m_record->contracts() : 412, lt.tm_wday, lt.tm_hour, lt.tm_min, lt.tm_sec);
        txtR(clock, DW - 38.f, 22.f, 16, ui::TEXT);
        const float pulse = 0.55f + 0.45f * std::fabs(std::sin(m_time * 2.2f));
        const std::string status = over ? "STATUS  LOST" : "STATUS  ALIVE";
        const sf::Color sc = over ? ui::RED : ui::GREEN;
        txtR(status, DW - 38.f, 52.f, 14, tdraw::alpha(sc, pulse), 1.8f);
        fill({ DW - 38.f - tw(status, 14, 1.8f) - 18.f, 57.f, 8.f, 8.f }, tdraw::alpha(sc, pulse));

        m_b.clear();
        m_b.rect(38.f, 92.f, DW - 76.f, 1.f, ui::CYAN_LOW);
        for (int i = 0; i <= 32; ++i) m_b.rect(38.f + i * (DW - 76.f) / 32.f, 92.f, 1.f, i % 4 == 0 ? 6.f : 3.f, ui::CYAN_LOW);
        m_b.draw(*m_window);
    }

    void drawFooter() {
        fill({ 38.f, 990.f, DW - 76.f, 1.f }, ui::CYAN_LOW);
        float x = 38.f;
        switch (m_page) {
        case Page::Main:
            x = keycap(x, 1006.f, "W/S", "SELECT"); x = keycap(x, 1006.f, "ENTER", "CONFIRM"); break;
        case Page::Codex:
            x = keycap(x, 1006.f, "W/S", "BROWSE"); x = keycap(x, 1006.f, "ESC", "BACK"); break;
        case Page::Doctrine:
            x = keycap(x, 1006.f, "W/S", "REEL"); x = keycap(x, 1006.f, "A/D", "BASIC / ADVANCED"); x = keycap(x, 1006.f, "ESC", "BACK"); break;
        case Page::Pause:
            x = keycap(x, 1006.f, "W/S", "SELECT"); x = keycap(x, 1006.f, "ENTER", "CONFIRM"); x = keycap(x, 1006.f, "ESC", "RESUME"); x = keycap(x, 1006.f, "T", "DOCTRINE"); break;
        default:
            x = keycap(x, 1006.f, "W/S", "SELECT"); x = keycap(x, 1006.f, "ENTER", "CONFIRM"); break;
        }
        txtR("UPLINK 98%   //   BUILD 2.0   //   DR-07", DW - 96.f, 1010.f, 12, ui::TEXT_DEAD);
    }

    void drawGlossary() {
        m_ui.glossaryTab({ DW - 72.f, 1002.f, 34.f, 30.f });
        switch (m_page) {
        case Page::Main:
            m_ui.drawGlossary("MAIN TERMINAL",
                "Live feed, your dossier and the contract record.\nEverything here works with keyboard or mouse.",
                { { "W / S",      "Move the selection up or down" },
                  { "MOUSE",      "Hover a row to select it" },
                  { "ENTER",      "Take the selected route" },
                  { "LEFT CLICK", "Take the route under the cursor" },
                  { "SEALED",     "Not built yet - the brief says what it will be" },
                  { "?",          "Open this glossary on any screen" },
                  { "F11",        "Toggle fullscreen" },
                  { "F5",         "Reload Lua scripts (dev)" } });
            break;
        case Page::Codex:
            m_ui.drawGlossary("CODEX",
                "Everything you have met in the field. An entry opens\nthe first time you see the thing in a contract.",
                { { "W / S", "Browse entries" },
                  { "ESC",   "Back to the terminal" },
                  { "LOCKED","Not met yet - go and find it" } });
            break;
        case Page::Doctrine:
            m_ui.drawGlossary("FIELD DOCTRINE",
                "Live reels of every mechanic, drawn with your ship.\nThe keycaps light up when the reel uses them.",
                { { "W / S", "Previous or next reel" },
                  { "A / D", "Jump between BASIC and ADVANCED" },
                  { "ESC",   "Close and go back" } });
            break;
        case Page::Pause:
            m_ui.drawGlossary("HOLD",
                "The run is suspended. Nothing out there is moving.",
                { { "ESC",              "Resume the hunt at once" },
                  { "T",                "Jump straight to Field Doctrine" },
                  { "PURGE AND RESTART","New sector. This run is abandoned" },
                  { "ABANDON",          "Leave the run, back to the terminal" } });
            break;
        case Page::GameOver:
            m_ui.drawGlossary("HUNTER LOST",
                "The run ended. Your hull design is kept.",
                { { "RE-ENGAGE", "Start a fresh sector with the same ship" },
                  { "RETURN",    "Back to the main terminal" } });
            break;
        default: break;
        }
    }

    // ========================================================================
    // MAIN PAGE
    // ========================================================================

    void drawFeed(const Rect& r) {
        const Rect v{ 12.f, 12.f, r.w - 24.f, r.h - 24.f };
        // A real fight in a hidden world (utils/LiveFeed), drawn as a picture.
        // Rendered at 1060 px wide: the camera sees 1060 world pixels, a
        // little closer than flight so a duel fills the panel.
        const unsigned fw = 1060u;
        m_feed.setSize({ fw, static_cast<unsigned>(fw * v.h / v.w) });
        m_feed.render();
        blitWorld(m_feed.texture(), v);
        drawFeedMarks(v);

        m_b.clear();
        bracketsB(v, tdraw::alpha(ui::CYAN, 0.8f), 18.f);
        const float cx = v.cx(), cy = v.cy();
        const sf::Color rc = tdraw::alpha(ui::CYAN, 0.35f);
        m_b.rect(cx - 30.f, cy, 12.f, 1.f, rc); m_b.rect(cx + 18.f, cy, 12.f, 1.f, rc);
        m_b.rect(cx, cy - 30.f, 1.f, 12.f, rc); m_b.rect(cx, cy + 18.f, 1.f, 12.f, rc);
        m_b.draw(*m_window);

        char buf[96];
        std::snprintf(buf, sizeof buf, "FEED %02d / HUNTER %s / DERANT RIM", m_feed.feedNo(), m_feed.callsign().c_str());
        txt(buf, v.x + 14.f, v.y + 10.f, 12, ui::CYAN_MID);
        if (std::fmod(m_time, 1.25f) < 0.65f) fill({ v.right() - 60.f, v.y + 14.f, 9.f, 9.f }, ui::RED);
        txt("LIVE", v.right() - 46.f, v.y + 10.f, 12, ui::RED);
        txt(timecode(), v.x + 14.f, v.bottom() - 26.f, 12, ui::TEXT_DIM);
        txt("HULL", v.right() - 240.f, v.bottom() - 26.f, 12, ui::TEXT_DIM);
        segBar(v.right() - 196.f, v.bottom() - 23.f, 120.f, 10.f, m_feed.hull01(), 12,
            m_feed.hull01() < 0.35f ? ui::RED : ui::CYAN, ui::CYAN_LOW);
        std::snprintf(buf, sizeof buf, "K %02d", m_feed.kills());
        txtR(buf, v.right() - 14.f, v.bottom() - 26.f, 12, ui::AMBER);
        scan(v.x, v.y, v.w, v.h, 46);
    }

    /// A hidden world's frame, stretched over a panel-local rect.
    void blitWorld(const sf::Texture* tex, const Rect& v) {
        if (!tex) { fill(v, sf::Color(3, 5, 8)); return; }
        sf::Sprite sp(*tex);
        const sf::Vector2u ts = tex->getSize();
        sp.setPosition({ v.x, v.y });
        sp.setScale({ v.w / std::max(1.f, static_cast<float>(ts.x)), v.h / std::max(1.f, static_cast<float>(ts.y)) });
        m_window->draw(sp);
    }

    /// Camera marks over the feed: target lock, hunter tag, and the static
    /// when the hunter's beacon dies.
    void drawFeedMarks(const Rect& v) {
        auto inside = [](sf::Vector2f u) { return u.x >= 0.02f && u.x <= 0.98f && u.y >= 0.02f && u.y <= 0.98f; };
        if (!m_feed.lost()) {
            std::string label;
            const sf::Vector2f tu = m_feed.targetUV(&label);
            if (inside(tu)) {
                const float tx = v.x + tu.x * v.w, ty = v.y + tu.y * v.h, b = 24.f;
                m_b.clear();
                bracketsB({ tx - b, ty - b, b * 2.f, b * 2.f }, tdraw::alpha(ui::RED, 0.95f), 7.f);
                m_b.draw(*m_window);
                txt(upper(label), tx + b + 6.f, ty - b - 2.f, 10, ui::RED);
            }
            const sf::Vector2f hu = m_feed.hunterUV();
            if (inside(hu)) {
                const float hx = v.x + hu.x * v.w, hy = v.y + hu.y * v.h;
                txt(m_feed.callsign(), hx + 20.f, hy + 14.f, 10, tdraw::alpha(ui::CYAN, 0.9f));
            }
            return;
        }
        // SIGNAL LOST: hold on the wreck, then the picture breaks up.
        const float k = m_feed.lostT();
        if (k > LiveFeed::LOST_HOLD * 0.5f) {
            m_b.clear();
            const float amt = std::clamp((k - LiveFeed::LOST_HOLD * 0.5f) / 0.5f, 0.f, 1.f);
            for (int i = 0; i < static_cast<int>(900 * amt); ++i)
                m_b.rect(v.x + frand() * v.w, v.y + frand() * v.h, 1.f + frand() * 3.f, 1.f,
                    tdraw::alpha(ui::TEXT, frand() * 0.5f));
            for (int i = 0; i < 6; ++i)
                m_b.rect(v.x, v.y + frand() * v.h, v.w, 2.f + frand() * 8.f, tdraw::alpha(ui::TEXT, 0.06f));
            m_b.draw(*m_window);
        }
        const float cx = v.cx(), cy = v.cy();
        char buf[64];
        if (k > 0.25f && k < LiveFeed::LOST_TOTAL - 0.9f) {
            fill({ cx - 210.f, cy - 46.f, 420.f, 92.f }, tdraw::alpha(ui::VOID_BG, 0.9f));
            txtC("SIGNAL LOST", cx, cy - 32.f, 30, ui::RED, 3.f);
            std::snprintf(buf, sizeof buf, "HUNTER %s  //  BEACON SILENT", m_feed.callsign().c_str());
            txtC(buf, cx, cy + 12.f, 12, ui::TEXT);
        }
        else if (k >= LiveFeed::LOST_TOTAL - 0.9f) {
            fill({ cx - 210.f, cy - 30.f, 420.f, 60.f }, tdraw::alpha(ui::VOID_BG, 0.9f));
            std::snprintf(buf, sizeof buf, "REACQUIRING FEED %02d ...", m_feed.feedNo() + 1);
            txtC(buf, cx, cy - 10.f, 18, ui::AMBER);
        }
    }

    void drawCommand(const Rect& r) {
        const float rowH = 46.f, x = 26.f, w = r.w - 52.f;
        float y = 22.f;
        std::string grp;
        for (std::size_t i = 0; i < m_mainRows.size(); ++i) {
            const Row& row = m_mainRows[i];
            if (row.group != grp) {
                grp = row.group;
                txt(grp, x, y + 2.f, 10, ui::CYAN_MID, 2.2f);
                const float gw = tw(grp, 10, 2.2f) + 10.f;
                fill({ x + gw, y + 9.f, w - gw, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
                y += 24.f;
            }
            rowWidget(m_mainRows, m_mainSel, static_cast<int>(i), x, y, w, rowH - 8.f, rowMeta(row), 22);
            y += rowH;
        }
        fill({ x, r.h - 66.f, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
        txt("LAST CONTRACT", x, r.h - 56.f, 10, ui::TEXT_DIM, 1.8f);
        if (m_record && m_record->hasRuns()) {
            const auto& lr = m_record->lastRun();
            const std::string s = std::string(lr.lost ? "HUNTER LOST" : "ABANDONED") + "  //  " + clockOf(lr.seconds) + " IN FIELD";
            txt(s, x, r.h - 36.f, 14, lr.lost ? ui::RED : ui::AMBER_HOT);
            txtR("SCRAP  " + groupNumber(lr.scrap), x + w, r.h - 36.f, 14, ui::AMBER);
        }
        else txt("NO CONTRACTS ON RECORD", x, r.h - 36.f, 14, ui::TEXT_DIM);
    }

    std::string rowMeta(const Row& row) const {
        if (row.locked) return "SEALED";
        if (row.id == "hunt") return "ROGUELIKE  //  DERANT RIM";
        if (row.id == "codx") {
            const auto [seen, total] = codexCount();
            return std::to_string(seen) + " / " + std::to_string(total) + " CATALOGUED";
        }
        if (row.id == "doct") return std::to_string(doctrine::lessons().size()) + " REELS";
        if (row.id == "refit") return shipLabel();
        return "";
    }

    /**
     * One selectable row: number, label, right-hand status, and the skewed
     * selection bar. Mouse hover moves the keyboard selection, so the two
     * input methods can never disagree about what is highlighted.
     */
    void rowWidget(std::vector<Row>& rows, int& sel, int i, float x, float y, float w, float h,
        const std::string& meta, unsigned size) {
        const Row& row = rows[i];
        const Rect hit{ x, y, w, h };
        if (!busy() && m_ui.hovering(hit)) {
            if (m_ui.leftEdge()) { m_mouseLive = true; sel = i; m_ui.consumeClick(); activate(row); }
            else if (m_mouseLive && sel != i) { sel = i; m_selT = m_time; }
        }
        const bool on = (i == sel);
        const bool blink = m_closing && on && m_closeT < 0.f && static_cast<int>((m_closeT + CONFIRM_BLINK) * 22.f) % 2 == 1;
        if (on && !blink) {
            sf::ConvexShape bar(4);
            bar.setPoint(0, { x + SKEW, y }); bar.setPoint(1, { x + w + SKEW, y });
            bar.setPoint(2, { x + w, y + h }); bar.setPoint(3, { x, y + h });
            bar.setFillColor(row.locked ? tdraw::alpha(ui::TEXT_DEAD, 0.6f) : ui::AMBER);
            m_window->draw(bar);
            const float k = std::clamp((m_time - m_selT) / 0.12f, 0.f, 1.f);
            fill({ x - 18.f - (1.f - k) * 10.f, y + 8.f, 6.f, h - 16.f }, tdraw::alpha(ui::AMBER, k));
        }
        else if (!on) fill({ x, y + h, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.7f));
        const bool lit = on && !row.locked && !blink;
        const sf::Color fg = row.locked ? ui::TEXT_DEAD : (lit ? ui::INK : ui::TEXT);
        char num[16]; std::snprintf(num, sizeof num, "%02d", i + 1);
        txt(num, x + 16.f, y + h * 0.5f - 9.f, 14, lit ? ui::INK : ui::TEXT_DIM);
        txt(row.label, x + 58.f, y + h * 0.5f - size * 0.62f, size, fg);
        if (row.locked) {
            const float lx = x + w - 16.f - tw(meta, 12, 1.4f) - 26.f, ly = y + h * 0.5f - 4.f;
            fill({ lx, ly, 12.f, 10.f }, ui::TEXT_DEAD);
            fill({ lx + 2.f, ly - 6.f, 2.f, 6.f }, ui::TEXT_DEAD);
            fill({ lx + 8.f, ly - 6.f, 2.f, 6.f }, ui::TEXT_DEAD);
            fill({ lx + 2.f, ly - 7.f, 8.f, 2.f }, ui::TEXT_DEAD);
        }
        if (!meta.empty()) txtR(meta, x + w - 16.f, y + h * 0.5f - 8.f, 12, lit ? ui::INK : (row.locked ? ui::TEXT_DEAD : ui::CYAN_MID));
    }

    void drawBrief(const Rect& r) {
        const Row& row = m_mainRows[m_mainSel];
        const float x = 26.f, w = r.w - 52.f;
        std::string title = row.label;
        std::vector<std::string> body;
        struct Bar { const char* k; float v; sf::Color c; std::string label; };
        std::vector<Bar> bars;
        if (row.id == "hunt") {
            body = { "ONE LIFE. A RANDOM FIELD. SALVAGE WHAT YOU CAN", "AND COME BACK IF YOU CAN." };
            bars = { { "THREAT", 0.72f, ui::RED, "HIGH" }, { "PAYOUT", 0.45f, ui::AMBER, "SCRAP X1.0" } };
        }
        else if (row.id == "camp") {
            body = { "A FIXED STORY ACROSS THE RIM. EVERY CHAPTER", "OFFERS MORE THAN ONE ROUTE. NO PERMADEATH." };
        }
        else if (row.id == "arse") {
            body = { "EVERY WEAPON, HULL AND MODULE YOU PULL OUT OF", "THE VOID, KEPT LIKE A LIBRARY." };
        }
        else if (row.id == "codx") {
            const auto [seen, total] = codexCount();
            body = { "WHAT YOU HAVE MET OUT THERE: HOSTILES, ROCKS,", "WRECKS. AND WHAT HAS NO ENTRY YET." };
            bars = { { "CATALOGUED", total ? static_cast<float>(seen) / total : 0.f, ui::CYAN,
                std::to_string(seen) + " / " + std::to_string(total) } };
        }
        else if (row.id == "doct") {
            body = { "LIVE REELS OF EVERY MECHANIC, FLOWN BY YOUR SHIP:", "FLIGHT AND GUNNERY UP TO RIFT BOLT AND VENT." };
            bars = { { "BASIC", 1.f, ui::CYAN, "4 REELS" }, { "ADVANCED", 1.f, ui::VIOLET, "4 REELS" } };
        }
        else if (row.id == "refit") {
            body = { "REBUILD THE HULL, MOVE THE MOUNTS, PAINT YOUR", "OWN TRAILS. THE FRAME DECIDES WHAT IT CARRIES." };
            if (m_design) {
                const auto& st = m_design->stats();
                const auto& sp = m_design->spec();
                bars = { { "GUNS", static_cast<float>(st.gunCount) / std::max(1, sp.maxGuns), ui::CYAN,
                             std::to_string(st.gunCount) + " OF " + std::to_string(sp.maxGuns) },
                         { "DRIVES", static_cast<float>(st.engineCount) / std::max(1, sp.maxEngines), ui::CYAN,
                             std::to_string(st.engineCount) + " OF " + std::to_string(sp.maxEngines) } };
            }
        }
        else if (row.id == "rites") {
            body = { "CONFIGURATION SUBSYSTEM. SEALED BY THE COMBINE." };
        }
        else if (row.id == "leave") {
            body = { "POWER DOWN THE TERMLINK AND EXIT.", "YOUR RECORD IS ALREADY SEALED." };
        }

        const bool denied = row.locked && m_time - m_denyT < 1.2f;
        txt(title, x, 20.f, 20, row.locked ? ui::TEXT_DEAD : ui::AMBER, 2.f);
        if (row.locked) txtR(denied ? "ACCESS DENIED" : "SEALED  //  LATER BUILD", x + w, 26.f,
            10, denied && std::fmod(m_time, 0.2f) < 0.1f ? ui::RED : ui::TEXT_DIM);
        fill({ x, 52.f, w, 1.f }, ui::CYAN_LOW);
        // Retyped on every selection change.
        float k = (m_time - m_selT) * 140.f;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const float n = std::clamp(k, 0.f, static_cast<float>(body[i].size()));
            k -= static_cast<float>(body[i].size());
            const std::string s = body[i].substr(0, static_cast<std::size_t>(n));
            const float ly = 64.f + static_cast<float>(i) * 24.f;
            txt(s, x, ly, 14, row.locked ? ui::TEXT_DIM : ui::TEXT);
            if (n > 0.f && n < body[i].size() && std::fmod(m_time, 0.25f) < 0.125f)
                fill({ x + tw(s, 14, 1.4f) + 4.f, ly + 2.f, 8.f, 14.f }, ui::CYAN);
        }
        float y = 64.f + static_cast<float>(body.size()) * 24.f + 12.f;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const float vis = std::clamp((m_time - m_selT - 0.25f - 0.07f * i) / 0.2f, 0.f, 1.f);
            if (vis <= 0.f) continue;
            txt(bars[i].k, x, y, 12, ui::TEXT_DIM);
            segBar(x + 130.f, y + 2.f, 230.f, 11.f, bars[i].v * vis, 20, bars[i].c, ui::CYAN_LOW);
            txtR(bars[i].label, x + w, y, 12, bars[i].c);
            y += 24.f;
        }
    }

    void drawDossier(const Rect& r) {
        const float x = 20.f, w = r.w - 40.f;
        const Rect box{ x, 26.f, w, 180.f };
        m_b.clear();
        for (int i = 1; i < 6; ++i) m_b.rect(box.x + i * box.w / 6.f, box.y, 1.f, box.h, tdraw::alpha(ui::CYAN_LOW, 0.3f));
        for (int i = 1; i < 4; ++i) m_b.rect(box.x, box.y + i * box.h / 4.f, box.w, 1.f, tdraw::alpha(ui::CYAN_LOW, 0.3f));
        bracketsB(box, tdraw::alpha(ui::CYAN_MID, 0.7f), 10.f);
        if (m_mesh.valid()) {
            const float s = 64.f / m_mesh.radius;
            const float a = -tdraw::PI * 0.5f + std::sin(m_time * 0.5f) * 0.35f;
            const sf::Vector2f c{ box.cx(), box.cy() + 6.f };
            const sf::Vector2f back{ -std::cos(a), -std::sin(a) };
            const sf::Color thrust = m_livery ? m_livery->paint.thrust : ui::AMBER_HOT;
            m_b.line(c + back * (m_mesh.radius * s * 0.85f), c + back * (m_mesh.radius * s * 0.85f + 12.f + std::sin(m_time * 30.f) * 4.f), 6.f, thrust);
            tdraw::drawHull(m_b, m_mesh, tdraw::hullXf(c, a, s),
                m_livery ? m_livery->paint.hull : ui::CYAN, m_livery ? m_livery->paint.outline : ui::TEXT, m_livery, &m_vscratch);
        }
        m_b.draw(*m_window);

        txt(shipLabel(), x, box.bottom() + 12.f, 20, ui::TEXT);
        std::string cls = "STOCK FRAME";
        if (m_design) cls = std::string(m_design->spec().name) + " FRAME  //  " + m_design->spec().pattern;
        txt(cls, x, box.bottom() + 40.f, 12, ui::TEXT_DIM);
        float y = box.bottom() + 66.f;
        if (m_design) {
            const auto& st = m_design->stats();
            const auto& ref = ship::ShipDesign::reference();
            auto bar = [&](const char* k, float v, float refv) {
                txt(k, x, y, 12, ui::TEXT_DIM);
                char b[16]; std::snprintf(b, sizeof b, "%d", static_cast<int>(std::round(v)));
                txtR(b, x + 112.f, y, 12, ui::TEXT);
                segBar(x + 124.f, y + 2.f, w - 124.f, 10.f, std::clamp(v / (refv * 1.8f), 0.f, 1.f), 14, ui::CYAN, ui::CYAN_LOW);
                y += 22.f;
            };
            bar("HULL", st.hpMax, ref.hpMax);
            bar("ENERGY", st.energyMax, ref.energyMax);
            bar("THRUST", st.forwardThrust, ref.forwardThrust);
            y += 6.f;
            fill({ x, y, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
            y += 10.f;
            txt("LOADOUT", x, y, 10, ui::CYAN_MID, 2.2f);
            y += 20.f;
            const bool heavy = m_design->hullClass() == ship::HullClass::Heavy;
            const std::string guns = std::to_string(std::max(1, st.primaryCount)) + " X PLASMA";
            const std::pair<const char*, std::string> lo[4] = {
                { "GUNS", guns }, { "SPECIAL", "RIFT BOLT" },
                { "DRIVES", std::to_string(st.engineCount) + " X ENGINE" },
                { "DEFENCE", heavy ? "DODGE + PARRY + BASH" : "DODGE + PARRY" } };
            for (const auto& [k, v] : lo) {
                txt(k, x, y, 12, ui::TEXT_DIM);
                txtR(v, x + w, y, 12, std::string(k) == "SPECIAL" ? ui::VIOLET : ui::TEXT);
                y += 20.f;
            }
        }
    }

    void drawLedger(const Rect& r) {
        const float x = 22.f, w = r.w - 44.f;
        const int lost = m_record ? m_record->lost() : m_hunterLosses;
        txt("HUNTERS LOST", x, 22.f, 12, ui::TEXT_DIM, 1.8f);
        char b[16]; std::snprintf(b, sizeof b, "%02d", lost);
        txt(b, x, 40.f, 44, lost > 0 ? ui::RED : ui::TEXT_DEAD);
        txtR("SYSTEM CASUALTIES", x + w, 22.f, 12, ui::TEXT_DIM, 1.8f);
        txtR(groupNumber(static_cast<long long>(m_casualties) + m_feed.casualties()), x + w, 42.f, 28, ui::TEXT);
        txtR("+4.3 / SEC", x + w, 80.f, 12, ui::AMBER_HOT);

        const float hy = 124.f, hh = 78.f;
        txt("LAST 16 CONTRACTS  //  TIME IN FIELD", x, hy - 6.f, 12, ui::TEXT_DIM);
        fill({ x, hy + 18.f + hh, w, 1.f }, ui::CYAN_LOW);
        const std::size_t N = record::HunterRecord::LEDGER_LEN;
        const float bw = (w - 4.f * (N - 1)) / N;
        if (!m_record || !m_record->hasRuns()) {
            txt("NO CONTRACTS ON RECORD", x, hy + 46.f, 12, ui::TEXT_DEAD);
            return;
        }
        const auto& runs = m_record->runs();
        float mx = 60.f;
        for (const auto& rn : runs) mx = std::max(mx, rn.seconds);
        m_b.clear();
        for (std::size_t i = 0; i < N; ++i) {
            const float bx = x + i * (bw + 4.f);
            const std::size_t off = N - runs.size();
            if (i < off) { m_b.rect(bx, hy + 18.f + hh - 2.f, bw, 2.f, tdraw::alpha(ui::CYAN_LOW, 0.5f)); continue; }
            const auto& rn = runs[i - off];
            const float h = std::max(3.f, rn.seconds / mx * hh);
            const bool last = (i == N - 1);
            m_b.rect(bx, hy + 18.f + hh - h, bw, h, last ? ui::AMBER : ui::CYAN_LOW);
            if (rn.lost) m_b.rect(bx, hy + 18.f + hh - h, bw, 3.f, ui::RED);
        }
        m_b.draw(*m_window);
    }

    void drawSector(const Rect& r) {
        const float x = 22.f, w = r.w - 44.f;
        const int cor = corruption();
        const bool danger = cor > 70;
        const std::pair<const char*, std::pair<std::string, sf::Color>> kv[4] = {
            { "PRESSURE", { danger ? "RISING" : "NOMINAL", danger ? ui::AMBER : ui::TEXT } },
            { "CONTRACT", { "OPEN", ui::GREEN } },
            { "HOSTILES", { std::to_string(m_feed.hostiles()), ui::RED } },
            { "DRIFT",    { fixed3(0.41f + std::sin(m_time * 0.3f) * 0.01f) + " AU", ui::TEXT } } };
        for (int i = 0; i < 4; ++i) {
            const float cx = x + (i % 2) * (w * 0.5f + 10.f), cy = 22.f + (i / 2) * 30.f;
            txt(kv[i].first, cx, cy, 14, ui::TEXT_DIM);
            txtR(kv[i].second.first, cx + w * 0.5f - 20.f, cy, 14, kv[i].second.second);
        }
        fill({ x, 90.f, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.7f));
        txt("VOID CORRUPTION", x, 104.f, 14, ui::VIOLET, 1.8f);
        txtR(std::to_string(cor) + "%", x + w, 96.f, 26, ui::VIOLET);
        const float t = cor / 100.f;
        segBar(x, 136.f, w, 18.f, t, 30, ui::VIOLET, ui::CYAN_LOW, [](float u) {
            return u < 0.4f ? ui::CYAN_MID : u < 0.7f ? ui::VIOLET : ui::RED; });
        fill({ x + 0.4f * w, 130.f, 1.f, 30.f }, tdraw::alpha(ui::TEXT, 0.8f));
        fill({ x + 0.7f * w, 130.f, 1.f, 30.f }, tdraw::alpha(ui::TEXT, 0.8f));
        txt("UNSTABLE", x + 0.4f * w + 4.f, 160.f, 10, ui::TEXT_DIM);
        txt("BREACH", x + 0.7f * w + 4.f, 160.f, 10, ui::TEXT_DIM);

        // The ONE element allowed to shout -- and only when it is true.
        if (danger) {
            const float sy = r.h - 50.f;
            m_b.clear();
            const float off = std::fmod(m_time * 20.f, 14.f);
            const sf::Color red = tdraw::alpha(ui::RED, 0.85f);
            for (int i = -2; i < static_cast<int>(w / 14.f) + 2; ++i) {
                const float xx = x + i * 14.f + off;
                m_b.tri({ xx, sy + 28.f }, { xx + 7.f, sy + 28.f }, { xx + 21.f, sy }, red);
                m_b.tri({ xx, sy + 28.f }, { xx + 21.f, sy }, { xx + 14.f, sy }, red);
            }
            // Trim the hatching to the strip: panel ground either side.
            m_b.rect(x - 40.f, sy - 1.f, 40.f, 30.f, ui::PANEL_BG);
            m_b.rect(x + w, sy - 1.f, 40.f, 30.f, ui::PANEL_BG);
            m_b.rect(x + 60.f, sy + 4.f, w - 120.f, 20.f, ui::VOID_BG);
            m_b.draw(*m_window);
            const float pulse = 0.65f + 0.35f * std::sin(m_time * 3.4f);
            txtC("CAUTION  -  HIGH RISK OF DEATH", x + w * 0.5f, sy + 6.f, 12, tdraw::alpha(ui::RED, pulse), 2.f);
        }
        else txt("FIELD STABLE  //  NO STANDING WARNINGS", x, r.h - 44.f, 12, ui::TEXT_DEAD);
    }

    void drawIntercept(const Rect& r) {
        const float x = 16.f, lh = 22.f;
        const int n = static_cast<int>((r.h - 60.f) / lh);
        const int start = std::max(0, static_cast<int>(m_wall.size()) - n);
        for (int i = start; i < static_cast<int>(m_wall.size()); ++i) {
            const WallLine& wl = m_wall[i];
            const float y = 22.f + (i - start) * lh;
            char ts[16]; std::snprintf(ts, sizeof ts, "%02d", (i * 7 + m_wallSerial) % 100);
            txt(ts, x, y + 1.f, 10, ui::TEXT_DEAD);
            sf::Color c = wl.signal ? ui::AMBER_HOT : tdraw::alpha(ui::TEXT_DIM, 0.85f);
            if (wl.signal && static_cast<int>(m_time * 9.f + i) % 7 == 0) c = ui::RED;
            if (i == static_cast<int>(m_wall.size()) - 1) {
                const std::size_t k = static_cast<std::size_t>(std::clamp((m_time - m_wallPushT) * 60.f, 0.f, static_cast<float>(wl.text.size())));
                txt(wl.text.substr(0, k), x + 26.f, y, 12, wl.signal ? ui::AMBER_HOT : ui::TEXT);
            }
            else txt(wl.text, x + 26.f, y, 12, c);
        }
        fill({ x, r.h - 34.f, r.w - 32.f, 1.f }, ui::CYAN_LOW);
        txt("2.5% SIGNAL  //  97.5% NOISE", x, r.h - 24.f, 10, ui::TEXT_DIM);
    }

    // ========================================================================
    // CODEX
    // ========================================================================

    /// Hostiles grouped by faction in archetype order, then the objects.
    /// Rebuilt every frame: a dozen strings, and it follows F5 for free.
    void buildCodexList() {
        m_codexList.clear();
        m_codexFactions.clear();
        if (m_reg) {
            for (const auto& f : m_reg->factions()) {
                std::string disp = upper(f.display.empty() ? f.key : f.display);
                bool any = false;
                for (const auto& a : m_reg->all()) {
                    if (a.faction != f.key) continue;
                    m_codexList.push_back({ a.key, upper(a.display.empty() ? a.key : a.display), disp, true });
                    any = true;
                }
                if (!any) m_codexFactions.push_back(disp);
            }
            for (const auto& a : m_reg->all()) {
                bool placed = false;
                for (const auto& e : m_codexList) placed |= (e.key == a.key);
                if (!placed) m_codexList.push_back({ a.key, upper(a.display.empty() ? a.key : a.display), "UNALIGNED", true });
            }
        }
        for (const auto& o : m_codexObjects) m_codexList.push_back({ o.first, o.second, "VOID OBJECTS", false });
        if (!m_codexList.empty()) m_codexSel = std::clamp(m_codexSel, 0, static_cast<int>(m_codexList.size()) - 1);
    }

    std::pair<int, int> codexCount() const {
        int seen = 0, total = 0;
        if (m_reg) for (const auto& a : m_reg->all()) { ++total; seen += (m_record && m_record->seen(a.key)) ? 1 : 0; }
        for (const auto& o : m_codexObjects) { ++total; seen += (m_record && m_record->seen(o.first)) ? 1 : 0; }
        return { seen, total };
    }

    bool codexSeen(const CodexEntry& e) const { return m_record && m_record->seen(e.key); }

    void stepCodex(int dir) {
        if (m_codexList.empty()) return;
        const int n = static_cast<int>(m_codexList.size());
        m_codexSel = ((m_codexSel + dir) % n + n) % n;
        m_selT = m_time;
    }

    void drawCodexIndex(const Rect& r) {
        const float x = 24.f, w = r.w - 48.f;
        float y = 22.f;
        std::string grp;
        for (std::size_t i = 0; i < m_codexList.size(); ++i) {
            const CodexEntry& e = m_codexList[i];
            if (e.group != grp) {
                if (!grp.empty()) y += 8.f;
                grp = e.group;
                txt(grp, x, y + 2.f, 10, ui::CYAN_MID, 2.2f);
                const float gw = tw(grp, 10, 2.2f) + 10.f;
                fill({ x + gw, y + 9.f, w - gw, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
                y += 22.f;
            }
            const bool known = codexSeen(e);
            const Rect hit{ x, y, w, 34.f };
            if (!busy() && (m_mouseLive || m_ui.leftEdge()) && m_ui.hovering(hit) && m_codexSel != static_cast<int>(i)) { m_codexSel = static_cast<int>(i); m_selT = m_time; }
            const bool on = static_cast<int>(i) == m_codexSel;
            if (on) {
                sf::ConvexShape bar(4);
                bar.setPoint(0, { x + 12.f, y }); bar.setPoint(1, { x + w + 12.f, y });
                bar.setPoint(2, { x + w, y + 34.f }); bar.setPoint(3, { x, y + 34.f });
                bar.setFillColor(known ? ui::AMBER : tdraw::alpha(ui::TEXT_DEAD, 0.7f));
                m_window->draw(bar);
            }
            char num[16]; std::snprintf(num, sizeof num, "%02d", static_cast<int>(i) + 1);
            txt(num, x + 14.f, y + 9.f, 12, on ? ui::INK : ui::TEXT_DIM);
            txt(known ? e.name : "UNCATALOGUED", x + 50.f, y + 6.f, 18, on ? ui::INK : (known ? ui::TEXT : ui::TEXT_DEAD));
            if (known) {
                const CodexText* ct = codexText(e.key);
                std::string cls = ct ? ct->cls : std::string();
                const std::size_t cut = cls.find("  //");
                if (cut != std::string::npos) cls = cls.substr(0, cut);
                txtR(cls, x + w - 12.f, y + 11.f, 10, on ? ui::INK : ui::CYAN_MID);
            }
            y += 38.f;
        }
        for (const auto& f : m_codexFactions) {
            y += 8.f;
            txt(f, x, y + 2.f, 10, ui::CYAN_MID, 2.2f);
            const float gw = tw(f, 10, 2.2f) + 10.f;
            fill({ x + gw, y + 9.f, w - gw, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
            y += 22.f;
            txt("NO CONTACT ON RECORD", x + 50.f, y + 6.f, 14, ui::TEXT_DEAD);
            y += 34.f;
        }
        const auto [seen, total] = codexCount();
        char b[48]; std::snprintf(b, sizeof b, "%d / %d CATALOGUED", seen, total);
        txt(b, x, r.h - 40.f, 12, ui::TEXT_DIM);
    }

    void drawCodexSpecimen(const Rect& r) {
        if (m_codexList.empty()) return;
        const CodexEntry& e = m_codexList[m_codexSel];
        const sf::Vector2f c{ r.w * 0.5f, r.h * 0.5f - 20.f };
        m_b.clear();
        for (int i = 1; i < 10; ++i) m_b.rect(i * r.w / 10.f, 1.f, 1.f, r.h - 2.f, tdraw::alpha(ui::CYAN_LOW, 0.25f));
        for (int i = 1; i < 12; ++i) m_b.rect(1.f, i * r.h / 12.f, r.w - 2.f, 1.f, tdraw::alpha(ui::CYAN_LOW, 0.25f));
        if (!codexSeen(e)) {
            for (int i = 0; i < 420; ++i) m_b.rect(c.x - 160.f + frand() * 320.f, c.y - 160.f + frand() * 320.f, 2.f, 2.f, tdraw::alpha(ui::TEXT_DEAD, frand()));
            m_b.draw(*m_window);
            txtC("NO RECORD", c.x, c.y + 190.f, 18, ui::TEXT_DEAD, 2.f);
            return;
        }
        const float a = -tdraw::PI * 0.5f + m_time * 0.4f;
        drawSpecimen(e, c, 150.f, a);
        const float sy = c.y - 180.f + std::fmod(m_time * 140.f, 360.f);
        m_b.rect(c.x - 220.f, sy, 440.f, 1.f, tdraw::alpha(ui::CYAN, 0.8f));
        m_b.rect(c.x - 220.f, sy - 6.f, 440.f, 6.f, tdraw::alpha(ui::CYAN, 0.08f));
        m_b.draw(*m_window);
        txt("SCALE  1 : 40", 24.f, r.h - 40.f, 12, ui::TEXT_DIM);
        txtR("SPIN  0.4 RAD/S", r.w - 24.f, r.h - 40.f, 12, ui::TEXT_DIM);
    }

    /// The specimen drawing. Hostiles are their real hulls; objects are
    /// stand-ins drawn in their in-flight colours.
    void drawSpecimen(const CodexEntry& e, sf::Vector2f c, float R, float a) {
        static const float kShape[9] = { 1.f, 0.82f, 1.08f, 0.9f, 1.f, 0.78f, 1.05f, 0.92f, 0.98f };
        if (e.hostile) {
            const enemyarch::ArchetypeDef* d = m_reg ? m_reg->byKey(e.key) : nullptr;
            if (!d) return;
            const float s = R / tdraw::archetypeRadius(*d);
            tdraw::drawArchetype(m_b, *d, tdraw::hullXf(c, a, s), tdraw::enemyFill(*d), tdraw::enemyEdge(*d), 2.4f / s * 1.6f, &m_pscratch);
            return;
        }
        const float pulse = 0.6f + 0.4f * std::sin(m_time * 4.f);
        if (e.key == "ASTEROID" || e.key == "MAGMATIC") {
            const bool mag = e.key == "MAGMATIC";
            const auto pts = tdraw::rockPoints(c, R * (mag ? 0.8f : 1.f), a * 0.5f, kShape, 9);
            m_b.fan(pts, mag ? sf::Color(60, 22, 12) : sf::Color(26, 30, 38));
            m_b.loop(pts, 3.f, mag ? ui::AMBER_HOT : ui::TEXT_DIM);
            if (mag) for (int i = 0; i < 5; ++i) {
                const float u = a * 0.5f + i * 1.3f;
                m_b.line(c, c + sf::Vector2f(std::cos(u), std::sin(u)) * (R * 0.55f), 3.f, tdraw::alpha(ui::AMBER_HOT, pulse));
            }
        }
        else if (e.key == "SALVAGE") {
            const sf::Color fillc(104, 74, 56), edge(168, 128, 98);
            for (int i = 0; i < 5; ++i) {
                const float u = a * 0.4f + i * 1.25f;
                const auto pts = tdraw::rockPoints(c + sf::Vector2f(std::cos(u), std::sin(u)) * (R * 0.42f), R * 0.48f, u, kShape, 9);
                m_b.fan(pts, fillc);
                m_b.loop(pts, 2.f, edge);
            }
        }
        else if (e.key == "WRECK") {
            const enemyarch::ArchetypeDef* d = m_reg ? m_reg->byKey("RAIDER") : nullptr;
            if (d) {
                const float s = R / tdraw::archetypeRadius(*d);
                tdraw::drawArchetype(m_b, *d, tdraw::hullXf(c, a, s), sf::Color(74, 60, 56), sf::Color(110, 100, 96, 200), 2.f, &m_pscratch);
                for (int i = 0; i < 3; ++i) {
                    const float u = a + 0.9f + i * 1.7f;
                    const sf::Vector2f p = c + sf::Vector2f(std::cos(u), std::sin(u)) * (R * 0.35f);
                    m_b.line(p - sf::Vector2f(std::cos(u + 1.f), std::sin(u + 1.f)) * 30.f, p + sf::Vector2f(std::cos(u + 1.f), std::sin(u + 1.f)) * 30.f, 8.f, ui::PANEL_BG);
                }
            }
        }
        else {
            const bool core = e.key == "UNSTABLE_CORE";
            std::vector<sf::Vector2f> pts;
            const int n = core ? 12 : 6;
            for (int i = 0; i < n; ++i) {
                const float u = a * 0.3f + tdraw::TAU * i / n;
                const float rr = R * (core ? (i % 2 ? 0.62f : 0.72f) : 0.7f);
                pts.push_back(c + sf::Vector2f(std::cos(u), std::sin(u)) * rr);
            }
            m_b.fan(pts, core ? sf::Color(85, 29, 12) : sf::Color(96, 52, 34));
            m_b.loop(pts, 3.f, ui::AMBER_HOT);
            m_b.ring(c, R * 0.3f, 4.f, tdraw::alpha(ui::AMBER_HOT, pulse));
            m_b.ring(c, R * (0.85f + 0.08f * pulse), 2.f, tdraw::alpha(ui::HAZARD, 0.5f * pulse));
        }
    }

    void drawCodexEntry(const Rect& r) {
        if (m_codexList.empty()) return;
        const CodexEntry& e = m_codexList[m_codexSel];
        const float x = 28.f, w = r.w - 56.f;
        txt(e.group, x, 26.f, 12, ui::CYAN_MID, 2.2f);
        if (!codexSeen(e)) {
            txt("UNCATALOGUED", x, 52.f, 28, ui::TEXT_DEAD, 2.f);
            txt("MEET IT IN THE FIELD TO OPEN THIS ENTRY.", x, 100.f, 12, ui::TEXT_DIM);
            return;
        }
        txt(e.name, x, 52.f, 30, e.hostile ? ui::RED : ui::TEXT, 2.f);
        const CodexText* ct = codexText(e.key);
        txt(ct ? ct->cls : std::string("NO FIELD NOTES"), x, 96.f, 12, ui::TEXT_DIM);
        fill({ x, 120.f, w, 1.f }, ui::CYAN_LOW);
        float y = 138.f;
        if (ct) {
            txt("THREAT", x, y, 12, ui::TEXT_DIM);
            segBar(x + 110.f, y + 2.f, w - 110.f, 11.f, ct->threat, 20, ui::RED, ui::CYAN_LOW);
            y += 42.f;
            txt("BEHAVIOUR", x, y, 10, ui::CYAN_MID, 2.2f); y += 22.f;
            for (const auto& l : ct->behaviour) { txt(l, x, y, 14, ui::TEXT); y += 24.f; }
            y += 14.f;
            txt("COUNTER", x, y, 10, ui::CYAN_MID, 2.2f); y += 22.f;
            txt(ct->counter, x, y, 14, ui::AMBER); y += 44.f;
        }
        else y += 20.f;
        fill({ x, y, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
        y += 18.f;
        const record::CodexStat* cs = m_record ? m_record->codex(e.key) : nullptr;
        char b[32];
        txt("DESTROYED", x, y, 12, ui::TEXT_DIM);
        txtR(groupNumber(cs ? cs->kills : 0), x + w, y, 12, ui::TEXT);
        y += 24.f;
        std::snprintf(b, sizeof b, "CONTRACT %02d", cs ? cs->firstContract : 0);
        txt("FIRST SEEN", x, y, 12, ui::TEXT_DIM);
        txtR(b, x + w, y, 12, ui::TEXT);
    }

    // ========================================================================
    // DOCTRINE
    // ========================================================================

    void stepReel(int dir) {
        const int n = static_cast<int>(doctrine::lessons().size());
        m_reel = ((m_reel + dir) % n + n) % n;
        m_stage.play(m_reel);
        m_selT = m_time;
    }

    void jumpReelGroup(bool advanced) {
        const auto& L = doctrine::lessons();
        for (int i = 0; i < static_cast<int>(L.size()); ++i)
            if (L[i].advanced == advanced) { if (m_reel != i && L[m_reel].advanced != advanced) { m_reel = i; m_stage.play(m_reel); m_selT = m_time; } return; }
    }

    void drawReelList(const Rect& r) {
        const auto& L = doctrine::lessons();
        const float x = 24.f, w = r.w - 48.f;
        float y = 24.f;
        int group = -1;
        for (int i = 0; i < static_cast<int>(L.size()); ++i) {
            const int g = L[i].advanced ? 1 : 0;
            if (g != group) {
                if (group >= 0) y += 14.f;
                group = g;
                const char* name = g ? "ADVANCED" : "BASIC";
                txt(name, x, y + 2.f, 10, g ? ui::VIOLET : ui::CYAN_MID, 2.2f);
                const float gw = tw(name, 10, 2.2f) + 10.f;
                fill({ x + gw, y + 9.f, w - gw, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
                y += 24.f;
            }
            const Rect hit{ x, y, w, 46.f };
            if (!busy() && (m_mouseLive || m_ui.leftEdge()) && m_ui.hovering(hit) && m_reel != i) { m_reel = i; m_stage.play(m_reel); m_selT = m_time; }
            const bool on = (i == m_reel);
            if (on) {
                sf::ConvexShape bar(4);
                bar.setPoint(0, { x + SKEW, y }); bar.setPoint(1, { x + w + SKEW, y });
                bar.setPoint(2, { x + w, y + 46.f }); bar.setPoint(3, { x, y + 46.f });
                bar.setFillColor(ui::AMBER);
                m_window->draw(bar);
            }
            else fill({ x, y + 46.f, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.7f));
            char num[16]; std::snprintf(num, sizeof num, "%02d", i + 1);
            txt(num, x + 16.f, y + 14.f, 14, on ? ui::INK : ui::TEXT_DIM);
            txt(L[i].name, x + 58.f, y + 10.f, 20, on ? ui::INK : ui::TEXT);
            char len[16]; std::snprintf(len, sizeof len, "0:%02d", static_cast<int>(std::ceil(L[i].length)));
            txtR(len, x + w - 10.f, y + 17.f, 10, on ? ui::INK : ui::TEXT_DIM);
            y += 54.f;
        }
        y += 20.f;
        fill({ x, y, w, 1.f }, tdraw::alpha(ui::CYAN_LOW, 0.6f));
        txt("SIMULATION", x, y + 14.f, 10, ui::CYAN_MID, 2.2f);
        txt("TRAINING SIMULATION", x, y + 38.f, 18, ui::TEXT_DEAD);
        txt("IN CONSTRUCTION  //  A PLAYABLE TUTORIAL FIELD", x, y + 64.f, 10, ui::TEXT_DIM);
        txt("W / S  REEL      A / D  GROUP      ESC  BACK", x, r.h - 40.f, 12, ui::TEXT_DIM);
    }

    void drawReel(const Rect& r) {
        const auto& L = doctrine::lessons();
        const doctrine::Lesson& les = L[m_reel];
        txt(les.name, 24.f, 20.f, 28, les.advanced ? ui::VIOLET : ui::CYAN, 2.f);
        txtR(les.advanced ? "ADVANCED" : "BASIC", r.w - 24.f, 30.f, 12, ui::TEXT_DIM, 2.f);
        const Rect v{ 24.f, 70.f, r.w - 48.f, 520.f };
        const float u = std::clamp(m_stage.t(), 0.f, les.length);

        // The scene is a real hidden world (utils/DoctrineStage) rendered at
        // 1:1 world pixels into its own texture; here it is just a picture.
        m_stage.setSize({ static_cast<unsigned>(v.w), static_cast<unsigned>(v.h) });
        m_stage.render();
        blitWorld(m_stage.texture(), v);
        m_lit = m_stage.lit();

        m_b.clear();
        bracketsB(v, tdraw::alpha(ui::CYAN, 0.8f), 18.f);
        m_b.draw(*m_window);
        char tag[64]; std::snprintf(tag, sizeof tag, "REEL %02d  //  %s", m_reel + 1, les.name);
        txt(tag, v.x + 14.f, v.y + 10.f, 12, ui::CYAN_MID);
        if (std::fmod(m_time, 1.25f) < 0.65f) {
            sf::ConvexShape play(3);
            play.setPoint(0, { v.right() - 62.f, v.y + 12.f }); play.setPoint(1, { v.right() - 52.f, v.y + 18.f });
            play.setPoint(2, { v.right() - 62.f, v.y + 24.f });
            play.setFillColor(ui::GREEN);
            m_window->draw(play);
        }
        txt("LOOP", v.right() - 44.f, v.y + 10.f, 12, ui::GREEN);
        scan(v.x, v.y, v.w, v.h, 36);

        const float sy = v.bottom() + 22.f;
        fill({ v.x, sy, v.w, 4.f }, ui::CYAN_LOW);
        fill({ v.x, sy, v.w * u / les.length, 4.f }, ui::CYAN);
        fill({ v.x + v.w * u / les.length - 2.f, sy - 5.f, 4.f, 14.f }, ui::AMBER);
        char t[48]; std::snprintf(t, sizeof t, "00:%04.1f  /  00:%04.1f", u, les.length);
        txtR(t, v.right(), sy + 14.f, 12, ui::TEXT_DIM);
        float kx = v.x;
        txt("INPUT", kx, sy + 18.f, 12, ui::TEXT_DIM);
        kx += 70.f;
        for (std::size_t i = 0; i < les.keys.size(); ++i) kx = keycap(kx, sy + 14.f, les.keys[i], "", i < m_lit.size() && m_lit[i]) + 6.f;
        txt(les.line1, v.x, sy + 62.f, 16, ui::TEXT);
        txt(les.line2, v.x, sy + 90.f, 16, ui::TEXT_DIM);
    }

    // ========================================================================
    // PAUSE / GAME OVER (centred command panel; their own pass comes later)
    // ========================================================================

    void drawPause(const Rect& r) {
        const float x = 30.f, w = r.w - 60.f;
        txt("CONTRACT SUSPENDED", x, 30.f, 22, ui::AMBER, 2.f);
        txtR(clockOf(m_runSeconds) + " IN FIELD", x + w, 36.f, 12, ui::TEXT_DIM);
        fill({ x, 66.f, w, 1.f }, ui::CYAN_LOW);
        float y = 84.f;
        for (std::size_t i = 0; i < m_pauseRows.size(); ++i) {
            rowWidget(m_pauseRows, m_pauseSel, static_cast<int>(i), x, y, w, 40.f, "", 20);
            y += 50.f;
        }
        txt("SCRAP THIS RUN", x, r.h - 44.f, 12, ui::TEXT_DIM);
        txtR(groupNumber(m_score), x + w, r.h - 44.f, 12, ui::AMBER);
    }

    void drawGameOver(const Rect& r) {
        const float x = 30.f, w = r.w - 60.f;
        txtC("HUNTER LOST", r.w * 0.5f, 34.f, 44, ui::RED, 1.6f);
        txtC("THE VOID KEEPS WHAT IT TAKES", r.w * 0.5f, 98.f, 14, ui::TEXT_DIM, 2.f);
        fill({ x, 130.f, w, 1.f }, ui::CYAN_LOW);
        txt("SCRAP SALVAGED", x, 146.f, 14, ui::TEXT_DIM);
        txtR(groupNumber(m_score), x + w, 142.f, 22, ui::AMBER);
        txt("TIME IN FIELD", x, 180.f, 14, ui::TEXT_DIM);
        txtR(clockOf(m_runSeconds), x + w, 176.f, 22, ui::TEXT);
        txt("HUNTERS LOST", x, 214.f, 14, ui::TEXT_DIM);
        txtR(std::to_string(m_record ? m_record->lost() : m_hunterLosses), x + w, 210.f, 22, ui::RED);
        float y = 268.f;
        for (std::size_t i = 0; i < m_overRows.size(); ++i) {
            rowWidget(m_overRows, m_overSel, static_cast<int>(i), x, y, w, 40.f, "", 20);
            y += 50.f;
        }
    }

    // ========================================================================
    // BOOT
    // ========================================================================

    void beginBoot() {
        m_page = Page::Boot;
        m_bootT = 0.f;
        m_slabClose = -1.f;
        m_lastSkip = -10.f;
    }

    void drawBoot() {
        const float t = m_bootT;
        const float x0 = 120.f, y0 = 110.f;
        const float dimAll = t > BOOT_SLAB_T ? 0.32f : 1.f;
        auto A = [&](sf::Color c) { return tdraw::alpha(c, dimAll); };

        // CRT power-on for the whole tube.
        m_b.clear();
        bracketsB({ 60.f, 60.f, DW - 120.f, DH - 120.f }, A(tdraw::alpha(ui::CYAN_MID, 0.5f)), 36.f);
        m_b.draw(*m_window);

        float y = y0;
        const char* head[2] = { "HUNTER-NET TERMLINK OS   V4.1.7", "(C) THE DERANT COMBINE  //  ALL RITES OBSERVED" };
        for (int i = 0; i < 2; ++i) {
            const std::string s = head[i];
            const std::size_t n = static_cast<std::size_t>(std::clamp((t - 0.35f - i * 0.25f) * 70.f, 0.f, static_cast<float>(s.size())));
            txt(s.substr(0, n), x0, y, i ? 16 : 26, A(i ? ui::TEXT_DIM : ui::CYAN));
            y += (i ? 16.f : 26.f) + 16.f;
        }
        y += 18.f;
        auto status = [&](float yy, const std::string& s, sf::Color c) {
            txt("[", x0 + 560.f, yy, 18, A(ui::TEXT_DIM));
            txt("]", x0 + 700.f, yy, 18, A(ui::TEXT_DIM));
            txtC(s, x0 + 636.f, yy, 18, A(c));
        };
        if (t > 0.85f) {
            txt("CORE MEMORY CHECK", x0, y, 18, A(ui::TEXT));
            txtR(std::to_string(static_cast<int>(std::clamp((t - 0.85f) / 0.35f, 0.f, 1.f) * 65536.f)) + "K", x0 + 520.f, y, 18, A(ui::TEXT));
            if (t > 1.2f) status(y, "OK", ui::CYAN);
        }
        y += 44.f;
        struct LogLine { const char* name; const char* st; sf::Color c; };
        const LogLine log[] = {
            { "COLD START", "OK", ui::CYAN }, { "REACTOR PRIMED", "OK", ui::CYAN },
            { "MACHINE SPIRIT", "ROUSED", ui::AMBER }, { "NAV LOCK  DERANT RIM", "OK", ui::CYAN },
            { "VOID SENSORS", "DEGRADED", ui::AMBER_HOT }, { "CONTRACT LEDGER", "SYNCED", ui::CYAN },
            { "WEAPON RITES", "OBSERVED", ui::CYAN }, { "ASTROPATH RELAY", "NO TRAFFIC", ui::TEXT_DIM },
            { "HUNTER RECORD", m_record && m_record->contracts() > 0 ? "LOADED" : "NEW FILE", ui::CYAN },
        };
        const int nLog = static_cast<int>(sizeof(log) / sizeof(log[0]));
        for (int i = 0; i < nLog; ++i) {
            const float tt = BOOT_LOG_T0 + i * BOOT_LOG_DT;
            if (t < tt) break;
            std::string lead = std::string(log[i].name) + " ";
            lead += std::string(std::max<std::size_t>(3, 34 - std::string(log[i].name).size()), '.');
            txt(lead, x0, y, 18, A(ui::TEXT));
            const bool ok = t > tt + 0.09f;
            status(y, ok ? log[i].st : ". . .", ok ? log[i].c : ui::TEXT_DIM);
            y += 32.f;
        }
        const float last = BOOT_LOG_T0 + nLog * BOOT_LOG_DT;
        const float pp = std::clamp((t - BOOT_LOG_T0) / (last + 0.4f - BOOT_LOG_T0), 0.f, 1.f);
        const float py = y0 + 140.f + 44.f + nLog * 32.f + 26.f;
        if (t > BOOT_LOG_T0) {
            txt("ROUSING MACHINE SPIRIT", x0, py, 16, A(ui::TEXT_DIM));
            txtR(std::to_string(static_cast<int>(pp * 100.f)) + "%", x0 + 772.f, py, 16, A(ui::CYAN));
            segBar(x0, py + 26.f, 772.f, 14.f, pp, 48, A(ui::CYAN), A(ui::CYAN_LOW));
        }
        if (pp >= 1.f) {
            const std::string s = "> JACK-IN AUTHORISED.  HUNTER " + callsign() + ", YOU ARE CLEARED.";
            const std::size_t n = static_cast<std::size_t>(std::clamp((t - last - 0.4f) * 60.f, 0.f, static_cast<float>(s.size())));
            txt(s.substr(0, n), x0, py + 70.f, 18, A(ui::AMBER));
            if (t < BOOT_SLAB_T && std::fmod(t, 0.66f) < 0.33f) fill({ x0 + tw(s.substr(0, n), 18, 1.4f) + 6.f, py + 72.f, 12.f, 18.f }, A(ui::CYAN));
        }
        else if (t > 0.35f && std::fmod(t, 0.66f) < 0.33f) fill({ x0, y + 4.f, 12.f, 18.f }, A(ui::CYAN));

        // Right column: the handshake scope and who you are.
        const float sx = 1020.f, sy = 180.f, sw = 760.f, sh = 300.f;
        if (t > 0.6f) {
            m_b.clear();
            bracketsB({ sx, sy, sw, sh }, A(tdraw::alpha(ui::CYAN_MID, 0.7f)), 16.f);
            for (int i = 1; i < 6; ++i) m_b.rect(sx + 1.f, sy + i * sh / 6.f, sw - 2.f, 1.f, A(tdraw::alpha(ui::CYAN_LOW, 0.35f)));
            for (int i = 1; i < 12; ++i) m_b.rect(sx + i * sw / 12.f, sy + 1.f, 1.f, sh - 2.f, A(tdraw::alpha(ui::CYAN_LOW, 0.25f)));
            const float lock = std::clamp((t - 1.f) / 1.8f, 0.f, 1.f);
            sf::Vector2f prev;
            for (int i = 0; i <= 240; ++i) {
                const float u = i / 240.f;
                const float carrier = std::sin(u * 28.f + t * 9.f) * 0.55f + std::sin(u * 61.f - t * 4.f) * 0.15f;
                const float noise = std::sin(i * 12.9898f + std::floor(t * 24.f) * 78.233f) * 0.9f;
                const sf::Vector2f p{ sx + u * sw, sy + sh * 0.5f + (noise + (carrier - noise) * lock) * sh * 0.36f };
                if (i) m_b.line(prev, p, 2.f, A(lock > 0.98f ? ui::CYAN : ui::CYAN_MID));
                prev = p;
            }
            m_b.draw(*m_window);
            txt("AUSPEX HANDSHAKE", sx + 16.f, sy - 9.f, 14, A(ui::CYAN_MID), 1.8f);
            txt(lock > 0.98f ? "CARRIER LOCKED" : "ACQUIRING CARRIER", sx + 16.f, sy + sh + 14.f, 14, A(lock > 0.98f ? ui::CYAN : ui::AMBER_HOT));
            txtR("BAND 7  //  412.07 MHZ", sx + sw - 16.f, sy + sh + 14.f, 14, A(ui::TEXT_DIM));
            if (t > 1.6f) {
                txt("HUNTER IDENTITY", sx, 560.f, 14, A(ui::CYAN_MID), 1.8f);
                fill({ sx, 582.f, sw, 1.f }, A(ui::CYAN_LOW));
                const int contracts = m_record ? m_record->contracts() : 0, lost = m_record ? m_record->lost() : 0;
                std::string frame = "STOCK";
                if (m_design) frame = std::string(m_design->spec().name) + "  //  " + m_design->spec().pattern;
                const std::pair<std::string, std::string> id[4] = {
                    { "CALLSIGN", callsign() }, { "FRAME", frame },
                    { "CONTRACTS", std::to_string(contracts) + " TAKEN  /  " + std::to_string(lost) + " LOST" },
                    { "STANDING", standing() } };
                for (int i = 0; i < 4; ++i) {
                    if (t < 1.7f + i * 0.12f) break;
                    txt(id[i].first, sx, 600.f + i * 30.f, 16, A(ui::TEXT_DIM));
                    txtR(id[i].second, sx + sw, 600.f + i * 30.f, 16, A(ui::TEXT));
                }
            }
        }
        if (t < BOOT_SLAB_T + 0.6f) txtR("ANY KEY TO SKIP", DW - 120.f, DH - 110.f, 14, ui::TEXT_DEAD);

        // The greeting: opens on the centre line, folds back the same way.
        if (t > BOOT_SLAB_T) {
            float sp = std::clamp((t - BOOT_SLAB_T) / 0.42f, 0.f, 1.f);
            if (m_slabClose >= 0.f) sp = 1.f - std::clamp(m_slabClose / CLOSE_DUR, 0.f, 1.f);
            fill({ 0.f, 0.f, DW, DH }, tdraw::alpha(ui::VOID_BG, 0.55f * std::clamp(sp * 2.f, 0.f, 1.f)));
            Panel slab{ { DW * 0.5f - 430.f, DH * 0.5f - 150.f, 860.f, 300.f }, "TERMLINK ESTABLISHED", "",
                Chrome::Heavy, ui::CYAN, 0.f, &MenuSystem::drawGreeting };
            const float saveOpen = m_openT;
            m_openT = t - BOOT_SLAB_T;   // drives the repaint flicker in drawPanel
            drawPanel(slab, sp);
            m_openT = saveOpen;
        }
        scan(0.f, 0.f, DW, DH, 26);
    }

    void drawGreeting(const Rect& r) {
        txtC("GREETINGS, HUNTER", r.w * 0.5f, 54.f, 56, ui::CYAN, 1.5f);
        fill({ 120.f, 140.f, r.w - 240.f, 1.f }, ui::CYAN_LOW);
        const int lost = m_record ? m_record->lost() : m_hunterLosses;
        const std::string sub = (lost > 0)
            ? "YOU HAVE RETURNED " + std::to_string(lost) + (lost == 1 ? " TIME." : " TIMES.") + "  THE VOID IS WAITING."
            : "THE VOID IS WAITING.";
        txtC(sub, r.w * 0.5f, 160.f, 18, ui::TEXT);
        if (std::fmod(m_time, 0.9f) < 0.45f) txtC("PRESS ANY KEY TO JACK IN", r.w * 0.5f, 226.f, 18, ui::AMBER, 2.f);
    }

    // ========================================================================
    // CODEX TEXT (scripts/codex.lua)
    // ========================================================================

    void loadCodexText() {
        m_codexText.clear();
        m_codexObjects.clear();
        if (!m_lua) return;
        sol::object entries = (*m_lua)["codex_entries"];
        if (entries.valid() && entries.is<sol::table>()) {
            for (auto& kv : entries.as<sol::table>()) {
                if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
                sol::table t = kv.second.as<sol::table>();
                CodexText ct;
                ct.cls = t["class"].get_or<std::string>("");
                ct.counter = t["counter"].get_or<std::string>("");
                ct.threat = std::clamp(t["threat"].get_or(0.f), 0.f, 1.f);
                sol::object b = t["behaviour"];
                if (b.valid() && b.is<sol::table>()) {
                    sol::table bt = b.as<sol::table>();
                    for (std::size_t i = 1; i <= bt.size(); ++i) ct.behaviour.push_back(bt[i].get_or<std::string>(""));
                }
                ct.valid = true;
                m_codexText[kv.first.as<std::string>()] = ct;
            }
        }
        sol::object objs = (*m_lua)["codex_objects"];
        if (objs.valid() && objs.is<sol::table>()) {
            sol::table ot = objs.as<sol::table>();
            for (std::size_t i = 1; i <= ot.size(); ++i) {
                sol::object o = ot[i];
                if (!o.valid() || !o.is<sol::table>()) continue;
                sol::table e = o.as<sol::table>();
                const std::string key = e["key"].get_or<std::string>("");
                if (!key.empty()) m_codexObjects.push_back({ key, e["name"].get_or<std::string>(key) });
            }
        }
        if (m_codexObjects.empty())
            m_codexObjects = { { "ASTEROID", "ASTEROID" }, { "MAGMATIC", "MAGMA ROCK" }, { "SALVAGE", "SALVAGE HEAP" },
                               { "WRECK", "WRECK" }, { "REACTOR", "REACTOR" }, { "UNSTABLE_CORE", "UNSTABLE CORE" } };
    }

    const CodexText* codexText(const std::string& key) const {
        auto it = m_codexText.find(key);
        return it == m_codexText.end() ? nullptr : &it->second;
    }

    // ========================================================================
    // INTERCEPT STREAM
    //
    // Three rules, and breaking any one turns this into a screensaver:
    //   1. Contrast far lower than feels right. It is TEXTURE.
    //   2. Authored pool, never random characters.
    //   3. Roughly 1 line in 40 says something. That is the entire trick.
    // ========================================================================

    void tickWall(float dt) {
        m_wallTimer -= dt;
        if (m_burstTimer > 0.f) m_burstTimer -= dt;
        else if (frand() < dt * 0.35f) m_burstTimer = 0.3f + frand() * 0.5f;
        const float interval = (m_burstTimer > 0.f) ? 0.09f : (0.45f + frand() * 0.5f);
        while (m_wallTimer <= 0.f) { m_wallTimer += interval; pushWallLine(); }
    }

    void pushWallLine() {
        const bool signal = (frand() < 0.025f);
        m_wall.push_back({ signal ? pickSignal() : pickNoise(), signal });
        if (m_wall.size() > 90) m_wall.erase(m_wall.begin());
        ++m_wallSerial;
        m_wallPushT = m_time;
    }

    std::string pickNoise() {
        static const char* kNoise[] = {
            "SENSOR SWEEP  ARC #  CLEAN", "HULL POLL  SEGMENT #  NOMINAL", "COOLANT LOOP #  IN TOLERANCE",
            "TRANSPONDER PING  ID #  NO REPLY", "MASS SHADOW  BEARING #", "REACTOR OUTPUT  # PCT  STABLE",
            "DEBRIS FIELD  # OBJECTS", "VOID TIDE  DELTA #", "LEDGER ENTRY #  ARCHIVED", "MUNITION RACK #  VERIFIED",
            "GYRO CAL  AXIS #  PASS", "THERMAL VENT #  CYCLING", "AUGUR BAND #  STATIC", "SALVAGE BEACON #  DORMANT",
            "RITE OF ACTIVATION  UNIT #", "NAV SPUR #  RECOMPUTED", "FUEL MASS  # UNITS", "PLATE #  MICROFRACTURE MINOR",
            "SIGNAL DISCIPLINE HELD", "MANIFEST  # ENTRIES SEALED", "AIRLOCK #  SEALED", "GUNNERY SERVO #  RANGE OK",
            "STAR FIX  ERROR #", "POWER BUS #  DRAW NOMINAL", "WASTE HEAT DUMPED  # KJ", "AUSPEX RETURN  ROCK",
            "ARCHIVE QUERY #  NO MATCH", "PRESSURE SEAL #  HOLDING", "DAMPER #  ENGAGED", "SHIELD LATTICE #  IDLE",
            "PROXIMITY CLOCK RESET", "COGITATOR IDLE  # CYCLES", "HARMONIC #  IN PHASE", "LITANY #  RECITED",
            "GHOST DISCARDED  BEARING #", "PORT #  CAPPED", "GRAV PLATE #  # W", "HULL TEMP  # K  FALLING",
            "ORDNANCE SAFETY ENGAGED", "SOLUTION #  CACHED", "COMMS BAND #  EMPTY", "MACHINE SPIRIT QUIESCENT",
            "OXYGEN RESERVE  # PCT", "DRIVE PLASMA CONTAINED", "ASTROPATH RELAY  NO TRAFFIC", "SCRAP ASSAY #  LOW YIELD",
            "DIAGNOSTIC BLOCK #", "AUGUR REALIGNED  STARBOARD", "CHRONOMETER DRIFT  # MS", "LOG ROTATED  FILE #",
        };
        return substitute(kNoise[m_rngNext() % 50]);
    }

    std::string pickSignal() {
        static const char* kSignal[] = {
            "HUNTER #  BEACON SILENT # HRS", "PARTIAL  ...IT IS NOT A STORM...", "RETURN MATCHES NO KNOWN HULL",
            "DISTRESS  ...DO NOT ANSWER IT...", "SECTOR # POPULATION NOW ZERO", "OBJECT # MOVES AGAINST THE TIDE",
            "VOICE MATCHES DECEASED  ENTRY #", "SOMETHING ANSWERED THE PING", "# CATALOGUE ENTRIES NOW ABSENT",
            "HULL FOUND  INTERIOR INVERTED", "LOOP  ...WE ARE STILL ABOARD...", "MASS EXCEEDS VOLUME BY #",
            "CONTRACT #  CLAIMANT NEVER BACK", "AUGUR POINTS AT NOTHING AGAIN",
        };
        return substitute(kSignal[m_rngNext() % 14]);
    }

    std::string substitute(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            if (c == '#') out += std::to_string(m_rngNext() % 9000 + 100);
            else out += c;
        }
        return out;
    }

    // ========================================================================
    // DRAWING HELPERS
    // ========================================================================

    void fill(const Rect& r, sf::Color c) {
        sf::RectangleShape s({ r.w, r.h });
        s.setPosition({ r.x, r.y });
        s.setFillColor(c);
        m_window->draw(s);
    }

    void bracketsB(const Rect& d, sf::Color c, float L) {
        m_b.rect(d.x, d.y, L, 1.f, c); m_b.rect(d.x, d.y, 1.f, L, c);
        m_b.rect(d.right() - L, d.y, L, 1.f, c); m_b.rect(d.right() - 1.f, d.y, 1.f, L, c);
        m_b.rect(d.x, d.bottom() - 1.f, L, 1.f, c); m_b.rect(d.x, d.bottom() - L, 1.f, L, c);
        m_b.rect(d.right() - L, d.bottom() - 1.f, L, 1.f, c); m_b.rect(d.right() - 1.f, d.bottom() - L, 1.f, L, c);
    }

    template <class F = std::nullptr_t>
    void segBar(float x, float y, float w, float h, float t, int cells, sf::Color on, sf::Color off, F colorFn = nullptr) {
        tdraw::Batch b;
        const float gap = 2.f, cw = (w - gap * (cells - 1)) / cells;
        for (int i = 0; i < cells; ++i) {
            const bool lit = (i + 0.5f) / cells <= t;
            sf::Color c = off;
            if (lit) {
                if constexpr (std::is_same_v<F, std::nullptr_t>) c = on;
                else c = colorFn(static_cast<float>(i) / cells);
            }
            else c = tdraw::alpha(off, 0.55f);
            b.rect(x + i * (cw + gap), y, cw, h, c);
        }
        b.draw(*m_window);
    }

    void scan(float x, float y, float w, float h, std::uint8_t a) {
        tdraw::Batch b;
        for (float yy = y; yy < y + h; yy += 3.f) b.rect(x, yy, w, 1.f, sf::Color(0, 0, 0, a));
        b.draw(*m_window);
    }

    /// Whole-tube squeeze: v=1 full picture, v=0 off. Height first, then
    /// width, leaving a dot -- the reverse of the boot's power-on.
    void tube(float v, sf::Vector2f S) {
        if (v >= 1.f) return;
        const float hf = std::clamp((v - 0.3f) / 0.7f, 0.f, 1.f);
        const float wf = std::clamp(v / 0.3f, 0.004f, 1.f);
        const float h = std::max(2.f, S.y * hf), w = S.x * wf;
        const float x = (S.x - w) * 0.5f, y = (S.y - h) * 0.5f;
        tdraw::Batch b;
        const sf::Color k(0, 0, 0);
        b.rect(0.f, 0.f, S.x, y, k); b.rect(0.f, y + h, S.x, S.y - y - h, k);
        b.rect(0.f, y, x, h, k); b.rect(x + w, y, S.x - x - w, h, k);
        if (hf < 0.98f) b.rect(x, S.y * 0.5f - 1.f, w, 3.f, tdraw::alpha(ui::TEXT, 0.9f * (1.f - hf)));
        if (v < 0.3f && v > 0.f) b.rect(S.x * 0.5f - 5.f, S.y * 0.5f - 3.f, 10.f, 6.f, tdraw::alpha(sf::Color::White, 1.f - v / 0.3f * 0.4f));
        b.draw(*m_window);
    }

    float keycap(float x, float y, const std::string& k, const std::string& label, bool lit = false) {
        const float w = tw(k, 14, 1.4f) + 14.f;
        if (lit) fill({ x, y, w, 22.f }, ui::AMBER);
        tdraw::Batch b;
        const sf::Color c = tdraw::alpha(ui::CYAN_MID, 0.8f);
        b.rect(x, y, w, 1.f, c); b.rect(x, y + 21.f, w, 1.f, c); b.rect(x, y, 1.f, 22.f, c); b.rect(x + w, y, 1.f, 22.f, c);
        if (!lit) b.rect(x + 1.f, y + 18.f, w - 1.f, 3.f, tdraw::alpha(ui::CYAN_LOW, 0.7f));
        b.draw(*m_window);
        txt(k, x + 7.f, y + 2.f, 14, lit ? ui::INK : ui::CYAN);
        if (label.empty()) return x + w + 10.f;
        txt(label, x + w + 10.f, y + 2.f, 14, ui::TEXT_DIM);
        return x + w + 10.f + tw(label, 14, 1.4f) + 26.f;
    }

    void txt(const std::string& s, float x, float y, unsigned size, sf::Color c, float spacing = 1.4f) {
        if (s.empty()) return;
        sf::Text t(*m_font, s, size);
        t.setLetterSpacing(spacing);
        t.setFillColor(c);
        t.setPosition({ std::round(x), std::round(y) });
        m_window->draw(t);
    }
    void txtR(const std::string& s, float rx, float y, unsigned size, sf::Color c, float spacing = 1.4f) {
        txt(s, rx - tw(s, size, spacing), y, size, c, spacing);
    }
    void txtC(const std::string& s, float cx, float y, unsigned size, sf::Color c, float spacing = 1.4f) {
        txt(s, cx - tw(s, size, spacing) * 0.5f, y, size, c, spacing);
    }
    float tw(const std::string& s, unsigned size, float spacing) const {
        if (!m_font || s.empty()) return 0.f;
        sf::Text t(*m_font, s, size);
        t.setLetterSpacing(spacing);
        const sf::FloatRect b = t.getLocalBounds();
        return b.position.x + b.size.x;
    }

    // ========================================================================
    // SMALL THINGS
    // ========================================================================

    /// Corruption climbs with your recent failures, so the line that sounds
    /// most like set dressing is reporting on you.
    int corruption() const {
        const int recent = m_record ? m_record->recentLosses() : m_hunterLosses;
        return std::clamp(38 + recent * 6, 0, 99);
    }

    std::string shipLabel() const {
        if (!m_shipName.empty()) return upper(m_shipName);
        return m_design ? std::string(m_design->spec().pattern) : std::string("UNNAMED HULL");
    }
    std::string callsign() const { return m_shipName.empty() ? std::string("07-DR") : upper(m_shipName); }

    std::string standing() const {
        if (!m_record || m_record->contracts() < 3) return "UNPROVEN";
        const float lr = static_cast<float>(m_record->lost()) / std::max(1, m_record->contracts());
        if (lr > 0.75f) return "EXPENDABLE";
        if (lr > 0.4f) return "TOLERATED";
        return "TRUSTED";
    }

    static std::string upper(std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }

    static std::string groupNumber(long long v) {
        std::string s = std::to_string(v);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, " ");
        return s;
    }

    static std::string clockOf(float seconds) {
        const int s = static_cast<int>(seconds);
        char b[16]; std::snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
        return b;
    }

    static std::string fixed3(float v) { char b[16]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

    std::string timecode() const {
        const int total = static_cast<int>(m_time * 100.f);
        char b[32]; std::snprintf(b, sizeof b, "T+00:%02d:%02d:%02d", (total / 6000) % 60, (total / 100) % 60, total % 100);
        return b;
    }

    std::uint32_t m_rng = 0x9E3779B9u;
    std::uint32_t m_rngNext() { m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5; return m_rng; }
    float frand() { return static_cast<float>(m_rngNext() % 100000) / 100000.f; }

    // ========================================================================
    // STATE
    // ========================================================================

    sf::RenderWindow* m_window = nullptr;
    sf::Font* m_font = nullptr;
    sf::Font* m_mono = nullptr;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_reg = nullptr;
    const record::HunterRecord* m_record = nullptr;
    const ship::ShipDesign* m_design = nullptr;
    const ship::Livery* m_livery = nullptr;
    std::string m_shipName;
    std::vector<sf::Vector2f> m_meshSource;
    tdraw::HullMesh m_mesh;
    bool m_seeded = false;

    tui::UI m_ui;
    MenuAction m_clickAction = MenuAction::None;
    GameState m_lastState = static_cast<GameState>(-1);
    Page m_page = Page::None;
    bool m_returnToCodex = false;

    // transitions
    float m_openT = 0.f;
    bool  m_closing = false;
    float m_closeT = 0.f;
    Next  m_next;
    float m_tubeOff = -1.f;
    float m_tubeOn = -1.f;
    std::string m_pathFrom;
    float m_pathT = 0.f;

    // selection
    int m_mainSel = 0, m_pauseSel = 0, m_overSel = 0, m_codexSel = 0, m_reel = 0;
    float m_selT = 0.f, m_denyT = -10.f;
    sf::Vector2f m_mouseAnchor;
    bool m_mouseArm = true, m_mouseLive = false;
    std::vector<bool> m_lit;

    // boot
    float m_bootT = 0.f, m_slabClose = -1.f, m_lastSkip = -10.f;

    // data
    int m_score = 0;
    int m_hunterLosses = 0;
    float m_runSeconds = 0.f;
    double m_casualties = 4182377.0;

    // codex
    std::map<std::string, CodexText> m_codexText;
    std::vector<std::pair<std::string, std::string>> m_codexObjects;
    std::vector<CodexEntry> m_codexList;
    std::vector<std::string> m_codexFactions;

    // live panels
    const zonearch::ZoneState* m_zoneState = nullptr;
    LiveFeed m_feed;                  ///< the tactical feed: a real hidden fight
    doctrine::Stage m_stage;          ///< field doctrine: real looping scenes

    // intercept
    std::vector<WallLine> m_wall;
    float m_wallTimer = 0.f, m_burstTimer = 0.f, m_wallPushT = 0.f;
    int m_wallSerial = 0;

    float m_time = 0.f;

    tdraw::Batch m_b;
    std::vector<sf::Vertex> m_vscratch;
    std::vector<sf::Vector2f> m_pscratch;
};
