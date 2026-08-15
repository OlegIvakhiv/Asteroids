#pragma once

#include <SFML/Graphics.hpp>
#include <map>
#include <string>

class InputRegistry {
public:
    static void init() {
        if (!s_keyMap.empty()) return; // Already initialised

        // Letters A-Z
        for (int i = 0; i < 26; ++i) {
            std::string name(1, 'A' + i);
            s_keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::A) + i);
        }
        // Numbers 0-9
        for (int i = 0; i < 10; ++i) {
            std::string name = std::to_string(i);
            s_keyMap[name] = static_cast<sf::Keyboard::Key>(static_cast<int>(sf::Keyboard::Key::Num0) + i);
        }
        // Special keys
        s_keyMap["Space"] = sf::Keyboard::Key::Space;
        s_keyMap["Enter"] = sf::Keyboard::Key::Enter;
        s_keyMap["LShift"] = sf::Keyboard::Key::LShift;
        s_keyMap["RShift"] = sf::Keyboard::Key::RShift;
        s_keyMap["LControl"] = sf::Keyboard::Key::LControl;
        s_keyMap["Escape"] = sf::Keyboard::Key::Escape;
        s_keyMap["Tab"] = sf::Keyboard::Key::Tab;

        // Mouse buttons
        s_mouseMap["MouseLeft"] = sf::Mouse::Button::Left;
        s_mouseMap["MouseRight"] = sf::Mouse::Button::Right;
        s_mouseMap["MouseMiddle"] = sf::Mouse::Button::Middle;
        s_mouseMap["MouseX1"] = sf::Mouse::Button::Extra1;
        s_mouseMap["MouseX2"] = sf::Mouse::Button::Extra2;
    }

    static bool isPressed(const std::string& name) {
        if (s_keyMap.count(name)) return sf::Keyboard::isKeyPressed(s_keyMap[name]);
        if (s_mouseMap.count(name)) return sf::Mouse::isButtonPressed(s_mouseMap[name]);
        return false;
    }

private:
    // inline static = defined here, no separate .cpp needed (C++17)
    inline static std::map<std::string, sf::Keyboard::Key> s_keyMap;
    inline static std::map<std::string, sf::Mouse::Button> s_mouseMap;
};