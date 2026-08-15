/**
 * @file GameConfig.hpp
 * @author Oleg Ivakhiv
 * @brief Part of the Modular Space Engine - Bachelor's Thesis Project
 * @version 1.1
 */


#pragma once

#include <SFML/Graphics.hpp>

namespace GameConfig {

	// ============================================================================
	// WINDOW & DISPLAY
	// ============================================================================

	constexpr sf::Vector2u WINDOW_SIZE{ 1920, 1080 };
	constexpr const char* WINDOW_TITLE = "Modular Space Engine";
	constexpr int WINDOW_FPS = 60;

	// ============================================================================
	// GAME WORLD
	// ============================================================================

	constexpr float PHYSICS_SCALE = 30.f;  // Box2D scale factor
	constexpr float GRAVITY_X = 0.0f;
	constexpr float GRAVITY_Y = 0.0f;

	// Viewport dimensions
	constexpr float VIEWPORT_WIDTH = 1280.f;
	constexpr float VIEWPORT_HEIGHT = 720.f;

	// ============================================================================
	// BACKGROUND & VISUALS
	// ============================================================================

	constexpr int STAR_COUNT = 400;
	constexpr int STAR_MIN_ALPHA = 100;
	constexpr int STAR_MAX_ALPHA = 255;

	// ============================================================================
	// ASTEROID SPAWNING & PHYSICS
	// ============================================================================

	constexpr float ASTEROID_SPAWN_INTERVAL = 0.5f;
	constexpr float ASTEROID_DESPAWN_RADIUS = 100.0f;
	constexpr int MAX_ASTEROIDS = 60;
	constexpr float ASTEROID_SPAWN_RADIUS = 2500.f;

	// ============================================================================
	// ENEMY SPAWNING & PHYSICS
	// ============================================================================

	constexpr float ENEMY_DESPAWN_RADIUS = 250.0f;
	constexpr int MAX_ENEMIES = 10;
	constexpr float ENEMY_SPAWN_RADIUS = 2500.f;

	// ============================================================================
	// PHYSICS SIMULATION
	// ============================================================================

	constexpr float FIXED_TIMESTEP = 1.0f / 60.0f;  // 60 Hz fixed physics
	constexpr int PHYSICS_SUBSTEPS = 6;

	// ============================================================================
	// PLAYER MECHANICS (loaded from Lua, but defaults here)
	// ============================================================================

	constexpr float PLAYER_DEFAULT_HP = 100.0f;
	constexpr float PLAYER_DEFAULT_ENERGY = 100.0f;

	// Invulnerability frames after taking damage
	constexpr float PLAYER_INVUL_TIME = 0.4f;      // Full invulnerability
	constexpr float PLAYER_CHEAP_INVUL_TIME = 0.1f; // Flash effect invul

	// ============================================================================
	// UI POSITIONS & SIZES
	// ============================================================================

	// Health bar
	constexpr sf::Vector2f HEALTH_BAR_POS{ 20.f, 20.f };
	constexpr sf::Vector2f HEALTH_BAR_SIZE{ 200.f, 20.f };

	// Energy bar
	constexpr sf::Vector2f ENERGY_BAR_POS{ 20.f, 45.f };
	constexpr sf::Vector2f ENERGY_BAR_SIZE{ 200.f, 10.f };

	// Score text
	constexpr sf::Vector2f SCORE_TEXT_POS{ 20.f, 60.f };
	constexpr unsigned int SCORE_TEXT_SIZE = 30;

	// ============================================================================
	// COLOR DEFINITIONS
	// ============================================================================

	// UI colors
	constexpr sf::Color COLOR_HEALTH_BG{ 50, 50, 50 };
	constexpr sf::Color COLOR_HEALTH_FILL{ 255, 0, 0 };
	constexpr sf::Color COLOR_ENERGY_NORMAL{ 0, 191, 255 };      // DeepSkyBlue
	constexpr sf::Color COLOR_ENERGY_OVERHEAT{ 255, 69, 0 };    // OrangeRed
	constexpr sf::Color COLOR_SCORE_TEXT{ 255, 255, 0 };        // Yellow

	// ============================================================================
	// ASSET PATHS
	// ============================================================================

	constexpr const char* PATH_FONT = "assets/upheavtt.ttf";
	constexpr const char* PATH_SCRIPT_PLAYER = "scripts/player.lua";
	constexpr const char* PATH_SCRIPT_ASTEROIDS = "scripts/asteroids.lua";
	constexpr const char* PATH_SCRIPT_ENEMY = "scripts/enemy.lua";

	// ============================================================================
	// DEBUG & DEVELOPMENT
	// ============================================================================

	constexpr bool DEBUG_SHOW_PHYSICS = false;
	constexpr bool DEBUG_LOG_ENTITIES = false;
	constexpr bool ENABLE_LUA_HOT_RELOAD = true;  // F5 to reload scripts

} // namespace GameConfig