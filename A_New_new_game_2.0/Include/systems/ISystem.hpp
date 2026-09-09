/**
 * @file ISystem.hpp
 * @brief Base interface for all game systems
 *
 * Defines the SystemContext struct (dependency injection container)
 * and the ISystem abstract class. All game systems derive from ISystem
 * and implement init() and update().
 *
 * @author Oleg Ivakhiv
 * @version 1.1
 */

#pragma once

#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"
#include <sol/sol.hpp>
#include <box2d/box2d.h>
#include <SFML/Graphics.hpp>

 /**
  * @struct SystemContext
  * @brief Container for all engine dependencies passed to systems
  *
  * This struct holds pointers/references to all major engine components.
  * It is passed to each system's init() method, allowing systems to
  * store references to the components they need without global variables.
  *
  * @note All pointers are guaranteed to be valid during the lifetime
  *       of the game (they are owned by SystemManager).
  */
struct SystemContext {
    EntityManager* em = nullptr;          ///< Entity manager (component storage)
    EntityFactory* ef = nullptr;          ///< Entity factory (creation)
    b2WorldId worldId;                    ///< Box2D physics world
    uint32_t playerEntityId = 0;          ///< Persistent ID of the player entity
    sol::state* lua = nullptr;            ///< Lua scripting state
    sf::RenderWindow* window = nullptr;   ///< SFML render window
    sf::View* gameView = nullptr;   ///< Shared world view — written by CameraSystem
    /// Loaded unit and faction tables. Owned by SystemManager, valid for the
    /// lifetime of the game. Passed here rather than made global to match how
    /// every other dependency in this project travels.
    const enemyarch::EnemyRegistry* enemyRegistry = nullptr;
};

/**
 * @class ISystem
 * @brief Abstract base class for all game systems
 *
 * All systems must derive from this interface. The SystemManager
 * calls init() once after construction, then update() every frame.
 *
 * Systems are stateless in terms of external dependencies – they
 * store only pointers/references to engine components (provided via
 * SystemContext) and their own internal state (member variables).
 */
class ISystem {
public:
    /**
     * @brief Virtual destructor for proper cleanup of derived classes
     */
    virtual ~ISystem() = default;

    /**
     * @brief Initialise the system with the engine context
     * @param ctx SystemContext containing all dependencies
     *
     * Called once by SystemManager after all engine components are ready.
     * Systems should store the pointers/references they need for later use.
     * No heavy allocation should be done here – it's intended for
     * storing dependencies and resetting internal state.
     */
    virtual void init(const SystemContext& ctx) = 0;

    /**
     * @brief Update the system logic
     * @param dt Delta time in seconds since the last frame
     *
     * Called every frame by SystemManager. Systems should perform
     * their main logic here (e.g., AI decisions, physics stepping,
     * rendering, particle spawning).
     *
     * Rendering systems may draw directly to the window; the
     * SystemManager ensures the view is set appropriately before
     * calling update() on rendering systems.
     */
    virtual void update(float dt) = 0;
};