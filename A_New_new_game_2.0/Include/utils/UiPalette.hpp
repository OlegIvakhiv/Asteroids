/**
 * @file UiPalette.hpp
 * @brief The one place Void Hunter's interface colours and chrome live.
 *
 * WHY THIS EXISTS
 * ---------------
 * The grim-futuristic look was already in the game -- it was just locked inside
 * MenuSystem as private `static inline const` members. The HUD never saw it, so
 * it grew its own unrelated palette: saturated reds and cyans, thick outlines,
 * smooth bars. Two interfaces built on two languages, in one game.
 *
 * Nothing here is new art direction. It is the menu's existing palette lifted
 * out so the HUD can use it too. MenuSystem keeps rendering exactly as before;
 * its constants just now point here.
 *
 *
 * THE RULES, so additions stay coherent
 * -------------------------------------
 *  1. BLACK IS THE GROUND. VOID_BG is near-black, not grey. Everything reads as
 *     light emitted by a display, never as a painted surface.
 *
 *  2. STRUCTURE IS DEAD, DATA IS ALIVE. Frames, rules and brackets use
 *     CYAN_LOW / CYAN_MID -- dim, desaturated, close to the background. Only
 *     actual values get full-intensity CYAN or AMBER. When everything glows,
 *     nothing reads.
 *
 *  3. HAIRLINES, NOT BORDERS. 1px. A 3px outline reads as a toy; 1px on near
 *     black reads as an instrument.
 *
 *  4. BRACKETS, NOT BOXES. Corner ticks imply a frame without drawing one.
 *     Cheaper on the eye, and it keeps the interior legible.
 *
 *  5. SEGMENTS, NOT GRADIENTS. Bars are ticked into cells. A smooth fill is a
 *     mood; a segmented one is a reading.
 *
 *  6. AMBER MEANS ATTENTION, RED MEANS HARM. Never decorative. The moment amber
 *     is used for flavour it stops working as a warning.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace ui {

    // ========================================================================
    // PALETTE  (lifted verbatim from MenuSystem so nothing shifts)
    // ========================================================================

    inline const sf::Color VOID_BG{ 2,   3,   5 };   ///< True ground
    inline const sf::Color PANEL_BG{ 6,   8,  12 };   ///< Panel interior
    inline const sf::Color CYAN{ 40, 245, 255 };   ///< Hot: live values
    inline const sf::Color CYAN_MID{ 20, 150, 170 };   ///< Borders
    inline const sf::Color CYAN_LOW{ 12,  70,  84 };   ///< Dead structure
    inline const sf::Color AMBER{ 255, 214,   0 };   ///< Selection, confirm
    inline const sf::Color RED{ 255,  48,   0 };   ///< Danger
    inline const sf::Color TEXT{ 214, 222, 232 };
    inline const sf::Color TEXT_DIM{ 84,  92, 104 };
    inline const sf::Color TEXT_DEAD{ 52,  58,  68 };

    // ---- HUD-specific extensions, in the same key ----
    //
    // AMBER_HOT is deliberately NOT the same orange as overheat. The overheat
    // state already owns red-orange (255,70,30); reusing it for the reward zone
    // would put "you are in trouble" and "you did the good thing" on the same
    // hue, and the player reads hue faster than shape.
    inline const sf::Color AMBER_HOT{ 255, 165,  40 };  ///< Perfect vent zone
    inline const sf::Color BLUE_COOL{ 40, 140, 220 };  ///< Good vent zone
    inline const sf::Color HAZARD{ 255,  70,  30 };  ///< Overheat / lockout

    // ========================================================================
    // METRICS
    // ========================================================================

    inline constexpr float HAIRLINE = 1.f;
    inline constexpr float CHAMFER = 6.f;   ///< Cut-corner size on panels
    inline constexpr float BRACKET = 11.f;  ///< Corner tick arm length
    inline constexpr float SEGMENT_GAP = 2.f;   ///< Gap between bar cells

    inline sf::Color alpha(sf::Color c, float a01) {
        return { c.r, c.g, c.b,
                 static_cast<std::uint8_t>(std::clamp(a01, 0.f, 1.f) * 255.f) };
    }

    inline sf::Color mix(sf::Color a, sf::Color b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return { static_cast<std::uint8_t>(a.r + (b.r - a.r) * t),
                 static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
                 static_cast<std::uint8_t>(a.b + (b.b - a.b) * t),
                 static_cast<std::uint8_t>(a.a + (b.a - a.a) * t) };
    }

    // ========================================================================
    // PRIMITIVES
    // ========================================================================

    inline void fill(sf::RenderTarget& t, float x, float y, float w, float h,
        sf::Color c) {
        sf::RectangleShape r({ w, h });
        r.setPosition({ x, y });
        r.setFillColor(c);
        t.draw(r);
    }

    inline void hline(sf::RenderTarget& t, float x, float y, float w, sf::Color c) {
        fill(t, x, y, w, HAIRLINE, c);
    }

    inline void vline(sf::RenderTarget& t, float x, float y, float h, sf::Color c) {
        fill(t, x, y, HAIRLINE, h, c);
    }

    /**
     * @brief Panel with cut corners. Rule 1 and 3.
     *
     * The chamfer is what separates this from a plain rectangle at a glance --
     * a square panel reads as a UI widget, a cut one reads as a machined plate.
     * Drawn as a triangle fan so the cut is real geometry rather than four
     * cover-up triangles in the background colour, which would break the
     * moment anything was drawn behind it.
     */
    inline void panel(sf::RenderTarget& t, float x, float y, float w, float h,
        sf::Color fillCol, float cut = CHAMFER) {
        cut = std::min({ cut, w * 0.5f, h * 0.5f });
        const sf::Vector2f p[8] = {
            { x + cut, y },        { x + w - cut, y },
            { x + w,   y + cut },  { x + w,       y + h - cut },
            { x + w - cut, y + h },{ x + cut,     y + h },
            { x,       y + h - cut }, { x,        y + cut },
        };
        sf::VertexArray va(sf::PrimitiveType::TriangleFan, 10);
        va[0] = sf::Vertex{ { x + w * 0.5f, y + h * 0.5f }, fillCol };
        for (int i = 0; i < 8; ++i) va[i + 1] = sf::Vertex{ p[i], fillCol };
        va[9] = sf::Vertex{ p[0], fillCol };
        t.draw(va);
    }

    /**
     * @brief Four corner ticks. Rule 4 -- implies a frame without drawing one.
     */
    inline void brackets(sf::RenderTarget& t, float x, float y, float w, float h,
        sf::Color c, float arm = BRACKET) {
        arm = std::min({ arm, w * 0.4f, h * 0.4f });
        // top-left
        hline(t, x, y, arm, c);              vline(t, x, y, arm, c);
        // top-right
        hline(t, x + w - arm, y, arm, c);      vline(t, x + w - HAIRLINE, y, arm, c);
        // bottom-left
        hline(t, x, y + h - HAIRLINE, arm, c); vline(t, x, y + h - arm, arm, c);
        // bottom-right
        hline(t, x + w - arm, y + h - HAIRLINE, arm, c);
        vline(t, x + w - HAIRLINE, y + h - arm, arm, c);
    }

    /**
     * @brief Segmented value bar. Rule 5.
     *
     * @param cells  How many discrete cells. Pick a count that maps to
     *               something the player can count -- shots remaining, hits
     *               survivable. An arbitrary count is just texture.
     */
    inline void segBar(sf::RenderTarget& t, float x, float y, float w, float h,
        float t01, int cells, sf::Color on, sf::Color off) {
        cells = std::max(1, cells);
        t01 = std::clamp(t01, 0.f, 1.f);
        const float cw = (w - SEGMENT_GAP * (cells - 1)) / cells;
        const float filled = t01 * cells;

        for (int i = 0; i < cells; ++i) {
            const float cx = x + i * (cw + SEGMENT_GAP);
            const float f = std::clamp(filled - i, 0.f, 1.f);
            fill(t, cx, y, cw, h, off);
            if (f > 0.f) fill(t, cx, y, cw * f, h, on);
        }
    }

    /**
     * @brief Uppercase tracked label. Letter-spacing is done by hand because
     *        SFML has no tracking control, and tight-set caps read as a word
     *        while spaced caps read as a readout.
     */
    inline void label(sf::RenderTarget& t, const sf::Font* font, float x, float y,
        const std::string& s, unsigned size, sf::Color c,
        float tracking = 2.f) {
        if (!font) return;
        float cx = x;
        for (char ch : s) {
            sf::Text g(*font);
            g.setCharacterSize(size);
            g.setString(std::string(1, static_cast<char>(std::toupper(ch))));
            g.setFillColor(c);
            g.setPosition({ cx, y });
            t.draw(g);
            cx += g.getLocalBounds().size.x + tracking + (ch == ' ' ? size * 0.25f : 1.f);
        }
    }

    /**
     * @brief Horizontal scanlines over a region.
     *
     * Kept subtle on purpose -- alpha above ~30 stops reading as a CRT and
     * starts reading as a texture bug, and it makes small text genuinely
     * harder to parse.
     */
    inline void scanlines(sf::RenderTarget& t, float x, float y, float w, float h,
        std::uint8_t a = 22, float spacing = 3.f) {
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        for (float sy = y; sy < y + h; sy += spacing) {
            const sf::Color c(0, 0, 0, a);
            const sf::Vector2f q[4] = { {x, sy}, {x + w, sy}, {x + w, sy + 1.f}, {x, sy + 1.f} };
            va.append(sf::Vertex{ q[0], c }); va.append(sf::Vertex{ q[1], c });
            va.append(sf::Vertex{ q[2], c });
            va.append(sf::Vertex{ q[0], c }); va.append(sf::Vertex{ q[2], c });
            va.append(sf::Vertex{ q[3], c });
        }
        t.draw(va);
    }

} // namespace ui