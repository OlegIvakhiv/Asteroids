/**
 * @file RefitSystem.hpp
 * @brief The refit bay: class frame, hitbox editor, decorative model, mounts, readout.
 *
 * Screen-space only. Owns no ship data -- it edits a ship::ShipDesign that
 * lives in SystemManager and persists between runs. Polls its own mouse and
 * keyboard so game.cpp only has to route the state.
 *
 * ============================================================================
 * TWO LAYERS, TWO MODES
 * ============================================================================
 *   HULL   the hitbox: <= 8 convex points, what Box2D collides with. Guns and
 *          drives mount here. Stats come from here.
 *   MODEL  what the player sees: any shape, up to 64 points. Must cover the
 *          hull, stay inside the dim envelope, and use <= 150% of its area.
 *          A spike above a gun is that gun's muzzle.
 *
 * Colour means ROLE, everywhere on this screen:
 *   CYAN    plasma gun          VIOLET  Rift (spinal) mount
 *   AMBER   drive               RED     refused / invalid
 *
 * ============================================================================
 * INTERACTION
 * ============================================================================
 *   M / Tab         switch HULL <-> MODEL
 *   Left drag       move a point (of the layer being edited)
 *   Right click     HULL:  fit/remove the mount under the cursor
 *                   MODEL: on a point removes it, anywhere else adds one
 *   R               HULL: Rift mount / toggle SHARED-DEDICATED
 *   Insert / =      add a point at the cursor      Delete  remove hovered point
 *   Y               mirror editing                 1 2 3   LIGHT / MEDIUM / HEAVY
 *   A               HULL: auto-mount               Esc     back
 *
 * The view zooms to fit the class frame and the model envelope, and holds
 * still while a drag is in progress.
 *
 * @author Oleg Ivakhiv
 * @version 3.0
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/TerminalUI.hpp"
#include "utils/ClassTuning.hpp"
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
        m_lua = ctx.lua;
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
        m_rejectText.clear();
        m_toast.clear();
        m_toastTimer = 0.f;

        // Prime EVERY edge-detector from the current physical state. Entering
        // this screen means Enter is still held from the row that opened it;
        // an unprimed detector reads that hold as a fresh press and exits on
        // frame one.
        m_kExit = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        m_kY = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Y);
        m_k1 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num1);
        m_k2 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num2);
        m_k3 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num3);
        m_kA = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A);
        m_kR = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::R);
        m_kM = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
        m_hoverDecor = -1;
        m_viewInit = false;
        m_kDel = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Delete);
        m_kAdd = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        m_lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        m_rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);

        m_inputLock = 0.12f;   // belt and braces against fast re-entry

        m_ui.primeInput();
        m_ui.resetReveal();
    }

    bool wantsExit() const { return m_exit; }
    void clearExit() { m_exit = false; }

    void update(float dt) override {
        if (!m_window || !m_font || !m_design) return;
        dt = std::clamp(dt, 0.f, 0.1f);
        m_time += dt;
        if (m_openTimer < 3.f) m_openTimer += dt;
        m_rejectFlash = std::max(0.f, m_rejectFlash - dt * 1.6f);
        if (m_toastTimer > 0.f) m_toastTimer -= dt;

        m_ui.begin(dt);
        updateView(dt);

        m_inputLock = std::max(0.f, m_inputLock - dt);
        if (m_inputLock <= 0.f && !m_ui.glossaryOpen()) handleInput();
        draw();
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
    /// The Rift's own colour -- WeaponSystem's bolt, charge sparks and rings.
    static inline const sf::Color VIOLET{ 175, 95, 255 };

    static constexpr float OPEN_DUR = 0.30f;
    static constexpr float HANDLE_R = 7.f;

    enum class Mode { Hull, Model };

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

    /// Screen view matching the CURRENT framebuffer. Rebuilt every frame --
    /// getDefaultView() is frozen at creation size.
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

    Rect hullRect()   const { return frac(0.015f, 0.105f, 0.600f, 0.775f); }
    Rect statsRect()  const { return frac(0.632f, 0.112f, 0.353f, 0.420f); }
    Rect mountRect()  const { return frac(0.632f, 0.560f, 0.353f, 0.180f); }
    Rect statusRect() const { return frac(0.632f, 0.768f, 0.353f, 0.112f); }

    sf::Vector2f toLocal(sf::Vector2f screen) const {
        const Rect r = hullRect();
        return { (screen.x - r.cx()) / m_zoom + m_viewCenter.x,
                 (screen.y - r.cy()) / m_zoom + m_viewCenter.y };
    }
    sf::Vector2f toScreen(sf::Vector2f local) const {
        const Rect r = hullRect();
        return { r.cx() + (local.x - m_viewCenter.x) * m_zoom,
                 r.cy() + (local.y - m_viewCenter.y) * m_zoom };
    }

    /**
     * @brief Fit the view to the class frame plus the model envelope.
     *
     * Symmetric about x = 0 so the centreline stays in the middle of the
     * panel. Frozen during a drag -- a view that rescales under the cursor
     * makes the point you are holding run away from it.
     */
    void updateView(float dt) {
        const auto& spec = m_design->spec();
        float maxX = spec.halfWidthPx, minY = spec.foreYPx, maxY = spec.aftYPx;
        const auto grow = [&](const std::vector<sf::Vector2f>& pts) {
            for (const auto& p : pts) {
                maxX = std::max(maxX, std::fabs(p.x));
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            };
        grow(m_design->envelope());
        grow(m_design->decor());

        const Rect r = hullRect();
        const float w = maxX * 2.f * 1.12f + 8.f;
        const float h = (maxY - minY) * 1.12f + 16.f;   // room for tags above spikes
        const float zoomT = std::clamp(std::min(r.w / w, r.h / h), 2.5f, 10.f);
        const sf::Vector2f centreT{ 0.f, (minY + maxY) * 0.5f - 2.f };

        if (!m_viewInit) { m_zoom = zoomT; m_viewCenter = centreT; m_viewInit = true; return; }
        if (m_dragging >= 0) return;
        const float k = 1.f - std::exp(-7.f * dt);
        m_zoom += (zoomT - m_zoom) * k;
        m_viewCenter += (centreT - m_viewCenter) * k;
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
        const bool model = (m_mode == Mode::Model);

        updateHover(inPanel);

        // ---- Mode ----
        const bool mKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
        if (mKey && !m_kM) setMode(model ? Mode::Hull : Mode::Model);
        m_kM = mKey;

        // ---- Left drag ----
        const int hovered = model ? m_hoverDecor : m_hoverPoint;
        if (lDown && !m_lDown && inPanel && hovered >= 0) m_dragging = hovered;
        if (!lDown) m_dragging = -1;

        if (m_dragging >= 0 && lDown) {
            if (model) {
                const sf::Vector2f target = snapFine(toLocal(m_mouse));
                if (m_dragging < m_design->decorPointCount()
                    && target != m_design->decor()[m_dragging]
                    && !m_design->moveDecorPoint(m_dragging, target))
                    reject(m_design->lastRejectReason());
            }
            else {
                const sf::Vector2f target = snap(toLocal(m_mouse));
                if (target != m_design->points()[m_dragging]
                    && !m_design->movePoint(m_dragging, target))
                    reject(m_design->lastRejectReason());
            }
        }

        // ---- Right click ----
        if (rDown && !m_rDown && inPanel) {
            if (model) {
                if (m_hoverDecor >= 0) {
                    if (!m_design->removeDecorPoint(m_hoverDecor)) reject(m_design->lastRejectReason());
                    m_hoverDecor = -1;
                }
                else if (!m_design->addDecorPoint(snapFine(toLocal(m_mouse))))
                    reject(m_design->lastRejectReason());
            }
            else {
                if (m_hoverPoint >= 0)     toggleGun(m_hoverPoint);
                else if (m_hoverEdge >= 0) toggleEngine(m_hoverEdge);
            }
        }

        // ---- Keys ----
        if (keyEdge(sf::Keyboard::Key::Y, m_kY)) toggleMirror();
        if (keyEdge(sf::Keyboard::Key::Num1, m_k1)) loadClass(ship::HullClass::Light);
        if (keyEdge(sf::Keyboard::Key::Num2, m_k2)) loadClass(ship::HullClass::Medium);
        if (keyEdge(sf::Keyboard::Key::Num3, m_k3)) loadClass(ship::HullClass::Heavy);

        if (keyEdge(sf::Keyboard::Key::A, m_kA) && !model) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        if (keyEdge(sf::Keyboard::Key::R, m_kR) && !model && m_hoverPoint >= 0) assignRift(m_hoverPoint);

        if (keyEdge(sf::Keyboard::Key::Delete, m_kDel)) {
            if (model && m_hoverDecor >= 0) {
                if (!m_design->removeDecorPoint(m_hoverDecor)) reject(m_design->lastRejectReason());
                m_hoverDecor = -1;
            }
            else if (!model && m_hoverPoint >= 0) {
                if (!m_design->removePoint(m_hoverPoint)) reject(m_design->lastRejectReason());
                m_hoverPoint = -1;
            }
        }

        const bool addKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        if (addKey && !m_kAdd && inPanel) {
            const bool ok = model ? m_design->addDecorPoint(snapFine(toLocal(m_mouse)))
                : m_design->addPoint(snap(toLocal(m_mouse)));
            if (!ok) reject(m_design->lastRejectReason());
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

    /// Hull grid: 2 units, so mirrored pairs land on matching coordinates.
    static sf::Vector2f snap(sf::Vector2f v) {
        return { std::round(v.x * 0.5f) * 2.f, std::round(v.y * 0.5f) * 2.f };
    }
    /// Model grid: 1 unit. Detail work needs the finer step.
    static sf::Vector2f snapFine(sf::Vector2f v) {
        return { std::round(v.x), std::round(v.y) };
    }

    void updateHover(bool inPanel) {
        m_hoverPoint = -1;
        m_hoverEdge = -1;
        m_hoverDecor = -1;
        if (!inPanel || m_dragging >= 0) return;

        if (m_mode == Mode::Model) {
            const auto& d = m_design->decor();
            float best = 9.f;
            for (int i = 0; i < static_cast<int>(d.size()); ++i) {
                const sf::Vector2f sp = toScreen(d[i]);
                const float dist = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
                if (dist < best) { best = dist; m_hoverDecor = i; }
            }
            return;
        }

        const auto& pts = m_design->points();
        float best = HANDLE_R + 3.f;
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            const sf::Vector2f sp = toScreen(pts[i]);
            const float d = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
            if (d < best) { best = d; m_hoverPoint = i; }
        }
        if (m_hoverPoint >= 0) return;

        best = HANDLE_R + 5.f;
        for (const auto& e : m_design->engineSlots()) {
            const sf::Vector2f sp = toScreen(e.position);
            const float d = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
            if (d < best) { best = d; m_hoverEdge = e.index; }
        }
    }

    void setMode(Mode m) {
        m_mode = m;
        m_dragging = -1;
        toast(m == Mode::Model ? "MODEL - SHAPE WHAT THE SHIP LOOKS LIKE" : "HULL - HITBOX AND MOUNTS");
    }

    // ---- Actions ----------------------------------------------------------

    void toggleGun(int vertexIndex) {
        if (m_design->isGunMounted(vertexIndex)) {
            m_design->unmountGun(vertexIndex);
            toast("WEAPON REMOVED");
        }
        else if (m_design->mountGun(vertexIndex)) {
            toast(m_design->stats().spinalVertex == vertexIndex ? "WEAPON FITTED - RIFT MOUNT"
                : "WEAPON FITTED - PLASMA");
        }
        else {
            reject(m_design->lastRejectReason());
        }
    }

    void toggleEngine(int edgeIndex) {
        if (m_design->isEngineMounted(edgeIndex)) {
            m_design->unmountEngine(edgeIndex);
            toast("DRIVE REMOVED");
        }
        else if (m_design->mountEngine(edgeIndex)) {
            toast("DRIVE FITTED");
        }
        else {
            reject(m_design->lastRejectReason());
        }
    }

    void assignRift(int vertexIndex) {
        const bool wasSpinal = (m_design->stats().spinalVertex == vertexIndex);
        if (!m_design->assignRift(vertexIndex)) { reject(m_design->lastRejectReason()); return; }
        if (!wasSpinal) toast("RIFT MOUNT MOVED");
        else toast(m_design->stats().riftShared ? "RIFT SHARED - FIRES PLASMA TOO"
            : "RIFT DEDICATED");
    }

    /**
     * @brief Load a class's preset hull.
     *
     * The tonnage bands do not overlap, so a hull can never "fit" a different
     * class -- switching always starts from that class's pattern. Mirror mode
     * carries over; a hull that snaps to a different shape than the button
     * promised would be worse than a clean preset.
     */
    void loadClass(ship::HullClass c) {
        const bool sym = m_design->symmetric();
        *m_design = ship::ShipDesign::preset(c);
        if (!sym) m_design->setSymmetric(false);
        const ship::HullClassSpec& s = ship::classSpec(c);
        toast(std::string(s.name) + " FRAME - " + s.pattern + " PATTERN");
    }

    void toggleMirror() {
        m_design->setSymmetric(!m_design->symmetric());
        toast(m_design->symmetric() ? "MIRROR ON - MOUNTS CLEARED, RE-MOUNT" : "MIRROR OFF");
        if (m_design->symmetric() && m_design->mountedGuns().empty()) m_design->autoMount();
    }

    void toast(const std::string& s) { m_toast = s; m_toastTimer = 2.2f; }

    void reject(const char* why) {
        m_rejectFlash = 1.f;
        m_rejectAt = m_mouse;
        m_rejectText = (why && *why) ? why : "REFUSED";
    }

    // ========================================================================
    // CLIPPING
    // ========================================================================

    /// Delegates to tui::UI so clipped widgets hit-test in the right space.
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
        title.setPosition({ size.x * 0.015f, size.y * 0.028f });
        m_window->draw(title);

        // ---- Layer tabs ----
        float tx = size.x * 0.015f + title.getGlobalBounds().size.x + 36.f;
        const float ty = size.y * 0.030f, th = 34.f;
        if (m_ui.button({ tx, ty, 120.f, th }, "HULL", m_mode == Mode::Hull, false, false, 16))
            if (m_mode != Mode::Hull) setMode(Mode::Hull);
        tx += 128.f;
        if (m_ui.button({ tx, ty, 120.f, th }, "MODEL", m_mode == Mode::Model, false, false, 16))
            if (m_mode != Mode::Model) setMode(Mode::Model);
        tx += 150.f;

        std::string sub = std::string(m_design->spec().name) + " FRAME";
        sf::Text cls(monoFont(), sub, 15);
        cls.setLetterSpacing(1.8f);
        cls.setFillColor(AMBER);
        cls.setPosition({ tx, size.y * 0.042f });
        m_window->draw(cls);

        sf::Text mir(monoFont(), m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF", 15);
        mir.setLetterSpacing(1.8f);
        mir.setFillColor(m_design->symmetric() ? CYAN : AMBER);
        mir.setPosition({ tx + cls.getGlobalBounds().size.x + 30.f, size.y * 0.042f });
        m_window->draw(mir);

        const ship::ValidationResult v = m_design->validate();
        sf::Text st(monoFont(), v.ok ? "HULL  AIRWORTHY" : "HULL  REJECTED", 16);
        st.setLetterSpacing(1.6f);
        st.setFillColor(v.ok ? GREEN : RED);
        st.setPosition({ size.x - st.getGlobalBounds().size.x - size.x * 0.015f,
                         size.y * 0.042f });
        m_window->draw(st);

        hline(size.x * 0.012f, size.y * 0.092f, size.x * 0.976f, CYAN_LOW);
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

    // ---- EDITOR panel -----------------------------------------------------
    void drawHullPanel() {
        const Rect r = hullRect();
        const bool model = (m_mode == Mode::Model);
        if (!panelChrome(r, model ? "MODEL" : "HULL GEOMETRY", 0.00f, model ? VIOLET : CYAN_MID, true)) return;

        const auto& pts = m_design->points();
        const auto& stats = m_design->stats();
        const auto& spec = m_design->spec();
        const auto& dc = m_design->decorCheck();
        const int n = static_cast<int>(pts.size());
        const float Z = m_zoom;

        beginClip(r);
        {
            const float cx = r.w * 0.5f, cy = r.h * 0.5f;
            const auto L = [&](sf::Vector2f v) {
                return sf::Vector2f{ cx + (v.x - m_viewCenter.x) * Z, cy + (v.y - m_viewCenter.y) * Z };
                };

            // ---- Grid, one line per 10 design units, only what is visible ----
            {
                sf::VertexArray grid(sf::PrimitiveType::Lines);
                const sf::Color gc(16, 27, 35);
                const sf::Vector2f lo = { m_viewCenter.x - cx / Z, m_viewCenter.y - cy / Z };
                const sf::Vector2f hi = { m_viewCenter.x + cx / Z, m_viewCenter.y + cy / Z };
                for (float g = std::floor(lo.x / 10.f) * 10.f; g <= hi.x; g += 10.f) {
                    const float x = L({ g, 0.f }).x;
                    grid.append({ { x, 0.f }, gc }); grid.append({ { x, r.h }, gc });
                }
                for (float g = std::floor(lo.y / 10.f) * 10.f; g <= hi.y; g += 10.f) {
                    const float y = L({ 0.f, g }).y;
                    grid.append({ { 0.f, y }, gc }); grid.append({ { r.w, y }, gc });
                }
                m_window->draw(grid);
            }

            // ---- Class frame (the hitbox's box) ----
            {
                const sf::Vector2f a = L({ -spec.halfWidthPx, spec.foreYPx });
                const sf::Vector2f b = L({ spec.halfWidthPx, spec.aftYPx });
                const bool breached = m_rejectFlash > 0.f && m_rejectText.find("FRAME") != std::string::npos;
                const std::uint8_t base = model ? 30 : 60;
                const sf::Color fc = breached
                    ? sf::Color(255, 48, 0, static_cast<std::uint8_t>(90 + 140 * m_rejectFlash))
                    : sf::Color(255, 214, 0, base);

                sf::RectangleShape frame({ b.x - a.x, b.y - a.y });
                frame.setPosition(a);
                frame.setFillColor(sf::Color(255, 214, 0, model ? 0 : 6));
                frame.setOutlineThickness(1.f);
                frame.setOutlineColor(fc);
                m_window->draw(frame);

                const sf::Color tc(255, 214, 0, model ? 70 : 170);
                const float t = 12.f;
                sf::VertexArray ticks(sf::PrimitiveType::Lines);
                const auto tick = [&](sf::Vector2f p, float sx, float sy) {
                    ticks.append({ p, tc }); ticks.append({ { p.x + t * sx, p.y }, tc });
                    ticks.append({ p, tc }); ticks.append({ { p.x, p.y + t * sy }, tc });
                    };
                tick(a, 1.f, 1.f);             tick({ b.x, a.y }, -1.f, 1.f);
                tick({ a.x, b.y }, 1.f, -1.f); tick(b, -1.f, -1.f);
                m_window->draw(ticks);

                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s FRAME", spec.name);
                sf::Text ft(monoFont(), buf, 11);
                ft.setLetterSpacing(1.8f);
                ft.setFillColor(tc);
                ft.setPosition({ a.x + 6.f, a.y + 4.f });
                m_window->draw(ft);
            }

            // ---- Model envelope: where the model may reach ----
            if (model) {
                const auto env = m_design->envelope();
                dashedPolygon(env, L, sf::Color(175, 95, 255, 110), 6.f, 5.f);
                sf::Text et(monoFont(), "MODEL LIMIT", 11);
                et.setLetterSpacing(1.8f);
                et.setFillColor(sf::Color(175, 95, 255, 140));
                const sf::Vector2f top = L(env[0]);
                sf::Vector2f topmost = top;
                for (const auto& p : env) { const sf::Vector2f q = L(p); if (q.y < topmost.y) topmost = q; }
                et.setPosition({ topmost.x + 10.f, topmost.y - 4.f });
                m_window->draw(et);
            }

            // ---- Centreline ----
            {
                const float ax = L({ 0.f, 0.f }).x;
                sf::VertexArray axis(sf::PrimitiveType::Lines, 2);
                const sf::Color ac = m_design->symmetric()
                    ? sf::Color(40, 245, 255, 70) : sf::Color(60, 66, 78, 70);
                axis[0] = { { ax, 0.f }, ac };
                axis[1] = { { ax, r.h }, ac };
                m_window->draw(axis);

                sf::Text fwd(monoFont(), "FORWARD", 12);
                fwd.setLetterSpacing(1.8f);
                fwd.setFillColor(TEXT_DEAD);
                fwd.setPosition({ ax + 12.f, 12.f });
                m_window->draw(fwd);

                sf::VertexArray arrow(sf::PrimitiveType::Lines, 6);
                arrow[0] = { { ax, 42.f }, TEXT_DEAD }; arrow[1] = { { ax, 14.f }, TEXT_DEAD };
                arrow[2] = { { ax, 14.f }, TEXT_DEAD }; arrow[3] = { { ax - 5.f, 23.f }, TEXT_DEAD };
                arrow[4] = { { ax, 14.f }, TEXT_DEAD }; arrow[5] = { { ax + 5.f, 23.f }, TEXT_DEAD };
                m_window->draw(arrow);
            }

            // ---- The two layers. The one being edited is drawn on top, bright. ----
            const auto& decor = m_design->decor();
            const bool showModel = m_design->decorAuthored() || model;
            const sf::Color modelLine = dc.ok ? (model ? CYAN : sf::Color(40, 245, 255, 90)) : RED;

            if (model) {
                // Model solid, hitbox as a ghost on top of it.
                fillPolygon(m_design->decor(), L, sf::Color(20, 60, 90, 150));
                strokePolygon(decor, L, modelLine, 2.f);
                if (n >= 3) dashedPolygon(pts, L, sf::Color(40, 245, 255, 150), 4.f, 4.f);
            }
            else {
                // Model as a silhouette behind the hull you are editing.
                if (showModel && decor.size() >= 3) {
                    fillPolygon(decor, L, sf::Color(20, 60, 90, 45));
                    strokePolygon(decor, L, dc.ok ? sf::Color(40, 245, 255, 55) : sf::Color(255, 48, 0, 120), 1.f);
                }
                if (n >= 3) {
                    fillPolygon(pts, L, sf::Color(20, 60, 90, 130));
                    strokePolygon(pts, L, CYAN, 2.f);
                }
            }

            // ---- Drives: the edge itself lights up. No nozzle art. ----
            for (const auto& e : m_design->engineSlots()) {
                const bool mounted = m_design->isEngineMounted(e.index);
                const bool hov = !model && (m_hoverEdge == e.index);
                if (!mounted && !hov && (model || !e.valid)) continue;

                const sf::Vector2f a = L(pts[e.index]);
                const sf::Vector2f b = L(pts[(e.index + 1) % n]);
                sf::Color c;
                float w;
                if (mounted) { c = model ? sf::Color(255, 214, 0, 90) : AMBER; w = model ? 2.f : 4.f; }
                else if (e.valid) { c = sf::Color(255, 214, 0, hov ? 150 : 45); w = hov ? 3.f : 1.5f; }
                else { c = sf::Color(255, 48, 0, 150); w = 2.f; }
                thickLine(a, b, c, w);

                if (hov) {
                    const sf::Vector2f mid = (a + b) * 0.5f;
                    sf::Text t(monoFont(), e.valid ? (mounted ? "DRIVE  RMB REMOVE" : "DRIVE  RMB FIT")
                        : e.reason, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(e.valid ? AMBER : RED);
                    t.setPosition({ mid.x + e.outward.x * 14.f + 6.f, mid.y + e.outward.y * 14.f });
                    m_window->draw(t);
                }
            }

            // ---- Guns ----
            for (const auto& g : m_design->gunSlots()) {
                const sf::Vector2f p = L(pts[g.index]);
                const bool mounted = m_design->isGunMounted(g.index);
                const bool spinal = mounted && stats.spinalVertex == g.index;
                const sf::Color role = spinal ? VIOLET : CYAN;

                if (model) {
                    // Show where each gun's bolts will leave: the model spike tip.
                    if (!mounted) continue;
                    const sf::Vector2f mz = L(m_design->muzzleFor(g.index));
                    dashedLine(p, mz, sf::Color(role.r, role.g, role.b, 150), 3.f, 3.f);
                    diamond(p, 3.f, sf::Color(role.r, role.g, role.b, 170));
                    cross(mz, 5.f, role);
                    continue;
                }

                const bool hov = (m_hoverPoint == g.index);
                const bool drag = (m_dragging == g.index);

                if (mounted) {
                    // A plain spike: reads as "a gun points this way", nothing more.
                    const float len = spinal ? 18.f : 13.f, half = spinal ? 5.f : 4.f;
                    sf::ConvexShape spike(3);
                    spike.setPoint(0, { p.x - half, p.y - 2.f });
                    spike.setPoint(1, { p.x + half, p.y - 2.f });
                    spike.setPoint(2, { p.x, p.y - len });
                    spike.setFillColor(role);
                    m_window->draw(spike);

                    if (spinal) {
                        sf::Text tag(monoFont(), stats.riftShared ? "RIFT+PLASMA" : "RIFT", 11);
                        tag.setLetterSpacing(1.6f);
                        tag.setFillColor(VIOLET);
                        tag.setPosition({ p.x - tag.getGlobalBounds().size.x * 0.5f, p.y - len - 18.f });
                        m_window->draw(tag);
                    }
                }

                const float sz = (drag || hov) ? 9.f : 7.f;
                sf::RectangleShape h({ sz, sz });
                h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                h.setFillColor(mounted ? role : (g.valid ? CYAN_MID : sf::Color(90, 100, 112)));
                m_window->draw(h);

                if (hov || drag) {
                    const float rs = sz + 8.f;
                    sf::RectangleShape ringSq({ rs, rs });
                    ringSq.setPosition({ p.x - rs * 0.5f, p.y - rs * 0.5f });
                    ringSq.setFillColor(sf::Color::Transparent);
                    ringSq.setOutlineThickness(1.f);
                    ringSq.setOutlineColor(AMBER);
                    m_window->draw(ringSq);

                    char buf[96];
                    if (!g.valid)
                        std::snprintf(buf, sizeof(buf), "%.0f deg  %s", g.interiorAngleDeg, g.reason);
                    else if (spinal)
                        std::snprintf(buf, sizeof(buf), "RIFT MOUNT  R: %s",
                            stats.gunCount > 1 ? (stats.riftShared ? "DEDICATE" : "SHARE") : "LONE GUN");
                    else if (mounted)
                        std::snprintf(buf, sizeof(buf), "PLASMA  R: MAKE RIFT MOUNT");
                    else
                        std::snprintf(buf, sizeof(buf), "%.0f deg  RMB FIT WEAPON", g.interiorAngleDeg);
                    sf::Text t(monoFont(), buf, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(g.valid ? (spinal ? VIOLET : GREEN) : RED);
                    t.setPosition({ p.x + 14.f, p.y - 6.f });
                    m_window->draw(t);
                }
            }

            // ---- Model handles ----
            if (model) {
                for (int i = 0; i < static_cast<int>(decor.size()); ++i) {
                    const sf::Vector2f p = L(decor[i]);
                    const bool hov = (m_hoverDecor == i), drag = (m_dragging == i);
                    const float sz = (hov || drag) ? 8.f : 5.f;
                    sf::RectangleShape h({ sz, sz });
                    h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                    h.setFillColor(hov || drag ? AMBER : CYAN);
                    m_window->draw(h);
                }
                if (m_hoverDecor >= 0 && m_dragging < 0) {
                    const sf::Vector2f p = L(decor[m_hoverDecor]);
                    sf::Text t(monoFont(), "DRAG MOVE   RMB REMOVE", 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(AMBER);
                    t.setPosition({ p.x + 12.f, p.y - 6.f });
                    m_window->draw(t);
                }
            }

            // ---- Centre of mass and thrust point (hull only) ----
            if (!model) {
                const sf::Vector2f com = L(stats.centreOfMass);
                cross(com, 7.f, sf::Color(255, 255, 255, 190));
                if (stats.engineCount > 0) {
                    const sf::Vector2f tp = L(stats.thrustPoint);
                    cross(tp, 5.f, AMBER);
                    if (std::fabs(stats.lateralOffsetPx) > 1.5f) thickLine(com, { tp.x, com.y }, sf::Color(255, 48, 0, 200), 1.f);
                }
            }

            // ---- Refused edit ----
            if (m_rejectFlash > 0.f) {
                const sf::Vector2f p{ m_rejectAt.x - r.x, m_rejectAt.y - r.y };
                const std::uint8_t a = static_cast<std::uint8_t>(230 * std::min(1.f, m_rejectFlash * 1.5f));
                cross(p, 10.f, sf::Color(255, 48, 0, a));
                sf::Text t(monoFont(), m_rejectText, 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(sf::Color(255, 48, 0, a));
                t.setPosition({ p.x + 14.f, p.y + 6.f });
                m_window->draw(t);
            }

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
        const auto& ref = ship::ShipDesign::reference();
        const auto ratio = [](float a, float b) { return (b > 1e-6f) ? a / b : 0.f; };

        const ship::ClassFeel feel = m_lua ? ship::classFeel(*m_lua, m_design->hullClass())
            : ship::defaultFeel(m_design->hullClass());
        const float regen = ratio(s.regenFraction, ref.regenFraction) * feel.regen;

        beginClip(r);
        {
            float y = 14.f;
            statBar(y, r.w, "HULL", s.hpMax, ref.hpMax, "%.0f");                      y += 33.f;
            statBar(y, r.w, "ENERGY", s.energyMax, ref.energyMax, "%.0f");            y += 33.f;
            statBar(y, r.w, "REGEN", regen, 1.f, "x%.2f");                            y += 33.f;
            statBar(y, r.w, "THRUST", m_design->mobilityRatio(), 1.f, "x%.2f");       y += 33.f;
            statBar(y, r.w, "AGILITY", m_design->agilityRatio(), 1.f, "x%.2f");       y += 40.f;

            hline(14.f, y - 10.f, r.w - 28.f, CYAN_LOW);

            // Class tier. Labels and values in two columns so no line needs
            // an abbreviation to fit the panel.
            char v[7][96];
            const float mf = m_design->mobilityFactor();
            std::snprintf(v[0], 96, "x%.2f     REVERSE  x%.2f",
                ratio(s.strafeAccel, ref.accel) * mf, ratio(s.reverseAccel, ref.accel) * mf);
            std::snprintf(v[1], 96, "%.0f px,  %.2f s i-frames", feel.dashDistancePx, feel.dashIframes);
            std::snprintf(v[2], 96, "%.2f s    COST  %.0f EN", feel.dashRecovery, feel.dashEnergyCost);
            std::snprintf(v[3], 96, "capacity x%.2f   cool x%.2f", feel.heatCapacity, feel.heatCool);
            std::snprintf(v[4], 96, "vent x%.2f   parry x%.2f", feel.qteWindow, feel.parryWindow);
            if (feel.poise > 0.f)
                std::snprintf(v[5], 96, "%.0f   knockback x%.2f%s", feel.poise, feel.knockback,
                    feel.hyperarmor > 0.5f ? "   ARMORED DODGE" : "");
            else
                std::snprintf(v[5], 96, "none - every heavy hit tumbles");
            if (feel.damageReduction > 0.f || feel.shoulderBash > 0.5f || feel.ramming > 0.5f)
                std::snprintf(v[6], 96, "-%.0f%%  dodge -%.0f%%%s%s", feel.damageReduction * 100.f,
                    feel.hyperarmorReduction * 100.f,
                    feel.shoulderBash > 0.5f ? "  BASH" : "", feel.ramming > 0.5f ? "  RAM" : "");
            else
                std::snprintf(v[6], 96, "none");
            static const char* labels[7] = { "STRAFE", "DODGE", "RECOVER", "HEAT", "WINDOWS", "POISE", "ARMOUR" };

            for (int i = 0; i < 7; ++i) {
                const float ly = y + static_cast<float>(i) * 16.f;
                sf::Text l(monoFont(), labels[i], 12);
                l.setLetterSpacing(1.4f);
                l.setFillColor(TEXT_DIM);
                l.setPosition({ 14.f, ly });
                m_window->draw(l);

                sf::Text t(monoFont(), v[i], 12);
                t.setLetterSpacing(1.2f);
                t.setFillColor(TEXT);
                t.setPosition({ 104.f, ly });
                m_window->draw(t);
            }
        }
        endClip();
    }

    /**
     * @brief One stat row: label, value, bar, and a tick at the reference build.
     *
     * The tick is the whole point: "compared to standard" without a second
     * column of figures.
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
        const bool better = (value >= refValue * 0.995f);

        sf::Text v(monoFont(), buf, 17);
        v.setLetterSpacing(1.3f);
        v.setFillColor(better ? GREEN : AMBER);
        v.setPosition({ panelW - v.getGlobalBounds().size.x - 16.f, y - 3.f });
        m_window->draw(v);

        const float bx = 14.f, by = y + 20.f, bw = panelW - 28.f, bh = 6.f;

        sf::RectangleShape track({ bw, bh });
        track.setPosition({ bx, by });
        track.setFillColor(sf::Color(16, 22, 30));
        m_window->draw(track);

        const float full = std::max(0.0001f, refValue / 0.45f);
        sf::RectangleShape fill({ bw * std::clamp(value / full, 0.f, 1.f), bh });
        fill.setPosition({ bx, by });
        fill.setFillColor(better ? GREEN : AMBER);
        m_window->draw(fill);

        vline(bx + bw * 0.45f, by - 3.f, bh + 6.f, sf::Color(214, 222, 232, 170));
    }

    // ---- LOADOUT panel ----------------------------------------------------
    void drawMountPanel() {
        const Rect r = mountRect();
        if (!panelChrome(r, "LOADOUT", 0.18f, CYAN_MID, false)) return;

        const auto& s = m_design->stats();
        const auto& spec = m_design->spec();
        const auto& tune = m_design->tuning();

        beginClip(r);
        {
            // ---- Reactor: one cell per unit. Guns cyan, drives amber, free green. ----
            const bool over = s.reactorUsed > s.reactorUnits;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "REACTOR  %d / %d", s.reactorUsed, s.reactorUnits);
            sf::Text rt(monoFont(), buf, 14);
            rt.setLetterSpacing(1.6f);
            rt.setFillColor(over ? RED : TEXT);
            rt.setPosition({ 14.f, 10.f });
            m_window->draw(rt);

            const int free = std::max(0, s.reactorUnits - s.reactorUsed);
            const char* regenNote = (free == 0) ? "NO SPARE POWER - REGEN AT FLOOR" : "SPARE UNITS FEED REGEN";
            sf::Text rn(monoFont(), regenNote, 11);
            rn.setLetterSpacing(1.3f);
            rn.setFillColor(free == 0 ? AMBER : TEXT_DIM);
            rn.setPosition({ r.w - rn.getGlobalBounds().size.x - 14.f, 13.f });
            m_window->draw(rn);

            const int cells = std::max(s.reactorUnits, s.reactorUsed);
            const float gap = 3.f;
            const float cw = std::min(28.f, (r.w - 28.f - gap * static_cast<float>(cells - 1))
                / static_cast<float>(std::max(1, cells)));
            const int gunCells = s.gunCount * tune.gunUpkeepUnits;
            for (int i = 0; i < cells; ++i) {
                sf::RectangleShape c({ cw, 12.f });
                c.setPosition({ 14.f + static_cast<float>(i) * (cw + gap), 34.f });
                if (i >= s.reactorUnits)          c.setFillColor(RED);
                else if (i < gunCells)            c.setFillColor(CYAN_MID);
                else if (i < s.reactorUsed)       c.setFillColor(sf::Color(200, 160, 0));
                else {
                    c.setFillColor(sf::Color(10, 30, 18));
                    c.setOutlineThickness(1.f);
                    c.setOutlineColor(GREEN);
                }
                m_window->draw(c);
            }

            // ---- Counts against class caps ----
            std::snprintf(buf, sizeof(buf), "WEAPONS %d/%d   DRIVES %d/%d   TONNAGE %.0f/%.0f",
                s.gunCount, spec.maxGuns, s.engineCount, spec.maxEngines, s.areaPx2, spec.maxAreaPx2);
            sf::Text ct(monoFont(), buf, 13);
            ct.setLetterSpacing(1.4f);
            ct.setFillColor(TEXT);
            ct.setPosition({ 14.f, 56.f });
            m_window->draw(ct);

            // ---- Rift mode: the one rule a player has to understand ----
            const char* rift;
            sf::Color rc;
            if (s.gunCount == 0) { rift = "RIFT     NO WEAPON TO FIRE FROM";               rc = RED; }
            else if (s.riftShared) { rift = "RIFT     SHARED - CHARGING STOPS PLASMA";       rc = AMBER; }
            else { rift = "RIFT     DEDICATED - PLASMA FIRES WHILE IT CHARGES"; rc = VIOLET; }
            sf::Text rf(monoFont(), rift, 13);
            rf.setLetterSpacing(1.4f);
            rf.setFillColor(rc);
            rf.setPosition({ 14.f, 80.f });
            m_window->draw(rf);
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

            const auto& dc = m_design->decorCheck();
            if (m_mode == Mode::Model || (m_design->decorAuthored() && !dc.ok)) {
                // The model's rules, as numbers the player can steer by.
                char buf[128];
                if (dc.ok)
                    std::snprintf(buf, sizeof(buf), "MODEL %d PTS   AREA %.0f%% OF %.0f%%",
                        m_design->decorPointCount(), dc.areaRatio * 100.f,
                        m_design->tuning().decorMaxAreaRatio * 100.f);
                else
                    std::snprintf(buf, sizeof(buf), "%s - GAME SHOWS THE HULL", dc.reason);
                sf::Text w(monoFont(), buf, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(dc.ok ? VIOLET : RED);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);

                if (m_toastTimer > 0.f) {
                    sf::Text tt(monoFont(), m_toast, 12);
                    tt.setLetterSpacing(1.3f);
                    tt.setFillColor(CYAN);
                    tt.setPosition({ 14.f, 54.f });
                    m_window->draw(tt);
                }
            }
            // A prediction, not a rule: thrust behind one side turns the nose
            // toward the other, and InputSystem applies exactly that.
            else if (std::fabs(s.lateralOffsetPx) > 1.5f && s.engineCount > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "THRUST OFF-AXIS %.0f px - NOSE PULLS %s",
                    std::fabs(s.lateralOffsetPx), s.lateralOffsetPx > 0.f ? "LEFT" : "RIGHT");
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

    /// On-screen buttons for every keyboard shortcut on this screen.
    void drawControls(const sf::Vector2f& size) {
        const float y = size.y - 92.f;
        float x = size.x * 0.015f;
        const float w = 150.f, h = 38.f, gap = 10.f;
        const bool model = (m_mode == Mode::Model);

        if (m_ui.button({ x, y, 170.f, h }, "BACK", false, false, false, 18))
            m_exit = true;
        x += 170.f + gap * 2.f;

        if (m_ui.button({ x, y, w, h },
            m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF",
            m_design->symmetric(), false, false, 16))
            toggleMirror();
        x += w + gap;

        if (model) {
            if (m_ui.button({ x, y, w, h }, "RESET MODEL", false, !m_design->decorAuthored(), false, 15)) {
                m_design->resetDecor();
                toast("MODEL RESET TO HULL");
            }
        }
        else if (m_ui.button({ x, y, w, h }, "AUTO-MOUNT", false, false, false, 16)) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        x += w + gap * 2.f;

        for (int i = 0; i < ship::HULL_CLASS_COUNT; ++i) {
            const auto c = static_cast<ship::HullClass>(i);
            if (m_ui.button({ x, y, 150.f, h }, ship::classSpec(c).name,
                m_design->hullClass() == c, false, false, 15))
                loadClass(c);
            x += 150.f + gap;
        }

        m_ui.glossaryTab({ size.x - 46.f, size.y * 0.100f, 32.f, 32.f });
        m_ui.drawGlossary("REFIT BAY",
            "HULL is the hitbox: what hits and gets hit, where guns and drives go.\n"
            "MODEL is what the ship looks like. It must cover the hull, stay inside\n"
            "the violet limit and use at most 150% of the hull's area.",
            { { "M / TAB",       "Switch HULL and MODEL" },
              { "LEFT DRAG",     "Move a point of the layer you are editing" },
              { "RIGHT CLICK",   "HULL: fit/remove weapon (point) or drive (edge)" },
              { "",              "MODEL: remove a point, or add one anywhere else" },
              { "R",             "HULL: fire the Rift from this gun / share-dedicate" },
              { "INSERT  =",     "Add a point at the cursor" },
              { "DELETE",        "Remove the point under the cursor" },
              { "Y",             "Mirror editing" },
              { "1 / 2 / 3",     "LIGHT / MEDIUM / HEAVY frame" },
              { "A",             "HULL: auto-mount a balanced loadout" },
              { "SPIKE = MUZZLE","A model spike above a gun is where it fires from" },
              { "CYAN / VIOLET", "Plasma gun / Rift mount" },
              { "AMBER EDGE",    "Drive - pushes the ship away from that edge" },
              { "RED LINE",      "Thrust off-centre - the nose will pull" } });
    }

    void drawHints(const sf::Vector2f& size) {
        const bool model = (m_mode == Mode::Model);
        sf::Text a(monoFont(), model
            ? "LMB DRAG POINT     RMB ADD / REMOVE POINT     INS ADD     DEL REMOVE"
            : "LMB DRAG POINT     RMB FIT/REMOVE     R RIFT MOUNT     INS ADD     DEL REMOVE", 13);
        a.setLetterSpacing(1.5f);
        a.setFillColor(TEXT_DIM);
        a.setPosition({ size.x * 0.015f, size.y - 46.f });
        m_window->draw(a);

        sf::Text b(monoFont(), model
            ? "M HULL     Y MIRROR     1-3 FRAME     ESC BACK"
            : "M MODEL     Y MIRROR     1-3 FRAME     A AUTO-MOUNT     ESC BACK", 13);
        b.setLetterSpacing(1.5f);
        b.setFillColor(TEXT_DIM);
        b.setPosition({ size.x * 0.015f, size.y - 28.f });
        m_window->draw(b);
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    const sf::Font& monoFont() const { return m_mono ? *m_mono : *m_font; }

    // ---- Polygon drawing in panel space (L maps local -> panel) -------------
    // ConvexShape cannot draw a concave model, so everything polygonal on
    // this screen goes through these.

    template <class Map>
    void fillPolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c) const {
        const auto tris = ship::detail::triangulate(poly);
        sf::VertexArray va(sf::PrimitiveType::Triangles, tris.size());
        for (std::size_t i = 0; i < tris.size(); ++i) va[i] = { L(tris[i]), c };
        m_window->draw(va);
    }

    /**
     * @brief Closed outline with MITER joins, centred on the edges.
     *
     * Drawing each edge as its own thick line left a notch at every corner,
     * which blunted exactly the spikes this screen exists to make.
     */
    template <class Map>
    void strokePolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c, float w) const {
        const std::size_t n = poly.size();
        if (n < 2) return;
        std::vector<sf::Vector2f> S(n);
        for (std::size_t i = 0; i < n; ++i) S[i] = L(poly[i]);

        const auto normalOf = [&](std::size_t i) {
            const sf::Vector2f e = S[(i + 1) % n] - S[i];
            const float l = std::max(0.0001f, std::sqrt(e.x * e.x + e.y * e.y));
            return sf::Vector2f(e.y / l, -e.x / l);
            };
        std::vector<sf::Vector2f> off(n);
        for (std::size_t i = 0; i < n; ++i) {
            const sf::Vector2f n1 = normalOf((i + n - 1) % n), n2 = normalOf(i);
            sf::Vector2f m = (n1 + n2) / std::max(1.f + (n1.x * n2.x + n1.y * n2.y), 0.0001f);
            const float ml = std::sqrt(m.x * m.x + m.y * m.y);
            if (ml > 10.f) m *= 10.f / ml;
            off[i] = m * (w * 0.5f);
        }
        sf::VertexArray va(sf::PrimitiveType::Triangles, n * 6);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const sf::Vector2f a0 = S[i] + off[i], a1 = S[i] - off[i];
            const sf::Vector2f b0 = S[j] + off[j], b1 = S[j] - off[j];
            va[i * 6 + 0] = { a0, c }; va[i * 6 + 1] = { b0, c }; va[i * 6 + 2] = { b1, c };
            va[i * 6 + 3] = { a0, c }; va[i * 6 + 4] = { b1, c }; va[i * 6 + 5] = { a1, c };
        }
        m_window->draw(va);
    }

    template <class Map>
    void dashedPolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c, float dash, float gap) const {
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) dashedLine(L(poly[i]), L(poly[(i + 1) % n]), c, dash, gap);
    }

    void thickLine(sf::Vector2f a, sf::Vector2f b, sf::Color c, float w) const {
        sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l < 0.001f) return;
        const sf::Vector2f nrm{ -d.y / l * w * 0.5f, d.x / l * w * 0.5f };
        sf::VertexArray q(sf::PrimitiveType::Triangles, 6);
        q[0] = { a + nrm, c }; q[1] = { b + nrm, c }; q[2] = { b - nrm, c };
        q[3] = { a + nrm, c }; q[4] = { b - nrm, c }; q[5] = { a - nrm, c };
        m_window->draw(q);
    }

    void dashedLine(sf::Vector2f a, sf::Vector2f b, sf::Color c, float dash, float gap) const {
        const sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l < 0.001f) return;
        const sf::Vector2f u = d / l;
        sf::VertexArray va(sf::PrimitiveType::Lines);
        for (float t = 0.f; t < l; t += dash + gap) {
            va.append({ a + u * t, c });
            va.append({ a + u * std::min(l, t + dash), c });
        }
        m_window->draw(va);
    }

    void diamond(sf::Vector2f p, float s, sf::Color c) const {
        sf::ConvexShape d(4);
        d.setPoint(0, { p.x, p.y - s }); d.setPoint(1, { p.x + s, p.y });
        d.setPoint(2, { p.x, p.y + s }); d.setPoint(3, { p.x - s, p.y });
        d.setFillColor(c);
        m_window->draw(d);
    }

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
    sol::state* m_lua = nullptr;   ///< For hull_classes overrides in the readout

    float m_time = 0.f;
    float m_openTimer = 0.f;

    tui::UI m_ui;

    sf::Vector2f m_mouse;
    int  m_dragging = -1;
    int  m_hoverPoint = -1;
    int  m_hoverEdge = -1;
    int  m_hoverDecor = -1;
    bool m_lDown = false, m_rDown = false;

    Mode         m_mode = Mode::Hull;
    float        m_zoom = 4.2f;
    sf::Vector2f m_viewCenter;
    bool         m_viewInit = false;

    float        m_rejectFlash = 0.f;
    sf::Vector2f m_rejectAt;
    std::string  m_rejectText;

    std::string m_toast;
    float m_toastTimer = 0.f;

    bool m_exit = false;
    bool m_kY = false, m_k1 = false, m_k2 = false, m_k3 = false;
    bool m_kA = false, m_kR = false, m_kM = false, m_kDel = false, m_kAdd = false, m_kExit = false;

    float m_inputLock = 0.f;
};