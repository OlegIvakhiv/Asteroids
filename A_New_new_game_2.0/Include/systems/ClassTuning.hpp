/**
 * @file ClassTuning.hpp
 * @brief Per-class FEEL: dodge tier, thermal profile, parry window, regen.
 *
 * ShipDesign derives what physics can derive -- mass, thrust, reactor. This
 * is the rest: rules that are a class identity rather than a consequence of
 * geometry. Dark Souls equip load, not Newton.
 *
 *   LIGHT   quick roll: longest, most i-frames, fastest recovery, cheapest.
 *           Runs hot, cools fast, big vent window, widest parry window.
 *   MEDIUM  standard roll. The golden middle; every scale here is 1.0.
 *   HEAVY   fat roll: short, few i-frames, long recovery, expensive.
 *           Soaks heat, cools slowly, tight vent window, narrowest parry.
 *
 * Parry OUTCOME is identical for every class. Only the timing window moves,
 * and the hull itself is the parry hitbox -- a heavy already reaches further,
 * so the narrower window is the trade, not a penalty stacked on a penalty.
 *
 * Every value can be overridden from Lua and hot-reloads with F5:
 *
 *   hull_classes = {
 *       light  = { dash_distance = 250, parry_window = 1.15, ... },
 *       medium = { ... },
 *       heavy  = { ... },
 *   }
 *
 * Missing keys fall back to the defaults below (sol2 get_or is silent -- a
 * typo in a key name reads as "use the default", not as an error).
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include <sol/sol.hpp>
#include "ShipDesign.hpp"

namespace ship {

    struct ClassFeel {
        // ---- Dodge ----
        float dashDistancePx = 210.f;   ///< Ground covered by the burst itself
        float dashDuration = 0.20f;     ///< Seconds of controlled burst
        float dashIframes = 0.20f;      ///< Invulnerability from the press
        float dashRecovery = 0.40f;     ///< Seconds after the burst before the next dodge
        float dashEnergyCost = 15.f;

        // ---- Thermal (multipliers on the weapon.* Lua values) ----
        float heatCapacity = 1.f;       ///< max_weapon_heat, heat_unlock_threshold
        float heatCool = 1.f;           ///< heat_cool_rate
        float heatVent = 1.f;           ///< heat_vent_rate
        float qteWindow = 1.f;          ///< qte_good_half, qte_perfect_half

        // ---- Misc ----
        float parryWindow = 1.f;        ///< parry_window multiplier
        float regen = 1.f;              ///< on top of the reactor's regen scale
    };

    inline ClassFeel defaultFeel(HullClass c) {
        ClassFeel f;
        switch (c) {
        case HullClass::Light:
            f.dashDistancePx = 250.f; f.dashDuration = 0.17f; f.dashIframes = 0.26f;
            f.dashRecovery = 0.22f;   f.dashEnergyCost = 12.f;
            f.heatCapacity = 0.75f;   f.heatCool = 1.40f; f.heatVent = 1.35f; f.qteWindow = 1.45f;
            f.parryWindow = 1.15f;    f.regen = 1.20f;
            break;
        case HullClass::Medium:
            break;   // all defaults
        case HullClass::Heavy:
            f.dashDistancePx = 160.f; f.dashDuration = 0.26f; f.dashIframes = 0.12f;
            f.dashRecovery = 0.70f;   f.dashEnergyCost = 20.f;
            f.heatCapacity = 1.35f;   f.heatCool = 0.72f; f.heatVent = 0.80f; f.qteWindow = 0.65f;
            f.parryWindow = 0.85f;    f.regen = 0.85f;
            break;
        }
        return f;
    }

    inline const char* classLuaKey(HullClass c) {
        switch (c) {
        case HullClass::Light: return "light";
        case HullClass::Heavy: return "heavy";
        default:               return "medium";
        }
    }

    /// Defaults, overlaid with `hull_classes.<class>` from Lua if present.
    inline ClassFeel classFeel(sol::state& lua, HullClass c) {
        ClassFeel f = defaultFeel(c);
        sol::optional<sol::table> all = lua["hull_classes"];
        if (!all) return f;
        sol::optional<sol::table> t = (*all)[classLuaKey(c)];
        if (!t) return f;

        f.dashDistancePx = (*t)["dash_distance"].get_or(f.dashDistancePx);
        f.dashDuration = std::max(0.05f, (*t)["dash_duration"].get_or(f.dashDuration));
        f.dashIframes = (*t)["dash_iframes"].get_or(f.dashIframes);
        f.dashRecovery = (*t)["dash_recovery"].get_or(f.dashRecovery);
        f.dashEnergyCost = (*t)["dash_cost"].get_or(f.dashEnergyCost);
        f.heatCapacity = (*t)["heat_capacity"].get_or(f.heatCapacity);
        f.heatCool = (*t)["heat_cool"].get_or(f.heatCool);
        f.heatVent = (*t)["heat_vent"].get_or(f.heatVent);
        f.qteWindow = (*t)["qte_window"].get_or(f.qteWindow);
        f.parryWindow = (*t)["parry_window"].get_or(f.parryWindow);
        f.regen = (*t)["regen"].get_or(f.regen);
        return f;
    }

    /// The feel for a player: its kit's class, or MEDIUM for a legacy ship.
    inline ClassFeel classFeelFor(sol::state& lua, const KitProfile& kit) {
        return classFeel(lua, kit.valid ? kit.hullClass : HullClass::Medium);
    }

} // namespace ship