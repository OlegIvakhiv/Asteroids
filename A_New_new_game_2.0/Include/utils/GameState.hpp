/**
 * @file GameState.hpp
 * @brief Top-level game state machine
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

enum class GameState {
    MainMenu,
    Playing,
    Paused,
    GameOver,
    Tutorial,
    Refit
};

/**
 * @enum MenuAction
 * @brief Result of confirming a menu selection, consumed by SystemManager/game.cpp
 */
enum class MenuAction {
    None,
    StartGame,
    ResumeGame,
    RestartGame,
    QuitGame,
    ShowTutorial,
    BackToMenu,
    ShowRefit
};
