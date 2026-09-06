/**
 * @file game.cpp
 * @brief Main game loop and entry point for VOID HUNTER
 *
 * @author Oleg Ivakhiv
 * @version 1.3 (window sizing, fullscreen toggle, refit input routing)
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
    // WINDOW
    // =========================================================================
    //
    // VideoMode sizes the CLIENT AREA, not the whole window. Asking for
    // 1920x1080 on a 1920x1080 desktop makes the OS add a titlebar and borders
    // on top of that, so the total exceeds the screen and the bottom of the
    // frame is pushed off-display -- which is what was eating the status bar
    // and the bottom fifth of every menu.
    //
    // 85% of the desktop leaves room for decoration on any monitor. F11 gives
    // real fullscreen, which is what most people actually want.
    // =========================================================================

    const sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
    const sf::Vector2u windowedSize{
        static_cast<unsigned>(desktop.size.x * 0.85f),
        static_cast<unsigned>(desktop.size.y * 0.85f)
    };

    sf::RenderWindow window(sf::VideoMode(windowedSize), "VOID HUNTER");
    window.setFramerateLimit(60);

    bool fullscreen = false;

    // Load font for UI text
    sf::Font font;
    if (!font.openFromFile("assets/upheavtt.ttf")) {
        std::cout << "Warning: Could not load font 'assets/upheavtt.ttf'!" << std::endl;
    }

    // =========================================================================
    // SYSTEM MANAGER INITIALIZATION
    // =========================================================================

    SystemManager manager(window, lua);
    manager.init();
    manager.getHudSystem().setFont(&font);
    manager.getMenuSystem().setFont(&font);
    manager.getRefitSystem().setFont(&font);

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
            // Resize needs no view fix-up here: every UI system rebuilds its
            // own screen view from the live window size each frame (uiView()),
            // and CameraSystem owns the world view. Draining the event is
            // enough. Do NOT reintroduce getDefaultView() anywhere -- it is
            // frozen at creation size and is what broke mouse input on resize.
            if (const auto* rs = event->getIf<sf::Event::Resized>()) {
                // Re-apply a view matching the new size so the world doesn't
                // stretch. UI screens build their own view each frame.
                window.setView(sf::View(sf::FloatRect({ 0.f, 0.f },
                    { static_cast<float>(rs->size.x),
                      static_cast<float>(rs->size.y) })));
            }
        }

        // ---- F11: toggle real fullscreen ----
        // Recreating the window keeps the same sf::RenderWindow object, so
        // every pointer systems hold stays valid. The framerate limit does NOT
        // survive create(), so it has to be reapplied.
        static bool f11WasPressed = false;
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F11)) {
            if (!f11WasPressed) {
                fullscreen = !fullscreen;
                window.create(
                    fullscreen ? desktop
                    : sf::VideoMode(windowedSize),
                    "VOID HUNTER",
                    fullscreen ? sf::State::Fullscreen : sf::State::Windowed);
                window.setFramerateLimit(60);
            }
            f11WasPressed = true;
        }
        else {
            f11WasPressed = false;
        }

        // ---- HOT RELOAD (F5 key) ----
        // Reloads ALL THREE scripts. Edge-detected: isKeyPressed is
        // level-triggered, so holding F5 used to re-parse every frame.
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
        // Refit handles its own Escape, so it must not also reach this.
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

        // ---- MENU NAVIGATION ----
        //
        // Refit is excluded: RefitSystem polls its own keyboard and mouse. If
        // both ran, the Enter that leaves the refit bay would ALSO be read
        // here as a menu confirm on whatever row happened to be selected.
        const GameState navState = manager.getState();
        if (navState != GameState::Playing && navState != GameState::Refit) {
            bool upPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Up);
            bool downPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::S)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Down);
            bool enterPressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);

            // Boot overlay: ANY input skips it, and that same input is
            // swallowed so you never accidentally launch a run while trying
            // to dismiss the intro.
            if (manager.getMenuSystem().isBooting()) {
                if (upPressed || downPressed || enterPressed
                    || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Space)
                    || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)) {
                    manager.getMenuSystem().skipBoot();
                }
                upWasPressed = upPressed;
                downWasPressed = downPressed;
                enterWasPressed = enterPressed;
            }
            else if (navState == GameState::Tutorial) {
                if (upPressed && !upWasPressed)     manager.getMenuSystem().scrollTutorial(1);
                if (downPressed && !downWasPressed) manager.getMenuSystem().scrollTutorial(-1);
                if (enterPressed && !enterWasPressed) {
                    manager.requestAction(manager.getMenuSystem().confirmSelection());
                }
                upWasPressed = upPressed;
                downWasPressed = downPressed;
                enterWasPressed = enterPressed;
            }
            else {
                if (upPressed && !upWasPressed)     manager.getMenuSystem().moveSelection(-1);
                if (downPressed && !downWasPressed) manager.getMenuSystem().moveSelection(1);
                if (enterPressed && !enterWasPressed) {
                    manager.requestAction(manager.getMenuSystem().confirmSelection());
                }
                upWasPressed = upPressed;
                downWasPressed = downPressed;
                enterWasPressed = enterPressed;
            }
        }
        else {
            // Clear the edge flags on the way out so the first keypress after
            // returning to a menu is seen as a fresh press, not a held one.
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
        // CLEAR THE WINDOW (CRITICAL - prevents ghosting)
        // =====================================================================
        window.clear(sf::Color(2, 3, 5));

        // =====================================================================
        // UPDATE ALL GAME SYSTEMS (includes rendering)
        // =====================================================================
        manager.update(dt);

        // ---- Present the frame ----
        window.display();
    }

    return 0;
}