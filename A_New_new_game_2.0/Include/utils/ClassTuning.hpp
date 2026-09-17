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
 * CHANGED in 1.3 -- GUNSHIP IDENTITY:
 *   damage_reduction      passive, every source
 *   hyperarmor_reduction  extra, during the dodge burst
 *   shoulder_bash         1 = the dodge is a cheap bash: damage + stagger + stun
 *                         on the first enemy it hits. Breaks a Berserker charge.
 *   ramming               1 = fast contact smashes small rocks for free, cracks
 *                         medium ones for a fraction of the damage, hurts enemies
 *   Poise regen reworked: LIGHT small and quick, MEDIUM fast, HEAVY huge but
 *   very slow -- a gunship that has eaten a chain has to earn its armour back.
 *
 * CHANGED in 1.2 -- POISE. Mass finally pays for itself:
 *   poise          hits drain it instead of tumbling you; at zero the next hit
 *                  breaks it and THAT hit tumbles. Refills after a break.
 *   poise_regen    per second, after poise_delay seconds without a hit
 *   knockback      multiplier on every shove the ship takes
 *   tumble         multiplier on stagger duration when poise does break
 *   hyperarmor     1 = poise cannot be drained during the dodge burst:
 *                  the heavy shoulders THROUGH a charge instead of around it
 * Poise damage per hit = knockback of that hit x visuals.poise_per_knockback
 * (default 0.1: bash 95, ram 110, asteroid 90, blast 40-90).
 *   LIGHT 0    everything tumbles -- dodge it
 *   MEDIUM 80  eats blast splash, not a direct bash / ram / asteroid
 *   HEAVY 340  holds a full ram chain; hyperarmor dodge eats one more for free
 *
 * CHANGED in 1.1: longer dodges, and a DRIFT after the burst -- momentum
 * bleeds off over dash_drift seconds instead of stopping on a pixel. Heavy
 * drifts longest: in open space the big ship should feel it can't brake.
 *
 * @version 1.3
 */

#pragma once

#include <sol/sol.hpp>
#include "ShipDesign.hpp"

namespace ship {

    struct ClassFeel {
        // ---- Dodge ----
        float dashDistancePx = 250.f;   ///< Ground covered by the burst itself (drift adds more)
        float dashDuration = 0.21f;     ///< Seconds of controlled burst
        float dashIframes = 0.20f;      ///< Invulnerability from the press
        float dashRecovery = 0.40f;     ///< Seconds after the burst before the next dodge
        float dashEnergyCost = 15.f;
        float dashCarry = 0.28f;        ///< Share of the burst's average speed kept when it ends
        float dashDrift = 0.32f;        ///< Seconds to bleed that carry back to your entry speed

        // ---- Thermal (multipliers on the weapon.* Lua values) ----
        float heatCapacity = 1.f;       ///< max_weapon_heat, heat_unlock_threshold
        float heatCool = 1.f;           ///< heat_cool_rate
        float heatVent = 1.f;           ///< heat_vent_rate
        float qteWindow = 1.f;          ///< qte_good_half, qte_perfect_half

        // ---- Misc ----
        float parryWindow = 1.f;        ///< parry_window multiplier
        float regen = 1.f;              ///< on top of the reactor's regen scale

        // ---- Poise ----
        float poise = 80.f;             ///< 0 = every stagger-grade hit tumbles. Medium eats splash.
        float poiseRegen = 60.f;        ///< per second. Medium keeps the fast regen.
        float poiseDelay = 1.2f;        ///< seconds without a hit before regen starts
        float knockback = 0.85f;        ///< shove multiplier
        float tumble = 1.f;             ///< stagger duration multiplier on a break
        float hyperarmor = 0.f;         ///< 1 = no poise loss during the dodge burst

        // ---- Damage ----
        float damageReduction = 0.f;    ///< passive, 0..0.9
        float hyperarmorReduction = 0.f;///< extra during the dodge burst, 0..0.9

        // ---- Shoulder bash (the dodge as a weapon) ----
        float shoulderBash = 0.f;       ///< 1 = on
        float shoulderDamage = 20.f;
        float shoulderKnock = 750.f;    ///< px/s given to the enemy
        float shoulderStun = 0.9f;      ///< seconds the enemy cannot move after the tumble starts
        float shoulderCounter = 1.75f;  ///< damage multiplier when it breaks a charge

        // ---- Ramming (fast contact) ----
        float ramming = 0.f;            ///< 1 = on
        float ramMinSpeed = 12.f;       ///< m/s of closing speed
        float ramDamagePerSpeed = 3.f;  ///< damage dealt per m/s above ramMinSpeed
        float ramBaseDamage = 15.f;
        float ramMediumTaken = 0.35f;   ///< share of a medium asteroid's damage still taken
    };

    inline ClassFeel defaultFeel(HullClass c) {
        ClassFeel f;
        f.dashDistancePx = 250.f; f.dashDuration = 0.21f;
        switch (c) {
        case HullClass::Light:
            f.dashDistancePx = 290.f; f.dashDuration = 0.18f; f.dashIframes = 0.26f;
            f.dashRecovery = 0.22f;   f.dashEnergyCost = 12.f;
            f.dashCarry = 0.20f;      f.dashDrift = 0.22f;
            f.heatCapacity = 0.75f;   f.heatCool = 1.40f; f.heatVent = 1.35f; f.qteWindow = 1.45f;
            f.parryWindow = 1.15f;    f.regen = 1.20f;
            // A skin of poise: shrugs off a small rock or chip fire, nothing heavier.
            f.poise = 30.f; f.poiseRegen = 30.f; f.poiseDelay = 1.0f;
            f.knockback = 1.10f; f.tumble = 1.f;
            break;
        case HullClass::Medium:
            break;   // all defaults
        case HullClass::Heavy:
            f.dashDistancePx = 200.f; f.dashDuration = 0.27f; f.dashIframes = 0.12f;
            f.dashRecovery = 0.70f;   f.dashEnergyCost = 20.f;
            f.dashCarry = 0.38f;      f.dashDrift = 0.45f;
            f.heatCapacity = 1.35f;   f.heatCool = 0.72f; f.heatVent = 0.80f; f.qteWindow = 0.65f;
            f.parryWindow = 0.85f;    f.regen = 0.85f;
            // Holds control through a full 3-ram Berserker chain (110 each) -- the
            // chain still costs ~285 HP, so tanking it is a choice, not a freebie.
            // A bash on top of that chain breaks it.
            // Regen is deliberately slow: 4s of quiet, then ~15s to refill. A
            // gunship cannot sit in a fight and out-heal its own armour.
            f.poise = 340.f; f.poiseRegen = 22.f; f.poiseDelay = 4.0f;
            f.knockback = 0.50f; f.tumble = 0.75f; f.hyperarmor = 1.f;
            f.damageReduction = 0.20f; f.hyperarmorReduction = 0.50f;
            f.shoulderBash = 1.f; f.ramming = 1.f;
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
        f.dashCarry = std::clamp((*t)["dash_carry"].get_or(f.dashCarry), 0.f, 1.f);
        f.dashDrift = std::max(0.f, (*t)["dash_drift"].get_or(f.dashDrift));
        f.heatCapacity = (*t)["heat_capacity"].get_or(f.heatCapacity);
        f.heatCool = (*t)["heat_cool"].get_or(f.heatCool);
        f.heatVent = (*t)["heat_vent"].get_or(f.heatVent);
        f.qteWindow = (*t)["qte_window"].get_or(f.qteWindow);
        f.parryWindow = (*t)["parry_window"].get_or(f.parryWindow);
        f.regen = (*t)["regen"].get_or(f.regen);
        f.poise = std::max(0.f, (*t)["poise"].get_or(f.poise));
        f.poiseRegen = (*t)["poise_regen"].get_or(f.poiseRegen);
        f.poiseDelay = (*t)["poise_delay"].get_or(f.poiseDelay);
        f.knockback = std::max(0.f, (*t)["knockback"].get_or(f.knockback));
        f.tumble = std::max(0.1f, (*t)["tumble"].get_or(f.tumble));
        f.hyperarmor = (*t)["hyperarmor"].get_or(f.hyperarmor);
        f.damageReduction = std::clamp((*t)["damage_reduction"].get_or(f.damageReduction), 0.f, 0.9f);
        f.hyperarmorReduction = std::clamp((*t)["hyperarmor_reduction"].get_or(f.hyperarmorReduction), 0.f, 0.9f);
        f.shoulderBash = (*t)["shoulder_bash"].get_or(f.shoulderBash);
        f.shoulderDamage = (*t)["shoulder_damage"].get_or(f.shoulderDamage);
        f.shoulderKnock = (*t)["shoulder_knock"].get_or(f.shoulderKnock);
        f.shoulderStun = (*t)["shoulder_stun"].get_or(f.shoulderStun);
        f.shoulderCounter = (*t)["shoulder_counter"].get_or(f.shoulderCounter);
        f.ramming = (*t)["ramming"].get_or(f.ramming);
        f.ramMinSpeed = (*t)["ram_min_speed"].get_or(f.ramMinSpeed);
        f.ramDamagePerSpeed = (*t)["ram_damage_per_speed"].get_or(f.ramDamagePerSpeed);
        f.ramBaseDamage = (*t)["ram_base_damage"].get_or(f.ramBaseDamage);
        f.ramMediumTaken = std::clamp((*t)["ram_medium_taken"].get_or(f.ramMediumTaken), 0.f, 1.f);
        return f;
    }

    /// The feel for a player: its kit's class, or MEDIUM for a legacy ship.
    inline ClassFeel classFeelFor(sol::state& lua, const KitProfile& kit) {
        return classFeel(lua, kit.valid ? kit.hullClass : HullClass::Medium);
    }

} // namespace ship