/**
 * @file LuaConfig.hpp
 * @brief Cached handles to Lua config tables, invalidated by hot-reload.
 *
 * ============================================================================
 * WHY THIS EXISTS
 * ============================================================================
 * Every system used to read tuning like this, on every call:
 *
 *     sol::optional<sol::table> v = (*m_lua)["visuals"];
 *     if (!v) return def;
 *     return (*v)[key].get_or(def);
 *
 * The expensive part is not the key lookup, it is materialising a
 * sol::table: that takes a slot in the Lua registry (luaL_ref) and frees it
 * again when the temporary dies. Done a few hundred times a frame it is pure
 * overhead for data that only changes on F5.
 *
 * luacfg::Table keeps ONE reference to the named global table and re-fetches
 * it only when the config EPOCH moves. SystemManager bumps the epoch in
 * reloadScripts(), so hot-reload keeps working and -- the important bit --
 * no system can "forget to refresh". A forgotten refresh would be the exact
 * silent-stale-default trap sol2 is already known for in this codebase.
 *
 * Behaviour is identical to the old helpers: a missing table, or a missing
 * key, returns the C++ default.
 * ============================================================================
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#define SOL_ALL_SAFETIES_ON 1
#define SOL_LUA_VERSION 504
#define LUA_ERRGCMM 9

#include <sol/sol.hpp>
#include <cstdint>

namespace luacfg {

    // ------------------------------------------------------------------------
    // CONFIG EPOCH
    // ------------------------------------------------------------------------
    // One process-wide counter. It is not game state, it is a cache-busting
    // signal: "the Lua tables you are holding may have been replaced".
    // Starts at 1 so a freshly constructed handle (seen = 0) always fetches.
    inline uint32_t& epochRef() { static uint32_t e = 1; return e; }
    inline uint32_t  epoch() { return epochRef(); }
    inline void      bumpEpoch() { ++epochRef(); }

    /**
     * @brief A named global Lua table, fetched once per config epoch.
     *
     * Usage in a system -- the state is passed on every read rather than
     * bound in init(), so there is no init() line to forget and a restart
     * that re-runs init() needs nothing extra:
     *
     *     luacfg::Table m_visuals{ "visuals" };
     *     float cfg(const char* k, float d) const { return m_visuals.get(m_lua, k, d); }
     */
    class Table {
    public:
        Table() = default;
        explicit Table(const char* globalName) : m_name(globalName) {}

        /// get_or on the cached table; `def` if there is no state, no table,
        /// or no such key. Same contract as the helpers it replaces.
        template <typename T>
        T get(sol::state* lua, const char* key, T def) const {
            if (!refresh(lua)) return def;
            return m_table[key].get_or(def);
        }

        /// True if the global currently exists and is a table.
        bool valid(sol::state* lua) const { return refresh(lua); }

    private:
        bool refresh(sol::state* lua) const {
            if (!lua) return false;
            const uint32_t now = epoch();
            if (m_seen != now || m_lua != lua) {
                m_lua = lua;
                m_seen = now;
                sol::optional<sol::table> t = (*m_lua)[m_name];
                m_valid = t.has_value();
                m_table = m_valid ? *t : sol::table();
            }
            return m_valid;
        }

        const char* m_name = "";
        mutable sol::state* m_lua = nullptr;
        mutable sol::table m_table;
        mutable uint32_t   m_seen = 0;
        mutable bool       m_valid = false;
    };

} // namespace luacfg
