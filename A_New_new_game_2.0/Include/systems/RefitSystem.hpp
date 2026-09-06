/**
 * @file RefitSystem.hpp
 * @brief The refit bay: hull editor, mount placement, live stat readout.
 *
 * Screen-space only. Owns no ship data -- it edits a ship::ShipDesign that
 * lives in SystemManager and persists between runs. Polls its own mouse and
 * keyboard so game.cpp only has to route the state.
 *
 * ============================================================================
 * VISUAL CONTRACT
 * ============================================================================
 * Same machine as MenuSystem: orthogonal, hard edges, flat fills, dark greys
 * and hot accents, panels that are windows rather than rectangles. Content
 * with moving parts renders through beginClip()/endClip() so nothing escapes
 * its frame. There is deliberate duplication with MenuSystem's chrome -- when
 * a third screen needs it, lift both into a shared TerminalUI.hpp rather than
 * copying a third time.
 *
 * ============================================================================
 * INTERACTION
 * ============================================================================
 *   Left drag       move a hull point
 *   Right click     toggle the mount under the cursor
 *                     on a vertex -> gun       on an edge -> drive
 *   Insert / =      add a point at the cursor (max 8, the b2ComputeHull cap)
 *   Delete          remove the hovered point (min 3)
 *   Y               toggle mirror editing
 *   1 2 3           load a pattern
 *   A               auto-mount everything valid
 *   Escape / Enter  back to the terminal
 *
 * Rejected drags do NOT snap. A point that lands somewhere the player did not
 * put it is worse than a point that refuses to move -- the cursor flashes red
 * and the hull stays where it was.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/TerminalUI.hpp"
#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

class RefitSystem : public ISystem {
public:
    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    void init(const SystemContext& ctx) override {
        m_window = ctx.window;
        m_ui.attach(ctx.window);
    }

    void setFont(sf::Font* f) { m_font = f; m_ui.setFonts(m_font, m_mono); }
    void setMonoFont(sf::Font* f) { m_mono = f; m_ui.setFonts(m_font, m_mono); }

    /// Point at the design SystemManager owns. Must be called before use.
    void setDesign(ship::ShipDesign* d) { m_design = d; }

    /// Clear transient interaction state. Call on every entry to the screen.
    void onEnter() {
        m_dragging = -1;
        m_hoverPoint = -1;
        m_hoverEdge = -1;
        m_exit = false;
        m_openTimer = 0.f;
        m_rejectFlash = 0.f;
        m_toast.clear();
        m_toastTimer = 0.f;

        // Prime EVERY edge-detector from the current physical state. Entering
        // this screen means Enter is still held from the row that opened it;
        // an unprimed detector reads that hold as a fresh press and exits on
        // frame one. Priming makes a held key require release-then-press.
        m_kExit = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        m_kY = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Y);
        m_k1 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num1);
        m_k2 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num2);
        m_k3 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num3);
        m_kA = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A);
        m_kDel = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Delete);
        m_kAdd = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        m_lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        m_rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);

        m_inputLock = 0.12f;   // belt and braces against fast re-entry

        m_ui.primeInput();     // same guard, for every tui widget on screen
        m_ui.resetReveal();
    }

    bool wantsExit() const { return m_exit; }
    void clearExit() { m_exit = false; }

    void update(float dt) override {
        if (!m_window || !m_font || !m_design) return;
        dt = std::clamp(dt, 0.f, 0.1f);
        m_time += dt;
        if (m_openTimer < 3.f) m_openTimer += dt;
        m_rejectFlash = std::max(0.f, m_rejectFlash - dt * 3.f);
        if (m_toastTimer > 0.f) m_toastTimer -= dt;

        m_ui.begin(dt);        // live screen view + mouse sample

        m_inputLock = std::max(0.f, m_inputLock - dt);
        // Hull editing is suppressed while the glossary is up, or a drag
        // started behind the overlay would keep running under it.
        if (m_inputLock <= 0.f && !m_ui.glossaryOpen()) handleInput();
        draw();
    }

    // ========================================================================
    // PATTERNS
    //
    // Content, not geometry, so they live here rather than in ShipDesign. The
    // player always starts from one of these -- never a blank grid. Most
    // people are not designers, and a hull they made that looks wrong is worse
    // than no editor at all.
    // ========================================================================

    static ship::ShipDesign patternInterceptor() {
        auto d = ship::ShipDesign::fromPoints({
            {   0.f, -34.f }, {  14.f,  16.f }, {   8.f,  26.f },
            {  -8.f,  26.f }, { -14.f,  16.f } });
        d.autoMount();
        return d;
    }

    static ship::ShipDesign patternStandard() {
        auto d = ship::ShipDesign::stock();
        d.autoMount();
        return d;
    }

    static ship::ShipDesign patternBastion() {
        auto d = ship::ShipDesign::fromPoints({
            { -24.f, -22.f }, {  24.f, -22.f }, {  34.f,  10.f },
            {  26.f,  32.f }, { -26.f,  32.f }, { -34.f,  10.f } });
        d.autoMount();
        return d;
    }

private:
    // ========================================================================
    // PALETTE  (identical to MenuSystem -- do not let these drift apart)
    // ========================================================================

    static inline const sf::Color VOID_BG{ 2,   3,   5 };
    static inline const sf::Color PANEL_BG{ 6,   8,  12 };
    static inline const sf::Color CYAN{ 40, 245, 255 };
    static inline const sf::Color CYAN_MID{ 20, 150, 170 };
    static inline const sf::Color CYAN_LOW{ 12,  70,  84 };
    static inline const sf::Color AMBER{ 255, 214,   0 };
    static inline const sf::Color RED{ 255,  48,   0 };
    static inline const sf::Color GREEN{ 80, 240, 130 };
    static inline const sf::Color TEXT{ 214, 222, 232 };
    static inline const sf::Color TEXT_DIM{ 84,  92, 104 };
    static inline const sf::Color TEXT_DEAD{ 52,  58,  68 };

    static constexpr float OPEN_DUR = 0.30f;
    static constexpr float ZOOM = 4.2f;   ///< Hull is ~+/-40px; blow it up.
    static constexpr float HANDLE_R = 7.f;    ///< Grab radius, screen px.

    struct Rect {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
        bool contains(sf::Vector2f p) const {
            return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
        }
    };

    // ========================================================================
    // LAYOUT
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

    sf::Vector2f viewSize() const {
        const sf::Vector2u s = m_window->getSize();
        return { static_cast<float>(s.x), static_cast<float>(s.y) };
    }

    Rect frac(float x, float y, float w, float h) const {
        const sf::Vector2f s = viewSize();
        return { x * s.x, y * s.y, w * s.x, h * s.y };
    }

    Rect hullRect()   const { return frac(0.020f, 0.112f, 0.560f, 0.760f); }
    Rect statsRect()  const { return frac(0.605f, 0.112f, 0.375f, 0.420f); }
    Rect mountRect()  const { return frac(0.605f, 0.560f, 0.375f, 0.180f); }
    Rect statusRect() const { return frac(0.605f, 0.768f, 0.375f, 0.104f); }

    sf::Vector2f toLocal(sf::Vector2f screen) const {
        const Rect r = hullRect();
        return { (screen.x - r.cx()) / ZOOM, (screen.y - r.cy()) / ZOOM };
    }
    sf::Vector2f toScreen(sf::Vector2f local) const {
        const Rect r = hullRect();
        return { r.cx() + local.x * ZOOM, r.cy() + local.y * ZOOM };
    }

    // ========================================================================
    // INPUT
    // ========================================================================

    void handleInput() {
        const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
        m_mouse = m_window->mapPixelToCoords(pix, uiView());

        const bool lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        const bool rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
        const bool inPanel = hullRect().contains(m_mouse);

        updateHover(inPanel);

        // ---- Left drag ----
        if (lDown && !m_lDown && inPanel && m_hoverPoint >= 0) m_dragging = m_hoverPoint;
        if (!lDown) m_dragging = -1;

        if (m_dragging >= 0 && lDown) {
            if (!m_design->movePoint(m_dragging, snap(toLocal(m_mouse)))) {
                m_rejectFlash = 1.f;
                m_rejectAt = m_mouse;
            }
        }

        // ---- Right click toggles the mount under the cursor ----
        if (rDown && !m_rDown && inPanel) {
            if (m_hoverPoint >= 0)     toggleGun(m_hoverPoint);
            else if (m_hoverEdge >= 0) toggleEngine(m_hoverEdge);
        }

        // ---- Keys ----
        if (keyEdge(sf::Keyboard::Key::Y, m_kY)) {
            m_design->setSymmetric(!m_design->symmetric());
            toast(m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF");
        }
        if (keyEdge(sf::Keyboard::Key::Num1, m_k1)) loadPattern(1);
        if (keyEdge(sf::Keyboard::Key::Num2, m_k2)) loadPattern(2);
        if (keyEdge(sf::Keyboard::Key::Num3, m_k3)) loadPattern(3);

        if (keyEdge(sf::Keyboard::Key::A, m_kA)) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        if (keyEdge(sf::Keyboard::Key::Delete, m_kDel) && m_hoverPoint >= 0) {
            if (!m_design->removePoint(m_hoverPoint)) toast("MINIMUM 3 POINTS");
            m_hoverPoint = -1;
        }

        const bool addKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        if (addKey && !m_kAdd && inPanel) {
            if (!m_design->addPoint(snap(toLocal(m_mouse))))
                toast("HULL FULL - 8 POINT LIMIT");
        }
        m_kAdd = addKey;

        const bool exitKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        if (exitKey && !m_kExit) m_exit = true;
        m_kExit = exitKey;

        m_lDown = lDown;
        m_rDown = rDown;
    }

    bool keyEdge(sf::Keyboard::Key k, bool& was) {
        const bool now = sf::Keyboard::isKeyPressed(k);
        const bool edge = now && !was;
        was = now;
        return edge;
    }

    /// 2-unit design grid: loose enough to feel free, tight enough that a
    /// mirrored pair lands on matching coordinates instead of near-misses.
    static sf::Vector2f snap(sf::Vector2f v) {
        return { std::round(v.x * 0.5f) * 2.f, std::round(v.y * 0.5f) * 2.f };
    }

    void updateHover(bool inPanel) {
        m_hoverPoint = -1;
        m_hoverEdge = -1;
        if (!inPanel || m_dragging >= 0) return;

        const auto& pts = m_design->points();
        float best = HANDLE_R + 3.f;
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            const sf::Vector2f s = toScreen(pts[i]);
            const float d = std::hypot(s.x - m_mouse.x, s.y - m_mouse.y);
            if (d < best) { best = d; m_hoverPoint = i; }
        }
        if (m_hoverPoint >= 0) return;

        best = HANDLE_R + 5.f;
        for (const auto& e : m_design->engineSlots()) {
            const sf::Vector2f s = toScreen(e.position);
            const float d = std::hypot(s.x - m_mouse.x, s.y - m_mouse.y);
            if (d < best) { best = d; m_hoverEdge = e.index; }
        }
    }

    /// ShipDesign has no single-mount removal, so unmount everything and
    /// replay the list minus one. Cheap at this scale and keeps the design
    /// class free of editor-shaped API.
    void rebuildMounts(std::vector<int> guns, std::vector<int> engines) {
        m_design->unmountAll();
        for (int g : guns)    m_design->mountGun(g);
        for (int e : engines) m_design->mountEngine(e);
    }

    void toggleGun(int vertexIndex) {
        auto guns = m_design->mountedGuns();
        const auto engines = m_design->mountedEngines();

        if (std::find(guns.begin(), guns.end(), vertexIndex) != guns.end()) {
            guns.erase(std::remove(guns.begin(), guns.end(), vertexIndex), guns.end());
            rebuildMounts(guns, engines);
            toast("WEAPON REMOVED");
            return;
        }
        if (!m_design->mountGun(vertexIndex)) {
            for (const auto& s : m_design->gunSlots())
                if (s.index == vertexIndex && !s.valid) { toast(s.reason); return; }
            toast("WEAPON LIMIT REACHED");
        }
    }

    void toggleEngine(int edgeIndex) {
        const auto guns = m_design->mountedGuns();
        auto engines = m_design->mountedEngines();

        if (std::find(engines.begin(), engines.end(), edgeIndex) != engines.end()) {
            engines.erase(std::remove(engines.begin(), engines.end(), edgeIndex), engines.end());
            rebuildMounts(guns, engines);
            toast("DRIVE REMOVED");
            return;
        }
        if (!m_design->mountEngine(edgeIndex)) {
            for (const auto& s : m_design->engineSlots())
                if (s.index == edgeIndex && !s.valid) { toast(s.reason); return; }
            toast("DRIVE LIMIT REACHED");
        }
    }

    void loadPattern(int n) {
        const bool sym = m_design->symmetric();
        if (n == 1) *m_design = patternInterceptor();
        if (n == 2) *m_design = patternStandard();
        if (n == 3) *m_design = patternBastion();
        m_design->setSymmetric(sym);
        toast(n == 1 ? "INTERCEPTOR PATTERN"
            : n == 2 ? "STANDARD PATTERN" : "BASTION PATTERN");
    }

    void toast(const std::string& s) { m_toast = s; m_toastTimer = 2.2f; }

    // ========================================================================
    // CLIPPING  (same mechanism as MenuSystem: panels are windows)
    // ========================================================================

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

    float revealOf(float delay) const {
        const float t = m_openTimer - delay;
        return (t <= 0.f) ? 0.f : std::clamp(t / OPEN_DUR, 0.f, 1.f);
    }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw() {
        m_window->setView(uiView());
        const sf::Vector2f size = viewSize();

        sf::RectangleShape bg(size);
        bg.setPosition({ 0.f, 0.f });
        bg.setFillColor(VOID_BG);
        m_window->draw(bg);

        drawHeader(size);
        drawHullPanel();
        drawStatsPanel();
        drawMountPanel();
        drawStatusPanel();
        drawHints(size);
        drawControls(size);
    }

    void drawHeader(const sf::Vector2f& size) {
        sf::Text title(*m_font, "REFIT BAY", 34);
        title.setLetterSpacing(1.8f);
        title.setFillColor(CYAN);
        title.setPosition({ size.x * 0.020f, size.y * 0.028f });
        m_window->draw(title);

        sf::Text sub(monoFont(), m_design->symmetric() ? "MIRROR  ON" : "MIRROR  OFF", 16);
        sub.setLetterSpacing(1.8f);
        sub.setFillColor(m_design->symmetric() ? CYAN : AMBER);
        sub.setPosition({ size.x * 0.34f, size.y * 0.042f });
        m_window->draw(sub);

        const ship::ValidationResult v = m_design->validate();
        sf::Text st(monoFont(), v.ok ? "HULL  AIRWORTHY" : "HULL  REJECTED", 16);
        st.setLetterSpacing(1.6f);
        st.setFillColor(v.ok ? GREEN : RED);
        st.setPosition({ size.x - st.getGlobalBounds().size.x - size.x * 0.020f,
                         size.y * 0.042f });
        m_window->draw(st);

        hline(size.x * 0.016f, size.y * 0.088f, size.x * 0.968f, CYAN_LOW);
    }

    bool panelChrome(const Rect& r, const std::string& label, float delay,
        sf::Color accent, bool heavy) {
        const float p = revealOf(delay);
        if (p <= 0.f) return false;

        const float wf = std::clamp(p / 0.34f, 0.06f, 1.f);
        const float hf = std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
        const float dw = r.w * wf;
        const float dh = std::max(2.f, r.h * hf);
        const Rect d{ r.cx() - dw * 0.5f, r.cy() - dh * 0.5f, dw, dh };

        sf::RectangleShape fill({ d.w, d.h });
        fill.setPosition({ d.x, d.y });
        fill.setFillColor(PANEL_BG);
        m_window->draw(fill);

        if (p < 0.98f) {
            const float a = std::clamp((1.f - p) * 2.2f, 0.f, 1.f);
            hline(d.x, d.cy(), d.w, sf::Color(accent.r, accent.g, accent.b,
                static_cast<std::uint8_t>(235 * a)));
        }

        const sf::Color c(accent.r, accent.g, accent.b, heavy ? 210 : 130);
        for (int i = 0; i < (heavy ? 2 : 1); ++i) {
            hline(d.x, d.y + static_cast<float>(i), d.w, c);
            hline(d.x, d.y + d.h - static_cast<float>(i), d.w, c);
        }
        vline(d.x, d.y, d.h, c);
        vline(d.x + d.w, d.y, d.h, c);

        if (p < 1.f) return false;

        if (!label.empty()) {
            sf::Text t(monoFont(), label, 14);
            t.setLetterSpacing(1.8f);
            if (heavy) {
                const float tw = t.getGlobalBounds().size.x + 14.f;
                sf::RectangleShape tab({ tw, 20.f });
                tab.setPosition({ r.x + 8.f, r.y - 10.f });
                tab.setFillColor(accent);
                m_window->draw(tab);
                t.setFillColor(sf::Color(4, 5, 8));
                t.setPosition({ r.x + 15.f, r.y - 8.f });
            }
            else {
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

    // ---- HULL panel: the editor itself -----------------------------------
    void drawHullPanel() {
        const Rect r = hullRect();
        if (!panelChrome(r, "HULL GEOMETRY", 0.00f, CYAN_MID, true)) return;

        const auto& pts = m_design->points();
        const auto& stats = m_design->stats();
        const int n = static_cast<int>(pts.size());

        beginClip(r);
        {
            const float cx = r.w * 0.5f, cy = r.h * 0.5f;
            const auto L = [&](sf::Vector2f v) {
                return sf::Vector2f{ cx + v.x * ZOOM, cy + v.y * ZOOM };
            };

            // ---- Grid, one line per 10 design units ----
            sf::VertexArray grid(sf::PrimitiveType::Lines);
            const sf::Color gc(18, 30, 38);
            for (float g = -80.f; g <= 80.f; g += 10.f) {
                grid.append({ { cx + g * ZOOM, 0.f }, gc });
                grid.append({ { cx + g * ZOOM, r.h }, gc });
                grid.append({ { 0.f, cy + g * ZOOM }, gc });
                grid.append({ { r.w, cy + g * ZOOM }, gc });
            }
            m_window->draw(grid);

            // ---- Centreline. Lit when it is an actual mirror axis. ----
            sf::VertexArray axis(sf::PrimitiveType::Lines, 2);
            const sf::Color ac = m_design->symmetric()
                ? sf::Color(40, 245, 255, 90) : sf::Color(60, 66, 78, 90);
            axis[0] = { { cx, 0.f }, ac };
            axis[1] = { { cx, r.h }, ac };
            m_window->draw(axis);

            // ---- Forward marker. The ship faces -Y and nothing else on this
            //      screen would tell you that. ----
            sf::Text fwd(monoFont(), "FORWARD", 12);
            fwd.setLetterSpacing(1.8f);
            fwd.setFillColor(TEXT_DEAD);
            fwd.setPosition({ cx + 12.f, 12.f });
            m_window->draw(fwd);

            sf::VertexArray arrow(sf::PrimitiveType::Lines, 6);
            arrow[0] = { { cx, 42.f }, TEXT_DEAD }; arrow[1] = { { cx, 14.f }, TEXT_DEAD };
            arrow[2] = { { cx, 14.f }, TEXT_DEAD }; arrow[3] = { { cx - 5.f, 23.f }, TEXT_DEAD };
            arrow[4] = { { cx, 14.f }, TEXT_DEAD }; arrow[5] = { { cx + 5.f, 23.f }, TEXT_DEAD };
            m_window->draw(arrow);

            // ---- Hull ----
            if (n >= 3) {
                sf::ConvexShape hull;
                hull.setPointCount(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i)
                    hull.setPoint(static_cast<std::size_t>(i), L(pts[i]));
                hull.setFillColor(sf::Color(20, 60, 90, 130));
                hull.setOutlineThickness(2.f);
                hull.setOutlineColor(CYAN);
                m_window->draw(hull);
            }

            // ---- Drive mounts, on edge midpoints ----
            for (const auto& e : m_design->engineSlots()) {
                const bool hov = (m_hoverEdge == e.index);
                if (!e.valid && !hov) continue;

                const sf::Vector2f p = L(e.position);
                const bool mounted = isMounted(m_design->mountedEngines(), e.index);
                const sf::Vector2f nrm = e.outward;
                const sf::Vector2f tan{ -nrm.y, nrm.x };
                const float half = mounted ? 11.f : 7.f;

                // A bar lying along the edge, extruded outward: reads as a
                // nozzle, so it can never be mistaken for a vertex handle.
                sf::ConvexShape bar(4);
                bar.setPoint(0, { p.x - tan.x * half, p.y - tan.y * half });
                bar.setPoint(1, { p.x + tan.x * half, p.y + tan.y * half });
                bar.setPoint(2, { p.x + tan.x * half + nrm.x * 9.f,
                                  p.y + tan.y * half + nrm.y * 9.f });
                bar.setPoint(3, { p.x - tan.x * half + nrm.x * 9.f,
                                  p.y - tan.y * half + nrm.y * 9.f });
                bar.setFillColor(mounted ? AMBER
                    : (e.valid ? sf::Color(20, 150, 170, hov ? 220 : 110)
                        : sf::Color(255, 48, 0, 90)));
                m_window->draw(bar);

                if (mounted) {
                    // Plume, so thrust reads as thrust rather than a marker.
                    const float f = 14.f + 5.f * std::sin(m_time * 9.f
                        + static_cast<float>(e.index));
                    sf::VertexArray pl(sf::PrimitiveType::Triangles, 3);
                    pl[0] = { { p.x - tan.x * 5.f + nrm.x * 9.f,
                                p.y - tan.y * 5.f + nrm.y * 9.f }, sf::Color(255, 214, 0, 170) };
                    pl[1] = { { p.x + tan.x * 5.f + nrm.x * 9.f,
                                p.y + tan.y * 5.f + nrm.y * 9.f }, sf::Color(255, 214, 0, 170) };
                    pl[2] = { { p.x + nrm.x * (9.f + f), p.y + nrm.y * (9.f + f) },
                              sf::Color(255, 90, 0, 0) };
                    m_window->draw(pl);
                }
            }

            // ---- Vertex handles and weapon mounts ----
            for (const auto& g : m_design->gunSlots()) {
                const sf::Vector2f p = L(pts[g.index]);
                const bool mounted = isMounted(m_design->mountedGuns(), g.index);
                const bool hov = (m_hoverPoint == g.index);
                const bool drag = (m_dragging == g.index);

                if (mounted) {
                    sf::VertexArray b(sf::PrimitiveType::Lines, 2);
                    b[0] = { { p.x, p.y }, AMBER };
                    b[1] = { { p.x, p.y - 16.f }, AMBER };
                    m_window->draw(b);

                    sf::RectangleShape muzzle({ 5.f, 5.f });
                    muzzle.setPosition({ p.x - 2.5f, p.y - 19.f });
                    muzzle.setFillColor(AMBER);
                    m_window->draw(muzzle);
                }

                // Square handle: square means draggable. Colour means mount state.
                const float s = (drag || hov) ? 9.f : 7.f;
                sf::RectangleShape h({ s, s });
                h.setPosition({ p.x - s * 0.5f, p.y - s * 0.5f });
                h.setFillColor(mounted ? AMBER : (g.valid ? CYAN : sf::Color(90, 100, 112)));
                m_window->draw(h);

                if (hov || drag) {
                    const float rs = s + 8.f;
                    sf::RectangleShape ring({ rs, rs });
                    ring.setPosition({ p.x - rs * 0.5f, p.y - rs * 0.5f });
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineThickness(1.f);
                    ring.setOutlineColor(AMBER);
                    m_window->draw(ring);

                    // The angle is the number that decides whether a gun fits.
                    // Showing it turns an opaque rejection into a rule.
                    char buf[80];
                    std::snprintf(buf, sizeof(buf), "%.0f deg  %s",
                        g.interiorAngleDeg, g.valid ? "MOUNTABLE" : g.reason);
                    sf::Text t(monoFont(), buf, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(g.valid ? GREEN : RED);
                    t.setPosition({ p.x + 14.f, p.y - 6.f });
                    m_window->draw(t);
                }
            }

            // ---- Centre of mass and thrust point: the pair that explains
            //      why an off-centre build spins. ----
            const sf::Vector2f com = L(stats.centreOfMass);
            cross(com, 7.f, sf::Color(255, 255, 255, 190));

            if (stats.engineCount > 0) {
                const sf::Vector2f tp = L(stats.thrustPoint);
                cross(tp, 5.f, AMBER);
                if (std::fabs(stats.lateralOffsetPx) > 1.5f) {
                    sf::VertexArray link(sf::PrimitiveType::Lines, 2);
                    link[0] = { com, sf::Color(255, 48, 0, 200) };
                    link[1] = { tp,  sf::Color(255, 48, 0, 200) };
                    m_window->draw(link);
                }
            }

            // ---- Rejected drag ----
            if (m_rejectFlash > 0.f) {
                const sf::Vector2f p{ m_rejectAt.x - r.x, m_rejectAt.y - r.y };
                const std::uint8_t a = static_cast<std::uint8_t>(220 * m_rejectFlash);
                cross(p, 10.f, sf::Color(255, 48, 0, a));
                sf::Text t(monoFont(), "WOULD BREAK HULL", 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(sf::Color(255, 48, 0, a));
                t.setPosition({ p.x + 14.f, p.y + 6.f });
                m_window->draw(t);
            }

            // ---- Discarded points. A point that vanishes with no explanation
            //      is the most confusing thing a builder UI can do. ----
            if (m_design->validate().wasConcave) {
                sf::Text t(monoFont(), "POINTS INSIDE THE HULL WERE DISCARDED", 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(AMBER);
                t.setPosition({ 12.f, r.h - 24.f });
                m_window->draw(t);
            }
        }
        endClip();
    }

    // ---- STATS panel ------------------------------------------------------
    void drawStatsPanel() {
        const Rect r = statsRect();
        if (!panelChrome(r, "PERFORMANCE", 0.10f, CYAN_MID, false)) return;

        const auto& s = m_design->stats();
        const auto& ref = referenceStats();

        beginClip(r);
        {
            float y = 20.f;
            statBar(y, r.w, "HULL", s.hpMax, ref.hpMax, "%.0f");  y += 40.f;
            statBar(y, r.w, "ENERGY", s.energyMax, ref.energyMax, "%.0f");  y += 40.f;
            statBar(y, r.w, "ACCEL", s.accel, ref.accel, "%.1f");  y += 40.f;
            statBar(y, r.w, "AGILITY", s.agility, ref.agility, "%.2f");  y += 48.f;

            hline(14.f, y - 14.f, r.w - 28.f, CYAN_LOW);

            char buf[240];
            std::snprintf(buf, sizeof(buf),
                "MASS       %.2f kg\n"
                "AREA       %.0f px2\n"
                "THRUST     %.0f N\n"
                "TOP SPEED  %.1f m/s",
                s.massKg, s.areaPx2, s.thrustN, s.topSpeed);
            sf::Text t(monoFont(), buf, 14);
            t.setLetterSpacing(1.4f);
            t.setLineSpacing(1.4f);
            t.setFillColor(TEXT_DIM);
            t.setPosition({ 14.f, y });
            m_window->draw(t);
        }
        endClip();
    }

    /**
     * @brief One stat row: label, value, bar, and a tick at the stock hull.
     *
     * The tick is the whole point. An absolute number means nothing on its
     * own -- what the player needs is "compared to standard", and a marker on
     * the bar says that without a second column of figures.
     */
    void statBar(float y, float panelW, const char* label,
        float value, float refValue, const char* fmt) {
        sf::Text l(monoFont(), label, 14);
        l.setLetterSpacing(1.7f);
        l.setFillColor(TEXT_DIM);
        l.setPosition({ 14.f, y });
        m_window->draw(l);

        char buf[32];
        std::snprintf(buf, sizeof(buf), fmt, value);
        const bool better = (value >= refValue);

        sf::Text v(monoFont(), buf, 18);
        v.setLetterSpacing(1.3f);
        v.setFillColor(better ? GREEN : AMBER);
        v.setPosition({ panelW - v.getGlobalBounds().size.x - 16.f, y - 3.f });
        m_window->draw(v);

        const float bx = 14.f, by = y + 20.f, bw = panelW - 28.f, bh = 6.f;

        sf::RectangleShape track({ bw, bh });
        track.setPosition({ bx, by });
        track.setFillColor(sf::Color(16, 22, 30));
        m_window->draw(track);

        // Scaled so stock sits at 45% of the bar -- leaves visible headroom
        // instead of pinning every good build to the far end.
        const float full = std::max(0.0001f, refValue / 0.45f);
        sf::RectangleShape fill({ bw * std::clamp(value / full, 0.f, 1.f), bh });
        fill.setPosition({ bx, by });
        fill.setFillColor(better ? GREEN : AMBER);
        m_window->draw(fill);

        vline(bx + bw * 0.45f, by - 3.f, bh + 6.f, sf::Color(214, 222, 232, 170));
    }

    // ---- MOUNT panel ------------------------------------------------------
    void drawMountPanel() {
        const Rect r = mountRect();
        if (!panelChrome(r, "MOUNTS", 0.18f, CYAN_MID, false)) return;

        const auto& s = m_design->stats();
        const auto& tune = m_design->tuning();
        const int guns = static_cast<int>(m_design->mountedGuns().size());

        beginClip(r);
        {
            char buf[300];
            std::snprintf(buf, sizeof(buf),
                "WEAPONS    %d mounted / %d slots\n"
                "DRIVES     %d mounted\n"
                "POWERED    %d of %d",
                guns, s.gunSlots, s.engineCount, s.gunsPowered, guns);
            sf::Text t(monoFont(), buf, 14);
            t.setLetterSpacing(1.4f);
            t.setLineSpacing(1.4f);
            t.setFillColor(TEXT);
            t.setPosition({ 14.f, 16.f });
            m_window->draw(t);

            // The energy gate stated plainly. This is the rule that stops a
            // tiny hull carrying four guns, and it is invisible unless said.
            if (s.gunsPowered < guns) {
                std::snprintf(buf, sizeof(buf),
                    "INSUFFICIENT POWER - %.0f NEEDED, %.0f AVAILABLE",
                    static_cast<float>(guns) * tune.weaponEnergyDraw, s.energyMax);
                sf::Text w(monoFont(), buf, 12);
                w.setLetterSpacing(1.3f);
                w.setFillColor(RED);
                w.setPosition({ 14.f, r.h - 26.f });
                m_window->draw(w);
            }
        }
        endClip();
    }

    // ---- STATUS panel -----------------------------------------------------
    void drawStatusPanel() {
        const Rect r = statusRect();
        const ship::ValidationResult v = m_design->validate();
        if (!panelChrome(r, "", 0.26f, v.ok ? CYAN_MID : RED, false)) return;

        const auto& s = m_design->stats();

        beginClip(r);
        {
            sf::Text t(monoFont(), v.message, 15);
            t.setLetterSpacing(1.5f);
            t.setFillColor(v.ok ? GREEN : RED);
            t.setPosition({ 14.f, 12.f });
            m_window->draw(t);

            // Not a rule -- a prediction. Box2D will apply this torque whether
            // or not the editor mentions it.
            if (std::fabs(s.yawAccelDegPerSec2) > 60.f) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "THRUST OFF-AXIS - YAWS %.0f deg/s2",
                    std::fabs(s.yawAccelDegPerSec2));
                sf::Text w(monoFont(), buf, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(AMBER);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);
            }
            else if (m_toastTimer > 0.f) {
                sf::Text w(monoFont(), m_toast, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(CYAN);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);
            }
        }
        endClip();
    }

    /**
     * @brief On-screen buttons for every keyboard shortcut on this screen.
     *
     * The shortcuts stay -- they are faster once learned. But "INS ADD" in a
     * hint bar tells a new player neither which key that is nor what it adds,
     * and there was no way out of this screen with the mouse at all.
     */
    void drawControls(const sf::Vector2f& size) {
        const float y = size.y - 92.f;
        float x = size.x * 0.020f;
        const float w = 150.f, h = 38.f, gap = 10.f;

        if (m_ui.button({ x, y, 190.f, h }, "BACK", false, false, false, 18))
            m_exit = true;
        x += 190.f + gap * 2.f;

        if (m_ui.button({ x, y, w, h },
            m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF",
            m_design->symmetric(), false, false, 16)) {
            m_design->setSymmetric(!m_design->symmetric());
            toast(m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF");
        }
        x += w + gap;

        if (m_ui.button({ x, y, w, h }, "AUTO-MOUNT", false, false, false, 16)) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        x += w + gap * 2.f;

        // Patterns. Named, not numbered -- "1" means nothing on a button.
        static const char* kNames[3] = { "INTERCEPTOR", "STANDARD", "BASTION" };
        for (int i = 0; i < 3; ++i) {
            if (m_ui.button({ x, y, 165.f, h }, kNames[i], false, false, false, 15))
                loadPattern(i + 1);
            x += 165.f + gap;
        }

        // Glossary tab, top-right under the header.
        m_ui.glossaryTab({ size.x - 46.f, size.y * 0.098f, 32.f, 32.f });
        m_ui.drawGlossary("REFIT BAY",
            "Draw a hull. Stats come from its geometry -- a bigger ship is\n"
            "slower because it is heavier, not because of a penalty.\n"
            "Guns mount on sharp forward VERTICES. Drives mount on rear EDGES.",
            { { "LEFT DRAG",   "Move a hull point" },
              { "RIGHT CLICK", "Mount or unmount under the cursor" },
              { "  on a point","Adds or removes a WEAPON" },
              { "  on an edge","Adds or removes a DRIVE" },
              { "INSERT",      "The Insert key - adds a hull point (max 8)" },
              { "=",           "Same as Insert, if your keyboard lacks it" },
              { "DELETE",      "Removes the point under the cursor (min 3)" },
              { "Y",           "Toggle mirror editing" },
              { "1 / 2 / 3",   "Load Interceptor, Standard or Bastion" },
              { "A",           "Fill every valid mount" },
              { "ESC",         "Back to the terminal" },
              { "WHITE CROSS", "Centre of mass" },
              { "AMBER CROSS", "Where thrust is applied" },
              { "RED LINE",    "Those two disagree - the ship will spin" },
              { "GREY POINT",  "Cannot take a gun; hover it to see why" } });
    }

    void drawHints(const sf::Vector2f& size) {
        sf::Text a(monoFont(),
            "LMB DRAG POINT     RMB MOUNT/UNMOUNT     INS ADD     DEL REMOVE", 13);
        a.setLetterSpacing(1.5f);
        a.setFillColor(TEXT_DIM);
        a.setPosition({ size.x * 0.020f, size.y - 46.f });
        m_window->draw(a);

        sf::Text b(monoFont(),
            "Y MIRROR     1-3 PATTERNS     A AUTO-MOUNT     ESC BACK", 13);
        b.setLetterSpacing(1.5f);
        b.setFillColor(TEXT_DIM);
        b.setPosition({ size.x * 0.020f, size.y - 28.f });
        m_window->draw(b);
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    /// Stock hull stats, built once, used as the tick on every bar.
    const ship::ShipStats& referenceStats() const {
        static const ship::ShipStats ref = [] {
            auto d = ship::ShipDesign::stock();
            d.autoMount();
            return d.stats();
        }();
        return ref;
    }

    static bool isMounted(const std::vector<int>& v, int i) {
        return std::find(v.begin(), v.end(), i) != v.end();
    }

    const sf::Font& monoFont() const { return m_mono ? *m_mono : *m_font; }

    void hline(float x, float y, float w, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x, y }, c }; l[1] = { { x + w, y }, c };
        m_window->draw(l);
    }
    void vline(float x, float y, float h, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x, y }, c }; l[1] = { { x, y + h }, c };
        m_window->draw(l);
    }
    void cross(sf::Vector2f p, float s, sf::Color c) const {
        sf::VertexArray v(sf::PrimitiveType::Lines, 4);
        v[0] = { { p.x - s, p.y }, c }; v[1] = { { p.x + s, p.y }, c };
        v[2] = { { p.x, p.y - s }, c }; v[3] = { { p.x, p.y + s }, c };
        m_window->draw(v);
    }

    // ========================================================================
    // STATE
    // ========================================================================

    sf::RenderWindow* m_window = nullptr;
    sf::Font* m_font = nullptr;
    sf::Font* m_mono = nullptr;
    ship::ShipDesign* m_design = nullptr;

    float m_time = 0.f;
    float m_openTimer = 0.f;

    tui::UI m_ui;                ///< shared widget layer (buttons, glossary)

    sf::Vector2f m_mouse;
    int  m_dragging = -1;
    int  m_hoverPoint = -1;
    int  m_hoverEdge = -1;
    bool m_lDown = false, m_rDown = false;

    float        m_rejectFlash = 0.f;
    sf::Vector2f m_rejectAt;

    std::string m_toast;
    float m_toastTimer = 0.f;

    bool m_exit = false;
    bool m_kY = false, m_k1 = false, m_k2 = false, m_k3 = false;
    bool m_kA = false, m_kDel = false, m_kAdd = false, m_kExit = false;

    // NEW: Input lock to prevent first-frame bounce
    float m_inputLock = 0.f;
};