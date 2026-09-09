/**
 * @file MenuSystem.hpp
 * @brief Terminal-style front end: main menu, pause, game over, tutorial.
 *
 * Purely screen-space UI. Owns its own selection state and a small list of
 * rows per GameState. Does NOT own the GameState itself -- that lives in
 * SystemManager, which calls setState() so this system knows which screen to
 * draw, and reads back confirmSelection()'s result to actually transition.
 *
 * DESIGN CONTRACT (read before editing the layout):
 *
 *   - This screen is a MACHINE, not a canopy. Everything is orthogonal: hard
 *     edges, flat fills, hairline strokes, no gradients, no rounded corners.
 *     The single skewed element is the selection bar -- the one thread
 *     connecting the terminal to the in-flight HUD. Do not skew anything
 *     else; skewed text on a terminal reads as a rendering fault.
 *
 *   - PANELS ARE WINDOWS, NOT RECTANGLES. Every panel with moving content
 *     renders through beginClip()/endClip(), which installs an sf::View with
 *     a viewport confined to the panel. Content is drawn in the panel's OWN
 *     coordinate space (0,0 = panel top-left) and is hard-clipped at the
 *     edges by the GPU. Never hand-position content into a panel rect and
 *     hope it stays inside -- that is how stars ended up outside the frame.
 *
 *   - Panels are deliberately NOT on a grid. Edges don't align, sizes don't
 *     match, border weights differ. This is a cheap, hard-working device
 *     bolted together by people with other priorities, not a designed
 *     product. If the layout ever starts to look tidy, break it again.
 *
 *   - Greys are DARK and accents are BRIGHT. Contrast comes from the gap
 *     between near-black structure and hot cyan/amber values, not from
 *     mid-grey text. Dark future, not hologram.
 *
 * @author Oleg Ivakhiv
 * @version 3.0 (clipped panels, CRT reveal, contrast pass)
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/TerminalUI.hpp"
#include "utils/UiPalette.hpp"       
#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

class MenuSystem : public ISystem {
public:
    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    void init(const SystemContext& ctx) override {
        m_window = ctx.window;
        m_ui.attach(ctx.window);
        seedStringWall();
        seedFeedStars();
    }

    void setFont(sf::Font* font) { m_font = font; m_ui.setFonts(m_font, m_mono); }

    /**
     * @brief Optional monospace face for the terminal layers.
     *
     * The datastream, boot log and readouts all want a fixed-width face to
     * read as machine output. Falls back to the main font if never set --
     * still works, just less convincingly. Drop any mono TTF into assets/
     * and call this; no other code changes needed.
     */
    void setMonoFont(sf::Font* font) { m_mono = font; m_ui.setFonts(m_font, m_mono); }

    void update(float dt) override {
        if (!m_window) return;
        m_ui.begin(dt);          // live screen view + mouse sample
        tick(dt);
        draw();
    }

    /**
     * @brief Take the action produced by a MOUSE click this frame, if any.
     *
     * Keyboard confirms still go through confirmSelection(); this is the
     * parallel path for clicking a row or an on-screen button. game.cpp polls
     * it each frame and hands the result to requestAction(). Returns None when
     * nothing was clicked, and clears itself so an action can only fire once.
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
        if (state == m_lastState) return;

        const GameState prev = m_lastState;
        m_lastState = state;
        m_selected = 0;
        m_score = score;
        m_scrollOffset = 0;
        m_tutPage = 0;
        m_clickAction = MenuAction::None;

        // Re-prime every edge detector. The Enter or click that opened this
        // screen is still held on our first frame here; unprimed, it reads as
        // a fresh press and fires whatever sits under the cursor.
        m_ui.primeInput();

        m_rows.clear();

        switch (state) {
        case GameState::MainMenu:
            m_title = "VOID HUNTER";
            m_rows = {
                { "ENGAGE THE HUNT",  "Drop into a generated sector.\nThe Void is already moving.", MenuAction::StartGame,    false },
                { "FIELD DOCTRINE",   "Flight, gunnery and survival\nprotocols.",                   MenuAction::ShowTutorial, false },
                { "REFIT BAY",        "Reshape the hull. Mount guns and drives.",                   MenuAction::ShowRefit,    false },
                { "MACHINE RITES",    "Configuration subsystem\noffline.",                          MenuAction::None,         true  },
                { "LEAVE THE SYSTEM", "Disengage. The Void does\nnot follow.",                      MenuAction::QuitGame,     false },
            };
            // Boot only on a COLD entry. Coming back from a dead run should
            // snap in -- the machine is already awake, and a terminal that
            // doesn't re-greet you after you die is quietly grim in exactly
            // the right way.
            if (prev == static_cast<GameState>(-1)) beginBoot();
            else                                    beginPanelOpen();
            break;

        case GameState::Paused:
            m_title = "HOLD";
            m_rows = {
                { "RESUME CONTRACT",    "Return to the hunt.",                     MenuAction::ResumeGame,   false },
                { "FIELD DOCTRINE",     "Flight, gunnery and survival\nprotocols.", MenuAction::ShowTutorial, false },
                { "PURGE AND RESTART",  "Abandon this sector.\nProgress is lost.",  MenuAction::RestartGame,  false },
                { "ABANDON TO TERMINAL","Leave the run. Back to the\nmain terminal.", MenuAction::BackToMenu, false },
                { "LEAVE THE SYSTEM",   "Disengage.",                              MenuAction::QuitGame,     false },
            };
            beginPanelOpen();
            break;

        case GameState::GameOver:
            m_title = "HUNTER LOST";
            m_rows = {
                { "RE-ENGAGE",          "A new sector.\nThe same Void.", MenuAction::RestartGame, false },
                { "RETURN TO TERMINAL", "Back to the main terminal.",    MenuAction::BackToMenu,  false },
                { "LEAVE THE SYSTEM",   "Disengage.",                    MenuAction::QuitGame,    false },
            };
            beginPanelOpen();
            break;

        case GameState::Tutorial:
            m_title = "FIELD DOCTRINE";
            m_rows = { { "RETURN TO TERMINAL", "", MenuAction::BackToMenu, false } };
            beginPanelOpen();
            break;

        default:
            break;
        }
    }

    /// Move selection, skipping locked rows.
    void moveSelection(int dir) {
        if (m_rows.empty() || dir == 0) return;
        const int n = static_cast<int>(m_rows.size());
        for (int step = 0; step < n; ++step) {
            m_selected = ((m_selected + dir) % n + n) % n;
            if (!m_rows[m_selected].locked) { m_selectAnim = 1.f; return; }
        }
    }

    MenuAction confirmSelection() const {
        if (m_rows.empty()) return MenuAction::None;
        const Row& r = m_rows[m_selected];
        return r.locked ? MenuAction::None : r.action;
    }

    void scrollTutorial(int direction) {
        if (m_lastState != GameState::Tutorial) return;
        m_scrollOffset = std::clamp(m_scrollOffset - direction * 30, 0, getMaxScrollOffset());
    }

    // ========================================================================
    // BOOT
    // ========================================================================

    bool isBooting() const { return m_bootActive; }

    /// Any keypress must land here. Non-negotiable: never trap the player.
    void skipBoot() {
        if (!m_bootActive) return;
        m_bootActive = false;
        m_bootTimer = BOOT_TOTAL;
        beginPanelOpen();
    }

    // ========================================================================
    // EXTERNAL DATA
    // ========================================================================

    void setHunterLosses(int losses) { m_hunterLosses = std::max(0, losses); }
    void setFeedContacts(int contacts) { m_feedContacts = std::max(0, contacts); }

private:
    // ========================================================================
    // TYPES
    // ========================================================================

    struct Row {
        std::string label;
        std::string desc;
        MenuAction  action = MenuAction::None;
        bool        locked = false;
    };

    struct WallLine {
        std::string text;
        bool        signal = false;
    };

    struct FeedStar {
        sf::Vector2f pos;       ///< PANEL-LOCAL pixels, not normalised
        float        speed = 0.f;
        float        size = 1.f;
        std::uint8_t bright = 120;
    };

    struct Rect {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
    };

    /// Panel chrome variants. Deliberately inconsistent -- see design contract.
    enum class Chrome {
        Brackets,   ///< Corner marks only. Lightest.
        Hairline,   ///< Full thin border.
        Heavy       ///< Thicker border, filled title tab.
    };

    struct TutorialSection {
        std::string title;
        std::string body;
        sf::Color   color;
        int         column;
    };

    // ========================================================================
    // PALETTE — aliases to the shared ui:: palette
    // ========================================================================
    // All colour values are now defined centrally in ui/UiPalette.hpp.
    // These aliases keep the existing references working without any visual
    // change, and ensure the menu and HUD stay in sync.
    static inline const sf::Color VOID_BG = ui::VOID_BG;
    static inline const sf::Color PANEL_BG = ui::PANEL_BG;
    static inline const sf::Color CYAN = ui::CYAN;
    static inline const sf::Color CYAN_MID = ui::CYAN_MID;
    static inline const sf::Color CYAN_LOW = ui::CYAN_LOW;
    static inline const sf::Color AMBER = ui::AMBER;
    static inline const sf::Color RED = ui::RED;
    static inline const sf::Color TEXT = ui::TEXT;
    static inline const sf::Color TEXT_DIM = ui::TEXT_DIM;
    static inline const sf::Color TEXT_DEAD = ui::TEXT_DEAD;

    static constexpr float BOOT_TOTAL = 4.6f;   ///< Cold start only, always skippable.
    static constexpr float OPEN_DUR = 0.34f;  ///< Per-panel CRT reveal length.
    static constexpr float SKEW = 14.f;   ///< px. The ONE skewed element.

    // ========================================================================
    // PER-FRAME SIMULATION
    // ========================================================================

    void tick(float dt) {
        dt = std::clamp(dt, 0.f, 0.1f);   // guard against debugger-pause spikes
        m_time += dt;

        if (m_bootActive) {
            m_bootTimer += dt;
            if (m_bootTimer >= BOOT_TOTAL) { m_bootActive = false; beginPanelOpen(); }
        }

        if (!m_bootActive && m_openTimer >= 0.f && m_openTimer < 3.f) m_openTimer += dt;

        m_selectAnim = std::max(0.f, m_selectAnim - dt * 7.f);

        tickStringWall(dt);
        tickFeed(dt);

        // Jittered so it doesn't read as a clean timer -- machines that count
        // corpses shouldn't tick evenly.
        m_casualties += dt * (2.6f + frand() * 3.4f);
    }

    // ---- String wall ------------------------------------------------------
    //
    // Three rules, and breaking any one turns this into a screensaver:
    //   1. Contrast far lower than feels right. It is TEXTURE.
    //   2. Authored pool, never random characters.
    //   3. Roughly 1 line in 40 says something. That is the entire trick.
    void tickStringWall(float dt) {
        m_wallTimer -= dt;

        if (m_burstTimer > 0.f)            m_burstTimer -= dt;
        else if (frand() < dt * 0.35f)     m_burstTimer = 0.3f + frand() * 0.5f;

        const float interval = (m_burstTimer > 0.f) ? 0.028f : (0.11f + frand() * 0.18f);
        while (m_wallTimer <= 0.f) {
            m_wallTimer += interval;
            pushWallLine();
        }
    }

    void pushWallLine() {
        const bool signal = (frand() < 0.025f);
        m_wall.push_back({ signal ? pickSignal() : pickNoise(), signal });
        if (m_wall.size() > 90) m_wall.erase(m_wall.begin());
    }

    // ---- Feed -------------------------------------------------------------
    //
    // Stars live in PANEL-LOCAL pixel space and are drawn inside the clip, so
    // they physically cannot escape the frame. Respawn is deliberately placed
    // past the right edge -- the viewport eats the overhang.
    void tickFeed(float dt) {
        const Rect r = feedRect();
        for (auto& s : m_feedStars) {
            s.pos.x -= s.speed * dt;
            if (s.pos.x < -6.f) {
                s.pos.x = r.w + 6.f;
                s.pos.y = frand() * r.h;
            }
            if (s.pos.y > r.h) s.pos.y = frand() * r.h;
        }
    }

    // ========================================================================
    // CLIPPING
    //
    // This is the mechanism that makes a panel an actual window. setViewport
    // takes NORMALISED coordinates relative to the render target, so it is
    // computed against window pixel size, not view size. Inside the clip the
    // origin is the panel's top-left corner.
    // ========================================================================

    /**
     * @brief A screen-space view matching the CURRENT framebuffer.
     *
     * getDefaultView() is frozen at window-creation size and never updates on
     * resize. Any layout or mouse mapping built on it silently drifts the
     * moment the window changes size -- text floats and clicks land nowhere.
     * Rebuild every frame; it is a few floats.
     */
    sf::View uiView() const {
        const sf::Vector2u s = m_window->getSize();
        return sf::View(sf::FloatRect({ 0.f, 0.f },
            { static_cast<float>(s.x), static_cast<float>(s.y) }));
    }

    /**
     * @brief Clip to a panel, DELEGATING to the shared UI.
     *
     * The delegation is the whole point. This used to install the viewport
     * itself, which meant tui::UI never learned the clip origin -- so any
     * widget drawn inside a panel hit-tested its PANEL-LOCAL rect against a
     * SCREEN-space cursor. The menu rows sit at local x 14..w-14, so they only
     * responded when the mouse was near the top-left corner of the window,
     * nowhere near the panel they were drawn in.
     *
     * Route every clip through tui::UI and screen widgets and clipped widgets
     * behave identically.
     */
    void beginClip(const Rect& r) { m_ui.beginClip({ r.x, r.y, r.w, r.h }); }

    void endClip() { m_ui.endClip(); }

    // ========================================================================
    // CRT REVEAL
    //
    // Old CRT switch-on: a bright horizontal line snaps across the middle,
    // then the picture opens outward from it. Cheap to fake -- expand width
    // fast, then height, and hold a hot line at the centre while the height
    // is still growing.
    // ========================================================================

    void beginBoot() { m_bootActive = true; m_bootTimer = 0.f; m_openTimer = -1.f; }
    void beginPanelOpen() { m_openTimer = 0.f; }

    float revealOf(float delay) const {
        if (m_bootActive) return 0.f;
        const float t = m_openTimer - delay;
        if (t <= 0.f) return 0.f;
        return std::clamp(t / OPEN_DUR, 0.f, 1.f);
    }

    /// Geometry of a panel mid-reveal. Width opens first, then height.
    Rect revealRect(const Rect& r, float p) const {
        const float wf = std::clamp(p / 0.34f, 0.06f, 1.f);
        const float hf = std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
        const float w = r.w * wf;
        const float h = std::max(2.f, r.h * hf);
        return { r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h };
    }

    // ========================================================================
    // LAYOUT
    //
    // Fractions of the view, chosen so no two panels share an edge. Resist
    // the urge to round these to tidy numbers.
    // ========================================================================

    sf::Vector2f viewSize() const {
        const sf::Vector2u s = m_window->getSize();
        return { static_cast<float>(s.x), static_cast<float>(s.y) };
    }

    Rect frac(float x, float y, float w, float h) const {
        const sf::Vector2f s = viewSize();
        return { x * s.x, y * s.y, w * s.x, h * s.y };
    }

    Rect feedRect()    const { return frac(0.020f, 0.112f, 0.470f, 0.500f); }
    Rect menuRect()    const { return frac(0.520f, 0.150f, 0.285f, 0.520f); }
    Rect descRect()    const { return frac(0.520f, 0.690f, 0.285f, 0.110f); }
    Rect streamRect()  const { return frac(0.820f, 0.118f, 0.162f, 0.800f); }
    Rect ledgerRect()  const { return frac(0.020f, 0.640f, 0.245f, 0.285f); }
    Rect sectorRect()  const { return frac(0.278f, 0.640f, 0.212f, 0.175f); }
    Rect corruptRect() const { return frac(0.278f, 0.838f, 0.212f, 0.087f); }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw() {
        // Own the view explicitly. SystemManager sets the default view before
        // calling us in Paused but NOT in MainMenu, where the last view set
        // was whatever gameplay left behind.
        m_window->setView(uiView());

        const sf::Vector2f size = viewSize();

        sf::RectangleShape bg(size);
        bg.setPosition({ 0.f, 0.f });
        bg.setFillColor(m_lastState == GameState::Paused
            ? sf::Color(2, 3, 5, 232) : VOID_BG);
        m_window->draw(bg);

        if (!m_font) return;

        if (m_lastState == GameState::Tutorial) {
            drawTutorial(size);
            drawGlossary(size);
            return;
        }

        drawHeader(size);

        if (m_lastState == GameState::MainMenu) {
            drawFeedPanel();
            drawStreamPanel();
            drawLedgerPanel();
            drawSectorPanel();
            drawCorruptPanel();
            drawMenuPanel(menuRect(), 0.10f);
            drawDescPanel();
        }
        else {
            // Pause / death: one panel, centred. No telemetry -- you already
            // know what the sector is doing, it just killed you.
            const sf::Vector2f s = viewSize();
            drawMenuPanel({ s.x * 0.32f, s.y * 0.26f, s.x * 0.36f, s.y * 0.46f }, 0.f);
        }

        drawFooterHint(size);
        drawGlossary(size);

        if (m_bootActive) drawBoot(size);
    }

    // ---- Header -----------------------------------------------------------
    void drawHeader(const sf::Vector2f& size) {
        if (m_bootActive) return;

        sf::Text title(*m_font, m_title, 34);
        title.setLetterSpacing(1.8f);
        title.setFillColor(m_lastState == GameState::GameOver ? RED : CYAN);
        title.setPosition({ size.x * 0.020f, size.y * 0.028f });
        m_window->draw(title);

        if (m_lastState == GameState::MainMenu) {
            static const char* kTabs[] = { "CAMPAIGN", "HUNT", "ARSENAL", "CODEX" };
            float x = size.x * 0.34f;
            for (int i = 0; i < 4; ++i) {
                const bool active = (i == 1);
                sf::Text tab(monoFont(), kTabs[i], 16);
                tab.setLetterSpacing(1.9f);
                tab.setFillColor(active ? CYAN : TEXT_DIM);
                tab.setPosition({ x, size.y * 0.042f });
                m_window->draw(tab);

                const float w = tab.getGlobalBounds().size.x;
                if (active) {
                    // Underline only. Never a box -- open frames, implied
                    // with two or three strokes, not four.
                    sf::RectangleShape ul({ w, 2.f });
                    ul.setPosition({ x, size.y * 0.042f + 22.f });
                    ul.setFillColor(CYAN);
                    m_window->draw(ul);
                }
                x += w + 30.f;
            }

            sf::Text st(monoFont(), "STATUS  ALIVE", 16);
            st.setLetterSpacing(1.6f);
            st.setFillColor(sf::Color(80, 240, 130, static_cast<std::uint8_t>(
                175 + 78 * std::sin(m_time * 2.2f))));
            st.setPosition({ size.x - st.getGlobalBounds().size.x - size.x * 0.020f,
                             size.y * 0.042f });
            m_window->draw(st);
        }

        hline(size.x * 0.016f, size.y * 0.088f, size.x * 0.968f, CYAN_LOW);
    }

    void drawFooterHint(const sf::Vector2f& size) {
        if (m_bootActive) return;
        sf::Text hint(monoFont(), "W/S OR MOUSE  SELECT      ENTER OR CLICK  CONFIRM", 14);
        hint.setLetterSpacing(1.6f);
        hint.setFillColor(TEXT_DIM);
        hint.setPosition({ size.x * 0.020f, size.y - 30.f });
        m_window->draw(hint);
    }

    // ---- Glossary ---------------------------------------------------------
    //
    // Every screen gets one. A control scheme discoverable only by guessing is
    // a broken screen. The tab sits top-right under the header; the overlay
    // blocks everything behind it while open.
    void drawGlossary(const sf::Vector2f& size) {
        if (m_bootActive) return;

        m_ui.glossaryTab({ size.x - 46.f, size.y * 0.098f, 32.f, 32.f });

        switch (m_lastState) {
        case GameState::MainMenu:
            m_ui.drawGlossary("MAIN TERMINAL",
                "Sector telemetry, live feed, and the command list.\n"
                "Everything here is navigable by keyboard or mouse.",
                { { "W / S",       "Move the selection up or down" },
                  { "UP / DOWN",   "Same as W / S" },
                  { "MOUSE",       "Hover a row to select it" },
                  { "ENTER",       "Confirm the highlighted row" },
                  { "LEFT CLICK",  "Confirm the row under the cursor" },
                  { "?",           "Open this glossary on any screen" },
                  { "F11",         "Toggle fullscreen" },
                  { "F5",          "Reload Lua scripts (dev)" },
                  { "F3",          "Toggle debug overlay (dev)" },
                  { "GREYED ROW",  "Subsystem offline - not selectable" } });
            break;
        case GameState::Paused:
            m_ui.drawGlossary("HOLD",
                "The run is suspended. Nothing out there is moving.",
                { { "ESC",              "Resume the hunt" },
                  { "T",                "Jump straight to Field Doctrine" },
                  { "RESUME CONTRACT",  "Go back into the run" },
                  { "PURGE AND RESTART","New sector. This run's progress is lost" },
                  { "ABANDON",          "Leave the run, back to the terminal" } });
            break;
        case GameState::GameOver:
            m_ui.drawGlossary("HUNTER LOST",
                "The run ended. Your hull design is kept.",
                { { "RE-ENGAGE",  "Start a fresh sector with the same ship" },
                  { "RETURN",     "Back to the main terminal" },
                  { "REFIT BAY",  "Reshape the hull from the terminal" } });
            break;
        case GameState::Tutorial:
            m_ui.drawGlossary("FIELD DOCTRINE",
                "Three pages: basics, advanced, controls.",
                { { "A / D",      "Previous or next page" },
                  { "LEFT/RIGHT", "Same as A / D" },
                  { "< >",        "Click the arrows to turn pages" },
                  { "ESC",        "Close and go back" },
                  { "BACK",       "Same, with the mouse" } });
            break;
        default: break;
        }
    }

    // ---- Panel chrome -----------------------------------------------------

    /**
     * @brief Draw a panel's frame and title, honouring the CRT reveal.
     * @return true if the panel is fully open and content should be drawn.
     */
    bool drawPanelChrome(const Rect& r, const std::string& label,
        float delay, Chrome chrome, sf::Color accent) {
        const float p = revealOf(delay);
        if (p <= 0.f) return false;

        const Rect d = revealRect(r, p);

        sf::RectangleShape fill({ d.w, d.h });
        fill.setPosition({ d.x, d.y });
        fill.setFillColor(PANEL_BG);
        m_window->draw(fill);

        // Hot centre line while the picture is still opening.
        if (p < 0.98f) {
            const float a = std::clamp((1.f - p) * 2.2f, 0.f, 1.f);
            hline(d.x, d.cy(), d.w,
                sf::Color(accent.r, accent.g, accent.b,
                    static_cast<std::uint8_t>(235 * a)));
        }

        switch (chrome) {
        case Chrome::Brackets: {
            const float L = std::min(22.f, d.w * 0.25f);
            const sf::Color c(accent.r, accent.g, accent.b, 190);
            hline(d.x, d.y, L, c);              vline(d.x, d.y, L, c);
            hline(d.x + d.w - L, d.y, L, c);    vline(d.x + d.w, d.y, L, c);
            hline(d.x, d.y + d.h, L, c);        vline(d.x, d.y + d.h - L, L, c);
            hline(d.x + d.w - L, d.y + d.h, L, c);
            vline(d.x + d.w, d.y + d.h - L, L, c);
            break;
        }
        case Chrome::Hairline: {
            const sf::Color c(accent.r, accent.g, accent.b, 130);
            hline(d.x, d.y, d.w, c);
            hline(d.x, d.y + d.h, d.w, c);
            vline(d.x, d.y, d.h, c);
            vline(d.x + d.w, d.y, d.h, c);
            break;
        }
        case Chrome::Heavy: {
            const sf::Color c(accent.r, accent.g, accent.b, 210);
            for (int i = 0; i < 2; ++i) {
                hline(d.x, d.y + i, d.w, c);
                hline(d.x, d.y + d.h - i, d.w, c);
            }
            vline(d.x, d.y, d.h, c);
            vline(d.x + d.w, d.y, d.h, c);
            break;
        }
        }

        if (p < 1.f) return false;

        if (!label.empty()) {
            sf::Text t(monoFont(), label, 14);
            t.setLetterSpacing(1.8f);

            if (chrome == Chrome::Heavy) {
                // Filled title tab -- dark text knocked out of a hot bar.
                const float tw = t.getGlobalBounds().size.x + 14.f;
                sf::RectangleShape tab({ tw, 20.f });
                tab.setPosition({ r.x + 8.f, r.y - 10.f });
                tab.setFillColor(accent);
                m_window->draw(tab);
                t.setFillColor(sf::Color(4, 5, 8));
                t.setPosition({ r.x + 15.f, r.y - 8.f });
            }
            else {
                // Plain label sitting on the border, with a short black
                // gap punched behind it so the stroke doesn't run through.
                const float tw = t.getGlobalBounds().size.x + 12.f;
                sf::RectangleShape gap({ tw, 4.f });
                gap.setPosition({ r.x + 10.f, r.y - 2.f });
                gap.setFillColor(VOID_BG);
                m_window->draw(gap);
                t.setFillColor(accent);
                t.setPosition({ r.x + 16.f, r.y - 9.f });
            }
            m_window->draw(t);
        }
        return true;
    }

    // ---- FEED panel -------------------------------------------------------
    //
    // PHASE 2 HOOK: swap the starfield below for the live AI skirmish render.
    // The clip, chrome, scanlines and label all stay -- they are what make
    // this read as a monitor, and what stop the contents escaping the frame.
    void drawFeedPanel() {
        const Rect r = feedRect();
        if (!drawPanelChrome(r, "FEED 04 / DERANT RIM", 0.00f, Chrome::Heavy, CYAN_MID))
            return;

        beginClip(r);
        {
            sf::VertexArray pts(sf::PrimitiveType::Triangles);
            pts.resize(m_feedStars.size() * 6);
            std::size_t vi = 0;
            for (const auto& s : m_feedStars) {
                const float h = s.size * 0.5f;
                const sf::Color c(s.bright, s.bright,
                    static_cast<std::uint8_t>(std::min(255, s.bright + 30)));
                pts[vi + 0] = { { s.pos.x - h, s.pos.y - h }, c };
                pts[vi + 1] = { { s.pos.x + h, s.pos.y - h }, c };
                pts[vi + 2] = { { s.pos.x - h, s.pos.y + h }, c };
                pts[vi + 3] = { { s.pos.x + h, s.pos.y - h }, c };
                pts[vi + 4] = { { s.pos.x + h, s.pos.y + h }, c };
                pts[vi + 5] = { { s.pos.x - h, s.pos.y + h }, c };
                vi += 6;
            }
            m_window->draw(pts);

            // Scanlines sell "monitor" harder than any frame detail.
            sf::VertexArray scan(sf::PrimitiveType::Lines);
            for (float y = 0.f; y < r.h; y += 3.f) {
                scan.append({ { 0.f, y }, sf::Color(0, 0, 0, 66) });
                scan.append({ { r.w, y }, sf::Color(0, 0, 0, 66) });
            }
            m_window->draw(scan);

            // Rolling sync bar -- one slow moving band, very low alpha.
            const float by = std::fmod(m_time * 26.f, r.h + 60.f) - 60.f;
            sf::RectangleShape band({ r.w, 48.f });
            band.setPosition({ 0.f, by });
            band.setFillColor(sf::Color(40, 245, 255, 9));
            m_window->draw(band);

            sf::Text tc(monoFont(), timecode(), 14);
            tc.setLetterSpacing(1.5f);
            tc.setFillColor(sf::Color(40, 245, 255, 150));
            tc.setPosition({ 10.f, r.h - 26.f });
            m_window->draw(tc);

            sf::Text ct(monoFont(), "CONTACTS " + std::to_string(m_feedContacts), 14);
            ct.setLetterSpacing(1.5f);
            ct.setFillColor(sf::Color(40, 245, 255, 150));
            ct.setPosition({ r.w - ct.getGlobalBounds().size.x - 10.f, r.h - 26.f });
            m_window->draw(ct);
        }
        endClip();

        // Record dot lives outside the clip so it sits on the chrome.
        if (std::fmod(m_time, 1.6f) < 0.9f) {
            sf::RectangleShape dot({ 8.f, 8.f });
            dot.setPosition({ r.x + r.w - 24.f, r.y + 12.f });
            dot.setFillColor(RED);
            m_window->draw(dot);
        }
    }

    // ---- DATASTREAM panel -------------------------------------------------
    //
    // Moved off the full-screen background and into its own column: as a
    // backdrop it sat underneath the feed panel and was invisible. Clipped,
    // so long lines are cut at the edge -- which reads correctly for a
    // stream nobody formatted for this screen.
    void drawStreamPanel() {
        const Rect r = streamRect();
        if (!drawPanelChrome(r, "DATASTREAM", 0.20f, Chrome::Hairline, CYAN_MID))
            return;
        if (m_wall.empty()) return;

        beginClip(r);
        {
            const float lineH = 17.f;
            const int   rows = static_cast<int>(r.h / lineH) + 2;
            const int   start = std::max(0, static_cast<int>(m_wall.size()) - rows);
            const float sub = std::fmod(m_time * 7.f, 1.f) * lineH;

            for (int i = start; i < static_cast<int>(m_wall.size()); ++i) {
                const WallLine& wl = m_wall[i];
                const float y = (i - start) * lineH - sub;

                sf::Text t(wallFont(), wl.text, 13);
                t.setLetterSpacing(1.1f);
                // Signal lines are warm and slightly brighter. SLIGHTLY. If
                // you can spot them without looking for them, drop the alpha.
                t.setFillColor(wl.signal ? sf::Color(255, 190, 80, 118)
                    : sf::Color(30, 170, 195, 62));
                t.setPosition({ 8.f, y });
                m_window->draw(t);
            }
        }
        endClip();
    }

    // ---- LEDGER panel -----------------------------------------------------
    //
    // The point of this block is that at least one number on it is honest.
    // HUNTERS LOST is yours. Everything else is sector noise.
    void drawLedgerPanel() {
        const Rect r = ledgerRect();
        if (!drawPanelChrome(r, "LEDGER", 0.28f, Chrome::Brackets, CYAN_MID))
            return;

        beginClip(r);
        {
            float y = 20.f;

            sf::Text k1(monoFont(), "HUNTERS LOST", 14);
            k1.setLetterSpacing(1.7f);
            k1.setFillColor(TEXT_DIM);
            k1.setPosition({ 14.f, y });
            m_window->draw(k1);
            y += 22.f;

            sf::Text v1(monoFont(), std::to_string(m_hunterLosses), 34);
            v1.setLetterSpacing(1.3f);
            v1.setFillColor(m_hunterLosses > 0 ? AMBER : TEXT_DEAD);
            v1.setPosition({ 14.f, y });
            m_window->draw(v1);
            y += 48.f;

            sf::Text k2(monoFont(), "SYSTEM CASUALTIES", 14);
            k2.setLetterSpacing(1.7f);
            k2.setFillColor(TEXT_DIM);
            k2.setPosition({ 14.f, y });
            m_window->draw(k2);
            y += 22.f;

            sf::Text v2(monoFont(), groupNumber(static_cast<long long>(m_casualties)), 22);
            v2.setLetterSpacing(1.3f);
            v2.setFillColor(CYAN);
            v2.setPosition({ 14.f, y });
            m_window->draw(v2);
        }
        endClip();
    }

    // ---- SECTOR panel -----------------------------------------------------
    void drawSectorPanel() {
        const Rect r = sectorRect();
        if (!drawPanelChrome(r, "SECTOR", 0.35f, Chrome::Hairline, CYAN_MID))
            return;

        const int corrupt = corruption();

        beginClip(r);
        {
            float y = 18.f;
            kv(12.f, y, "DESIG", "DERANT RIM", CYAN);           y += 24.f;
            kv(12.f, y, "PRESSURE", corrupt > 70 ? "RISING" : "NOMINAL",
                corrupt > 70 ? AMBER : CYAN);                        y += 24.f;
            kv(12.f, y, "CONTRACT", "HIGH", CYAN);           y += 24.f;
            kv(12.f, y, "CONTACTS", std::to_string(m_feedContacts), CYAN);
        }
        endClip();
    }

    // ---- CORRUPTION panel -------------------------------------------------
    //
    // The ONLY element allowed to shout. Four simultaneous warnings on a
    // screen where nothing has happened is wallpaper inside thirty seconds.
    void drawCorruptPanel() {
        const Rect r = corruptRect();
        const int corrupt = corruption();
        const bool danger = corrupt > 70;

        if (!drawPanelChrome(r, "", 0.42f, Chrome::Hairline, danger ? RED : CYAN_MID))
            return;

        const std::uint8_t pulse = static_cast<std::uint8_t>(
            160 + 95 * (0.5f + 0.5f * std::sin(m_time * 3.4f)));

        beginClip(r);
        {
            sf::Text a(monoFont(), "VOID CORRUPTION  " + std::to_string(corrupt) + "%", 16);
            a.setLetterSpacing(1.6f);
            a.setFillColor(danger ? sf::Color(255, 48, 0, pulse) : CYAN);
            a.setPosition({ 12.f, 10.f });
            m_window->draw(a);

            if (danger) {
                sf::Text b(monoFont(), "CAUTION - HIGH RISK OF DEATH", 13);
                b.setLetterSpacing(1.5f);
                b.setFillColor(sf::Color(255, 48, 0, pulse));
                b.setPosition({ 12.f, 32.f });
                m_window->draw(b);
            }
        }
        endClip();
    }

    // ---- MENU panel -------------------------------------------------------
    void drawMenuPanel(const Rect& r, float delay) {
        if (!drawPanelChrome(r, "COMMAND", delay, Chrome::Heavy, CYAN_MID))
            return;

        beginClip(r);
        {
            const float step = 50.f;
            const float blockH = static_cast<float>(m_rows.size()) * step;
            float y = std::max(46.f, (r.h - blockH) * 0.5f);

            if (m_lastState == GameState::GameOver) {
                sf::Text sc(monoFont(), "FINAL TALLY  " + groupNumber(m_score), 20);
                sc.setLetterSpacing(1.6f);
                sc.setFillColor(CYAN);
                sc.setPosition({ 26.f, y - 46.f });
                m_window->draw(sc);
            }

            for (std::size_t i = 0; i < m_rows.size(); ++i) {
                const Row& row = m_rows[i];
                const float ry = y + static_cast<float>(i) * step;

                // Mouse target. Rows keep their own caret and skewed bar, so
                // they hit-test by hand rather than using tui::button -- but
                // they still consume the click so it can't fall through to a
                // widget underneath.
                const tui::Rect hit{ 14.f, ry - 7.f, r.w - 28.f, 36.f };
                const bool hot = !row.locked && m_ui.hovering(hit);

                // Hover moves the keyboard selection too, so the two input
                // methods can never disagree about what is highlighted.
                if (hot && m_selected != static_cast<int>(i)) {
                    m_selected = static_cast<int>(i);
                    m_selectAnim = 1.f;
                }
                if (hot && m_ui.leftEdge()) {
                    m_clickAction = row.action;
                    m_ui.consumeClick();
                }

                const bool sel = (static_cast<int>(i) == m_selected);

                if (sel) {
                    // THE one skewed element -- a parallelogram carrying the
                    // HUD's angle. Everything else on this screen is square.
                    sf::Text probe(*m_font, row.label, 26);
                    const float bw = std::min(r.w - 24.f,
                        probe.getGlobalBounds().size.x + 52.f);
                    const float bh = 36.f;
                    const float bx = 14.f, by = ry - 7.f;

                    sf::ConvexShape bar(4);
                    bar.setPoint(0, { bx + SKEW,      by });
                    bar.setPoint(1, { bx + bw + SKEW, by });
                    bar.setPoint(2, { bx + bw,        by + bh });
                    bar.setPoint(3, { bx,             by + bh });
                    bar.setFillColor(sf::Color(255, 214, 0,
                        static_cast<std::uint8_t>(205 + 50 * m_selectAnim)));
                    m_window->draw(bar);
                }

                sf::Text caret(monoFont(), sel ? ">" : (row.locked ? "x" : " "), 22);
                caret.setFillColor(sel ? sf::Color(4, 5, 8) : TEXT_DEAD);
                caret.setPosition({ 22.f, ry });
                m_window->draw(caret);

                sf::Text lbl(*m_font, row.label, 26);
                lbl.setLetterSpacing(1.4f);
                lbl.setFillColor(sel ? sf::Color(4, 5, 8)
                    : (row.locked ? TEXT_DEAD : TEXT));
                lbl.setPosition({ 46.f, ry });
                m_window->draw(lbl);
            }
        }
        endClip();
    }

    // ---- DESCRIPTION panel ------------------------------------------------
    //
    // Fixed position, updates on selection. This is what lets the labels stay
    // in-fiction: "PROSECUTE WAR PARAMETERS (CONTINUE)" pays for atmosphere
    // with the flavour name then refunds it in the parenthesis, and once
    // every row has one the eye reads only the parentheses.
    void drawDescPanel() {
        if (m_rows.empty()) return;
        const Rect r = descRect();
        if (!drawPanelChrome(r, "", 0.50f, Chrome::Brackets, CYAN_LOW)) return;

        const std::string& d = m_rows[m_selected].desc;
        if (d.empty()) return;

        beginClip(r);
        {
            sf::Text t(monoFont(), d, 15);
            t.setLetterSpacing(1.3f);
            t.setLineSpacing(1.35f);
            t.setFillColor(TEXT_DIM);
            t.setPosition({ 14.f, 12.f });
            m_window->draw(t);
        }
        endClip();
    }

    // ---- BOOT overlay -----------------------------------------------------
    void drawBoot(const sf::Vector2f& size) {
        const float t = m_bootTimer;

        sf::RectangleShape curtain(size);
        curtain.setPosition({ 0.f, 0.f });
        curtain.setFillColor(VOID_BG);
        m_window->draw(curtain);

        static const char* kLog[] = {
            "COLD START ...............  OK",
            "REACTOR PRIMED ..........  OK",
            "MACHINE SPIRIT ..........  ROUSED",
            "NAV LOCK  DERANT RIM ....  OK",
            "VOID SENSORS ............  DEGRADED",
            "CONTRACT LEDGER .........  SYNCED",
            "WEAPON RITES ............  OBSERVED",
        };
        const int shown = std::min(7, static_cast<int>(t / 0.30f));

        for (int i = 0; i < shown; ++i) {
            sf::Text l(monoFont(), kLog[i], 18);
            l.setLetterSpacing(1.7f);
            l.setFillColor((i == 4) ? sf::Color(255, 150, 40) : CYAN);
            l.setPosition({ size.x * 0.045f, size.y * 0.10f + i * 30.f });
            m_window->draw(l);
        }

        // Blinking cursor after the last visible line -- the thing that makes
        // a log feel like it is still thinking rather than finished.
        if (shown < 7 && std::fmod(m_time, 0.7f) < 0.4f) {
            sf::RectangleShape cur({ 11.f, 19.f });
            cur.setPosition({ size.x * 0.045f, size.y * 0.10f + shown * 30.f + 3.f });
            cur.setFillColor(CYAN);
            m_window->draw(cur);
        }

        // Greeting. Variable, because a machine that counts your deaths back
        // at you is the single most memorable thing on this screen.
        if (t > 2.35f) {
            const float a = std::clamp((t - 2.35f) / 0.35f, 0.f, 1.f)
                * std::clamp(1.f - (t - 3.95f) / 0.65f, 0.f, 1.f);

            // Same CRT open, applied to the greeting slab.
            const float p = std::clamp((t - 2.35f) / 0.45f, 0.f, 1.f);
            const float hh = 66.f * std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
            if (p < 0.98f) {
                hline(size.x * 0.20f, size.y * 0.50f, size.x * 0.60f,
                    sf::Color(40, 245, 255,
                        static_cast<std::uint8_t>(235 * (1.f - p))));
            }

            if (hh > 4.f) {
                sf::Text g(*m_font, "GREETINGS HUNTER", 48);
                g.setLetterSpacing(2.0f);
                g.setFillColor(sf::Color(40, 245, 255,
                    static_cast<std::uint8_t>(255 * a)));
                centerOrigin(g);
                g.setPosition({ size.x * 0.5f, size.y * 0.50f - 16.f });
                m_window->draw(g);

                const std::string sub = (m_hunterLosses > 0)
                    ? "YOU HAVE RETURNED " + std::to_string(m_hunterLosses) + " TIMES"
                    : "THE VOID IS WAITING";

                sf::Text s(monoFont(), sub, 20);
                s.setLetterSpacing(2.6f);
                s.setFillColor(sf::Color(214, 222, 232,
                    static_cast<std::uint8_t>(225 * a)));
                centerOrigin(s);
                s.setPosition({ size.x * 0.5f, size.y * 0.50f + 34.f });
                m_window->draw(s);
            }
        }

        sf::Text skip(monoFont(), "ANY KEY TO SKIP", 14);
        skip.setLetterSpacing(1.8f);
        skip.setFillColor(TEXT_DIM);
        skip.setPosition({ size.x - skip.getGlobalBounds().size.x - size.x * 0.020f,
                           size.y - 32.f });
        m_window->draw(skip);
    }

    // ========================================================================
    // TUTORIAL
    // ========================================================================

    std::vector<TutorialSection> getTutorialSections() const {
        return {
            { "MOVEMENT",
              "W / S / A / D or Arrow Keys\n"
              "Move in any direction.\n"
              "Ship faces your mouse cursor.",
              sf::Color(100, 200, 255), 0 },
            { "TURBO BOOST",
              "Hold LShift for speed boost.\n"
              "Consumes Energy (blue bar).\n"
              "Running out = overheat penalty.",
              sf::Color(255, 200, 100), 0 },
            { "DASH",
              "Press Space to dash.\n"
              "Direction: movement or forward.\n"
              "Costs energy, short cooldown.\n"
              "Great for evasion!",
              sf::Color(100, 255, 255), 0 },
            { "ENERGY MANAGEMENT",
              "Blue bar = Energy Drive.\n"
              "Used for dash and turbo.\n"
              "Regenerates when not boosting.",
              sf::Color(50, 150, 255), 0 },
            { "SCORING",
              "Destroy asteroids & enemies.\n"
              "Bigger targets = bigger rewards.",
              sf::Color(255, 215, 0), 0 },
            { "SHOOTING",
              "Left Click to fire.\n"
              "Each shot builds Heat (orange bar).\n"
              "Full heat = gun overheats!\n"
              "Wait to cool down.",
              sf::Color(255, 140, 60), 1 },
            { "COMBAT TIPS",
              "- Dash to dodge incoming fire\n"
              "- Parry for counter-attacks\n"
              "- Manage energy & heat\n"
              "- Rift Shot = massive damage\n"
              "- Watch enemy telegraphs!",
              sf::Color(255, 200, 200), 1 },
        };
    }

    /**
     * @brief Tutorial content, split into three pages.
     *
     * Replaces the scrolling column layout. There was never enough text to
     * justify scrolling -- and getMaxScrollOffset() was GUESSING the content
     * height with a hardcoded "- 400", so the scroll limit was wrong whenever
     * the text changed. Pages have a fixed, known extent.
     */
    std::vector<TutorialSection> tutorialPage(int page) const {
        const auto all = getTutorialSections();
        std::vector<TutorialSection> out;
        for (std::size_t i = 0; i < all.size(); ++i) {
            int p = 0;
            if (i >= 2 && i <= 4) p = 1;   // advanced
            if (i >= 5) p = 2;             // gunnery and tips
            if (p == page) out.push_back(all[i]);
        }
        return out;
    }

    static const char* tutorialPageName(int p) {
        return (p == 0) ? "BASIC FLIGHT" : (p == 1) ? "ADVANCED" : "GUNNERY";
    }

    void drawTutorial(const sf::Vector2f& size) {
        sf::Text title(*m_font, "FIELD DOCTRINE", 34);
        title.setLetterSpacing(1.8f);
        title.setFillColor(CYAN);
        title.setPosition({ size.x * 0.020f, size.y * 0.028f });
        m_window->draw(title);

        sf::Text pn(monoFont(), tutorialPageName(m_tutPage), 16);
        pn.setLetterSpacing(1.9f);
        pn.setFillColor(AMBER);
        pn.setPosition({ size.x * 0.34f, size.y * 0.042f });
        m_window->draw(pn);

        hline(size.x * 0.016f, size.y * 0.088f, size.x * 0.968f, CYAN_LOW);

        const Rect body{ size.x * 0.020f, size.y * 0.130f,
                         size.x * 0.960f, size.y * 0.680f };
        if (!drawPanelChrome(body, "DOCTRINE", 0.f, Chrome::Hairline, CYAN_MID))
            return;

        beginClip(body);
        {
            float y = 30.f;
            for (const auto& sec : tutorialPage(m_tutPage)) {
                sf::Text st(monoFont(), sec.title, 22);
                st.setLetterSpacing(1.7f);
                st.setFillColor(sec.color);
                st.setPosition({ 32.f, y });
                m_window->draw(st);
                y += 34.f;

                sf::Text sb(monoFont(), sec.body, 17);
                sb.setFillColor(TEXT);
                sb.setLineSpacing(1.4f);
                sb.setPosition({ 46.f, y });
                m_window->draw(sb);

                int lines = 1;
                for (char c : sec.body) if (c == '\n') lines++;
                y += lines * 25.f + 30.f;
            }
        }
        endClip();

        // ---- Page controls ----
        const float navY = size.y - 78.f;
        const float cx = size.x * 0.5f;

        if (m_tutPage > 0 && m_ui.iconButton({ cx - 150.f, navY, 40.f, 40.f }, "<"))
            m_tutPage--;
        if (m_tutPage < 2 && m_ui.iconButton({ cx + 110.f, navY, 40.f, 40.f }, ">"))
            m_tutPage++;

        // Page dots: cheap, and they say "there is more" without a sentence.
        for (int i = 0; i < 3; ++i) {
            const float dx = cx - 26.f + static_cast<float>(i) * 26.f;
            sf::RectangleShape d({ 10.f, 10.f });
            d.setPosition({ dx, navY + 15.f });
            d.setFillColor(i == m_tutPage ? AMBER : TEXT_DEAD);
            m_window->draw(d);
        }

        // ---- Back. Escape works too; neither was discoverable before. ----
        if (m_ui.button({ size.x * 0.020f, navY, 260.f, 40.f },
            "BACK TO TERMINAL", false, false, false, 20)) {
            m_clickAction = MenuAction::BackToMenu;
        }

        if (!m_ui.glossaryOpen()) {
            if (m_ui.keyEdge(sf::Keyboard::Key::Escape))
                m_clickAction = MenuAction::BackToMenu;
            if (m_ui.keyEdge(sf::Keyboard::Key::A) || m_ui.keyEdge(sf::Keyboard::Key::Left))
                m_tutPage = std::max(0, m_tutPage - 1);
            if (m_ui.keyEdge(sf::Keyboard::Key::D) || m_ui.keyEdge(sf::Keyboard::Key::Right))
                m_tutPage = std::min(2, m_tutPage + 1);
        }

        sf::Text hint(monoFont(), "A / D  PAGE      ESC  BACK", 14);
        hint.setLetterSpacing(1.6f);
        hint.setFillColor(TEXT_DIM);
        hint.setPosition({ size.x - hint.getGlobalBounds().size.x - size.x * 0.020f,
                           size.y - 30.f });
        m_window->draw(hint);
    }

    int getMaxScrollOffset() const {
        int total = 0;
        for (const auto& s : getTutorialSections()) {
            int lines = 1;
            for (char c : s.body) if (c == '\n') lines++;
            total += lines + 1;
        }
        return std::max(0, total * 25 - 400);
    }

    // ========================================================================
    // CONTENT POOLS
    //
    // MIGRATION NOTE: these belong in Lua next to the other tunables so they
    // hot-reload on F5. They are in C++ for now on purpose -- sol2's get_or()
    // returns silent defaults when a key is missing, and an empty datastream
    // fails invisibly. Move them once the layout is settled and you can SEE
    // that the table loaded.
    //
    // Target is ~150-200 noise lines. What is here is a starting set.
    // ========================================================================

    void seedStringWall() { for (int i = 0; i < 70; ++i) pushWallLine(); }

    std::string pickNoise() {
        static const char* kNoise[] = {
            "SENSOR SWEEP  ARC #  CLEAN",
            "HULL POLL  SEGMENT #  NOMINAL",
            "COOLANT LOOP #  IN TOLERANCE",
            "TRANSPONDER PING  ID #  NO REPLY",
            "MASS SHADOW  BEARING #",
            "REACTOR OUTPUT  # PCT  STABLE",
            "DEBRIS FIELD  # OBJECTS",
            "VOID TIDE  DELTA #",
            "LEDGER ENTRY #  ARCHIVED",
            "MUNITION RACK #  VERIFIED",
            "GYRO CAL  AXIS #  PASS",
            "THERMAL VENT #  CYCLING",
            "AUGUR BAND #  STATIC",
            "SALVAGE BEACON #  DORMANT",
            "RITE OF ACTIVATION  UNIT #",
            "NAV SPUR #  RECOMPUTED",
            "FUEL MASS  # UNITS",
            "PLATE #  MICROFRACTURE MINOR",
            "SIGNAL DISCIPLINE HELD",
            "MANIFEST  # ENTRIES SEALED",
            "AIRLOCK #  SEALED",
            "GUNNERY SERVO #  RANGE OK",
            "STAR FIX  ERROR #",
            "POWER BUS #  DRAW NOMINAL",
            "WASTE HEAT DUMPED  # KJ",
            "AUSPEX RETURN  ROCK",
            "ARCHIVE QUERY #  NO MATCH",
            "PRESSURE SEAL #  HOLDING",
            "DAMPER #  ENGAGED",
            "SHIELD LATTICE #  IDLE",
            "PROXIMITY CLOCK RESET",
            "COGITATOR IDLE  # CYCLES",
            "HARMONIC #  IN PHASE",
            "LITANY #  RECITED",
            "GHOST DISCARDED  BEARING #",
            "PORT #  CAPPED",
            "GRAV PLATE #  # W",
            "HULL TEMP  # K  FALLING",
            "ORDNANCE SAFETY ENGAGED",
            "SOLUTION #  CACHED",
            "COMMS BAND #  EMPTY",
            "MACHINE SPIRIT QUIESCENT",
            "OXYGEN RESERVE  # PCT",
            "DRIVE PLASMA CONTAINED",
            "ASTROPATH RELAY  NO TRAFFIC",
            "SCRAP ASSAY #  LOW YIELD",
            "DIAGNOSTIC BLOCK #",
            "AUGUR REALIGNED  STARBOARD",
            "CHRONOMETER DRIFT  # MS",
            "LOG ROTATED  FILE #",
        };
        return substitute(kNoise[m_rng_next() % 50]);
    }

    std::string pickSignal() {
        static const char* kSignal[] = {
            "HUNTER #  BEACON SILENT # HRS",
            "PARTIAL  ...IT IS NOT A STORM...",
            "RETURN MATCHES NO KNOWN HULL",
            "DISTRESS  ...DO NOT ANSWER IT...",
            "SECTOR # POPULATION NOW ZERO",
            "OBJECT # MOVES AGAINST THE TIDE",
            "VOICE MATCHES DECEASED  ENTRY #",
            "SOMETHING ANSWERED THE PING",
            "# CATALOGUE ENTRIES NOW ABSENT",
            "HULL FOUND  INTERIOR INVERTED",
            "LOOP  ...WE ARE STILL ABOARD...",
            "MASS EXCEEDS VOLUME BY #",
            "CONTRACT #  CLAIMANT NEVER BACK",
            "AUGUR POINTS AT NOTHING AGAIN",
        };
        return substitute(kSignal[m_rng_next() % 14]);
    }

    std::string substitute(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            if (c == '#') out += std::to_string(m_rng_next() % 9000 + 100);
            else          out += c;
        }
        return out;
    }

    void seedFeedStars() {
        const Rect r = feedRect();
        m_feedStars.resize(110);
        for (auto& s : m_feedStars) {
            s.pos = { frand() * r.w, frand() * r.h };
            s.speed = 3.f + frand() * 34.f;
            s.size = 1.f + (m_rng_next() % 3 == 0 ? 1.f : 0.f);
            s.bright = static_cast<std::uint8_t>(45 + m_rng_next() % 150);
        }
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    /// Corruption climbs with your own failures, so the line that sounds most
    /// like set dressing is actually reporting on you.
    int corruption() const { return std::clamp(38 + m_hunterLosses * 6, 0, 99); }

    void kv(float x, float y, const std::string& key,
        const std::string& val, sf::Color valColor) {
        sf::Text k(monoFont(), key, 14);
        k.setLetterSpacing(1.6f);
        k.setFillColor(TEXT_DIM);
        k.setPosition({ x, y });
        m_window->draw(k);

        sf::Text v(monoFont(), val, 15);
        v.setLetterSpacing(1.4f);
        v.setFillColor(valColor);
        v.setPosition({ x + 92.f, y - 1.f });
        m_window->draw(v);
    }

    const sf::Font& monoFont() const { return m_mono ? *m_mono : *m_font; }
    const sf::Font& wallFont() const { return m_mono ? *m_mono : *m_font; }

    void hline(float x, float y, float w, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x,     y }, c };
        l[1] = { { x + w, y }, c };
        m_window->draw(l);
    }

    void vline(float x, float y, float h, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x, y     }, c };
        l[1] = { { x, y + h }, c };
        m_window->draw(l);
    }

    static void centerOrigin(sf::Text& text) {
        const sf::FloatRect b = text.getLocalBounds();
        text.setOrigin({ b.position.x + b.size.x * 0.5f,
                         b.position.y + b.size.y * 0.5f });
    }

    /// Space-grouped digits read more like machine output than commas do.
    static std::string groupNumber(long long v) {
        std::string s = std::to_string(v);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, " ");
        return s;
    }

    std::string timecode() const {
        const int total = static_cast<int>(m_time);
        auto pad = [](int v) { return (v < 10 ? std::string("0") : std::string()) + std::to_string(v); };
        return pad((total / 3600) % 100) + ":" + pad((total / 60) % 60) + ":" + pad(total % 60);
    }

    /// Local xorshift: keeps the menu off rand(), which gameplay also uses.
    std::uint32_t m_rng = 0x9E3779B9u;
    std::uint32_t m_rng_next() {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return m_rng;
    }
    float frand() { return static_cast<float>(m_rng_next() % 100000) / 100000.f; }

    // ========================================================================
    // STATE
    // ========================================================================

    sf::RenderWindow* m_window = nullptr;
    sf::Font* m_font = nullptr;
    sf::Font* m_mono = nullptr;

    GameState m_lastState = static_cast<GameState>(-1);
    std::vector<Row> m_rows;
    std::string m_title;
    int m_selected = 0;
    int m_score = 0;
    int m_scrollOffset = 0;      ///< legacy; tutorial is paged now
    int m_tutPage = 0;           ///< 0 basics, 1 advanced, 2 gunnery

    tui::UI     m_ui;            ///< shared widget layer (mouse, glossary)
    MenuAction  m_clickAction = MenuAction::None;

    float m_time = 0.f;
    float m_selectAnim = 0.f;
    float m_openTimer = -1.f;   ///< <0 = panels held shut (boot running)

    bool  m_bootActive = false;
    float m_bootTimer = 0.f;

    std::vector<WallLine> m_wall;
    float m_wallTimer = 0.f;
    float m_burstTimer = 0.f;

    std::vector<FeedStar> m_feedStars;
    int   m_feedContacts = 0;

    int    m_hunterLosses = 0;
    double m_casualties = 4182377.0;
};