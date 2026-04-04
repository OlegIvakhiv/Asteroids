/**
 * @file game.cpp
 * @brief Main game loop and entry point for the Modular Space Engine
 * 
 * Initializes Box2D physics world, Lua scripting, and SFML rendering.
 * Main loop follows a fixed order of systems:
 * 1. Input handling (player controls)
 * 2. Physics simulation (Box2D step)
 * 3. World cleanup (despawn distant entities)
 * 4. Enemy spawning (timed intervals)
 * 5. Damage processing (collisions, death)
 * 6. Weapon updates (shooting, bullet lifetime)
 * 7. Particle effects (update and draw)
 * 8. AI behavior (enemy state machine)
 * 9. Background parallax scrolling
 * 10. Rendering (UI, particles, entities, stars)
 * 
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <SFML/Graphics.hpp>
#include "EntityManager.hpp"
#include "Systems.hpp"
#include <sol/sol.hpp>
#include <iostream>
#include <vector>
#include <random>

// Legacy global score variable (not used - em.totalScore is the source of truth)
int score = 0;


/**
 * @brief Main entry point
 * @return Exit code (0 on success)
 */
int main() {
    // =========================================================================
    // BOX2D PHYSICS WORLD INITIALIZATION
    // =========================================================================
    
    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = { 0.0f, 0.0f };      // No gravity in space
    b2WorldId worldId = b2CreateWorld(&worldDef);

    // =========================================================================
    // LUA SCRIPTING INITIALIZATION
    // =========================================================================
    
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::math);  // Basic Lua + math functions
    InputRegistry::init();                               // Setup keyboard/mouse mappings

    // Load configuration scripts
    try {
        lua.script_file("scripts/player.lua");      // Player stats, shape, colors
        lua.script_file("scripts/asteroids.lua");   // Asteroid types and spawn settings
        lua.script_file("scripts/enemy.lua");       // Enemy stats and behavior
    } catch (const std::exception& e) {
        std::cerr << "Could not load Lua script: " << e.what() << std::endl;
    }

    // =========================================================================
    // SFML WINDOW & RENDERING SETUP
    // =========================================================================
    
    sf::RenderWindow window(sf::VideoMode({ 1920, 1080 }), "Modular Space Engine");
    window.setFramerateLimit(60);               // Cap at 60 FPS
    
    EntityManager em;                           // Create entity manager
    uint32_t playerEntityId = em.createPlayer({ 640.f, 360.f }, lua, worldId);
    
    em.initBackground(window.getSize(), 400);   // Generate starfield

    sf::Clock clock;                            // Delta time measurement

    // Load font for UI text
    sf::Font font;
    if (!font.openFromFile("assets/upheavtt.ttf")) {
        std::cout << "Error loading font!" << std::endl;
    }

    // Score display text
    sf::Text scoreText(font);
    scoreText.setCharacterSize(30);
    scoreText.setFillColor(sf::Color::Yellow);
    scoreText.setPosition({ 20.f, 60.f });

    // Camera view (will follow player)
    sf::View gameView = window.getDefaultView();

    // =========================================================================
    // MAIN GAME LOOP
    // =========================================================================
    
    while (window.isOpen()) {
        // ---------------------------------------------------------------------
        // HOT RELOAD (F5 key) - Allows live script updates during development
        // ---------------------------------------------------------------------
        if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F5)) {
            lua.script_file("scripts/enemy.lua");
            std::cout << "AI Script Reloaded!" << std::endl;
        }

        // ---------------------------------------------------------------------
        // EVENT HANDLING
        // ---------------------------------------------------------------------
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
            if (event->is<sf::Event::FocusLost>()) {
                // Optional: pause game when window loses focus
            }
        }

        // ---------------------------------------------------------------------
        // DELTA TIME (time since last frame)
        // ---------------------------------------------------------------------
        float dt = clock.restart().asSeconds();

        // ---------------------------------------------------------------------
        // GET PLAYER INDEX (entity ID is stable, index changes with deletions)
        // ---------------------------------------------------------------------
        size_t playerIdx = em.getEntityIndex(playerEntityId);
        if (playerIdx == (size_t)-1) {
            // Player is dead - exit game loop
            break;
        }

        // =====================================================================
        // GAME SYSTEMS UPDATE (executed in order every frame)
        // =====================================================================
        
        InputSystem::update(em, playerEntityId, dt, window, lua);   // Player controls
        PhysicsSystem::update(em, worldId, dt);                     // Box2D simulation
        PhysicsSystem::cleanup(em, worldId, playerEntityId, lua);   // Despawn distant entities
        EnemySystem::update(em, lua, worldId, playerEntityId);      // Spawn enemies/asteroids
        DamageSystem::update(em, worldId, playerEntityId, dt, lua); // Collisions, death, scoring
        WeaponSystem::update(em, worldId, playerEntityId, dt, lua); // Shooting, bullet lifetime
        ParticleSystem::update(em, dt);                             // Explosion particles
        AISystem::update(em, playerEntityId, dt, lua);              // Enemy AI behavior

        // Background parallax scrolling (needs player velocity)
        auto& playerPhysics = em.physics[playerIdx];
        b2Vec2 b2Vel = b2Body_GetLinearVelocity(playerPhysics.bodyId);
        sf::Vector2f playerVel(b2Vel.x * SCALE, b2Vel.y * SCALE);
        BackgroundSystem::update(em, playerVel, window.getSize(), dt);

        // =====================================================================
        // RENDERING
        // =====================================================================
        
        window.clear(sf::Color(10, 10, 15));    // Dark space background

        // ---------------------------------------------------------------------
        // UI ELEMENTS (drawn in screen space, not affected by camera)
        // ---------------------------------------------------------------------
        
        auto& hp = em.healths[playerIdx];       // Player health for health bar
        auto& tf = em.transforms[playerIdx];    // Player transform for energy bar

        // Score text
        scoreText.setString("Score: " + std::to_string(em.totalScore));
        window.setView(window.getDefaultView());
        window.draw(scoreText);

        // Health bar (red)
        sf::RectangleShape healthBarBack({ 200.f, 20.f });
        healthBarBack.setPosition({ 20.f, 20.f });
        healthBarBack.setFillColor(sf::Color(50, 50, 50));   // Dark gray background

        float displayHp = std::max(0.f, std::min(hp.currentHp, hp.maxHp));
        float barWidth_HP = (displayHp / hp.maxHp) * 200.f;

        sf::RectangleShape healthBarFront({ barWidth_HP, 20.f });
        healthBarFront.setPosition({ 20.f, 20.f });
        healthBarFront.setFillColor(sf::Color::Red);        // Red fill

        window.draw(healthBarBack);
        window.draw(healthBarFront);

        // Energy bar (cyan normally, orange when overheated)
        sf::RectangleShape energyBarBack({ 200.f, 10.f });
        energyBarBack.setPosition({ 20.f, 45.f });
        energyBarBack.setFillColor(sf::Color(50, 50, 50));   // Dark gray background

        float displayEnergy = std::max(0.f, std::min(tf.energyDrive, tf.maxEnergyDrive));
        float barWidth_energy = (displayEnergy / tf.maxEnergyDrive) * 200.f;

        sf::RectangleShape energyBarFront({ barWidth_energy, 10.f });
        energyBarFront.setPosition({ 20.f, 45.f });

        // Color changes based on overheat state
        if (tf.overheatTimer > 0) {
            energyBarFront.setFillColor(sf::Color(255, 69, 0));   // OrangeRed (overheated)
        } else {
            energyBarFront.setFillColor(sf::Color(0, 191, 255));  // DeepSkyBlue (normal)
        }

        window.draw(energyBarBack);
        window.draw(energyBarFront);

        // ---------------------------------------------------------------------
        // GAME WORLD (camera follows player)
        // ---------------------------------------------------------------------
        
        sf::Vector2f playerPos = em.transforms[playerIdx].position;
        gameView.setCenter(playerPos);
        window.setView(gameView);

        // Draw particles (explosions, impacts)
        ParticleSystem::draw(window, em);

        // Draw stars (parallax background - uses default view)
        window.setView(window.getDefaultView());
        BackgroundSystem::draw(window, em);

        // Draw all game entities (player, asteroids, enemies, bullets)
        RenderSystem::draw(em, window, lua, playerEntityId);

        // Present the frame to the screen
        window.display();
    }
    
    return 0;
}