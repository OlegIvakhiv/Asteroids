/**
 * @file game.cpp
 * @brief Main game loop and entry point for the Modular Space Engine
 *
 * @author Oleg Ivakhiv
 * @version 1.2 (refactored with SystemManager)
 */

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <SFML/Graphics.hpp>
#include "core/SystemManager.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <algorithm>

int main() {
    // =========================================================================
    // LUA SCRIPTING INITIALIZATION
    // =========================================================================

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::math);

    try {
        lua.script_file("scripts/player.lua");
        lua.script_file("scripts/asteroids.lua");
        lua.script_file("scripts/enemy.lua");
        std::cout << "Scripts reloaded!" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Could not load Lua script: " << e.what() << std::endl;
        return 1;
    }

    // =========================================================================
    // SFML WINDOW & RENDERING SETUP
    // =========================================================================

    sf::RenderWindow window(sf::VideoMode({ 1920, 1080 }), "Modular Space Engine");
    window.setFramerateLimit(60);

    // Load font for UI text
    sf::Font font;
    if (!font.openFromFile("assets/upheavtt.ttf")) {
        std::cout << "Warning: Could not load font 'assets/upheavtt.ttf'!" << std::endl;
    }

    // (scoreText removed -- HudSystem owns the score display now.)

    // =========================================================================
    // SYSTEM MANAGER INITIALIZATION
    // =========================================================================

    SystemManager manager(window, lua);
    manager.init();
    manager.getHudSystem().setFont(&font);
    manager.getMenuSystem().setFont(&font);

    EntityManager& em = manager.getEntityManager();
    uint32_t playerEntityId = manager.getPlayerId();

    // =========================================================================
    // MAIN GAME LOOP
    // =========================================================================

    sf::Clock clock;

    bool escWasPressed = false;
    bool upWasPressed = false;
    bool downWasPressed = false;
    bool enterWasPressed = false;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
        }

        // ---- HOT RELOAD (F5 key) ----
        // Reloads ALL THREE scripts. It used to reload only enemy.lua, which
        // meant every value in `visuals`, `weapon`, `asteroid_visuals` and the
        // asteroid type tables was silently NOT hot-reloadable despite living
        // in Lua specifically so it would be.
        //
        // Edge-detected: isKeyPressed is level-triggered, so holding F5 used to
        // re-parse and re-execute all scripts ~60 times per second.
        static bool f5WasPressed = false;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F5)) {
            if (!f5WasPressed) {
                try {
                    lua.script_file("scripts/player.lua");
                    lua.script_file("scripts/asteroids.lua");
                    lua.script_file("scripts/enemy.lua");
                    std::cout << "Scripts reloaded!" << std::endl;
                }
                catch (const std::exception& e) {
                    std::cerr << "Failed to reload Lua script: " << e.what() << std::endl;
                }
            }
            f5WasPressed = true;
        }
        else {
            f5WasPressed = false;
        }

        // ---- DELTA TIME ----
        float dt = clock.restart().asSeconds();
        dt = std::min(dt, 0.05f);

        // ---- ESCAPE: toggle pause, only meaningful while playing/paused ----
        bool escPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape);
        if (escPressed && !escWasPressed) {
            GameState s = manager.getState();
            if (s == GameState::Playing || s == GameState::Paused) {
                manager.togglePause();
            }
        }
        escWasPressed = escPressed;

        // ---- T key for quick tutorial from pause ----
        static bool tWasPressed = false;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::T)) {
            if (!tWasPressed) {
                if (manager.getState() == GameState::Paused) {
                    manager.requestAction(MenuAction::ShowTutorial);
                }
            }
            tWasPressed = true;
        }
        else {
            tWasPressed = false;
        }

        // ---- MENU NAVIGATION: only outside active gameplay ----
        if (manager.getState() != GameState::Playing) {
            bool upPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Up);
            bool downPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::S)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Down);
            bool enterPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);

            // Tutorial screen uses W/S for scrolling instead of selection
            if (manager.getState() == GameState::Tutorial) {
                
                if (upPressed && !upWasPressed) {
                    manager.getMenuSystem().scrollTutorial(1);  
                }
                
                if (downPressed && !downWasPressed) {
                    manager.getMenuSystem().scrollTutorial(-1);   
                }
                // Enter still confirms the "Back" button
                if (enterPressed && !enterWasPressed) {
                    MenuAction action = manager.getMenuSystem().confirmSelection();
                    manager.requestAction(action);
                }
            }
            else {
                // Normal menu navigation
                if (upPressed && !upWasPressed)     manager.getMenuSystem().moveSelection(-1);
                if (downPressed && !downWasPressed) manager.getMenuSystem().moveSelection(1);
                if (enterPressed && !enterWasPressed) {
                    MenuAction action = manager.getMenuSystem().confirmSelection();
                    manager.requestAction(action);
                }
            }

            upWasPressed = upPressed;
            downWasPressed = downPressed;
            enterWasPressed = enterPressed;
        }
        else {
            upWasPressed = downWasPressed = enterWasPressed = false;
        }

        static bool f3WasPressed = false;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F3)) {
            if (!f3WasPressed) {
                manager.getDebugSystem().toggle();
                std::cout << "Debug " << (manager.getDebugSystem().isEnabled() ? "ON" : "OFF") << std::endl;
            }
            f3WasPressed = true;
        }
        else {
            f3WasPressed = false;
        }


        // =====================================================================
        // CLEAR THE WINDOW (CRITICAL – prevents ghosting)
        // =====================================================================
        window.clear(sf::Color(10, 10, 15));

        // =====================================================================
        // UPDATE ALL GAME SYSTEMS (includes rendering)
        // =====================================================================
        manager.update(dt);

        // =====================================================================
        // UI RENDERING (screen space)
        // =====================================================================
        // NOTE: the HUD is drawn by HudSystem inside manager.update(). The
        // duplicate scoreText draw that used to be here was never setString'd
        // inside the loop, so it rendered stale text on top of the real HUD.
        //
        // The `hp` / `statTf` locals that used to be here indexed with a
        // playerIdx captured BEFORE manager.update() -- which destroys entities
        // -- so the index could be stale by the time it was used. Both were
        // unused leftovers from the pre-HudSystem bars.
        // ---- Present the frame ----
        window.display();
    }

    return 0;
}