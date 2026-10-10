/**
 * @file HunterRecord.hpp
 * @brief The terminal's memory: contracts flown, hunters lost, codex sightings.
 *
 * Until now every number on the main terminal was either sector noise or lived
 * only as long as the process -- HUNTERS LOST reset to zero every launch, so
 * "YOU HAVE RETURNED 7 TIMES" could never be true. This is the one small file
 * that makes the honest numbers honest:
 *
 *   - CONTRACTS   every run started (ENGAGE / RE-ENGAGE / PURGE AND RESTART)
 *   - LOST        every run that ended in death
 *   - LEDGER      the last 16 runs: seconds in the field, lost or abandoned,
 *                 scrap carried. Drives the ledger bars and LAST CONTRACT.
 *   - CODEX       per key (archetype key, or an object category from
 *                 asteroids.lua `codex`): seen, destroyed, first contract.
 *
 * FORMAT: plain text, one record per line, so a broken file is readable and a
 * hand edit is possible. Unknown lines are skipped, not fatal. Lives in
 * saves/hunter_record.txt next to ships/.
 *
 *   contracts 42
 *   lost 7
 *   run 251.4 1 860
 *   codex BERSERKER 1 214 3
 *
 * WRITES are rare by design: at the end of a run and on quit. A sighting in
 * the middle of a fight only flips a flag in memory (`dirty`).
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <string>
#include <map>
#include <deque>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

namespace record {

    struct CodexStat {
        bool seen = false;
        int  kills = 0;
        int  firstContract = 0;   ///< 1-based contract number of the first sighting
    };

    struct RunEntry {
        float seconds = 0.f;
        bool  lost = false;       ///< false = abandoned / restarted / quit
        int   scrap = 0;
    };

    class HunterRecord {
    public:
        static constexpr std::size_t LEDGER_LEN = 16;

        // ---- Totals ----
        int contracts() const { return m_contracts; }
        int lost() const { return m_lost; }
        const std::deque<RunEntry>& runs() const { return m_runs; }
        bool hasRuns() const { return !m_runs.empty(); }
        const RunEntry& lastRun() const { return m_runs.back(); }

        /// Losses inside the visible ledger only. Corruption reads this, so it
        /// tracks how you are doing NOW instead of pinning at 99 for life.
        int recentLosses() const {
            int n = 0;
            for (const auto& r : m_runs) n += r.lost ? 1 : 0;
            return n;
        }

        // ---- Run bookkeeping ----
        void beginRun() { ++m_contracts; m_dirty = true; }

        void endRun(float seconds, bool lost, int scrap) {
            if (lost) ++m_lost;
            m_runs.push_back({ std::max(0.f, seconds), lost, std::max(0, scrap) });
            while (m_runs.size() > LEDGER_LEN) m_runs.pop_front();
            m_dirty = true;
        }

        // ---- Codex ----
        void see(const std::string& key) {
            if (key.empty()) return;
            CodexStat& c = m_codex[key];
            if (c.seen) return;
            c.seen = true;
            c.firstContract = std::max(1, m_contracts);
            m_dirty = true;
        }

        void kill(const std::string& key) {
            if (key.empty()) return;
            CodexStat& c = m_codex[key];
            if (!c.seen) { c.seen = true; c.firstContract = std::max(1, m_contracts); }
            ++c.kills;
            m_dirty = true;
        }

        const CodexStat* codex(const std::string& key) const {
            auto it = m_codex.find(key);
            return it == m_codex.end() ? nullptr : &it->second;
        }
        bool seen(const std::string& key) const {
            const CodexStat* c = codex(key);
            return c && c->seen;
        }

        // ---- Persistence ----
        static std::filesystem::path path() { return std::filesystem::path("saves") / "hunter_record.txt"; }

        bool load() {
            std::ifstream f(path());
            if (!f) return false;
            m_runs.clear();
            m_codex.clear();
            std::string line;
            while (std::getline(f, line)) {
                std::istringstream in(line);
                std::string tag;
                if (!(in >> tag)) continue;
                if (tag == "contracts") in >> m_contracts;
                else if (tag == "lost") in >> m_lost;
                else if (tag == "run") {
                    RunEntry r; int l = 0;
                    if (in >> r.seconds >> l >> r.scrap) { r.lost = (l != 0); m_runs.push_back(r); }
                }
                else if (tag == "codex") {
                    std::string key; int s = 0; CodexStat c;
                    if (in >> key >> s >> c.kills >> c.firstContract) { c.seen = (s != 0); m_codex[key] = c; }
                }
            }
            while (m_runs.size() > LEDGER_LEN) m_runs.pop_front();
            m_contracts = std::max(0, m_contracts);
            m_lost = std::clamp(m_lost, 0, m_contracts > 0 ? m_contracts : m_lost);
            m_dirty = false;
            return true;
        }

        bool save() {
            std::error_code ec;
            std::filesystem::create_directories(path().parent_path(), ec);
            std::ofstream f(path(), std::ios::trunc);
            if (!f) return false;
            f << "# VOID HUNTER // hunter record. Plain text; safe to read.\n";
            f << "contracts " << m_contracts << "\n";
            f << "lost " << m_lost << "\n";
            for (const auto& r : m_runs) f << "run " << r.seconds << " " << (r.lost ? 1 : 0) << " " << r.scrap << "\n";
            for (const auto& [k, c] : m_codex)
                f << "codex " << k << " " << (c.seen ? 1 : 0) << " " << c.kills << " " << c.firstContract << "\n";
            m_dirty = false;
            return static_cast<bool>(f);
        }

        bool dirty() const { return m_dirty; }
        void saveIfDirty() { if (m_dirty) save(); }

    private:
        int m_contracts = 0;
        int m_lost = 0;
        std::deque<RunEntry> m_runs;
        std::map<std::string, CodexStat> m_codex;
        bool m_dirty = false;
    };

} // namespace record
