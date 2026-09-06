/**
 * @file TerminalUI.hpp
 * @brief Shared immediate-mode widget layer for every terminal screen.
 *
 * MenuSystem and RefitSystem had grown two copies of the same palette, panel
 * chrome, CRT reveal and clipping code. They had already started to drift.
 * This is the single copy, plus the one thing neither had: widgets that know
 * where they landed on screen, so they can be clicked.
 *
 * ============================================================================
 * WHY IMMEDIATE MODE
 * ============================================================================
 * Retained-mode UI needs a widget tree, layout pass, and IDs that survive
 * between frames. For five screens of flat panels that is enormous overkill.
 * Here a button is a function that draws itself and returns whether it was
 * clicked this frame:
 *
 *     if (ui.button({x, y, w, h}, "BACK")) goBackToMenu();
 *
 * No registration, no callbacks, no state to keep in sync with the drawing.
 *
 * ============================================================================
 * THE TWO TRAPS THIS LAYER EXISTS TO CLOSE
 * ============================================================================
 *
 * 1. FROZEN VIEW. sf::RenderWindow::getDefaultView() is captured at window
 *    creation and never updates on resize. Layout built on it drifts and
 *    mapPixelToCoords() returns coordinates from a screen that no longer
 *    exists -- which is exactly why mouse input died after maximising.
 *    uiView() rebuilds from the live framebuffer every frame. Nothing in any
 *    screen should call getDefaultView() again.
 *
 * 2. HELD KEYS ACROSS SCREEN CHANGES. The Enter that opens a screen is still
 *    physically down on that screen's first frame. Edge flags initialised to
 *    false read it as a fresh press, so the screen opens and immediately
 *    closes -- the refit bay's first-entry bounce. keyEdge() defends against
 *    this structurally: a key seen for the first time is recorded at its
 *    CURRENT state and cannot report an edge until it is released and pressed
 *    again. primeInput() clears the table so every screen entry re-primes.
 *    You cannot forget to do this per-key, because there is no per-key code.
 *
 * ============================================================================
 * CLIP-LOCAL COORDINATES
 * ============================================================================
 * Panels render through beginClip(), which installs a viewport and shifts the
 * origin to the panel's top-left. Widgets drawn inside therefore use local
 * coordinates while the mouse arrives in screen space. The UI tracks the
 * active clip origin and hit-tests against localMouse(), so a button works
 * identically inside or outside a clip and callers never convert by hand.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tui {

    // ============================================================================
    // PALETTE
    //
    // Greys are DARK, accents are BRIGHT. Contrast comes from the gap between
    // near-black structure and hot cyan/amber values, never from mid-grey text.
    // Dark future, not hologram. These were duplicated in two files; if a third
    // screen needs a colour, add it HERE.
    // ============================================================================

    inline const sf::Color VOID_BG{ 2,   3,   5 };  ///< true ground
    inline const sf::Color PANEL_BG{ 6,   8,  12 };  ///< panel interior
    inline const sf::Color CYAN{ 40, 245, 255 };  ///< hot: values, active
    inline const sf::Color CYAN_MID{ 20, 150, 170 };  ///< borders
    inline const sf::Color CYAN_LOW{ 12,  70,  84 };  ///< dead structure
    inline const sf::Color AMBER{ 255, 214,   0 };  ///< selection, confirm
    inline const sf::Color RED{ 255,  48,   0 };  ///< danger
    inline const sf::Color GREEN{ 80, 240, 130 };  ///< ok
    inline const sf::Color TEXT{ 214, 222, 232 };  ///< body
    inline const sf::Color TEXT_DIM{ 84,  92, 104 };  ///< keys, captions
    inline const sf::Color TEXT_DEAD{ 52,  58,  68 };  ///< locked / disabled
    inline const sf::Color INK{ 4,   5,   8 };  ///< knocked-out text

    inline constexpr float OPEN_DUR = 0.30f;  ///< per-panel CRT reveal length
    inline constexpr float SKEW = 14.f;   ///< the ONE skewed element

    // ============================================================================
    // TYPES
    // ============================================================================

    struct Rect {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;

        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
        float right() const { return x + w; }
        float bottom() const { return y + h; }

        bool contains(sf::Vector2f p) const {
            return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
        }
        Rect inset(float m) const { return { x + m, y + m, w - m * 2.f, h - m * 2.f }; }
    };

    /// Panel border weights. Deliberately inconsistent across a screen -- this is
    /// a cheap device bolted together, not a designed product.
    enum class Chrome {
        Brackets,   ///< corner marks only, lightest
        Hairline,   ///< full thin border
        Heavy       ///< double top/bottom, filled title tab
    };

    /// One row in a glossary overlay.
    struct GlossaryEntry {
        const char* key;    ///< "RMB", "INS", "W / S"
        const char* what;   ///< what it does, plain language
    };

    // ============================================================================
    // UI
    // ============================================================================

    class UI {
    public:
        // ------------------------------------------------------------------
        // SETUP
        // ------------------------------------------------------------------

        void attach(sf::RenderWindow* w) { m_window = w; }
        void setFonts(sf::Font* main, sf::Font* mono) { m_font = main; m_mono = mono; }
        bool ready() const { return m_window && m_font; }

        /**
         * @brief Start a frame. Call once, before any drawing.
         *
         * Installs the live screen view, samples the mouse, and advances the
         * reveal clock.
         */
        void begin(float dt) {
            if (!m_window) return;
            dt = std::clamp(dt, 0.f, 0.1f);
            m_time += dt;
            if (m_openTimer < 5.f) m_openTimer += dt;

            m_window->setView(uiView());

            const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
            m_mouse = m_window->mapPixelToCoords(pix, uiView());

            const bool l = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
            const bool r = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
            m_leftEdge = l && !m_lastLeft;
            m_rightEdge = r && !m_lastRight;
            m_lastLeft = l;
            m_lastRight = r;

            m_clickUsed = false;
            m_clip = { 0.f, 0.f, 0.f, 0.f };
            m_clipped = false;
            m_drawingModal = false;
        }

        /**
         * @brief Re-prime input. Call from every screen's onEnter().
         *
         * Clearing the key table forces each key to re-register at its current
         * physical state on next use, so anything still held from the previous
         * screen cannot fire. This is the structural fix for the first-entry
         * bounce -- there is no per-key list to keep up to date.
         */
        void primeInput() {
            m_keys.clear();
            m_lastLeft = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
            m_lastRight = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
            m_leftEdge = m_rightEdge = false;
            m_clickUsed = true;   // swallow the click that opened this screen
            m_glossaryOpen = false;
        }

        /// Restart the CRT open animation for every panel.
        void resetReveal() { m_openTimer = 0.f; }

        float time() const { return m_time; }

        // ------------------------------------------------------------------
        // VIEW & LAYOUT
        // ------------------------------------------------------------------

        /// Screen-space view matching the CURRENT framebuffer. Never getDefaultView().
        sf::View uiView() const {
            const sf::Vector2u s = m_window->getSize();
            return sf::View(sf::FloatRect({ 0.f, 0.f },
                { static_cast<float>(std::max(1u, s.x)),
                  static_cast<float>(std::max(1u, s.y)) }));
        }

        sf::Vector2f size() const {
            const sf::Vector2u s = m_window->getSize();
            return { static_cast<float>(std::max(1u, s.x)),
                     static_cast<float>(std::max(1u, s.y)) };
        }

        Rect frac(float x, float y, float w, float h) const {
            const sf::Vector2f s = size();
            return { x * s.x, y * s.y, w * s.x, h * s.y };
        }

        // ------------------------------------------------------------------
        // INPUT
        // ------------------------------------------------------------------

        sf::Vector2f mouse() const { return m_mouse; }

        /// Mouse in the active clip's coordinate space. Identical to mouse()
        /// outside a clip, so widget code never needs to know which it is in.
        sf::Vector2f localMouse() const {
            return m_clipped ? sf::Vector2f{ m_mouse.x - m_clip.x, m_mouse.y - m_clip.y }
            : m_mouse;
        }

        bool leftEdge() const { return m_leftEdge && !m_clickUsed; }

        /// Mark this frame's click as handled. For hand-drawn hit regions that
        /// aren't button() -- menu rows keep their own caret and skew, so they
        /// hit-test themselves but must still stop the click falling through.
        void consumeClick() { m_clickUsed = true; }
        bool rightEdge() const { return m_rightEdge; }
        bool leftDown() const { return m_lastLeft; }

        /**
         * @brief Edge-detected key press.
         *
         * A key not seen before is recorded at its current state and reports no
         * edge, so a key held over from the previous screen cannot fire on the
         * first frame. See primeInput().
         */
        bool keyEdge(sf::Keyboard::Key k) {
            const bool now = sf::Keyboard::isKeyPressed(k);
            const int id = static_cast<int>(k);
            auto it = m_keys.find(id);
            if (it == m_keys.end()) { m_keys.emplace(id, now); return false; }
            const bool edge = now && !it->second;
            it->second = now;
            return edge;
        }

        static bool keyDown(sf::Keyboard::Key k) { return sf::Keyboard::isKeyPressed(k); }

        // ------------------------------------------------------------------
        // CLIPPING
        // ------------------------------------------------------------------

        /**
         * @brief Render into a panel with hard GPU clipping.
         *
         * Installs a viewport confined to @p r and shifts the origin to its
         * top-left, so content is drawn in panel-local coordinates and cannot
         * escape the frame. Always pair with endClip().
         */
        void beginClip(const Rect& r) {
            const sf::Vector2u ws = m_window->getSize();
            if (ws.x == 0 || ws.y == 0 || r.w <= 0.f || r.h <= 0.f) return;
            const float W = static_cast<float>(ws.x), H = static_cast<float>(ws.y);

            sf::View v;
            v.setSize({ r.w, r.h });
            v.setCenter({ r.w * 0.5f, r.h * 0.5f });
            v.setViewport(sf::FloatRect({ r.x / W, r.y / H }, { r.w / W, r.h / H }));
            m_window->setView(v);

            m_clip = r;
            m_clipped = true;
        }

        void endClip() {
            m_window->setView(uiView());
            m_clipped = false;
            m_clip = { 0.f, 0.f, 0.f, 0.f };
        }

        // ------------------------------------------------------------------
        // CRT REVEAL
        // ------------------------------------------------------------------

        float reveal(float delay) const {
            const float t = m_openTimer - delay;
            return (t <= 0.f) ? 0.f : std::clamp(t / OPEN_DUR, 0.f, 1.f);
        }

        /**
         * @brief Draw a panel's frame and title, honouring the CRT reveal.
         * @return true only when fully open, i.e. when content should be drawn.
         *
         * Old CRT switch-on: a hot line snaps across the middle, then the picture
         * opens outward from it. Width expands first, then height.
         */
        bool panel(const Rect& r, const std::string& label, float delay,
            Chrome chrome, sf::Color accent) {
            const float p = reveal(delay);
            if (p <= 0.f) return false;

            const float wf = std::clamp(p / 0.34f, 0.06f, 1.f);
            const float hf = std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
            const float dw = r.w * wf;
            const float dh = std::max(2.f, r.h * hf);
            const Rect d{ r.cx() - dw * 0.5f, r.cy() - dh * 0.5f, dw, dh };

            fill(d, PANEL_BG);

            if (p < 0.98f) {
                const float a = std::clamp((1.f - p) * 2.2f, 0.f, 1.f);
                hline(d.x, d.cy(), d.w, withAlpha(accent, static_cast<std::uint8_t>(235 * a)));
            }

            switch (chrome) {
            case Chrome::Brackets: {
                const float L = std::min(22.f, std::min(d.w, d.h) * 0.25f);
                const sf::Color c = withAlpha(accent, 190);
                hline(d.x, d.y, L, c);                    vline(d.x, d.y, L, c);
                hline(d.right() - L, d.y, L, c);          vline(d.right(), d.y, L, c);
                hline(d.x, d.bottom(), L, c);             vline(d.x, d.bottom() - L, L, c);
                hline(d.right() - L, d.bottom(), L, c);   vline(d.right(), d.bottom() - L, L, c);
                break;
            }
            case Chrome::Hairline: {
                const sf::Color c = withAlpha(accent, 130);
                hline(d.x, d.y, d.w, c);
                hline(d.x, d.bottom(), d.w, c);
                vline(d.x, d.y, d.h, c);
                vline(d.right(), d.y, d.h, c);
                break;
            }
            case Chrome::Heavy: {
                const sf::Color c = withAlpha(accent, 210);
                for (int i = 0; i < 2; ++i) {
                    hline(d.x, d.y + static_cast<float>(i), d.w, c);
                    hline(d.x, d.bottom() - static_cast<float>(i), d.w, c);
                }
                vline(d.x, d.y, d.h, c);
                vline(d.right(), d.y, d.h, c);
                break;
            }
            }

            if (p < 1.f) return false;

            if (!label.empty()) {
                if (chrome == Chrome::Heavy) {
                    // Filled title tab: dark text knocked out of a hot bar.
                    const float tw = textWidth(label, 14, true, 1.8f) + 14.f;
                    fill({ r.x + 8.f, r.y - 10.f, tw, 20.f }, accent);
                    text({ r.x + 15.f, r.y - 8.f }, label, 14, INK, true, 1.8f);
                }
                else {
                    // Plain label on the border, with a gap punched behind it so
                    // the stroke does not run through the glyphs.
                    const float tw = textWidth(label, 14, true, 1.8f) + 12.f;
                    fill({ r.x + 10.f, r.y - 2.f, tw, 4.f }, VOID_BG);
                    text({ r.x + 16.f, r.y - 9.f }, label, 14, accent, true, 1.8f);
                }
            }
            return true;
        }

        // ------------------------------------------------------------------
        // WIDGETS
        // ------------------------------------------------------------------

        bool hovering(const Rect& r) const {
            if (m_glossaryOpen && !m_drawingModal) return false;
            return r.contains(localMouse());
        }

        /**
         * @brief A clickable button. Draws itself; returns true on the click.
         *
         * @param skewed  Carry the in-flight HUD's angle. Reserve this for the
         *                primary selection bar -- it is the ONE non-orthogonal
         *                element on a terminal screen and stops meaning anything
         *                if everything uses it.
         *
         * Only one widget can consume a click per frame, so overlapping buttons
         * cannot both fire.
         */
        bool button(const Rect& r, const std::string& label,
            bool selected = false, bool disabled = false,
            bool skewed = false, unsigned charSize = 20) {
            const bool hot = !disabled && hovering(r);
            const bool active = selected || hot;

            if (active && !disabled) {
                if (skewed) {
                    sf::ConvexShape bar(4);
                    bar.setPoint(0, { r.x + SKEW,       r.y });
                    bar.setPoint(1, { r.right() + SKEW, r.y });
                    bar.setPoint(2, { r.right(),        r.bottom() });
                    bar.setPoint(3, { r.x,              r.bottom() });
                    bar.setFillColor(withAlpha(AMBER, hot ? 255 : 210));
                    m_window->draw(bar);
                }
                else {
                    fill(r, withAlpha(AMBER, hot ? 255 : 210));
                }
            }
            else if (!disabled) {
                // Idle: a hairline box, so the target is discoverable without
                // shouting. A button nobody can see is not a button.
                const sf::Color c = withAlpha(CYAN_MID, 120);
                hline(r.x, r.y, r.w, c);
                hline(r.x, r.bottom(), r.w, c);
                vline(r.x, r.y, r.h, c);
                vline(r.right(), r.y, r.h, c);
            }

            const sf::Color fg = disabled ? TEXT_DEAD : (active ? INK : TEXT);
            const float tw = textWidth(label, charSize, false, 1.4f);
            text({ r.cx() - tw * 0.5f, r.cy() - static_cast<float>(charSize) * 0.72f },
                label, charSize, fg, false, 1.4f);

            if (disabled || !hot) return false;
            if (!leftEdge()) return false;
            m_clickUsed = true;
            return true;
        }

        /// Small square button for glyphs -- "?", "<", ">", "X".
        bool iconButton(const Rect& r, const std::string& glyph,
            sf::Color accent = CYAN_MID, unsigned charSize = 18) {
            const bool hot = hovering(r);

            fill(r, hot ? accent : PANEL_BG);
            const sf::Color c = withAlpha(accent, hot ? 255 : 160);
            hline(r.x, r.y, r.w, c);
            hline(r.x, r.bottom(), r.w, c);
            vline(r.x, r.y, r.h, c);
            vline(r.right(), r.y, r.h, c);

            const float tw = textWidth(glyph, charSize, true, 1.2f);
            text({ r.cx() - tw * 0.5f, r.cy() - static_cast<float>(charSize) * 0.72f },
                glyph, charSize, hot ? INK : accent, true, 1.2f);

            if (!hot || !leftEdge()) return false;
            m_clickUsed = true;
            return true;
        }

        // ------------------------------------------------------------------
        // GLOSSARY
        //
        // Every screen gets one. A control scheme that is only discoverable by
        // guessing is a broken screen -- "INS ADD" in the hint bar tells you
        // nothing about which key that is or what it adds.
        // ------------------------------------------------------------------

        bool glossaryOpen() const { return m_glossaryOpen; }
        void closeGlossary() { m_glossaryOpen = false; }

        /// The "?" tab. Place it in a screen corner; it toggles the overlay.
        bool glossaryTab(const Rect& r) {
            const bool wasOpen = m_glossaryOpen;
            // While open the tab lives on the modal layer so it can still be hit.
            const bool prevModal = m_drawingModal;
            if (wasOpen) m_drawingModal = true;

            const bool hit = iconButton(r, m_glossaryOpen ? "X" : "?",
                m_glossaryOpen ? AMBER : CYAN_MID);
            m_drawingModal = prevModal;

            if (hit) m_glossaryOpen = !m_glossaryOpen;
            return m_glossaryOpen;
        }

        /**
         * @brief Draw the glossary overlay. No-op when closed.
         *
         * While open, every widget outside it stops responding -- otherwise a
         * click meant for the overlay falls through and fires whatever is behind.
         */
        void drawGlossary(const std::string& title, const std::string& blurb,
            const std::vector<GlossaryEntry>& entries) {
            if (!m_glossaryOpen) return;

            m_drawingModal = true;

            const sf::Vector2f s = size();
            fill({ 0.f, 0.f, s.x, s.y }, sf::Color(2, 3, 5, 225));

            const Rect box{ s.x * 0.22f, s.y * 0.14f, s.x * 0.56f, s.y * 0.72f };
            fill(box, PANEL_BG);
            const sf::Color c = withAlpha(CYAN, 220);
            for (int i = 0; i < 2; ++i) {
                hline(box.x, box.y + static_cast<float>(i), box.w, c);
                hline(box.x, box.bottom() - static_cast<float>(i), box.w, c);
            }
            vline(box.x, box.y, box.h, c);
            vline(box.right(), box.y, box.h, c);

            beginClip(box);
            {
                float y = 22.f;
                text({ 26.f, y }, title, 26, CYAN, false, 1.7f);
                y += 44.f;

                if (!blurb.empty()) {
                    text({ 26.f, y }, blurb, 15, TEXT, true, 1.3f, 1.35f);
                    y += 30.f + 21.f * static_cast<float>(countLines(blurb));
                }

                hline(26.f, y - 12.f, box.w - 52.f, CYAN_LOW);

                for (const auto& e : entries) {
                    if (y > box.h - 30.f) break;   // clipped anyway; skip the work
                    text({ 26.f, y }, e.key, 15, AMBER, true, 1.6f);
                    text({ 26.f + box.w * 0.26f, y }, e.what, 15, TEXT_DIM, true, 1.3f);
                    y += 26.f;
                }
            }
            endClip();

            const Rect close{ box.right() - 42.f, box.y + 12.f, 30.f, 30.f };
            if (iconButton(close, "X", AMBER)) m_glossaryOpen = false;

            text({ box.x + 26.f, box.bottom() - 32.f },
                "ESC OR CLICK X TO CLOSE", 13, TEXT_DEAD, true, 1.5f);

            if (keyEdge(sf::Keyboard::Key::Escape)) m_glossaryOpen = false;

            m_drawingModal = false;
        }

        // ------------------------------------------------------------------
        // TRANSITION FADER
        //
        // Not a widget: a screen-wide veil the caller drives. Lives here because
        // every screen needs the same one, and a state change that cuts hard
        // feels chopped.
        // ------------------------------------------------------------------

        void veil(float alpha01) {
            const float a = std::clamp(alpha01, 0.f, 1.f);
            if (a <= 0.001f) return;
            const sf::Vector2f s = size();
            fill({ 0.f, 0.f, s.x, s.y },
                sf::Color(2, 3, 5, static_cast<std::uint8_t>(255 * a)));
        }

        // ------------------------------------------------------------------
        // PRIMITIVES
        // ------------------------------------------------------------------

        void fill(const Rect& r, sf::Color c) const {
            sf::RectangleShape s({ r.w, r.h });
            s.setPosition({ r.x, r.y });
            s.setFillColor(c);
            m_window->draw(s);
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

        void text(sf::Vector2f pos, const std::string& str, unsigned size,
            sf::Color c, bool mono = true, float spacing = 1.4f,
            float lineSpacing = 1.f) const {
            if (!m_font) return;
            sf::Text t(fontFor(mono), str, size);
            t.setLetterSpacing(spacing);
            if (lineSpacing != 1.f) t.setLineSpacing(lineSpacing);
            t.setFillColor(c);
            t.setPosition(pos);
            m_window->draw(t);
        }

        /// Right-aligned convenience: pass the RIGHT edge, not the left.
        void textRight(float rightX, float y, const std::string& str, unsigned size,
            sf::Color c, bool mono = true, float spacing = 1.4f) const {
            text({ rightX - textWidth(str, size, mono, spacing), y },
                str, size, c, mono, spacing);
        }

        float textWidth(const std::string& str, unsigned size,
            bool mono = true, float spacing = 1.4f) const {
            if (!m_font) return 0.f;
            sf::Text t(fontFor(mono), str, size);
            t.setLetterSpacing(spacing);
            return t.getGlobalBounds().size.x;
        }

        static sf::Color withAlpha(sf::Color c, std::uint8_t a) {
            return sf::Color(c.r, c.g, c.b, a);
        }

        const sf::Font& fontFor(bool mono) const {
            return (mono && m_mono) ? *m_mono : *m_font;
        }

    private:
        static int countLines(const std::string& s) {
            int n = 1;
            for (char ch : s) if (ch == '\n') ++n;
            return n;
        }

        sf::RenderWindow* m_window = nullptr;
        sf::Font* m_font = nullptr;
        sf::Font* m_mono = nullptr;

        float m_time = 0.f;
        float m_openTimer = 0.f;

        sf::Vector2f m_mouse;
        bool m_lastLeft = false, m_lastRight = false;
        bool m_leftEdge = false, m_rightEdge = false;
        bool m_clickUsed = false;

        Rect m_clip;
        bool m_clipped = false;

        bool m_glossaryOpen = false;
        bool m_drawingModal = false;

        std::unordered_map<int, bool> m_keys;
    };

} // namespace tui