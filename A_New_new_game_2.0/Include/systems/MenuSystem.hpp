/**
 * @file MenuSystem.hpp
 * @brief Renders and drives the main menu, pause menu, game-over screen, and tutorial
 *
 * Purely screen-space UI. Owns its own selection state and a small list of
 * options per GameState. Does NOT own the GameState itself -- that lives in
 * SystemManager, which calls setContextState() so this system knows which
 * menu to draw, and reads back confirmSelection()'s result to actually
 * transition.
 *
 * @author Oleg Ivakhiv
 * @version 1.1 (added tutorial)
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include <SFML/Graphics.hpp>
#include <vector>
#include <string>

class MenuSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_window = ctx.window;
    }

    /// HudSystem.setFont-style late binding; font isn't ready at init() time.
    void setFont(sf::Font* font) { m_font = font; }

    /// Called by SystemManager every frame while NOT in the Playing state.
    void update(float dt) override {
        (void)dt;
        if (!m_window) return;
        draw();
    }

    /**
     * @brief Tell the menu which screen to show and rebuild its option list
     *
     * Cheap to call every frame; only rebuilds when the state actually
     * changed, so held keys don't reset the selection every tick.
     */
    void setState(GameState state, int score = 0) {
        if (state == m_lastState) return;
        m_lastState = state;
        m_selected = 0;
        m_score = score;
        m_scrollOffset = 0;  // Reset scroll when changing screens

        m_options.clear();
        switch (state) {
        case GameState::MainMenu:
            m_title = "MODULAR SPACE ENGINE";
            m_options = { "Start", "Tutorial", "Quit" };
            break;
        case GameState::Paused:
            m_title = "PAUSED";
            m_options = { "Resume", "Restart", "Quit" };
            break;
        case GameState::GameOver:
            m_title = "GAME OVER";
            m_options = { "Restart", "Quit" };
            break;
        case GameState::Tutorial:
            m_title = "HOW TO PLAY";
            m_options = { "Back to Menu" };
            break;
        default:
            m_options = {};
            break;
        }
    }

    void moveSelection(int dir) {
        if (m_options.empty()) return;
        const int n = static_cast<int>(m_options.size());
        m_selected = ((m_selected + dir) % n + n) % n;
    }

    /**
     * @brief Resolve the highlighted option into an action for the caller
     *
     * The caller (SystemManager/game.cpp) owns what each action actually
     * DOES (reset the world, close the window, etc.) -- this system only
     * reports intent.
     */
    MenuAction confirmSelection() const {
        if (m_options.empty()) return MenuAction::None;
        const std::string& opt = m_options[m_selected];

        if (opt == "Start")   return MenuAction::StartGame;
        if (opt == "Resume")  return MenuAction::ResumeGame;
        if (opt == "Restart") return MenuAction::RestartGame;
        if (opt == "Quit")    return MenuAction::QuitGame;
        if (opt == "Tutorial") return MenuAction::ShowTutorial;
        if (opt == "Back to Menu") return MenuAction::BackToMenu;
        return MenuAction::None;
    }

    // Tutorial scroll control
    void scrollTutorial(int direction) {
        if (m_lastState != GameState::Tutorial) return;
        const int maxScroll = getMaxScrollOffset();
        m_scrollOffset = std::clamp(m_scrollOffset - direction * 30, 0, maxScroll);
    }

private:
    // ---- Tutorial Section Struct (moved to class scope) ----
    struct TutorialSection {
        std::string title;
        std::string body;
        sf::Color color;
        int column;  // 0 = left, 1 = right
    };

    // ---- Get tutorial sections (reusable) ----
    std::vector<TutorialSection> getTutorialSections() const {
        return {
            // LEFT COLUMN (column 0)
            {
                "MOVEMENT",
                "W / S / A / D or Arrow Keys\n"
                "Move in any direction.\n"
                "Ship faces your mouse cursor.",
                sf::Color(100, 200, 255),
                0
            },
            {
                "TURBO BOOST",
                "Hold LShift for speed boost.\n"
                "Consumes Energy (blue bar).\n"
                "Running out = overheat penalty.",
                sf::Color(255, 200, 100),
                0
            },
            {
                "DASH",
                "Press Space to dash.\n"
                "Direction: movement or forward.\n"
                "Costs energy, short cooldown.\n"
                "Great for evasion!",
                sf::Color(100, 255, 255),
                0
            },
            {
                "ENERGY MANAGEMENT",
                "Blue bar = Energy Drive.\n"
                "Used for dash and turbo.\n"
                "Regenerates when not boosting.",
                sf::Color(50, 150, 255),
                0
            },
            {
                "SCORING",
                "Destroy asteroids & enemies.\n"
                "Bigger targets = bigger rewards.",
                sf::Color(255, 215, 0),
                0
            },
            // RIGHT COLUMN (column 1)
            {
                "SHOOTING",
                "Left Click to fire.\n"
                "Each shot builds Heat (orange bar).\n"
                "Full heat = gun overheats!\n"
                "Wait to cool down.",
                sf::Color(255, 255, 100),
                1
            },
            {
                "RIFT SHOT",
                "Hold Right Click to charge.\n"
                "Release a powerful projectile.\n"
                "Detonate with Left Click.\n"
                "Different effects based on target!",
                sf::Color(200, 100, 255),
                1
            },
            {
                "PARRY",
                "Press R to parry.\n"
                "Deflects enemy bullets back!\n"
                "Repels asteroids & stuns enemies.\n"
                "Has cooldown - time it well!",
                sf::Color(0, 255, 200),
                1
            },
            {
                "HEAT MANAGEMENT",
                "Orange bar = Weapon Heat.\n"
                "Increases when firing.\n"
                "Overheated = gun lockout.",
                sf::Color(255, 100, 50),
                1
            },
            {
                "COMBAT TIPS",
                "\u2022 Dash to dodge incoming fire\n"
                "\u2022 Parry for counter-attacks\n"
                "\u2022 Manage energy & heat\n"
                "\u2022 Rift Shot = massive damage\n"
                "\u2022 Watch enemy telegraphs!",
                sf::Color(255, 200, 200),
                1
            }
        };
    }

    void draw() {
        const sf::View& v = m_window->getView();
        const sf::Vector2f center = v.getCenter();
        const sf::Vector2f size = v.getSize();

        // ---- Dim backdrop ----
        sf::RectangleShape backdrop(size);
        backdrop.setPosition(center - size * 0.5f);
        backdrop.setFillColor(sf::Color(5, 5, 10, 190));
        m_window->draw(backdrop);

        if (!m_font) return;

        // ---- Special handling for Tutorial screen ----
        if (m_lastState == GameState::Tutorial) {
            drawTutorial(center, size);
            return;
        }

        // ---- Title ----
        sf::Text title(*m_font, m_title, 54);
        title.setFillColor(sf::Color(0, 220, 255));
        title.setStyle(sf::Text::Bold);
        centerOrigin(title);
        title.setPosition({ center.x, center.y - 140.f });
        m_window->draw(title);

        // ---- Score (game over only) ----
        if (m_lastState == GameState::GameOver) {
            sf::Text scoreText(*m_font, "Score: " + std::to_string(m_score), 28);
            scoreText.setFillColor(sf::Color(255, 255, 255));
            centerOrigin(scoreText);
            scoreText.setPosition({ center.x, center.y - 70.f });
            m_window->draw(scoreText);
        }

        // ---- Options ----
        const float startY = (m_lastState == GameState::GameOver) ? center.y : center.y + 20.f;
        const float spacing = 52.f;

        for (size_t i = 0; i < m_options.size(); ++i) {
            const bool isSelected = (static_cast<int>(i) == m_selected);

            sf::Text opt(*m_font, m_options[i], isSelected ? 36u : 30u);
            opt.setFillColor(isSelected ? sf::Color(255, 220, 60) : sf::Color(200, 200, 210));
            centerOrigin(opt);
            opt.setPosition({ center.x, startY + i * spacing });
            m_window->draw(opt);

            if (isSelected) {
                drawSelectionCarets(opt);
            }
        }

        // ---- Footer hint ----
        sf::Text hint(*m_font,
            m_lastState == GameState::MainMenu ? "W/S or Up/Down to select   -   Enter to confirm" :
            m_lastState == GameState::GameOver ? "W/S or Up/Down to select   -   Enter to confirm" :
            "W/S or Up/Down to select   -   Enter to confirm", 16);
        hint.setFillColor(sf::Color(140, 140, 150));
        centerOrigin(hint);
        hint.setPosition({ center.x, center.y + (m_options.size() * spacing) + 40.f });
        m_window->draw(hint);
    }

    /**
     * @brief Draw the tutorial screen with all game mechanics explained
     */
    void drawTutorial(const sf::Vector2f& center, const sf::Vector2f& size) {
        // ---- Title (fixed position) ----
        sf::Text title(*m_font, "HOW TO PLAY", 48);
        title.setFillColor(sf::Color(0, 220, 255));
        title.setStyle(sf::Text::Bold);
        centerOrigin(title);
        title.setPosition({ center.x, center.y - size.y * 0.42f });
        m_window->draw(title);

        // ---- Two-column layout ----
        const float columnWidth = size.x * 0.35f;
        const float gap = 60.f;
        const float startX = center.x - columnWidth - gap * 0.5f;

        // contentY moves OPPOSITE to scroll direction
        // Scroll down (S) ? offset increases ? content moves UP
        float contentY = center.y - size.y * 0.30f - m_scrollOffset;

        // ---- Get tutorial sections ----
        auto sections = getTutorialSections();

        // ---- Calculate total height for scrolling ----
        float totalHeight = 0;
        for (const auto& section : sections) {
            // Title height
            totalHeight += 30.f;
            // Body height (count lines)
            int lineCount = 1;
            for (char c : section.body) if (c == '\n') lineCount++;
            totalHeight += lineCount * 22.f;
            totalHeight += 20.f; // Spacing between sections
        }
        // Add some padding
        totalHeight += 40.f;

        // Update max scroll based on actual content height
        const float visibleHeight = size.y * 0.55f;
        const int maxScroll = std::max(0, static_cast<int>(totalHeight - visibleHeight));

        // ---- Clip region for scrolling ----
        const float topClip = center.y - size.y * 0.34f;
        const float bottomClip = center.y + size.y * 0.34f;

        // ---- Draw sections in two columns ----
        float leftY = contentY;
        float rightY = contentY;
        float columnHeights[2] = { 0, 0 };

        // First pass: calculate heights to align columns
        for (const auto& section : sections) {
            int lineCount = 1;
            for (char c : section.body) if (c == '\n') lineCount++;
            float height = 30.f + lineCount * 22.f + 20.f;
            if (section.column == 0) columnHeights[0] += height;
            else columnHeights[1] += height;
        }

        // Align columns: start right column lower if it has less content
        float rightOffset = std::max(0.f, columnHeights[0] - columnHeights[1]);
        rightY -= rightOffset * 0.5f;

        // Second pass: draw
        for (const auto& section : sections) {
            float x = (section.column == 0) ? startX : startX + columnWidth + gap;
            float y = (section.column == 0) ? leftY : rightY;

            // ---- Section title ----
            sf::Text sectionTitle(*m_font, section.title, 22);
            sectionTitle.setFillColor(section.color);
            sectionTitle.setStyle(sf::Text::Bold);
            sectionTitle.setPosition({ x, y });

            if (y + 30.f > topClip && y < bottomClip) {
                m_window->draw(sectionTitle);
            }
            y += 28.f;

            // ---- Section body ----
            sf::Text sectionBody(*m_font, section.body, 16);
            sectionBody.setFillColor(sf::Color(210, 210, 220));
            sectionBody.setLineSpacing(1.3f);
            sectionBody.setPosition({ x + 15.f, y });

            if (y + 50.f > topClip && y < bottomClip) {
                m_window->draw(sectionBody);
            }

            // Calculate body height for column tracking
            int lineCount = 1;
            for (char c : section.body) if (c == '\n') lineCount++;
            float height = 28.f + lineCount * 22.f + 20.f;

            if (section.column == 0) leftY += height;
            else rightY += height;
        }

        // ---- Scroll indicator ----
        if (maxScroll > 0) {
            float scrollProgress = static_cast<float>(m_scrollOffset) / maxScroll;

            sf::RectangleShape scrollTrack({ 6.f, size.y * 0.30f });
            scrollTrack.setPosition({ center.x + size.x * 0.44f, center.y - size.y * 0.15f });
            scrollTrack.setFillColor(sf::Color(60, 60, 80, 100));
            m_window->draw(scrollTrack);

            sf::RectangleShape scrollHandle({ 6.f, size.y * 0.08f });
            scrollHandle.setPosition({ center.x + size.x * 0.44f,
                                       center.y - size.y * 0.15f + scrollProgress * size.y * 0.22f });
            scrollHandle.setFillColor(sf::Color(0, 220, 255, 180));
            m_window->draw(scrollHandle);

            // Show scroll percentage
            sf::Text percentText(*m_font, std::to_string(static_cast<int>(scrollProgress * 100)) + "%", 14);
            percentText.setFillColor(sf::Color(140, 140, 150));
            centerOrigin(percentText);
            percentText.setPosition({ center.x + size.x * 0.44f + 30.f, center.y - size.y * 0.15f + size.y * 0.15f });
            m_window->draw(percentText);
        }

        // ---- Back button (always at bottom) ----
        const bool isSelected = (m_selected == 0);
        sf::Text backBtn(*m_font, "Back to Menu", isSelected ? 32u : 26u);
        backBtn.setFillColor(isSelected ? sf::Color(255, 220, 60) : sf::Color(200, 200, 210));
        centerOrigin(backBtn);
        backBtn.setPosition({ center.x, center.y + size.y * 0.40f });
        m_window->draw(backBtn);

        if (isSelected) {
            drawSelectionCarets(backBtn);
        }

        // ---- Footer hint ----
        sf::Text hint(*m_font, "W/S or Up/Down to scroll   -   Enter to confirm", 16);
        hint.setFillColor(sf::Color(140, 140, 150));
        centerOrigin(hint);
        hint.setPosition({ center.x, center.y + size.y * 0.45f });
        m_window->draw(hint);
    }

    int getMaxScrollOffset() const {
        // Calculate based on actual content
        auto sections = getTutorialSections();
        int totalLines = 0;
        for (const auto& section : sections) {
            int lineCount = 1;
            for (char c : section.body) if (c == '\n') lineCount++;
            totalLines += lineCount + 1; // +1 for title
        }
        // Each line ~22px, sections have 20px spacing
        // Visible area is about 400px
        return std::max(0, totalLines * 25 - 400);
    }

    void drawSelectionCarets(const sf::Text& text) const {
        sf::ConvexShape caretL(3), caretR(3);
        const sf::Vector2f p = text.getPosition();
        const float cw = text.getGlobalBounds().size.x * 0.5f + 26.f;

        caretL.setPoint(0, { p.x - cw, p.y - 8.f });
        caretL.setPoint(1, { p.x - cw + 12.f, p.y });
        caretL.setPoint(2, { p.x - cw, p.y + 8.f });
        caretL.setFillColor(sf::Color(255, 220, 60));

        caretR.setPoint(0, { p.x + cw, p.y - 8.f });
        caretR.setPoint(1, { p.x + cw - 12.f, p.y });
        caretR.setPoint(2, { p.x + cw, p.y + 8.f });
        caretR.setFillColor(sf::Color(255, 220, 60));

        m_window->draw(caretL);
        m_window->draw(caretR);
    }

    static void centerOrigin(sf::Text& text) {
        sf::FloatRect b = text.getLocalBounds();
        text.setOrigin({ b.position.x + b.size.x * 0.5f, b.position.y + b.size.y * 0.5f });
    }

    sf::RenderWindow* m_window = nullptr;
    sf::Font* m_font = nullptr;

    GameState m_lastState = static_cast<GameState>(-1);
    std::vector<std::string> m_options;
    std::string m_title;
    int m_selected = 0;
    int m_score = 0;
    int m_scrollOffset = 0;  // For scrolling tutorial content
};