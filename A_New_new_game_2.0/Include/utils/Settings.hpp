/**
 * @file Settings.hpp
 * @brief MACHINE RITES: the player's settings, saves/settings.txt.
 *
 * Same shape as HunterRecord: plain text, one `key value` per line, written
 * when the settings page closes. A missing or half-written file is fine --
 * whatever is missing keeps its default, and unknown keys are skipped, so a
 * file from an older or newer build still loads.
 *
 * For now: sound levels only. Everything is 0..1 (shown as 0-100%).
 * DISPLAY and CONTROLS have rows in the page, sealed, for later builds.
 *
 * @author Oleg Ivakhiv
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace settings {

    /// The mixer channels, in the order the page lists them.
    enum Channel : int { MASTER = 0, EFFECTS, TELLS, AMBIENCE, INTERFACE, CHANNEL_COUNT };

    struct Settings {
        std::array<float, CHANNEL_COUNT> volume{ 0.8f, 1.f, 1.f, 0.8f, 0.8f };

        static constexpr std::array<float, CHANNEL_COUNT> DEFAULTS{ 0.8f, 1.f, 1.f, 0.8f, 0.8f };

        /// File keys -- never rename one: old files would lose that level.
        static const char* key(int c) {
            static const char* k[CHANNEL_COUNT] = { "volume_master", "volume_effects", "volume_tells",
                "volume_ambience", "volume_interface" };
            return (c >= 0 && c < CHANNEL_COUNT) ? k[c] : "";
        }

        float get(int c) const { return (c >= 0 && c < CHANNEL_COUNT) ? volume[c] : 0.f; }
        void set(int c, float v) {
            if (c < 0 || c >= CHANNEL_COUNT) return;
            v = std::clamp(v, 0.f, 1.f);
            if (v != volume[c]) { volume[c] = v; m_dirty = true; }
        }
        void restoreDefaults() {
            for (int c = 0; c < CHANNEL_COUNT; ++c) set(c, DEFAULTS[c]);
        }
        bool isDefault() const {
            for (int c = 0; c < CHANNEL_COUNT; ++c) if (std::abs(volume[c] - DEFAULTS[c]) > 0.001f) return false;
            return true;
        }

        // ---- Persistence ----
        static std::filesystem::path path() { return std::filesystem::path("saves") / "settings.txt"; }

        bool load() {
            std::ifstream f(path());
            if (!f) return false;
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream in(line);
                std::string k; float v = 0.f;
                if (!(in >> k >> v)) continue;
                for (int c = 0; c < CHANNEL_COUNT; ++c)
                    if (k == key(c)) volume[c] = std::clamp(v, 0.f, 1.f);
            }
            m_dirty = false;
            return true;
        }

        bool save() {
            std::error_code ec;
            std::filesystem::create_directories(path().parent_path(), ec);
            std::ofstream f(path(), std::ios::trunc);
            if (!f) return false;
            f << "# VOID HUNTER // machine rites. Plain text; levels are 0.0 - 1.0.\n";
            for (int c = 0; c < CHANNEL_COUNT; ++c) f << key(c) << " " << volume[c] << "\n";
            m_dirty = false;
            return static_cast<bool>(f);
        }

        bool dirty() const { return m_dirty; }
        void saveIfDirty() { if (m_dirty) save(); }

    private:
        bool m_dirty = false;
    };

} // namespace settings
