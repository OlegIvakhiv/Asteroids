/**
 * @file AISystem.hpp
 * @brief Enemy artificial intelligence: perception, behaviour, and manoeuvring
 *
 * ============================================================================
 * VERSION 2.0 — WHAT CHANGED AND WHY
 * ============================================================================
 *
 * 1. VISION CONES replace the 360-degree radius.
 *    A pirate now has to be FACING you. Three senses feed the same signal:
 *      - CONE     : normal sight, long range, limited FOV
 *      - PROXIMITY: short omnidirectional "I can hear your engines"
 *      - DAMAGE   : being shot alerts you instantly, regardless of facing
 *    The last two matter more than they look. A cone-only enemy can be stood
 *    directly behind and shot forever without ever reacting, which reads as
 *    broken rather than as stealth.
 *
 * 2. SUSPICION GATES PATROL -> ALERT.
 *    Sight no longer flips straight to COMBAT. Suspicion accumulates while you
 *    are visible (faster when close), so a glimpse at the edge of range is not
 *    the same event as walking into someone's face.
 *
 * 3. DISCRETE MANOEUVRES replace the continuous chase/orbit loop.
 *    The old code recomputed "chase if far, orbit if near" at 10 Hz. That is a
 *    control loop, and control loops CONVERGE — every enemy settled onto the
 *    same clean circle at the same radius. That convergence is exactly what
 *    reads as the player having a gravity field. Now each enemy commits to one
 *    manoeuvre for 0.6-1.8s, so movement has visible intent and direction
 *    changes.
 *
 * 4. PERSONALITY per enemy: preferred range, aggression, strafe handedness,
 *    noise phase. Identical AI running in parallel is what makes a group look
 *    like one organism.
 *
 * 5. SHOT TELEGRAPHS. Enemies wind up visibly before firing, with aim LOCKED
 *    at wind-up start. Locking the aim is what makes the telegraph honest —
 *    if it re-aimed at the moment of firing, the wind-up would be decoration
 *    and dodging it would do nothing.
 *
 * 6. STAGGER on hard impact, mirroring the player's tumble/recovery.
 *
 * 7. BULLET STORM: spin-and-spray panic move under asteroid pressure.
 *
 * 8. RAM CHARGE: windup -> charge -> recover. The charge is unstoppable;
 *    the windup is the player's only window to avoid it.
 *
 * 9. BROADSIDE FACING: naval units keep their broadside to the player and
 *    let their turrets track, rather than turning nose‑on.
 *
 * ============================================================================
 * VERSION 2.1 — BERSERKER SUPPORT
 * ============================================================================
 *
 * 10. BASH: windup -> lunge -> recover. The roster's one PARRIABLE attack.
 *     The strike resolves on proximity at the lunge, not on a Box2D contact
 *     begin -- a Berserker already grinding against your hull would never
 *     generate a fresh begin-touch, so a contact-driven bash would silently
 *     stop working exactly when it is most in your face. This system raises
 *     EnemyComponent::bashStrikePending; DamageSystem decides parry vs. hit.
 *
 * 11. RAM CHAINS: ram_chain_min/max queue extra charges, each with its own
 *     short re-aim windup. A charge that CONNECTS ends the chain (DamageSystem
 *     zeroes ramChainLeft) -- chaining into a tumbling player is a stunlock,
 *     not a pattern. Defaults are 1/1, so the Barge is unchanged.
 *
 * 12. MANOEUVRE PROFILES: `maneuver_profile = "melee"` never strafes, never
 *     falls back, and never flinches away from a hit. It closes or it lunges.
 *
 * 13. WOLF CIRCLE (Maneuver::CIRCLE). A melee unit runs straight in until it
 *     reaches melee_circle_range, then switches to a hard tangential orbit
 *     with a steady inward bite -- it keeps closing, but on a spiral instead
 *     of a line. The orbit direction is chosen to CUT THE PLAYER OFF (it
 *     matches the player's lateral drift), so it reads as picking a side
 *     rather than as a coin flip.
 *
 * 14. ATTACK EXCLUSIVITY. Two fixes for melee units firing and swinging at
 *     once, which asked the player to dodge a bullet and parry a lunge in the
 *     same beat:
 *       - `hold_fire_range`: inside it the gun is dead. One threat at a time.
 *       - `melee_shot_clear`: no melee commit until this long after the last
 *         round was fired, so rounds already in flight have resolved.
 *
 * 15. BURST FIRE (`burst_count` / `burst_pause`): a series, then a real pause
 *     with a visible sway. Both default to off.
 *
 * ============================================================================
 * VERSION 2.2 -- MANIAC SUPPORT
 * ============================================================================
 *
 * 16. FRENZY (FrenzyState). A low-HP, one-way override: Ignite (colour shift
 *     + laugh, the "rules just changed" beat) -> Charge (ranged kit dropped,
 *     straight at the player) -> resolved by DamageSystem on contact, into
 *     Thrown if the player parried. It is checked BEFORE stun, telegraphs and
 *     every other attack: nothing interrupts a Maniac who has decided.
 *
 * 17. SKID ROCKETS. AISystem only decides WHEN to fire and eats the recovery;
 *     the flight behaviour lives in WeaponSystem and the blast in DamageSystem.
 *
 * 18. MICRO-RECOVERY (`micro_recover`). A short window after a volley in which
 *     no attack may start -- he still moves, so it reads as reloading rather
 *     than as a stun. It is the punish window the Maniac would otherwise lack,
 *     since unlike the Berserker he never commits to a long attack.
 *
 * 19. `maneuver_profile = "erratic"`: short timers, frequent direction flips,
 *     no settled strafe band. Twitchy on purpose -- he is hard to lead, and
 *     that is his defence instead of armour.
 *
 * ============================================================================
 * VERSION 2.3 -- AMBUSH
 * ============================================================================
 *
 * 20. DORMANT UNITS (EnemyComponent::dormant). Spawned TURNED OFF by the
 *     director (`ambush_chance`) or by summon(). No perception, no cone, no
 *     steering, no timers: the unit is an object drifting with the wrecks.
 *     It wakes on the player within `ambush_wake_range` of its hull edge --
 *     360 degrees, no facing -- or on any player damage (DamageSystem and
 *     WeaponSystem call EnemyComponent::provoke() at every player-sourced
 *     site). Waking skips PATROL and ALERT entirely: it goes straight to
 *     COMBAT, knowing exactly where you are.
 *
 * 21. REBOOT (`ambush_wake_time`). The first beat after waking: drag back
 *     on, tumble killed, hull swinging onto the player, no attack of any
 *     kind. Same principle as the shot telegraph -- an ambush the player can
 *     react to is a scare; one they cannot is just damage.
 *
 * ============================================================================
 * VERSION 2.4 -- BLOODSEEKER (DUELIST) SUPPORT
 * ============================================================================
 *
 * 22. TWO MODES (`duel_enabled`). MELEE runs off the archetype table -- the
 *     Berserker kit. RANGE runs off its `duel_range` sub-table: same unit,
 *     different numbers, picked once per frame at the top of the loop. Every
 *     line below that reads `config` therefore reads the CURRENT mode without
 *     knowing modes exist.
 *
 * 23. THE SWITCH ROLL (playtest pass; replaces mode timers, the HP-pressure
 *     trigger, the parry trigger and the far-distance dive). Every
 *     `duel_roll_interval` seconds of free time he rolls
 *     `duel_switch_chance`; each miss adds `duel_switch_step`, a switch
 *     resets it. Nothing switches mid-bash, mid-ram or mid-attack.
 *
 * 24. DISENGAGE (MELEE -> RANGE). Owns the ship until he reaches range or
 *     `disengage_max_time` runs out. Backpedals nose-on with a weave, gun
 *     silent, retro plumes at the prow. Sidesteps any plasma round that would
 *     hit, leaving a hull-shaped afterimage; DamageSystem lets the rest pass
 *     through. That second half is the guarantee, this half is what the
 *     player SEES. Parry, ramming, kinetic rocks and AoE all still land.
 *
 * 25. BLOOD DIVE (RANGE -> MELEE). Windup (brake, swing on, prow flare) ->
 *     Dive (steered rush, no contact damage, fully hittable) -> on reaching
 *     bash range it hands off to updateBash THE SAME FRAME, which opens the
 *     normal cyan windup at full length. A transition never shortens the
 *     parry read. If the player is already in bash range when RANGE ends,
 *     there is no rush: the bash simply starts. Overshooting or running out
 *     of rush ends in DiveRecover, the punish window.
 *
 * 26. `maneuver_profile = "kite"`: holds a band and never closes inside it.
 *     Inside 75% of the band it backs off; outside 130% it approaches;
 *     between, it strafes and repositions. No attack runs.
 *
 * ============================================================================
 * VERSION 2.5 -- BLOODSEEKER RANGE KIT
 * ============================================================================
 *
 * 27. LANCER (`lancer_*`). Charge -> Lock -> fire -> Recover. During Charge
 *     the aim solves the true intercept with the player's velocity every
 *     frame (quadratic, not a fudge factor), so a player holding a straight
 *     line gets hit. Lock freezes it for `lancer_lock` -- the honest window:
 *     the lane snaps bone-white, and any change of course from here makes it
 *     miss. A player standing still has zero lead, so stillness is no answer.
 *
 * 28. SUPPRESSION CONE (`cone_*`). Windup (brake, swing on, wedge fills) ->
 *     Fire (rooted: zero velocity, heading locked, rounds at random inside
 *     the wedge) -> Recover (still rooted). The cone's direction locks at the
 *     end of the windup; a cone that kept tracking would erase the punish.
 *     The wedge is drawn to exactly the rounds' reach.
 *
 *     Both run only in RANGE and only from a free state; both are dispatched
 *     ahead of the mode timer, so a dive never cuts one off. A stun (here) or
 *     a stagger (DamageSystem) cancels either outright.
 *
 * ============================================================================
 * VERSION 2.9 -- ROSTER PASS (Barge, Maniac, death-chaos melee)
 * ============================================================================
 *
 * 36. FERAL MELEE. A feral unit fighting another ship keeps its bash and ram.
 *     The bash strike carries bashTargetId (DamageSystem resolves it on that
 *     hull, x death_chaos_damage); a feral ram hurts any hull it meets.
 *     death_chaos_time is 10s.
 *
 * 37. BARGE. The turret's eye (TurretSystem: turretSees) is ORed into the
 *     hull's sight -- one brain, two eyes, either one triggers both -- and
 *     the turret searches toward ai.lastKnownPlayerPos on alert (turretLook).
 *     SUMMON (`summon_*`): COMBAT, on a cooldown, under summon_max_alive:
 *     the hull charges (slows, swells across the beam, hatches vent), then
 *     spits summon_count Wardogs out of both sponsons, already in COMBAT,
 *     stamped summonedBy. A stagger or stun mid-charge breaks it. ESCORT:
 *     a unit with escort_radius is a leader (never a follower); escort_takes
 *     filters who follows; escort_leash keeps followers close in COMBAT too.
 *
 * 38. MANIAC. The passive trail is off (`mine_trail_enabled`). The carpet
 *     rolls a shape per run -- straight, ARC toward the player's side, or S
 *     (pickMineRunShape). MINE TOSS (`toss_*`): close in, windup, one spin
 *     throwing toss_count armed, lit mines into a ring, dizzy recover. Toss
 *     mines skip mine_max_active. Mines decay (WeaponSystem / DamageSystem).
 *
 * 39. CHARGED BURST (`burst_windup`, rebuilt in 3.0). updateChargedBurst():
 *     charge (the telegraph: tracking, sparks, ship still flies) -> a
 *     COMMITTED volley of burst_count rounds burst_interval apart -> pause.
 *     Hold-fire and range gate only the start. Anything that clears the
 *     telegraph cancels it cleanly; bash, ram, a duel mode flip and an
 *     execution wait for it (burstBusy).
 *
 * ============================================================================
 * VERSION 3.2 -- SQUAD AI (playtest rework of 3.1)
 * ============================================================================
 *
 * 42. SQUAD LAYER. updateSquad() once a frame, beginSquadUnit() per unit,
 *     squadGrant() at every attack start, squadSteer() on the movement
 *     intent. Changes WHEN a heavy may attack, where units stand, and keeps
 *     the pack on the player -- never the attack itself. `squad_ai` in
 *     Enemy.lua; `squad_role` per archetype. Feral units and units fighting
 *     another ship are outside it. Full description above updateSquad().
 *
 * 43. ATTACK TURNS replace 3.1's threat budget (which left three Berserkers
 *     hovering, open, then attacking one by one). Only `assault` units
 *     (Berserker; Maniac in his assault mood) are gated: 1-2 hold a turn at
 *     once (cap re-rolled per hand-off), a turn is never cut mid-attack, the
 *     next pick is weighted by time waited and nearness. A lone heavy always
 *     has the turn. Everyone else (Wardog, Raider, Barge, Bloodseeker) is
 *     never gated.
 *
 * 44. STALKING. A heavy without a turn circles at stalk_ring on its flank
 *     bearing -- no closing in, never parked.
 *
 * 45. FLANKING. Non-elites fan round the player from the pack's mean
 *     bearing, in standing order.
 *
 * 46. BREATHER. The player tumbling: no new turn, no new heavy attack, for
 *     breather_time.
 *
 * 47. MANIAC MOODS, PACK SIGHT, CHASE. The Maniac re-rolls assault /
 *     support / berserk every few seconds (role and preferred range).
 *     Any packmate seeing the player keeps every COMBAT member's memory
 *     fresh within share_sight_radius and pulls ALERT units within
 *     call_radius into COMBAT. A COMBAT unit past chase_distance (or 1.6x
 *     its preferred range) steers for the player's lead at up to chase_mult
 *     speed and thrust. combat_lose_* defaults raised in Enemy.lua.
 *
 * ============================================================================
 * VERSION 3.0 -- PLAYTEST: BURST, EXECUTION, LANCER
 * ============================================================================
 *
 * 40. EXECUTION IS COMMITTED. Only the victim dying or him being broken out
 *     (stagger / stun / parry) ends it. A target that drifts out of reach
 *     during the strike is chased again; exec_max_time is a runaway guard.
 *
 * 41. LANCER = BEAM. Charge pours from the side spikes into the maw
 *     (lancer_spike / lancer_maw, lancerPoints()); the fire raises a
 *     BeamShot that DamageSystem resolves as hitscan next frame (pierce,
 *     mirror parry). Aim leads only by the player's drift to the fire moment.
 *
 * ============================================================================
 * VERSION 2.8 -- BLOODSEEKER PACK MECHANICS
 * ============================================================================
 *
 * 32. AURA. Units with `aura_radius` (the Bloodseeker) give every Rakshari
 *     inside it `aura_bonus` -- EnemyComponent::packMult -- on move speed and
 *     attack COOLDOWNS (fire, bash, ram, rocket, mine run, turret). Windups,
 *     telegraphs and recoveries are never scaled: buffed units attack more
 *     often, never less readably. `pack_immune` (Maniac) and the sources
 *     themselves are excluded. Feral beats execution beats aura; no stacking.
 *
 * 33. EXECUTION (`exec_*`). Every exec_check while free: find the lowest
 *     `execute_rank`, lowest-HP ally under exec_hp_fraction in exec_range
 *     (Wardogs, Maniacs, Barges carry no rank). ExecApproach (steered rush,
 *     back to the player) -> ExecStrike (gold windup) -> kill: HP to 0 and
 *     `executed` (DamageSystem drops no scrap), then buffPack(): +exec_bonus
 *     for exec_time and every recovery and cooldown ends. Breakable like a
 *     dive; gives up if the target dies, escapes or the approach times out.
 *
 * 34. DEATH-CHAOS TARGETING. DamageSystem sets feralTimer on the pack when a
 *     Bloodseeker dies. A feral unit picks the nearest ship -- the player or
 *     another enemy -- every death_chaos_retarget s and the whole loop runs
 *     with that ship in the player's place. Against a ship its bash and ram
 *     are off (both resolve on the player) and hold-fire is ignored; its
 *     rounds carry feralMult and hurt enemy hulls. Perception is skipped.
 *
 * 35. ESCORT. In PATROL, a unit inside a Bloodseeker's escort_radius holds a
 *     slot on a slowly turning ring around him instead of wandering.
 *
 * ============================================================================
 * VERSION 2.7 -- BLOODSEEKER PLAYTEST PASS
 * ============================================================================
 *
 * 31. POST-STUN EVADE WINDOW. A stun from a parry or a parried round ends in
 *     `post_stun_evade_time` of perfect dodging (duelEvadeTimer), in either
 *     mode: the same DamageSystem pass-through as the disengage, plus
 *     evadeKick() sidesteps whenever he is free. The disengage itself now
 *     holds for `disengage_min_time` even if he starts at range, sliding
 *     sideways instead of running off.
 *
 * ============================================================================
 * VERSION 2.6 -- BLOODSEEKER MELEE TRICKS
 * ============================================================================
 *
 * 29. FEINT (`feint_*`). Rolled when a bash starts. At `feint_break` of the
 *     windup the bash state drops to None (the cyan crescent vanishes on the
 *     frame), then FeintBreak: a dead stop and a retro flare for
 *     `feint_hesitate` -- the half-beat tell -- then FeintFallback: backpedal
 *     firing `feint_spray_count` rounds. A bash promised by a dive or a ram
 *     cancel (`bashHonest`) never feints.
 *
 * 30. RAM CANCEL (`ram_cancel_*`). During a charge link: planned (rolled per
 *     link, fires at `ram_cancel_point`) or reactive (rolled once per link,
 *     the moment the player dodges). The charge ends on the frame -- no more
 *     invulnerability, lane and white-hot hull gone -- and he goes into a
 *     short DiveWindup (`ram_cancel_brake`) and the Blood Dive rush, which
 *     hands off to an honest bash. `duelTrick` keeps both tricks from
 *     resetting the mode timer.
 *
 * State split: this system owns AIState (decisions). Presentation and impact
 * state live on EnemyComponent so DamageSystem and RenderSystem can reach them.
 *
 * @author Oleg Ivakhiv
 * @version 3.2
 */

#pragma once

#include "ISystem.hpp"
#include "utils/components.hpp"
#include "utils/Afterimage.hpp"
#include "core/EnemyArchetypes.hpp"
#include <unordered_map>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

class AISystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;
        m_dev = ctx.dev;

        // Entity ids restart at 1 on every run (EntityManager::reset). A brain
        // cached for id 12 last run would otherwise be inherited by whatever
        // spawns as id 12 this run -- personality, suspicion and all.
        // pruneCache() only sweeps above 64 entries, so it never caught this.
        m_aiCache.clear();
    }

    /// Read-only brain lookup for the dev overlay. nullptr if this entity has
    /// not been through update() yet.
    const AIState* brainOf(uint32_t entityId) const {
        auto it = m_aiCache.find(entityId);
        return (it == m_aiCache.end()) ? nullptr : &it->second;
    }

    /// Dev overlay: this unit's place in the squad ("" = not in it).
    std::string squadTag(uint32_t entityId) const {
        if (m_sqSlots.find(entityId) == m_sqSlots.end() && !m_sqTurns.count(entityId)) return "";
        const char* mood = "";
        auto mi = m_sqMood.find(entityId);
        if (mi != m_sqMood.end())
            mood = mi->second.mood == ManiacMood::Assault ? " [ASSAULT]"
                 : (mi->second.mood == ManiacMood::Support ? " [SUPPORT]" : " [BERSERK]");
        auto t = m_sqTurns.find(entityId);
        char b[96];
        if (t != m_sqTurns.end())
            std::snprintf(b, sizeof(b), "SQUAD TURN %.1fs%s%s", std::max(0.f, t->second.left), mood,
                m_sqBreather > 0.f ? "  BREATHER" : "");
        else if (m_sqWaited.count(entityId) && (mi == m_sqMood.end() || mi->second.mood == ManiacMood::Assault))
            std::snprintf(b, sizeof(b), "SQUAD STALKING (cap %d)%s", m_sqCap, mood);
        else
            std::snprintf(b, sizeof(b), "SQUAD FREE%s", mood);
        return b;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;
        if (!m_registry || m_registry->empty()) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        // The player. Most units fight this; a FERAL one (death-chaos) swaps
        // in whatever ship it picked, per unit, at the top of the loop.
        const sf::Vector2f truePlayerPos = m_em->transforms[playerIdx].position;
        const b2Vec2 pvb = b2Body_GetLinearVelocity(m_em->physics[playerIdx].bodyId);
        const sf::Vector2f truePlayerVel(pvb.x * SCALE, pvb.y * SCALE);
        m_playerVel = truePlayerVel;   // pickManeuver needs it to choose a side
        m_clock += dt;

        // Did the player just spend their dodge? A refit ship runs a timed
        // burst (dashTimer); a legacy ship only restarts its cooldown. The
        // Bloodseeker's ram cancel reacts to exactly this (note 30).
        {
            const auto& ps = m_em->players[playerIdx];
            m_playerDashing = ps.dashTimer > 0.f
                || (ps.dashMaxCooldown > 0.f && ps.dashCooldown > ps.dashMaxCooldown - 0.2f);
        }
        const bool truePlayerDashing = m_playerDashing;

        updateHomingAsteroids(dt);

        // ---- Pack: who is projecting an aura this frame (note 32) ----
        collectPackSources();

        // ---- Squad: budget, engagement, flank slots (notes 42-46) ----
        updateSquad(dt, playerIdx, truePlayerPos);

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;

            auto& tf = m_em->transforms[i];
            auto& health = m_em->healths[i];
            auto& ec = m_em->enemies[i];
            const uint32_t entityId = tf.entityId;
            auto& ai = m_aiCache[entityId];
            const b2BodyId bodyId = m_em->physics[i].bodyId;

            // ================================================================
            // ARCHETYPE CONFIG – per‑entity resolution
            // ================================================================
            const enemyarch::ArchetypeDef& adef = m_registry->resolve(ec.archetype);
            sol::table config = adef.config;

            // ---- Duelist: RANGE mode fights from its own table (note 22) ----
            const bool duel = config["duel_enabled"].get_or(false);
            if (duel) config = duelTable(adef.config, ec.duelMode);

            // ---- Personality rolled AFTER config is loaded ----
            if (!ai.initialised) rollPersonality(ai, entityId, config);

            // ================================================================
            // TARGET -- the player, unless death-chaos made this one feral
            // ================================================================
            // Everything below says "player" and means "target". A feral
            // unit fighting another ship uses its whole kit on it: the bash
            // strike carries bashTargetId and DamageSystem resolves a feral
            // ram on any hull it meets (note 36). Hold-fire is ignored.
            sf::Vector2f playerPos = truePlayerPos;
            sf::Vector2f playerVel = truePlayerVel;
            m_vsShip = false;
            m_curRadius = adef.radius;
            const bool feral = ec.feralTimer > 0.f && ec.powered();
            if (feral && feralTarget(dt, i, ec, tf.position, truePlayerPos, truePlayerVel,
                playerPos, playerVel)) m_vsShip = true;
            m_playerVel = playerVel;
            m_playerDashing = m_vsShip ? false : truePlayerDashing;

            // ---- Pack: aura / execution buff / frenzy (notes 32-34) ----
            updatePack(ec, adef.config, tf.position, entityId, feral);
            beginSquadUnit(entityId, adef.config, feral);
            // Fire support stands off: a Raider rolled to live at 220px is
            // not covering anyone (note 46).
            if (m_sqOn) {
                const float mr = adef.config["squad_min_range"].get_or(0.f);
                if (mr > 0.f) ai.preferredRange = std::max(ai.preferredRange, mr);
                // The Maniac's mood sets where he wants to be (note 47).
                if (m_sqBaseRole == SquadRole::Wild) {
                    const float r = maniacMoodRange(entityId);
                    if (r > 0.f) ai.preferredRange = r;
                }
            }

            const float enginePower = config["engine_power"].get_or(200.0f) * ec.packMult;
            const float maxSpeed = config["max_speed"].get_or(20.0f) * ec.packMult;
            m_dodgeDuration = config["dodge_duration"].get_or(0.42f);
            m_dodgeCooldown = config["dodge_cooldown"].get_or(1.1f);
            m_dodgeManoeuvreTime = config["dodge_manoeuvre_time"].get_or(0.25f);
            m_dodgeSpeed = config["dodge_speed"].get_or(620.f);

            tf.visualOffsetAngle = 0.f;
            tf.visualPivot = { 0.f, 0.f };
            tf.visualScale = { 1.f, 1.f };

            // ================================================================
            // DEV FREEZE -- no thinking, no timers, bleed off momentum
            // ================================================================
            if (m_dev && m_dev->isAIFrozen(entityId)) {
                holdFrozen(dt, ec, bodyId);
                continue;
            }

            // ================================================================
            // AMBUSH -- dormant, then the reboot beat
            // ================================================================
            if (ec.dormant && !updateDormant(tf, ec, health, ai, bodyId, config, adef,
                playerPos, playerVel)) continue;

            tickTimers(dt, ec);

            if (ec.wakeTimer > 0.f) {
                updateReboot(dt, tf, ec, bodyId, playerPos, config);
                continue;
            }

            // ================================================================
            // STAGGER — owns rotation completely, blocks everything
            // ================================================================
            if (updateStagger(dt, i, tf, ec)) continue;

            // ---- Hoist these for ram and everything else ----
            const sf::Vector2f enemyPos = tf.position;
            sf::Vector2f toPlayer = playerPos - enemyPos;
            const float distToPlayer = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);
            const sf::Vector2f toPlayerN = (distToPlayer > 0.01f)
                ? sf::Vector2f(toPlayer.x / distToPlayer, toPlayer.y / distToPlayer)
                : sf::Vector2f(0.f, -1.f);
            m_curDistToPlayer = distToPlayer;

            // ================================================================
            // RAM CHARGE — owns movement completely while active
            // ================================================================
            if (updateRam(dt, i, tf, ec, ai, bodyId, config,
                playerPos, distToPlayer, toPlayerN)) continue;

            // ---- Stun (after ram, because ram cannot be stunned) ----
            // ================================================================
            // FRENZY -- checked before everything, including stun
            // ================================================================
            // A Maniac who has ignited cannot be talked out of it. Letting a
            // stun or a telegraph interrupt this would turn the one moment the
            // player is supposed to read as irreversible into another
            // interruptible attack.
            if (updateFrenzy(dt, i, tf, ec, ai, bodyId, config, toPlayerN, distToPlayer))
                continue;

            if (health.stunTimer > 0.f) {
                health.stunTimer -= dt;
                // ---- Post-stun evade window (note 31) ----
                // A stun from a parry or a parried round (DamageSystem sets
                // duelStunEvade) ends in post_stun_evade_time of perfect
                // dodging, armed on the frame the stun runs out.
                if (health.stunTimer <= 0.f && ec.duelStunEvade) {
                    ec.duelStunEvade = false;
                    ec.duelEvadeTimer = adef.config["post_stun_evade_time"].get_or(0.f);
                }
                ec.telegraphActive = false;
                ec.telegraphTimer = 0.f;
                ec.bashState = BashState::None;     // Stun breaks any melee commit
                ec.bashStrikePending = false;
                if (ec.duelBreakable()) endDuelShift(ec, ai, adef.config);   // ...and a dive or feint
                if (ec.duelAttack != DuelAttack::None) cancelRangeAttack(ec);   // ...and a lancer / cone
                breakSummonAndToss(ec, adef.config);                              // ...and a summon / toss
                const float w = 0.06f * std::sin(health.stunTimer * 40.f);
                tf.visualScale.x *= 1.f + w;
                tf.visualScale.y *= 1.f - w;
                continue;
            }

            // ================================================================
            // DUELIST — mode timers, Disengage, Blood Dive (notes 22-25)
            // ================================================================
            // After stun on purpose: a stunned Bloodseeker is a punished one,
            // and that must hold mid-disengage too.
            if (duel && updateDuel(dt, tf, ec, health, ai, bodyId, adef,
                toPlayerN, distToPlayer)) continue;

            // ================================================================
            // BULLET STORM — runs instead of normal behaviour
            // ================================================================
            if (ec.stormActive || ec.stormRecoverTimer > 0.f) {
                updateBulletStorm(dt, i, tf, ec, ai, bodyId, config);
                continue;
            }

            // ================================================================
            // PERCEPTION
            // ================================================================
            if (feral) {
                // Feral: no searching, no suspicion. It is already fighting.
                ai.currentState = EnemyState::COMBAT;
                ai.hasSeenPlayer = true;
                ai.timeSinceSeen = 0.f;
                ai.lastKnownPlayerPos = playerPos;
                ai.lastKnownPlayerVel = playerVel;
                ec.visualState = EnemyState::COMBAT;
            }
            else {
                // One brain, two eyes: the turret's cone (TurretSystem,
                // last frame) counts as the hull seeing you (note 37).
                const bool sees = canSee(tf, enemyPos, toPlayerN, distToPlayer, health, ai, config)
                    || ec.turretSees;
                updatePerception(dt, sees, playerPos, playerVel, distToPlayer, ai, ec, tf, config);

                // ---- Pack sight (note 47) ----
                // A packmate with eyes on you keeps everyone's memory fresh,
                // and pulls the searching ones nearby into the fight.
                if (m_sqCfg.enabled && m_sqBaseRole != SquadRole::None && !m_sqSeers.empty()) {
                    if (ai.currentState == EnemyState::COMBAT &&
                        packSeesNear(enemyPos, m_sqCfg.shareSightRadius)) {
                        ai.timeSinceSeen = 0.f;
                        ai.lastKnownPlayerPos = playerPos;
                        ai.lastKnownPlayerVel = playerVel;
                    }
                    else if (ai.currentState == EnemyState::ALERT &&
                        packSeesNear(enemyPos, m_sqCfg.callRadius)) {
                        ai.suspicion = 1.f;
                        ai.hasSeenPlayer = true;
                        ai.timeSinceSeen = 0.f;
                        ai.lastKnownPlayerPos = playerPos;
                        ai.lastKnownPlayerVel = playerVel;
                        enterState(ai, ec, EnemyState::COMBAT, AlertIcon::Spotted, config);
                        ec.visualState = ai.currentState;
                    }
                }
            }
            ec.turretLook = ai.lastKnownPlayerPos;   // where an alert turret searches
            ec.turretHasLook = ai.hasSeenPlayer;

            // ================================================================
            // BULLET STORM CONSIDERATION (throttled) – only if enabled
            // ================================================================
            if (config["storm_enabled"].get_or(true)) {
                ai.stormScanTimer -= dt;
                if (ai.stormScanTimer <= 0.f) {
                    const float elapsed = 0.25f + (rand() % 10) / 100.f;
                    considerBulletStorm(elapsed, i, enemyPos, ai, ec, config);
                    ai.stormScanTimer = elapsed;
                }
            }

            if (ec.stormActive) {
                updateBulletStorm(dt, i, tf, ec, ai, bodyId, config);
                continue;
            }

            // ================================================================
            // SUMMON (Barge) -- the hull charges and spits Wardogs (note 37)
            // ================================================================
            if (updateSummon(dt, tf, ec, ai, bodyId, config, adef, playerPos, distToPlayer))
                continue;

            // ================================================================
            // ROCKET VOLLEY — before the bash, so a Maniac at mid range
            // commits to the volley rather than drifting into a lunge
            // ================================================================
            updateRockets(dt, tf, ec, ai, entityId, config, toPlayerN, distToPlayer);
            updateMines(dt, tf, ec, ai, entityId, config);

            // ================================================================
            // MINE TOSS -- spin, ring of lit mines (note 38)
            // ================================================================
            if (updateMineToss(dt, tf, ec, ai, bodyId, config, adef, distToPlayer)) continue;

            // ================================================================
            // MINE RUN — owns the ship while it lays its field
            // ================================================================
            if (updateMineRun(dt, i, tf, ec, ai, bodyId, config,
                toPlayerN, distToPlayer)) continue;

            // ================================================================
            // BASH — owns movement, rotation and guns while active
            // ================================================================
            if (updateBash(dt, tf, ec, ai, bodyId, config, adef,
                distToPlayer, toPlayerN)) continue;

            // triggerDodgeBurst needs the body
            m_dodgeBodyId = bodyId;
            m_dodgePos = enemyPos;

            // ================================================================
            // MOVEMENT INTENT
            // ================================================================
            ai.reactionTimer -= dt;
            if (ai.reactionTimer <= 0.f) {
                ai.reactionTimer = 0.10f + (rand() % 10) / 100.f;
                ai.smoothedDesiredVel = decideVelocity(dt, ai, ec, enemyPos, playerPos,
                    toPlayerN, distToPlayer, maxSpeed, config);
            }

            sf::Vector2f avoidance = computeAvoidance(dt, i, enemyPos, entityId, ai, ec, maxSpeed, config);

            updateChaosDodge(dt, ai, ec, toPlayerN, distToPlayer, config);

            if (ai.dodgeBurstTimer > 0.f) {
                ai.dodgeBurstTimer -= dt;
                const float u = ai.dodgeBurstTimer / std::max(0.01f, ai.dodgeBurstDuration);
                avoidance += ai.dodgeBurstDir * (maxSpeed * 4.f * u);
            }

            sf::Vector2f finalDesiredVel = ai.smoothedDesiredVel + avoidance;

            // ---- Squad: flank slot, stalking (notes 44-45) ----
            if (ai.currentState == EnemyState::COMBAT)
                finalDesiredVel = squadSteer(enemyPos, playerPos, maxSpeed, finalDesiredVel);

            // ---- Chase (note 47): fallen behind in a fight -> burn to catch up ----
            float chaseK = 1.f;
            if (ai.currentState == EnemyState::COMBAT && !feral && m_sqCfg.enabled &&
                m_sqBaseRole != SquadRole::None) {
                const float start = std::max(m_sqCfg.chaseDistance, ai.preferredRange * 1.6f);
                if (distToPlayer > start) {
                    const float k = std::clamp((distToPlayer - start) / 400.f, 0.f, 1.f);
                    chaseK = 1.f + (m_sqCfg.chaseMult - 1.f) * k;
                    sf::Vector2f to = playerPos + playerVel * 0.5f - enemyPos;
                    const float tl = std::sqrt(to.x * to.x + to.y * to.y);
                    if (tl > 1.f) {
                        const sf::Vector2f want = (to / tl) * (maxSpeed * 9.f * chaseK);
                        finalDesiredVel = finalDesiredVel * (1.f - k) + want * k;
                    }
                }
            }

            // ---- Escort leash (note 37) ----
            // A leader with `escort_leash` (the Barge) keeps its escort close
            // in the fight too: past the leash they are pulled back toward
            // him, so Wardogs screen the Barge instead of streaming off.
            if (m_hasEscort && m_escortLeash > 0.f && ai.currentState == EnemyState::COMBAT) {
                const sf::Vector2f d = m_escortPos - enemyPos;
                const float dl = std::sqrt(d.x * d.x + d.y * d.y);
                if (dl > m_escortLeash) {
                    const float pull = std::clamp((dl - m_escortLeash) / 220.f, 0.f, 1.f) * 0.75f;
                    finalDesiredVel = finalDesiredVel * (1.f - pull) + (d / dl) * (maxSpeed * 7.f) * pull;
                }
            }

            // ================================================================
            // STEERING — mass‑compensated
            // ================================================================
            const b2Vec2 currentVel = b2Body_GetLinearVelocity(bodyId);
            b2Vec2 impulse = { finalDesiredVel.x / SCALE - currentVel.x,
                               finalDesiredVel.y / SCALE - currentVel.y };

            float gain = 50.f;
            float maxForce = enginePower * dt * chaseK;

            if (config["steer_mass_compensate"].get_or(false)) {
                const float refMass = config["steer_reference_mass"].get_or(7.5f);
                const float k = (refMass > 0.01f)
                    ? b2Body_GetMass(bodyId) / refMass : 1.f;
                gain *= k;
                maxForce *= k;
            }

            const float impulseLen = std::sqrt(impulse.x * impulse.x +
                impulse.y * impulse.y);
            if (impulseLen > maxForce) {
                const float s = maxForce / impulseLen;
                impulse.x *= s; impulse.y *= s;
            }
            b2Body_ApplyForceToCenter(bodyId, { impulse.x * gain,
                                                impulse.y * gain }, true);

            // ================================================================
            // SHOOTING
            // ================================================================
            updateShooting(dt, i, tf, ec, ai, entityId, playerPos, playerVel,
                distToPlayer, config);

            // Hold fire covers this too. A rock cleared out of the way at
            // point-blank still puts a bullet on screen next to a bash tell,
            // and the player cannot tell from the muzzle flash who it was
            // aimed at.
            {
                const float hf = config["hold_fire_range"].get_or(0.f);
                if (!(hf > 0.f && distToPlayer < hf))
                    opportunisticAsteroidShot(dt, i, tf, ec, entityId, bodyId, config);
            }

            // ================================================================
            // ROTATION + IDLE ANIMATION
            // ================================================================
            updateRotation(dt, tf, ec, ai, finalDesiredVel, toPlayer, bodyId, config);
        }

        pruneCache();
    }

private:
    // ========================================================================
    // DEV FREEZE
    // ========================================================================
    /**
     * @brief Park a unit whose AI the dev menu switched off.
     *
     * Every COMMITTED attack is cancelled, not paused. A ram frozen in Charge
     * is still invulnerable and still hurts on contact (DamageSystem reads
     * ramState, not the AI), so "frozen" would mean "stationary trap". Same
     * for a telegraph left lit -- it reads as a shot that never comes.
     *
     * Frenzy is left alone on purpose: a Thrown Maniac is DamageSystem's
     * physics bomb, not AI, and yanking its state here would defuse it.
     *
     * Velocity is damped rather than zeroed so knockback from your shots is
     * still visible on a frozen target -- that is half of what you freeze one
     * to look at.
     */
    void holdFrozen(float dt, EnemyComponent& ec, b2BodyId bodyId) {
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.turretTelegraphActive = false;
        ec.shotgunState = 0;                // a frozen wedge would sit there lit
        ec.summonState = 0;
        ec.tossState = 0;
        ec.ramState = RamState::None;
        ec.bashState = BashState::None;
        ec.bashStrikePending = false;
        ec.mineRunState = MineRunState::None;
        ec.stormActive = false;
        ec.stormRecoverTimer = 0.f;
        // A frozen dive would sail on at full speed; a frozen disengage would
        // leave him dodging everything you shoot at the thing you froze.
        ec.duelShift = DuelShift::None;
        ec.duelAttack = DuelAttack::None;   // a frozen cone would sit there lit
        ec.duelTrick = false;
        ec.bashFeint = false;
        ec.feintShotsLeft = 0;

        if (!b2Body_IsValid(bodyId)) return;
        const float k = std::exp(-4.f * dt);
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
        b2Body_SetAngularVelocity(bodyId, b2Body_GetAngularVelocity(bodyId) * k);
    }

    // ========================================================================
    // AMBUSH
    // ========================================================================
    /**
     * @brief One frame of a dormant unit. Returns true if it woke this frame.
     *
     * An object has no commitments, so anything the damage side put on it --
     * a rock's stagger, a stun, a flash -- is dropped rather than saved up
     * to play out the moment it wakes.
     */
    bool updateDormant(TransformComponent& tf, EnemyComponent& ec, HealthComponent& health,
        AIState& ai, b2BodyId bodyId, sol::table& config, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f playerPos, sf::Vector2f playerVel)
    {
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.turretTelegraphActive = false;
        ec.staggerTimer = 0.f;
        ec.staggerRecoverTimer = 0.f;
        ec.hitFlashTimer = 0.f;
        ec.alertIconTimer = 0.f;
        health.stunTimer = 0.f;
        ec.visualState = EnemyState::PATROL;

        // PhysicsSystem only syncs rotation for bullets and rocks; for an
        // enemy the AI owns tf.rotation and pushes it INTO the body. A
        // dormant hull is tumbling under physics instead, so the direction
        // flips: read it back, or the drawn hull freezes while the hitbox
        // spins underneath it.
        if (b2Body_IsValid(bodyId))
            tf.rotation = b2Rot_GetAngle(b2Body_GetRotation(bodyId)) * 180.f / 3.14159265f;

        // Contact zone: measured from the HULL EDGE, not the centre, so the
        // same number means the same gap for a Raider and for a Barge.
        const sf::Vector2f d = playerPos - tf.position;
        const float gap = std::sqrt(d.x * d.x + d.y * d.y) - adef.radius;
        const bool inZone = gap <= config["ambush_wake_range"].get_or(220.f);

        if (!inZone && !ec.provoked) return false;

        wakeUp(tf, ec, ai, bodyId, config, adef, playerPos, playerVel);
        return true;
    }

    void wakeUp(const TransformComponent& tf, EnemyComponent& ec, AIState& ai, b2BodyId bodyId,
        sol::table& config, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f playerPos, sf::Vector2f playerVel)
    {
        ec.dormant = false;
        ec.provoked = false;
        ec.wakeDuration = std::max(0.f, config["ambush_wake_time"].get_or(0.45f));
        ec.wakeTimer = ec.wakeDuration;

        // Back under power: the configured drag returns (createEnemy zeroed
        // it so the hull could drift like a wreck).
        if (b2Body_IsValid(bodyId)) {
            b2Body_SetLinearDamping(bodyId, config["lineardrag_factor"].get_or(1.0f));
            b2Body_SetAngularDamping(bodyId, config["angulardrag_factor"].get_or(2.0f));
        }

        // It was waiting FOR you. No suspicion ramp, no ALERT search: it
        // knows exactly where you are, and it is already fighting.
        ai.suspicion = 1.f;
        ai.hasSeenPlayer = true;
        ai.timeSinceSeen = 0.f;
        ai.lastKnownPlayerPos = playerPos;
        ai.lastKnownPlayerVel = playerVel;
        enterState(ai, ec, EnemyState::COMBAT, AlertIcon::Spotted, config);

        // enterState's first-contact move is a startled FALLBACK for ranged
        // units. An ambusher is not startled -- it holds its ground and
        // slides into a firing line. Melee keeps its ATTACK_RUN.
        if (!isMelee(config)) {
            ai.maneuver = Maneuver::STRAFE;
            ai.maneuverTimer = 0.6f + (rand() % 30) / 100.f;
        }
        ec.visualState = EnemyState::COMBAT;

        // The reveal: one hard ring in the faction colour and a small kick.
        // Flat, like every other ring in the game.
        m_em->spawnShockRing(tf.position, adef.radius * 0.6f, adef.radius * 2.6f,
            0.35f, adef.color, 3.f, 210.f);
        m_em->addTrauma(0.12f);
    }

    /// The reboot beat: no thrust, no weapons, tumble killed, hull swinging
    /// onto the player. Units that fight broadside (facing_mode = "velocity")
    /// do not turn nose-on here either -- their turret wakes instead.
    void updateReboot(float dt, TransformComponent& tf, EnemyComponent& ec, b2BodyId bodyId,
        sf::Vector2f playerPos, sol::table& config)
    {
        ec.wakeTimer = std::max(0.f, ec.wakeTimer - dt);
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        if (!b2Body_IsValid(bodyId)) return;

        b2Body_SetAngularVelocity(bodyId, 0.f);
        if (config["facing_mode"].get_or<std::string>("target") == "velocity") return;

        sf::Vector2f dir = playerPos - tf.position;
        const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (l < 0.01f) return;
        dir /= l;
        turnToward(tf, bodyId, dir, config["rotation_speed"].get_or(4.f) * 1.5f, dt);
    }

    // ========================================================================
    // PERSONALITY
    // ========================================================================
    void rollPersonality(AIState& ai, uint32_t entityId, const sol::table& config) {
        const uint32_t h = entityId * 2654435761u;
        auto frac = [&](int shift) {
            return static_cast<float>((h >> shift) & 0xFF) / 255.f;
            };

        // Fixed profile (swarm / naval)
        if (!config["personality_variance"].get_or(true)) {
            const bool naval =
                config["facing_mode"].get_or<std::string>("target") == "velocity";

            // Naval units stand off at a fraction of TURRET range, not hull
            // range -- the Barge's attack_range is 0 because its hull gun is
            // switched off, and 0 * anything is a ship trying to orbit at
            // point-blank.
            const float band = naval
                ? config["turret_range"].get_or(800.f)
                : config["attack_range"].get_or(380.f);

            ai.preferredRange = band * (naval ? 0.62f : 0.70f);
            ai.aggression = config["fixed_aggression"].get_or(0.8f);

            // Explicit override. A melee unit's gun range says nothing about
            // where it wants to BE -- the Berserker shoots from 400 but wants
            // to live at 80.
            const float pref = config["preferred_range"].get_or(0.f);
            if (pref > 0.f) ai.preferredRange = pref;
        }
        else {
            ai.preferredRange = 220.f + frac(0) * 220.f;
            ai.aggression = 0.25f + frac(8) * 0.65f;
        }
        ai.strafeDir = (frac(16) > 0.5f) ? 1.f : -1.f;
        ai.jitterPhase = frac(24) * 6.28318f;
        ai.maneuver = Maneuver::STRAFE;
        ai.maneuverTimer = 0.f;
        ai.initialised = true;
    }

    void tickTimers(float dt, EnemyComponent& ec) {
        if (ec.alertIconTimer > 0.f) ec.alertIconTimer = std::max(0.f, ec.alertIconTimer - dt);
        if (ec.hitFlashTimer > 0.f) ec.hitFlashTimer = std::max(0.f, ec.hitFlashTimer - dt);
        if (ec.dodgeFlashTimer > 0.f) ec.dodgeFlashTimer = std::max(0.f, ec.dodgeFlashTimer - dt);
        // Attack COOLDOWNS run at the pack multiplier (aura / execution /
        // frenzy). Windups, telegraphs and recoveries never do.
        const float adt = dt * ec.packMult;
        if (ec.bashCooldown > 0.f) ec.bashCooldown = std::max(0.f, ec.bashCooldown - adt);
        if (ec.shotPauseTimer > 0.f) ec.shotPauseTimer = std::max(0.f, ec.shotPauseTimer - dt);
        if (ec.shotClearTimer > 0.f) ec.shotClearTimer = std::max(0.f, ec.shotClearTimer - dt);
        if (ec.microRecover > 0.f) ec.microRecover = std::max(0.f, ec.microRecover - dt);
        if (ec.rocketCooldown > 0.f) ec.rocketCooldown = std::max(0.f, ec.rocketCooldown - adt);
        if (ec.mineRunCooldown > 0.f) ec.mineRunCooldown = std::max(0.f, ec.mineRunCooldown - adt);
        if (ec.execBuffTimer > 0.f) ec.execBuffTimer = std::max(0.f, ec.execBuffTimer - dt);
        if (ec.feralTimer > 0.f) {
            ec.feralTimer = std::max(0.f, ec.feralTimer - dt);
            if (ec.feralTimer <= 0.f) ec.feralTargetId = 0;   // calmed down: back on the player
        }
        if (ec.duelEvadeTimer > 0.f) ec.duelEvadeTimer = std::max(0.f, ec.duelEvadeTimer - dt);
        if (ec.duelEvadeKick > 0.f) ec.duelEvadeKick = std::max(0.f, ec.duelEvadeKick - dt);

        // Trail outlives the charge by design; this must keep running in every
        // state or the wake freezes on screen when the ram ends.
        if (ec.ramTrailFade > 0.f && ec.ramState != RamState::Charge) {
            ec.ramTrailFade = std::max(0.f, ec.ramTrailFade - dt * 2.2f);  // ~0.45s
            if (ec.ramTrailFade <= 0.f) ec.ramTrailCount = 0;
        }
    }

    // ========================================================================
    // PERCEPTION
    // ========================================================================
    bool canSee(const TransformComponent& tf, sf::Vector2f enemyPos,
        sf::Vector2f toPlayerN, float dist,
        const HealthComponent& health, AIState& ai, sol::table& config)
    {
        (void)enemyPos; (void)health;

        const float visionRange = config["vision_range"].get_or(620.f);
        const float fovDeg = config["vision_fov"].get_or(110.f);
        const float proximity = config["proximity_sense"].get_or(150.f);

        if (dist < proximity) return true;
        if (dist > visionRange) return false;

        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f forward(std::sin(r), -std::cos(r));
        const float d = forward.x * toPlayerN.x + forward.y * toPlayerN.y;

        float half = fovDeg * 0.5f;
        if (ai.currentState != EnemyState::PATROL) {
            half *= config["vision_fov_alert_mult"].get_or(1.45f);
        }
        half = std::min(half, 175.f);

        if (d < std::cos(half * 3.14159f / 180.f)) return false;
        return true;
    }

    void updatePerception(float dt, bool sees, sf::Vector2f playerPos, sf::Vector2f playerVel,
        float dist, AIState& ai, EnemyComponent& ec,
        const TransformComponent& tf, sol::table& config)
    {
        const float visionRange = config["vision_range"].get_or(620.f);
        const bool wasHit = (ec.hitFlashTimer > 0.f);

        if (ec.timesHit >= 2 && ai.currentState != EnemyState::COMBAT) {
            ai.suspicion = 1.f;
            ai.lastKnownPlayerPos = playerPos;
            ai.lastKnownPlayerVel = playerVel;
            ai.hasSeenPlayer = true;
            ai.timeSinceSeen = 0.f;
            enterState(ai, ec, EnemyState::COMBAT, AlertIcon::Spotted, config);
            ec.visualState = ai.currentState;
            return;
        }

        if (sees || wasHit) {
            ai.timeSinceSeen = 0.f;
            ai.lastKnownPlayerPos = playerPos;
            ai.lastKnownPlayerVel = playerVel;
            ai.hasSeenPlayer = true;

            const float closeness = std::clamp(1.f - dist / std::max(1.f, visionRange), 0.f, 1.f);
            const float rate = config["suspicion_rate"].get_or(1.1f) * (0.45f + closeness * 1.35f);
            ai.suspicion = std::min(1.f, ai.suspicion + rate * dt * (wasHit ? 4.f : 1.f));
        }
        else {
            ai.timeSinceSeen += dt;
            ai.suspicion = std::max(0.f, ai.suspicion - config["suspicion_decay"].get_or(0.35f) * dt);
        }

        const float toCombat = config["suspicion_combat"].get_or(0.62f);
        const float toAlert = config["suspicion_alert"].get_or(0.18f);

        switch (ai.currentState) {

        case EnemyState::PATROL:
            if (ai.suspicion >= toCombat) {
                enterState(ai, ec, EnemyState::COMBAT, AlertIcon::Spotted, config);
            }
            else if (ai.suspicion >= toAlert) {
                enterState(ai, ec, EnemyState::ALERT, AlertIcon::Suspicion, config);
                ai.searchTimer = config["alert_search_time"].get_or(6.0f);
            }
            break;

        case EnemyState::ALERT:
            if (ai.suspicion >= toCombat) {
                enterState(ai, ec, EnemyState::COMBAT, AlertIcon::Spotted, config);
            }
            else {
                ai.searchTimer -= dt;
                sf::Vector2f toLast = ai.lastKnownPlayerPos - tf.position;
                const float dLast = std::sqrt(toLast.x * toLast.x + toLast.y * toLast.y);

                if (ai.searchTimer <= 0.f || (dLast < 90.f && !sees && ai.timeSinceSeen > 1.5f)) {
                    enterState(ai, ec, EnemyState::PATROL, AlertIcon::Lost, config);
                    ai.suspicion = 0.f;
                    ai.hasSeenPlayer = false;
                    ai.searchTimer = 0.f;
                }
            }
            break;

        case EnemyState::COMBAT: {
            const float loseTime = config["combat_lose_time"].get_or(3.2f);
            const float loseDist = config["combat_lose_distance"].get_or(950.f);

            if (ai.timeSinceSeen > loseTime && dist > loseDist) {
                enterState(ai, ec, EnemyState::ALERT, AlertIcon::Lost, config);
                ai.searchTimer = config["combat_search_time"].get_or(9.0f);
                ai.suspicion = 0.55f;
            }
            break;
        }
        }

        ec.visualState = ai.currentState;
    }

    void enterState(AIState& ai, EnemyComponent& ec, EnemyState s,
        AlertIcon icon, sol::table& config) {
        if (ai.currentState == s) return;
        ai.currentState = s;

        ec.alertIcon = icon;
        ec.alertIconDuration = config["alert_icon_time"].get_or(1.1f);
        ec.alertIconTimer = ec.alertIconDuration;

        ai.maneuverTimer = 0.f;

        if (s == EnemyState::COMBAT) {
            // Startle-back on first contact is right for a pirate who values
            // his hull. A Berserker's first reaction to seeing you is to come.
            ai.maneuver = isMelee(config) ? Maneuver::ATTACK_RUN : Maneuver::FALLBACK;
            ai.maneuverTimer = 0.25f + (rand() % 20) / 100.f;
        }
    }

    // ========================================================================
    // MOVEMENT INTENT
    // ========================================================================
    sf::Vector2f decideVelocity(float dt, AIState& ai, EnemyComponent& ec,
        sf::Vector2f enemyPos, sf::Vector2f playerPos,
        sf::Vector2f toPlayerN, float dist,
        float maxSpeed, sol::table& config)
    {
        (void)dt;

        if (ai.currentState == EnemyState::COMBAT) {
            return combatVelocity(ai, ec, enemyPos, playerPos, toPlayerN, dist, maxSpeed, config);
        }

        if (ai.currentState == EnemyState::ALERT) {
            const sf::Vector2f predicted = ai.lastKnownPlayerPos +
                ai.lastKnownPlayerVel * config["alert_lead_time"].get_or(0.7f);

            sf::Vector2f toTarget = predicted - enemyPos;
            const float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);

            if (d > 60.f) {
                const float sway = std::sin(ai.searchTimer * 2.6f + ai.jitterPhase) * 0.45f;
                sf::Vector2f dir = toTarget / d;
                sf::Vector2f perp(-dir.y, dir.x);
                dir += perp * sway;
                const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (dl > 0.01f) dir /= dl;
                return dir * (maxSpeed * 9.f);
            }

            sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
            return perp * ai.strafeDir * (maxSpeed * 3.5f);
        }

        // PATROL
        // ---- Escort: near a Bloodseeker, wander AROUND him (note 35) ----
        // Each unit holds its own slot on a slowly turning ring, so a pack
        // reads as deferring to him before anyone has seen the player.
        if (m_hasEscort) {
            const float slotR = 150.f + 70.f * std::fmod(ai.jitterPhase * 0.618f, 1.f);
            const float a = ai.jitterPhase + m_clock * 0.25f * ai.strafeDir;
            const sf::Vector2f slot = m_escortPos + sf::Vector2f(std::cos(a), std::sin(a)) * slotR;
            sf::Vector2f to = slot - enemyPos;
            const float dd = std::sqrt(to.x * to.x + to.y * to.y);
            if (dd < 30.f) return { 0.f, 0.f };
            return (to / dd) * (maxSpeed * std::min(7.f, 2.f + dd / 60.f));
        }

        ai.patrolWaitTimer -= 0.15f;
        if (ai.patrolWaitTimer <= 0.f) {
            const float angle = (rand() % 360) * 3.14159f / 180.f;
            ai.patrolTarget = enemyPos + sf::Vector2f(std::cos(angle), std::sin(angle)) * 340.f;
            ai.patrolWaitTimer = 3.5f + (rand() % 30) / 10.f;
        }

        sf::Vector2f toTarget = ai.patrolTarget - enemyPos;
        const float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
        if (d > 50.f) return (toTarget / d) * (maxSpeed * 4.f);
        return { 0.f, 0.f };
    }

    sf::Vector2f combatVelocity(AIState& ai, EnemyComponent& ec,
        sf::Vector2f enemyPos, sf::Vector2f playerPos,
        sf::Vector2f toPlayerN, float dist,
        float maxSpeed, sol::table& config)
    {
        ai.maneuverTimer -= 0.15f;

        if (ai.maneuverTimer <= 0.f) {
            pickManeuver(ai, ec, dist, toPlayerN, config);
        }

        const sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
        const float band = ai.preferredRange;

        m_noiseTime += 0.0016f;
        const float nx = std::sin(m_noiseTime * 3.1f + ai.jitterPhase) * 0.22f;
        const float ny = std::cos(m_noiseTime * 2.3f + ai.jitterPhase * 1.7f) * 0.22f;
        const sf::Vector2f noise(nx, ny);

        switch (ai.maneuver) {

        case Maneuver::APPROACH: {
            sf::Vector2f dir = toPlayerN + perp * ai.strafeDir * 0.55f + noise;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (9.f + ai.aggression * 5.f));
        }

        case Maneuver::ATTACK_RUN: {
            sf::Vector2f dir = toPlayerN + noise * 0.4f;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (15.f + ai.aggression * 7.f));
        }

        case Maneuver::FALLBACK: {
            sf::Vector2f dir = -toPlayerN + perp * ai.strafeDir * 0.35f + noise;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (10.f + (1.f - ai.aggression) * 5.f));
        }

        case Maneuver::REPOSITION: {
            const float a = ai.jitterPhase + m_noiseTime * 0.8f;
            const sf::Vector2f target = playerPos +
                sf::Vector2f(std::cos(a), std::sin(a)) * (band * 1.35f);
            sf::Vector2f dir = target - enemyPos;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * 11.f);
        }

        case Maneuver::CIRCLE: {
            // Tangential first, with a constant inward bite so the orbit is a
            // spiral rather than a stable ring. A pure circle would hold
            // range forever -- this one always ends at bash distance.
            const float inward = config["melee_circle_inward"].get_or(0.26f);
            sf::Vector2f dir = perp * ai.strafeDir + toPlayerN * inward + noise * 0.5f;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (11.f + ai.aggression * 7.f));
        }

        case Maneuver::STRAFE:
        default: {
            const float err = (dist - band) / std::max(1.f, band);
            const float radial = std::clamp(err, -0.9f, 0.9f);

            sf::Vector2f dir = toPlayerN * radial + perp * ai.strafeDir + noise;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (7.f + ai.aggression * 3.f));
        }
        }
    }

    void pickManeuver(AIState& ai, EnemyComponent& ec, float dist,
        sf::Vector2f toPlayerN, sol::table& config) {
        // ---- Naval units: hold the circle, no lunges or retreats ----
        if (config["facing_mode"].get_or<std::string>("target") == "velocity") {
            ai.maneuver = (rand() % 100 < 80) ? Maneuver::STRAFE : Maneuver::REPOSITION;
            ai.maneuverTimer = 1.8f + (rand() % 140) / 100.f;
            if (rand() % 100 < 12) ai.strafeDir = -ai.strafeDir;
            return;
        }

        // ---- Erratic units: twitchy, never settled. ----
        // Short timers and frequent flips. Being hard to LEAD is this unit's
        // defence -- it has no armour bonus and no committed attack to hide
        // behind, so if it moved in readable straight lines it would simply be
        // a slower Raider with a worse gun.
        if (config["maneuver_profile"].get_or<std::string>("standard") == "erratic") {
            const int r = rand() % 100;
            const float band = ai.preferredRange;

            if (dist > band * 1.35f)      ai.maneuver = Maneuver::APPROACH;
            else if (dist < band * 0.55f) ai.maneuver = Maneuver::FALLBACK;
            else if (r < 45)              ai.maneuver = Maneuver::STRAFE;
            else if (r < 70)              ai.maneuver = Maneuver::REPOSITION;
            else if (r < 88)              ai.maneuver = Maneuver::APPROACH;
            else                          ai.maneuver = Maneuver::FALLBACK;

            // Half the usual dwell, so no single line of travel lasts long
            // enough to aim at comfortably.
            ai.maneuverTimer = 0.18f + (rand() % 30) / 100.f;
            if (rand() % 100 < 55) ai.strafeDir = -ai.strafeDir;
            return;
        }

        // ---- Kite units: hold the band, never come inside it (note 26) ----
        // The Bloodseeker's RANGE mode. Unlike "standard" there is no
        // ATTACK_RUN at all: in this mode the only way he closes is the Blood
        // Dive, and an ordinary manoeuvre that charged in would blur the one
        // read that says "he is switching to melee now".
        if (config["maneuver_profile"].get_or<std::string>("standard") == "kite") {
            const float band = ai.preferredRange;
            const int r = rand() % 100;

            if (dist < band * 0.75f) {
                ai.maneuver = Maneuver::FALLBACK;
                ai.maneuverTimer = 0.40f + (rand() % 30) / 100.f;
            }
            else if (dist > band * 1.30f) {
                ai.maneuver = Maneuver::APPROACH;
                ai.maneuverTimer = 0.45f + (rand() % 35) / 100.f;
            }
            else {
                ai.maneuver = (r < 70) ? Maneuver::STRAFE : Maneuver::REPOSITION;
                ai.maneuverTimer = 0.55f + (rand() % 80) / 100.f;
            }
            if (rand() % 100 < 30) ai.strafeDir = -ai.strafeDir;
            return;
        }

        // ---- Melee units: run it down, then circle it. ----
        //
        //   beyond melee_circle_range : straight in (ATTACK_RUN / APPROACH)
        //   inside it                 : CIRCLE, with occasional hard APPROACH
        //
        // No STRAFE (that is a Raider holding a band), no FALLBACK (no retreat
        // instinct), no hit-flinch. The BASH state owns the actual strike;
        // ATTACK_RUN at point-blank would just body-slam for generic contact
        // damage with no tell -- exactly the untelegraphed hurt this unit
        // exists to avoid.
        if (isMelee(config)) {
            const float circleR = config["melee_circle_range"].get_or(320.f);
            const int r = rand() % 100;

            if (dist > circleR) {
                ai.maneuver = (r < 45 + static_cast<int>(ai.aggression * 45.f))
                    ? Maneuver::ATTACK_RUN : Maneuver::APPROACH;
                ai.maneuverTimer = 0.45f + (rand() % 45) / 100.f;

                // Coming out of a straight run, pick the side to swing around
                // from. Matching the player's lateral drift means cutting them
                // off rather than chasing their tail -- the wolf move. Below
                // the threshold (player barely moving sideways) it stays
                // random, so two of them do not always pick the same side.
                if (ai.maneuver == Maneuver::ATTACK_RUN) {
                    const float lat = toPlayerN.x * m_playerVel.y -
                        toPlayerN.y * m_playerVel.x;
                    if (std::fabs(lat) > config["melee_cutoff_speed"].get_or(90.f))
                        ai.strafeDir = (lat > 0.f) ? 1.f : -1.f;
                    else if (rand() % 100 < 30)
                        ai.strafeDir = -ai.strafeDir;
                }
            }
            else {
                // Inside the ring. Mostly orbit; sometimes dive straight in so
                // the circling never settles into a readable metronome.
                ai.maneuver = (r < 72) ? Maneuver::CIRCLE : Maneuver::APPROACH;
                ai.maneuverTimer = (ai.maneuver == Maneuver::CIRCLE)
                    ? 0.6f + (rand() % 60) / 100.f
                    : 0.3f + (rand() % 25) / 100.f;

                // Reversing mid-orbit is the tell that keeps it from being a
                // fixed carousel, but do it rarely: too often and the spiral
                // never converges on bash range.
                if (rand() % 100 < 14) ai.strafeDir = -ai.strafeDir;
            }
            return;
        }

        const float band = ai.preferredRange;
        const int roll = rand() % 100;

        if (rand() % 100 < 35) ai.strafeDir = -ai.strafeDir;

        if (dist > band * 1.6f) {
            ai.maneuver = (roll < 25 + static_cast<int>(ai.aggression * 35))
                ? Maneuver::ATTACK_RUN : Maneuver::APPROACH;
            ai.maneuverTimer = 0.7f + (rand() % 60) / 100.f;
        }
        else if (dist < band * 0.55f) {
            ai.maneuver = (roll < 70 - static_cast<int>(ai.aggression * 40))
                ? Maneuver::FALLBACK : Maneuver::STRAFE;
            ai.maneuverTimer = 0.5f + (rand() % 50) / 100.f;
        }
        else {
            if (roll < 55)      ai.maneuver = Maneuver::STRAFE;
            else if (roll < 72) ai.maneuver = Maneuver::REPOSITION;
            else if (roll < 88) ai.maneuver = Maneuver::ATTACK_RUN;
            else                ai.maneuver = Maneuver::FALLBACK;
            ai.maneuverTimer = 0.6f + (rand() % 120) / 100.f;
        }

        if (ec.hitFlashTimer > 0.f && rand() % 100 < 45) {
            ai.maneuver = Maneuver::FALLBACK;
            ai.maneuverTimer = 0.5f;
        }
    }

    // ========================================================================
    // RAM CHARGE
    // ========================================================================
    bool updateRam(float dt, size_t i, TransformComponent& tf, EnemyComponent& ec,
        AIState& ai, b2BodyId bodyId, sol::table& config,
        sf::Vector2f playerPos, float dist, sf::Vector2f toPlayerN)
    {
        if (ec.ramCooldown > 0.f) ec.ramCooldown -= dt * ec.packMult;

        switch (ec.ramState) {

        case RamState::None: {
            if (!config["ram_enabled"].get_or(false)) return false;
            if (ec.ramCooldown > 0.f) return false;
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.bashState != BashState::None) return false;   // one commit at a time
            if (ec.summonState != 0) return false;               // mid-summon: the charge is the hatch, not the hull
            if (ec.burstBusy()) return false;                    // charged burst runs to its end
            if (ec.shotClearTimer > 0.f) return false;           // see updateBash
            // A dive must end in the bash it promised, never in a ram that
            // happened to roll while the rush crossed the ram band.
            if (ec.duelShift != DuelShift::None) return false;

            // Barge: fires when you are too close OR too far.
            // Berserker: near = 0 (the bash owns close range) and a max, so it
            // charges across the MID band -- "long-mid range" per the roster.
            const float farT = config["ram_far_trigger"].get_or(700.f);
            const float nearT = config["ram_near_trigger"].get_or(210.f);
            const float maxT = config["ram_max_trigger"].get_or(1.0e9f);
            if (dist < nearT || (dist > farT && dist < maxT)) {
                // Squad: a whole chain is one commit (note 43).
                {
                    const float w = config["ram_windup"].get_or(0.85f);
                    const float hold = (w + config["ram_charge_duration"].get_or(1.3f))
                        * std::max(1, config["ram_chain_max"].get_or(1));
                    if (!squadGrant(hold, true, false, w)) return false;
                }
                float chance = config["ram_trigger_chance"].get_or(0.9f);
                if (ec.timesHit == 0)
                    chance *= config["ram_surprise_bonus"].get_or(2.0f);
                if ((rand() % 100) / 100.f > std::min(1.f, chance)) {
                    ec.ramCooldown = config["ram_reroll_delay"].get_or(2.0f);
                    return false;
                }

                // Chain length rolled once, up front. Default 1/1 == no chain.
                const int cMin = std::max(1, config["ram_chain_min"].get_or(1));
                const int cMax = std::max(cMin, config["ram_chain_max"].get_or(1));
                ec.ramChainLeft = (cMin + rand() % (cMax - cMin + 1)) - 1;

                ec.ramState = RamState::Windup;
                ec.ramDuration = config["ram_windup"].get_or(0.85f);
                ec.ramTimer = ec.ramDuration;
                ec.ramGlow = 0.f;
                ec.turretTelegraphActive = false;
                ec.turretBurstLeft = 0;
                ec.telegraphActive = false;
                ec.telegraphTimer = 0.f;
            }
            return false;
        }

        case RamState::Windup: {
            ec.ramTimer -= dt;
            ec.ramGlow = 1.f - (ec.ramTimer / std::max(0.01f, ec.ramDuration));

            ec.ramDir = toPlayerN;

            const float target = std::atan2(toPlayerN.y, toPlayerN.x) * 180.f / 3.14159f + 90.f;
            float d = target - tf.rotation;
            while (d > 180.f) d -= 360.f;
            while (d < -180.f) d += 360.f;
            // 6.0 is the Barge's ponderous swing. A chain re-aim has ~0.35s to
            // come round after overshooting, so the Berserker needs more.
            tf.rotation += d * config["ram_windup_turn"].get_or(6.0f) * dt;
            b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
                b2MakeRot(tf.rotation * 3.14159f / 180.f));

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            b2Body_SetLinearVelocity(bodyId, { v.x * 0.90f, v.y * 0.90f });

            if ((rand() % 100) < static_cast<int>(20 + 60 * ec.ramGlow)) {
                const sf::Vector2f jet = tf.position - ec.ramDir * (30.f + rand() % 40);
                m_em->particles.push_back({ jet,
                    ec.ramDir * -(60.f + rand() % 120),
                    sf::Color(255, static_cast<uint8_t>(120 + rand() % 80), 60, 230),
                    0.30f, 0.34f, 3.f + rand() % 4 });
            }

            if (ec.ramTimer <= 0.f) {
                ec.ramState = RamState::Charge;
                ec.ramDuration = config["ram_charge_duration"].get_or(1.25f);
                ec.ramTimer = ec.ramDuration;

                // ---- Ram cancel plan for THIS link (note 30) ----
                ec.ramCancelRolled = false;
                ec.ramCancelAt = -1.f;
                if (config["ram_cancel_enabled"].get_or(false) &&
                    (rand() % 1000) / 1000.f < config["ram_cancel_chance"].get_or(0.2f))
                    ec.ramCancelAt = ec.ramDuration
                        * (1.f - config["ram_cancel_point"].get_or(0.3f));
                // Snap hull to the committed heading
                lockHeading(tf, bodyId, ec.ramDir);

                const float spd = config["ram_charge_speed"].get_or(1150.f);
                b2Body_SetLinearVelocity(bodyId,
                    { ec.ramDir.x * spd / SCALE, ec.ramDir.y * spd / SCALE });

                m_em->spawnShockRing(tf.position, 20.f, 200.f, 0.30f,
                    sf::Color(255, 160, 70), 5.f, 320.f);
                m_em->requestHitstop(0.02f, 0.06f, 0.35f);
            }
            return true;
        }

        case RamState::Charge: {
            ec.ramTimer -= dt;
            ec.ramGlow = 1.f;

            // ---- Ram cancel (note 30) ----
            // Planned: partway in. Reactive: once per link, the moment the
            // player spends their dodge -- which is exactly when a bash they
            // must PARRY is the right question to ask next.
            if (config["ram_cancel_enabled"].get_or(false)) {
                bool cancel = (ec.ramCancelAt >= 0.f && ec.ramTimer <= ec.ramCancelAt);
                if (!cancel && m_playerDashing && !ec.ramCancelRolled) {
                    ec.ramCancelRolled = true;
                    cancel = (rand() % 1000) / 1000.f < config["ram_cancel_on_dodge"].get_or(0.65f);
                }
                if (cancel) {
                    cancelRamIntoRush(tf, ec, bodyId, config, toPlayerN, dist);
                    return true;
                }
            }

            // Re-assert heading every frame
            lockHeading(tf, bodyId, ec.ramDir);

            const float spd = config["ram_charge_speed"].get_or(1150.f);
            b2Body_SetLinearVelocity(bodyId,
                { ec.ramDir.x * spd / SCALE, ec.ramDir.y * spd / SCALE });

            clearAsteroidsInPath(i, tf.position, config);

            // Sample the trail
            sampleRamTrail(dt, tf, ec);

            for (int k = 0; k < 3; ++k) {
                const sf::Vector2f side((rand() % 60) - 30.f, (rand() % 60) - 30.f);
                m_em->particles.push_back({ tf.position + side,
                    -ec.ramDir * (200.f + rand() % 260),
                    sf::Color(255, static_cast<uint8_t>(150 + rand() % 90),
                              static_cast<uint8_t>(60 + rand() % 60), 235),
                    0.40f, 0.46f, 3.f + rand() % 5 });
            }

            if (ec.ramTimer <= 0.f) {
                if (ec.ramChainLeft > 0 && ai.currentState == EnemyState::COMBAT) {
                    // Next link: re-aim at where the player is NOW. Shorter
                    // windup than the opener, but the same glow and lane --
                    // every link is still announced.
                    --ec.ramChainLeft;
                    ec.ramState = RamState::Windup;
                    ec.ramDuration = config["ram_chain_windup"].get_or(0.35f);
                    ec.ramTimer = ec.ramDuration;
                    ec.ramGlow = 0.f;
                }
                else {
                    ec.ramChainLeft = 0;
                    ec.ramState = RamState::Recover;
                    ec.ramDuration = config["ram_recover"].get_or(1.9f);
                    ec.ramTimer = ec.ramDuration;
                    ec.ramCooldown = config["ram_cooldown"].get_or(15.f);
                }
            }
            return true;
        }

        case RamState::Recover: {
            ec.ramTimer -= dt;
            ec.ramGlow = std::max(0.f, ec.ramTimer / std::max(0.01f, ec.ramDuration)) * 0.35f;

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            b2Body_SetLinearVelocity(bodyId, { v.x * 0.955f, v.y * 0.955f });

            tf.visualOffsetAngle += 4.f * std::sin(m_noiseTime * 22.f);
            tf.visualPivot = { 0.f, -18.f };

            if (ec.ramTimer <= 0.f) {
                ec.ramState = RamState::None;
                ec.ramGlow = 0.f;
            }
            return true;
        }
        }
        return false;
    }

    void clearAsteroidsInPath(size_t self, sf::Vector2f pos, sol::table& config) {
        // 78 was sized for the Barge's beam. Per-unit now.
        const float reach = config["ram_clear_reach"].get_or(78.f);

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j == self) continue;
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Asteroid) continue;

            const sf::Vector2f d = m_em->transforms[j].position - pos;
            if (d.x * d.x + d.y * d.y > reach * reach) continue;

            m_em->healths[j].currentHp = 0.f;
            m_em->spawnImpact(m_em->transforms[j].position,
                sf::Color(255, 190, 110), d);
        }
    }

    // ========================================================================
    // RAM TRAIL BUFFER
    // ========================================================================
    void sampleRamTrail(float dt, const TransformComponent& tf, EnemyComponent& ec) {
        ec.ramTrailFade = 1.f;
        ec.ramTrailTimer -= dt;
        if (ec.ramTrailTimer > 0.f) return;
        ec.ramTrailTimer = 0.025f;

        const int n = std::min(ec.ramTrailCount + 1, EnemyComponent::RAM_TRAIL_MAX);
        for (int k = n - 1; k > 0; --k) ec.ramTrail[k] = ec.ramTrail[k - 1];
        ec.ramTrail[0] = tf.position;
        ec.ramTrailCount = n;
    }

    void lockHeading(TransformComponent& tf, b2BodyId bodyId, sf::Vector2f dir) {
        if (std::fabs(dir.x) < 1e-5f && std::fabs(dir.y) < 1e-5f) return;
        tf.rotation = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
            b2MakeRot(tf.rotation * 3.14159f / 180.f));
        b2Body_SetAngularVelocity(bodyId, 0.f);
    }

    // ========================================================================
    // MINE RUN — the laying dash
    // ========================================================================
    //
    // A committed sprint that lays a wall of mines ACROSS the player's ground
    // rather than behind the Maniac's own. Dropping only in his wake meant the
    // field was always somewhere the player had no reason to go; this puts it
    // where they are about to be.
    //
    // Harmless to touch. No damage, no invulnerability, no knockback -- the
    // hazard is what it leaves, not the ship. That is the whole separation
    // from a Berserker charge, and it has to survive tuning: the moment this
    // deals contact damage it becomes a worse version of an attack that
    // already exists.
    //
    // Returns true while it owns the ship.
    bool updateMineRun(float dt, size_t i, TransformComponent& tf, EnemyComponent& ec,
        AIState& ai, b2BodyId bodyId, sol::table& config,
        sf::Vector2f toPlayerN, float dist)
    {
        (void)i;

        switch (ec.mineRunState) {

        case MineRunState::None: {
            if (!config["mine_run_enabled"].get_or(false)) return false;
            if (!config["mine_enabled"].get_or(false)) return false;
            if (ec.mineRunCooldown > 0.f) return false;
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.frenzyState != FrenzyState::None) return false;
            if (ec.microRecover > 0.f || ec.rocketsLeft != 0) return false;
            if (ec.bashState != BashState::None) return false;

            const float minR = config["mine_run_min_range"].get_or(240.f);
            const float maxR = config["mine_run_max_range"].get_or(800.f);
            if (dist < minR || dist > maxR) return false;
            if (countMines(tf.entityId) >= config["mine_max_active"].get_or(6)) return false;
            if (!squadGrant(config["mine_run_windup"].get_or(0.45f) + config["mine_run_time"].get_or(0.85f)))
                return false;

            // ---- Pick the line ----
            // Aim at where the player is GOING, offset sideways, so the run
            // crosses their path instead of chasing it. A run straight at them
            // lays mines they simply back away from.
            sf::Vector2f lead = toPlayerN;
            const float pv = std::sqrt(m_playerVel.x * m_playerVel.x +
                m_playerVel.y * m_playerVel.y);
            if (pv > 40.f) {
                const sf::Vector2f pd = m_playerVel / pv;
                const float weight = config["mine_run_lead"].get_or(0.55f);
                lead = toPlayerN + pd * weight;
                const float l = std::sqrt(lead.x * lead.x + lead.y * lead.y);
                if (l > 0.01f) lead /= l;
            }

            ec.mineRunState = MineRunState::Windup;
            ec.mineRunDuration = config["mine_run_windup"].get_or(0.45f);
            ec.mineRunTimer = ec.mineRunDuration;
            ec.mineRunDir = lead;
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            return true;
        }

        case MineRunState::Windup: {
            ec.mineRunTimer -= dt;
            ec.mineRunDir = toPlayerN;   // keeps tracking until the run starts
            turnToward(tf, bodyId, ec.mineRunDir,
                config["mine_run_turn"].get_or(9.f), dt);

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-6.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });

            // Sparks off the back: something is about to come out of there.
            if ((rand() % 100) < 50) {
                const float r = tf.rotation * 3.14159f / 180.f;
                const sf::Vector2f aft(-std::sin(r), std::cos(r));
                m_em->particles.push_back({
                    tf.position + aft * 26.f,
                    aft * (40.f + rand() % 90),
                    sf::Color(255, 180, 90, 225), 0.20f, 0.20f, 2.f + rand() % 2 });
            }

            if (ec.mineRunTimer <= 0.f) {
                ec.mineRunState = MineRunState::Run;
                ec.mineRunDuration = config["mine_run_time"].get_or(0.85f);
                ec.mineRunTimer = ec.mineRunDuration;
                ec.mineRunDrop = 0.f;   // distance accumulator: first drop is immediate
                pickMineRunShape(ec, config, toPlayerN);
                lockHeading(tf, bodyId, ec.mineRunDir);
            }
            return true;
        }

        case MineRunState::Run: {
            ec.mineRunTimer -= dt;
            // ---- Bend (note 38): an arc or an S, not only a straight wall ----
            if (ec.mineRunShape != 0) {
                if (ec.mineRunShape == 2 && ec.mineRunTimer <= ec.mineRunDuration * 0.5f &&
                    ec.mineRunTimer + dt > ec.mineRunDuration * 0.5f)
                    ec.mineRunCurve = -ec.mineRunCurve;   // S: flip half way
                const float a = ec.mineRunCurve * dt * 3.14159265f / 180.f;
                const float c = std::cos(a), s = std::sin(a);
                ec.mineRunDir = { ec.mineRunDir.x * c - ec.mineRunDir.y * s,
                                  ec.mineRunDir.x * s + ec.mineRunDir.y * c };
            }
            lockHeading(tf, bodyId, ec.mineRunDir);

            const float spd = config["mine_run_speed"].get_or(760.f);
            b2Body_SetLinearVelocity(bodyId,
                { ec.mineRunDir.x * spd / SCALE, ec.mineRunDir.y * spd / SCALE });

            // ---- Spacing is DISTANCE, not time ----
            // A timed drop bunches the whole carpet into one clump whenever
            // the run is slow or short, and the blast zones then sit on top of
            // each other: six mines covering one mine's worth of ground. The
            // gap defaults to a full blast radius, so the zones touch without
            // overlapping and the carpet actually spans a line the player has
            // to go around rather than a spot they step past.
            ec.mineRunDrop -= std::sqrt(
                (ec.mineRunDir.x * spd * dt) * (ec.mineRunDir.x * spd * dt) +
                (ec.mineRunDir.y * spd * dt) * (ec.mineRunDir.y * spd * dt));

            if (ec.mineRunDrop <= 0.f &&
                countMines(tf.entityId) < config["mine_max_active"].get_or(6)) {
                const sf::Vector2f back = -ec.mineRunDir;
                dropMine(tf.position + back * 28.f,
                    back * (30.f + rand() % 40), back, tf.entityId, config);
                ec.mineRunDrop = config["mine_run_gap"].get_or(
                    config["mine_blast_radius"].get_or(130.f));
            }

            if (ec.mineRunTimer <= 0.f) {
                ec.mineRunState = MineRunState::Recover;
                ec.mineRunDuration = config["mine_run_recover"].get_or(0.55f);
                ec.mineRunTimer = ec.mineRunDuration;
            }
            return true;
        }

        case MineRunState::Recover: {
            ec.mineRunTimer -= dt;
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-4.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });

            tf.visualOffsetAngle += 5.f * std::sin(ec.mineRunTimer * 21.f);

            if (ec.mineRunTimer <= 0.f) {
                ec.mineRunState = MineRunState::None;
                ec.mineRunCooldown = config["mine_run_cooldown"].get_or(7.f)
                    + (rand() % 200) / 100.f;
            }
            return true;
        }
        }
        return false;
    }

    /**
     * @brief Straight, arc or S for this carpet (note 38).
     *
     * Rolled when the run starts (the windup still reads the same: brake,
     * turn, sparks). An ARC bends toward the player's side of the line, so
     * the carpet wraps round them instead of fencing one flank; an S bends
     * the other way half way through. `mine_run_curve` is the turn rate in
     * deg/s -- at the default 95 over 1.25s the arc sweeps ~120 deg on a
     * ~470px radius.
     */
    void pickMineRunShape(EnemyComponent& ec, sol::table& config, sf::Vector2f toPlayerN) {
        const float ws = std::max(0.f, config["mine_run_straight_weight"].get_or(1.f));
        const float wa = std::max(0.f, config["mine_run_arc_weight"].get_or(0.f));
        const float wz = std::max(0.f, config["mine_run_s_weight"].get_or(0.f));
        const float total = ws + wa + wz;
        ec.mineRunShape = 0;
        ec.mineRunCurve = 0.f;
        if (total <= 0.f) return;
        const float roll = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * total;
        if (roll < ws) return;
        ec.mineRunShape = (roll < ws + wa) ? 1 : 2;
        // Which side is the player on? cross(dir, toPlayer) > 0 = clockwise
        // in screen space (y down), i.e. a positive turn bends toward them.
        const float cr = ec.mineRunDir.x * toPlayerN.y - ec.mineRunDir.y * toPlayerN.x;
        const float side = (std::fabs(cr) < 0.05f) ? ((rand() % 2) ? 1.f : -1.f) : (cr > 0.f ? 1.f : -1.f);
        ec.mineRunCurve = side * config["mine_run_curve"].get_or(95.f);
    }

    // ========================================================================
    // MINE TOSS -- the Maniac's get-off-me (note 38)
    // ========================================================================
    //
    // Close in, he brakes (windup: hull shakes, sparks all round), SPINS and
    // throws `toss_count` mines outward one by one through the spin -- a
    // ring, already armed and already lit (`toss_fuse`), so they go off a
    // beat later wherever they have drifted to. Then a dizzy recover.
    //
    // Counterplay: the windup is the cue to LEAVE or to get right on top of
    // him (the ring lands ~toss_speed/1.4 px out; the centre is the gap).
    // Toss mines do not count against mine_max_active.
    bool updateMineToss(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, sol::table& config, const enemyarch::ArchetypeDef& adef, float dist)
    {
        if (!config["toss_enabled"].get_or(false)) return false;
        if (ec.tossCooldown > 0.f && ec.tossState == 0) ec.tossCooldown -= dt * ec.packMult;

        switch (ec.tossState) {
        case 0: {
            if (ec.tossCooldown > 0.f) return false;
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.frenzyState != FrenzyState::None) return false;
            if (ec.mineRunState != MineRunState::None) return false;
            if (ec.bashState != BashState::None || ec.rocketsLeft != 0) return false;
            if (ec.microRecover > 0.f) return false;
            if (dist > config["toss_trigger_range"].get_or(320.f)) return false;
            if (!squadGrant(config["toss_windup"].get_or(0.5f) + config["toss_spin_time"].get_or(0.5f))) return false;
            ec.tossState = 1;
            ec.tossDuration = config["toss_windup"].get_or(0.5f);
            ec.tossTimer = ec.tossDuration;
            ec.tossSpin = (rand() % 2) ? 1.f : -1.f;
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            m_em->spawnShockRing(tf.position, adef.radius * 0.6f, adef.radius * 2.2f, 0.40f,
                sf::Color(255, 150, 60), 3.f, 210.f);
            return true;
        }
        case 1: {   // WINDUP: brake, shake, sparks all round
            ec.tossTimer -= dt;
            const float u = 1.f - ec.tossTimer / std::max(0.01f, ec.tossDuration);
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = std::exp(-7.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
            b2Body_SetAngularVelocity(bodyId, 0.f);
            tf.visualOffsetAngle += 7.f * u * std::sin(m_clock * 60.f);
            if ((rand() % 100) < static_cast<int>(40 + 50 * u)) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                const sf::Vector2f d(std::cos(a), std::sin(a));
                m_em->particles.push_back({ tf.position + d * adef.radius * 0.8f,
                    d * (60.f + rand() % 80), sf::Color(255, 175, 80, 230), 0.22f, 0.22f, 2.f + rand() % 2 });
            }
            if (ec.tossTimer <= 0.f) {
                ec.tossState = 2;
                ec.tossDuration = config["toss_spin_time"].get_or(0.5f);
                ec.tossTimer = ec.tossDuration;
                ec.tossThrown = 0;
                ec.tossBaseAngle = (tf.rotation - 90.f) * 3.14159265f / 180.f;
            }
            return true;
        }
        case 2: {   // SPIN: one full turn, a mine every 1/n of it
            ec.tossTimer -= dt;
            const float u = 1.f - ec.tossTimer / std::max(0.01f, ec.tossDuration);
            const float turn = 360.f / std::max(0.05f, ec.tossDuration);
            tf.rotation += ec.tossSpin * turn * dt;
            b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
                b2MakeRot(tf.rotation * 3.14159265f / 180.f));
            b2Body_SetLinearVelocity(bodyId, { 0.f, 0.f });
            const int n = std::max(1, config["toss_count"].get_or(6));
            while (ec.tossThrown < n && u >= static_cast<float>(ec.tossThrown) / n) {
                const float a = ec.tossBaseAngle + ec.tossSpin * 6.2831853f * ec.tossThrown / n;
                tossMine(tf, ec, adef, config, sf::Vector2f(std::cos(a), std::sin(a)));
                ++ec.tossThrown;
            }
            if (ec.tossTimer <= 0.f) {
                ec.tossState = 3;
                ec.tossDuration = config["toss_recover"].get_or(0.7f);
                ec.tossTimer = ec.tossDuration;
            }
            return true;
        }
        case 3: {   // RECOVER: dizzy, drifting, the punish window
            ec.tossTimer -= dt;
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = std::exp(-3.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
            tf.visualOffsetAngle += 6.f * std::sin(ec.tossTimer * 19.f);
            if (ec.tossTimer <= 0.f) {
                ec.tossState = 0;
                ec.tossCooldown = config["toss_cooldown"].get_or(8.f) + (rand() % 150) / 100.f;
            }
            return true;
        }
        default:
            ec.tossState = 0;
            return false;
        }
    }

    /// One toss mine: thrown outward, armed, fuse lit.
    void tossMine(const TransformComponent& tf, EnemyComponent& ec,
        const enemyarch::ArchetypeDef& adef, sol::table& config, sf::Vector2f dir)
    {
        (void)ec;
        const sf::Vector2f pos = tf.position + dir * (adef.radius + 12.f);
        const uint32_t id = m_ef->createMine(*m_em, pos,
            dir * config["toss_speed"].get_or(300.f), tf.entityId, m_worldId, config);
        const size_t bi = m_em->getEntityIndex(id);
        if (bi != (size_t)-1 && bi < m_em->bullets.size()) {
            auto& b = m_em->bullets[bi];
            b.mineTossed = true;
            b.armTimer = 0.f;
            b.mineArmed = true;
            b.mineFuseTime = config["toss_fuse"].get_or(1.4f);
            m_em->lightMineFuse(b, pos);
        }
        for (int k = 0; k < 6; ++k) {
            const float life = 0.16f + (rand() % 14) / 100.f;
            m_em->particles.push_back({ pos, dir * (120.f + rand() % 120)
                + sf::Vector2f(-dir.y, dir.x) * static_cast<float>((rand() % 120) - 60),
                sf::Color(255, 190, 110, 230), life, life, 2.f + rand() % 2 });
        }
    }

    // ========================================================================
    // SUMMON -- the Barge spits Wardogs (note 37)
    // ========================================================================
    //
    // On a cooldown, in COMBAT, while the summoner has room under
    // `summon_max_alive`: the hull CHARGES (`summon_windup`: it slows, swells
    // across the beam, and the sponson hatches glow and vent), then SPITS
    // `summon_count` Wardogs out of both sides at `summon_spit_speed`. They
    // arrive already fighting (COMBAT, player spotted) and escort him
    // (escort_takes / escort_leash on the Barge). The turret holds fire
    // through the whole charge: that charge is the tell.
    //
    // A stagger or a stun mid-charge breaks it: nothing comes out, and the
    // next try waits `summon_fail_cooldown`.
    bool updateSummon(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, sol::table& config, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f playerPos, float dist)
    {
        if (!config["summon_enabled"].get_or(false)) return false;

        switch (ec.summonState) {
        case 0: {
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.summonCooldown <= -999.f) {
                ec.summonCooldown = config["summon_first_delay"].get_or(4.f);
                return false;
            }
            if (ec.summonCooldown > 0.f) { ec.summonCooldown -= dt * ec.packMult; return false; }
            if (ec.ramState != RamState::None || ec.shotgunState != 0) return false;
            if (dist > config["summon_max_range"].get_or(1100.f)) return false;
            const int per = std::max(1, config["summon_count"].get_or(2));
            if (countSummoned(tf.entityId) + per > config["summon_max_alive"].get_or(4)) {
                ec.summonCooldown = 1.5f;   // full: look again shortly
                return false;
            }
            ec.summonState = 1;
            ec.summonDuration = config["summon_windup"].get_or(1.1f);
            ec.summonTimer = ec.summonDuration;
            ec.turretTelegraphActive = false;
            ec.turretBurstLeft = 0;
            m_em->spawnShockRing(tf.position, adef.radius * 0.5f, adef.radius * 1.4f, 0.40f,
                sf::Color(255, 150, 60), 4.f, 200.f);
            return true;
        }
        case 1: {   // CHARGE
            ec.summonTimer -= dt;
            const float u = 1.f - ec.summonTimer / std::max(0.01f, ec.summonDuration);
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = std::exp(-2.5f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
            // Swells across the beam -- the cargo pushing at the sponsons --
            // with a shudder that gets worse as it fills.
            tf.visualScale.x *= 1.f + 0.11f * u + 0.025f * u * std::sin(m_clock * 52.f);
            tf.visualScale.y *= 1.f + 0.03f * u;
            for (float side : { -1.f, 1.f }) {
                if ((rand() % 100) >= static_cast<int>(30 + 60 * u)) continue;
                sf::Vector2f out;
                const sf::Vector2f h = summonHatch(tf, config, side, out);
                m_em->particles.push_back({ h + out * 4.f,
                    out * (50.f + rand() % (60 + static_cast<int>(160 * u)))
                    + sf::Vector2f(-out.y, out.x) * static_cast<float>((rand() % 80) - 40),
                    sf::Color(255, static_cast<uint8_t>(120 + 90 * u), 60, 235),
                    0.20f, 0.20f, 2.f + 2.f * u });
            }
            if (ec.summonTimer <= 0.f) {
                spitWardogs(tf, ec, bodyId, config, playerPos);
                ec.summonState = 2;
                ec.summonDuration = config["summon_recover"].get_or(0.6f);
                ec.summonTimer = ec.summonDuration;
            }
            return true;
        }
        case 2: {   // RECOVER: the hatches slam, the hull settles
            ec.summonTimer -= dt;
            const float u = ec.summonTimer / std::max(0.01f, ec.summonDuration);
            tf.visualScale.x *= 1.f - 0.05f * u * std::sin(u * 20.f);
            if (ec.summonTimer <= 0.f) {
                ec.summonState = 0;
                ec.summonCooldown = config["summon_cooldown"].get_or(14.f) + (rand() % 200) / 100.f;
            }
            return true;
        }
        default:
            ec.summonState = 0;
            return false;
        }
    }

    /// World position of the port (side -1) or starboard (+1) hatch, and its
    /// outward normal. `summon_hatch` is authored starboard, x the scale.
    sf::Vector2f summonHatch(const TransformComponent& tf, sol::table& config, float side,
        sf::Vector2f& out) const
    {
        const float sc = config["scale"].get_or(1.f);
        float hx = 30.f, hy = 4.f;
        if (sol::optional<sol::table> h = config["summon_hatch"]) {
            hx = (*h)["x"].get_or(hx);
            hy = (*h)["y"].get_or(hy);
        }
        const sf::Vector2f local(side * hx * sc, hy * sc);
        const float r = tf.rotation * 3.14159265f / 180.f;
        const float c = std::cos(r), s = std::sin(r);
        out = sf::Vector2f(c, s) * side;
        return tf.position + sf::Vector2f(local.x * c - local.y * s, local.x * s + local.y * c);
    }

    int countSummoned(uint32_t summoner) const {
        int n = 0;
        for (size_t j = 0; j < m_em->enemies.size() && j < m_em->healths.size(); ++j)
            if (m_em->enemies[j].summonedBy == summoner && m_em->healths[j].currentHp > 0.f) ++n;
        return n;
    }

    void spitWardogs(const TransformComponent& tf, EnemyComponent& ec, b2BodyId bodyId,
        sol::table& config, sf::Vector2f playerPos)
    {
        (void)ec;
        const std::string key = config["summon_key"].get_or<std::string>("WARDOG");
        const uint8_t wid = m_registry->idOf(key);
        if (wid == enemyarch::INVALID_ARCHETYPE) return;
        const auto* wdef = m_registry->byId(wid);
        const float wr = wdef ? wdef->radius : 30.f;
        const int per = std::max(1, config["summon_count"].get_or(2));
        const float spd = config["summon_spit_speed"].get_or(520.f);
        const b2Vec2 hv = b2Body_GetLinearVelocity(bodyId);
        const uint32_t self = tf.entityId;

        for (int n = 0; n < per; ++n) {
            const float side = (n % 2) ? -1.f : 1.f;
            sf::Vector2f out;
            const sf::Vector2f hatch = summonHatch(tf, config, side, out);
            // Out past the sponson, and a little fore/aft if a side gets two.
            const float hr = tf.rotation * 3.14159265f / 180.f;
            const sf::Vector2f fwd(std::sin(hr), -std::cos(hr));
            const float along = (n / 2) * (wr * 1.6f) * ((n / 2) % 2 ? -1.f : 1.f);
            const sf::Vector2f pos = hatch + out * (wr + 10.f) + fwd * along;

            const uint32_t id = m_ef->createEnemy(*m_em, pos, *m_lua, m_worldId, *m_registry, wid, false);
            const size_t idx = m_em->getEntityIndex(id);
            if (idx == (size_t)-1 || idx >= m_em->enemies.size()) continue;

            const float deg = std::atan2(out.y, out.x) * 180.f / 3.14159265f + 90.f;
            m_em->transforms[idx].rotation = deg;
            const b2BodyId nb = m_em->physics[idx].bodyId;
            if (b2Body_IsValid(nb)) {
                b2Body_SetTransform(nb, b2Body_GetPosition(nb), b2MakeRot(deg * 3.14159265f / 180.f));
                b2Body_SetLinearVelocity(nb, { hv.x + out.x * spd / SCALE, hv.y + out.y * spd / SCALE });
            }
            auto& ne = m_em->enemies[idx];
            ne.summonedBy = self;
            ne.visualState = EnemyState::COMBAT;
            AIState& na = m_aiCache[id];
            na.currentState = EnemyState::COMBAT;
            na.hasSeenPlayer = true;
            na.suspicion = 1.f;
            na.timeSinceSeen = 0.f;
            na.lastKnownPlayerPos = playerPos;

            // The spit: a hard ring at the hatch and a gout of hot gas.
            m_em->spawnShockRing(hatch, 6.f, 70.f, 0.22f, sf::Color(255, 160, 70), 4.f, 240.f);
            m_em->spawnExplosion(hatch + out * 10.f, sf::Color(255, 150, 70), 12, 2.6f);
            for (int k = 0; k < 10; ++k) {
                const float life = 0.25f + (rand() % 20) / 100.f;
                m_em->particles.push_back({ hatch, out * (160.f + rand() % 220)
                    + fwd * static_cast<float>((rand() % 160) - 80),
                    sf::Color(190, 170, 160, 190), life, life, 4.f + rand() % 4 });
            }
        }
        m_em->addTrauma(0.14f);
    }

    // ========================================================================
    // FRENZY — the Maniac's low-HP suicide override
    // ========================================================================
    //
    // Returns true while it owns the ship. Everything is one-way: there is no
    // transition back to None, and no cooldown, because the fiction and the
    // gameplay agree that this is the last thing he does.
    //
    //   Ignite  brakes hard, shakes, colour ramps, sparks. Short but LOUD --
    //           if the player misses this beat the charge is unreadable.
    //   Charge  tracks the player loosely (not a locked lane like the Barge's
    //           ram: he is steering himself into you, not firing himself).
    //           DamageSystem resolves contact, and parrying flips him to Thrown.
    //   Thrown  no AI at all. A spinning bomb on a 2s fuse; DamageSystem blows
    //           him up on first impact or when the timer runs out.
    bool updateFrenzy(float dt, size_t i, TransformComponent& tf, EnemyComponent& ec,
        AIState& ai, b2BodyId bodyId, sol::table& config,
        sf::Vector2f toPlayerN, float dist)
    {
        (void)dist;

        // ---- Trigger ----
        if (ec.frenzyState == FrenzyState::None) {
            if (!config["suicide_enabled"].get_or(false)) return false;
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.microRecover > 0.f) return false;   // spec: not while recovering

            const float maxHp = std::max(1.f, m_em->healths[i].maxHp);
            const float frac = m_em->healths[i].currentHp / maxHp;
            if (frac > config["suicide_hp_fraction"].get_or(0.3f)) return false;

            ec.frenzyState = FrenzyState::Ignite;
            ec.frenzyTimer = config["suicide_ignite_time"].get_or(0.8f);
            ec.frenzy = 0.f;
            ec.frenzyFuse = config["suicide_fuse"].get_or(5.0f);
            ec.frenzyGrace = 0.f;
            ec.mineRunState = MineRunState::None;
            ec.tossState = 0;

            // Drop everything he was doing. A rocket in the tube at the moment
            // he ignites would arrive during the charge and muddy the read.
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            ec.rocketsLeft = 0;
            ec.bashState = BashState::None;
            ec.bashStrikePending = false;
            ec.stormActive = false;

            m_em->spawnShockRing(tf.position, 10.f, 150.f, 0.45f,
                sf::Color(255, 210, 80), 5.f, 230.f);
            m_em->addTrauma(0.22f);
        }

        switch (ec.frenzyState) {

        case FrenzyState::Ignite: {
            ec.frenzyTimer -= dt;
            const float u = std::clamp(1.f - ec.frenzyTimer /
                std::max(0.01f, config["suicide_ignite_time"].get_or(0.8f)), 0.f, 1.f);
            ec.frenzy = u;

            // Brake and shake. Coming to a near-stop makes the ignition read as
            // a decision rather than as another movement state.
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-4.5f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });
            b2Body_SetAngularVelocity(bodyId, 0.f);

            tf.visualOffsetAngle += (9.f + 14.f * u) * std::sin(m_noiseTime * 61.f);

            // Shake and swell. The blink itself is drawn by RenderSystem off
            // frenzyBlinkHz, so it speaks with one voice across hull colour,
            // corona and exhaust instead of each system picking its own beat.
            ec.frenzyBlinkHz = 2.5f + 3.5f * u;

            // Shake hard. Together with the blink this is the "rules just
            // changed" beat, and it has to survive a screen with a dozen other
            // things moving on it.
            tf.visualOffsetAngle += (6.f + 9.f * u) * std::sin(m_noiseTime * 77.f);
            const float pulse = 1.f + 0.10f * u * std::sin(m_noiseTime * 23.f);
            tf.visualScale.x *= pulse * (1.f + 0.09f * u);
            tf.visualScale.y *= pulse * (1.f + 0.09f * u);

            // The laugh, as sparks. No audio system to lean on, so the beat has
            // to carry on motion and particles alone.
            if ((rand() % 100) < 55) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({
                    tf.position + sf::Vector2f(std::cos(a), std::sin(a)) * 20.f,
                    sf::Vector2f(std::cos(a), std::sin(a)) * (90.f + rand() % 160),
                    sf::Color(255, static_cast<uint8_t>(150 + rand() % 100), 40, 235),
                    0.30f, 0.30f, 2.f + rand() % 3 });
            }

            if (ec.frenzyTimer <= 0.f) {
                ec.frenzyState = FrenzyState::Charge;
                ec.frenzyTimer = config["suicide_max_time"].get_or(9.f);
                ec.frenzy = 1.f;
            }
            return true;
        }

        case FrenzyState::Charge: {
            ec.frenzyTimer -= dt;
            ec.frenzy = 1.f;

            // ---- Blink rate is RANGE ----
            // Slow far away, frantic up close. The player never has to read a
            // number or a bar: how fast he is flashing IS how close he is to
            // going off in their face.
            const float blastR = config["suicide_blast_radius"].get_or(270.f);
            const float near01 = 1.f - std::clamp(dist / std::max(1.f, blastR * 2.2f), 0.f, 1.f);
            ec.frenzyBlinkHz = 2.5f + 14.f * near01 * near01;

            // ---- Fuse ----
            // He does not go off on contact alone: the fuse has to be out AND
            // the player has to be inside the blast. That makes the charge a
            // countdown the player can out-run rather than a touch of death,
            // and it is what gives "get distance" a real answer.
            if (ec.frenzyFuse > 0.f) {
                ec.frenzyFuse -= dt;
                if (ec.frenzyFuse <= 0.f)
                    ec.frenzyGrace = config["suicide_grace"].get_or(1.0f);
            }
            else {
                if (dist <= blastR * config["suicide_detonate_fraction"].get_or(0.7f)) {
                    m_em->healths[i].currentHp = 0.f;   // DamageSystem blows him up
                    return true;
                }
                // Out of fuse, player out of reach: one last second to close.
                ec.frenzyGrace -= dt;
                if (ec.frenzyGrace <= 0.f) {
                    m_em->healths[i].currentHp = 0.f;
                    return true;
                }
            }

            turnToward(tf, bodyId, toPlayerN, config["suicide_turn_rate"].get_or(5.0f), dt);

            // Steered, not railed. A locked lane would make him dodgeable the
            // same way a ram is, and the spec wants the answer to be parry or
            // kill -- not sidestep.
            const float spd = config["suicide_speed"].get_or(700.f);
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const b2Vec2 want = { toPlayerN.x * spd / SCALE, toPlayerN.y * spd / SCALE };
            const float k = 1.f - std::exp(-6.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x + (want.x - v.x) * k,
                                               v.y + (want.y - v.y) * k });

            tf.visualOffsetAngle += 6.f * std::sin(m_noiseTime * 47.f);

            // Shake and swell on the same beat as the blink, so the ship
            // visibly winds up as it closes.
            {
                const float hz = std::max(1.f, ec.frenzyBlinkHz);
                const float beat = 1.f + 0.09f * std::sin(m_noiseTime * hz * 6.28318f);
                tf.visualScale.x *= beat;
                tf.visualScale.y *= beat;
                tf.visualOffsetAngle += (4.f + 7.f * near01) * std::sin(m_noiseTime * 83.f);
            }

            // ---- Engine burn ----
            // Thrown straight out the back in a fat cone. EffectsSystem already
            // opens the throttle for frenzy; this is the raw sparkle on top,
            // and it scales with how close he is.
            {
                const float r = tf.rotation * 3.14159f / 180.f;
                const sf::Vector2f aft(-std::sin(r), std::cos(r));
                const sf::Vector2f side(-aft.y, aft.x);
                const int n = 3 + static_cast<int>(5.f * near01);
                for (int k = 0; k < n; ++k) {
                    const float lat = ((rand() % 200) - 100) / 100.f;
                    const float life = 0.16f + (rand() % 20) / 100.f;
                    m_em->particles.push_back({
                        tf.position + aft * 22.f + side * (lat * 12.f),
                        aft * (220.f + rand() % 320) + side * (lat * 130.f),
                        sf::Color(255, static_cast<uint8_t>(170 + rand() % 85),
                            static_cast<uint8_t>(60 + rand() % 90), 240),
                        life, life, 3.f + rand() % 3 });
                }
            }

            // Spark output rises with the blink, so the closer he gets the
            // more he visibly comes apart.
            const int sparks = 2 + static_cast<int>(6.f * near01);
            for (int k2 = 0; k2 < sparks; ++k2) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                const sf::Vector2f d(std::cos(a), std::sin(a));
                m_em->particles.push_back({
                    tf.position + d * 24.f, d * (70.f + rand() % 160),
                    sf::Color(255, static_cast<uint8_t>(60 + rand() % 70), 30, 225),
                    0.22f, 0.22f, 2.f + rand() % 3 });
            }

            // Hard backstop, well past the fuse.
            if (ec.frenzyTimer <= 0.f) m_em->healths[i].currentHp = 0.f;
            return true;
        }

        case FrenzyState::Thrown: {
            // DamageSystem owns the fuse and the blast. This branch only adds
            // the shake: he is a lit bomb tumbling away, and the frantic blink
            // (set at throw time) tells the player how long they have.
            ec.frenzy = 1.f;
            tf.visualOffsetAngle += 11.f * std::sin(m_noiseTime * 71.f);
            return true;
        }

        default: return false;
        }
    }

    // ========================================================================
    // SKID ROCKETS
    // ========================================================================
    //
    // One volley of `rocket_count`, spaced by `rocket_spacing` so the two
    // rounds arrive on different lines rather than as one wide wall, then a
    // mandatory micro-recovery. Fired at the player's ENTITY ID, not their
    // position: the flight code steers, and steering is the point.
    void updateRockets(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        uint32_t entityId, sol::table& config, sf::Vector2f toPlayerN, float dist)
    {
        (void)dt;
        if (!config["rocket_enabled"].get_or(false)) return;
        if (ai.currentState != EnemyState::COMBAT) return;
        if (ec.frenzyState != FrenzyState::None) return;
        if (ec.bashState != BashState::None || ec.ramState != RamState::None) return;
        if (ec.tossState != 0) return;   // spinning: one commit at a time

        // ---- Mid-volley ----
        if (ec.rocketsLeft != 0) {
            ec.rocketVolleyTimer -= dt;
            if (ec.rocketVolleyTimer > 0.f) return;

            fireRocket(tf, ec, entityId, toPlayerN, config, ec.rocketsLeft == -1);
            if (ec.rocketsLeft == -1) ec.rocketsLeft = 0;
            else --ec.rocketsLeft;
            if (ec.rocketsLeft > 0) {
                ec.rocketVolleyTimer = config["rocket_spacing"].get_or(0.22f);
            }
            else {
                // The punish window. Short enough not to feel like a stun,
                // long enough that closing on him after a volley is a real
                // option rather than a coin flip.
                ec.microRecover = config["micro_recover"].get_or(0.8f);
                // Mines right after the volley: the punish window is not free.
                ec.mineTimer = std::min(ec.mineTimer, 0.05f);
                ec.rocketCooldown = config["rocket_cooldown"].get_or(4.5f)
                    + (rand() % 120) / 100.f;
            }
            return;
        }

        if (ec.microRecover > 0.f || ec.rocketCooldown > 0.f) return;
        if (ec.telegraphActive) return;

        const float minR = config["rocket_min_range"].get_or(260.f);
        const float maxR = config["rocket_max_range"].get_or(900.f);
        if (dist < minR || dist > maxR) return;
        if (!squadGrant(config["rocket_count_max"].get_or(3) * config["rocket_spacing"].get_or(0.22f) + 0.6f)) {
            ec.rocketCooldown = m_sqCfg.retryDelay;
            return;
        }

        // Two shapes of attack off one weapon, rolled per volley:
        //   SALVO  2-3 tracking rockets, spaced -- a wall you route around.
        //   SNIPE  one rocket at ~2x speed -- a shot you react to.
        // Same tracking on both, so the skill is reading WHICH one left the
        // tube, not learning two different behaviours.
        if ((rand() % 100) < static_cast<int>(config["rocket_fast_chance"].get_or(0.35f) * 100.f)) {
            ec.rocketsLeft = -1;   // sentinel: one fast round
        }
        else {
            const int cMin = std::max(1, config["rocket_count_min"].get_or(2));
            const int cMax = std::max(cMin, config["rocket_count_max"].get_or(3));
            ec.rocketsLeft = cMin + rand() % (cMax - cMin + 1);
        }
        ec.rocketVolleyTimer = 0.f;
    }

    // ========================================================================
    // MINES
    // ========================================================================
    //
    // Dropped behind him while he moves, and in a small cluster right after a
    // volley -- which is what makes chasing him down immediately after rockets
    // the greedy option it is meant to be. The active cap is per unit and
    // counted live, so a long fight cannot carpet the arena.
    void updateMines(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        uint32_t entityId, sol::table& config)
    {
        if (!config["mine_enabled"].get_or(false)) return;
        // The passive trail is off by default (Maniac pass): the field comes
        // from the carpet and the toss, both of which you can see coming.
        if (!config["mine_trail_enabled"].get_or(false)) return;
        if (ec.frenzyState != FrenzyState::None) return;   // ranged kit is gone
        if (ec.mineRunState != MineRunState::None) return; // the run drops its own
        if (ai.currentState != EnemyState::COMBAT) return;

        ec.mineTimer -= dt;
        if (ec.mineTimer > 0.f) return;

        // Only drop while actually moving: a mine laid by a stationary ship
        // lands on top of him and reads as a bug rather than as a trail.
        const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[
            m_em->getEntityIndex(entityId)].bodyId);
        const sf::Vector2f vel(v.x * SCALE, v.y * SCALE);
        const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (speed < config["mine_min_speed"].get_or(60.f)) return;

        if (countMines(entityId) >= config["mine_max_active"].get_or(4)) {
            ec.mineTimer = 1.0f;   // at cap: check again shortly
            return;
        }

        // Behind him, with a little of his own momentum, so it drifts off the
        // exact line he took -- a perfectly spaced trail looks authored.
        const sf::Vector2f back = -vel / std::max(1.f, speed);
        const sf::Vector2f jitter((float)((rand() % 40) - 20), (float)((rand() % 40) - 20));
        dropMine(tf.position + back * 30.f + jitter,
            back * (25.f + rand() % 40) + vel * 0.15f, back, entityId, config);

        ec.mineTimer = config["mine_interval"].get_or(2.6f) + (rand() % 90) / 100.f;
    }

    /// Lay one mine with its release spark. The spark points aft: it reads as
    /// something being ejected, which is what stops a mine appearing out of
    /// nowhere behind a ship the player was already tracking.
    void dropMine(sf::Vector2f pos, sf::Vector2f drift, sf::Vector2f back,
        uint32_t entityId, sol::table& config)
    {
        m_ef->createMine(*m_em, pos, drift, entityId, m_worldId, config);

        const sf::Vector2f side(-back.y, back.x);
        for (int k = 0; k < 7; ++k) {
            const float lat = ((rand() % 200) - 100) / 100.f;
            const float life = 0.18f + (rand() % 16) / 100.f;
            m_em->particles.push_back({ pos,
                back * (30.f + rand() % 70) + side * (lat * 90.f),
                sf::Color(255, 190, 110, 230), life, life, 2.f + rand() % 2 });
        }
    }

    int countMines(uint32_t ownerId) const {
        int n = 0;
        for (const auto& b : m_em->bullets)
            if (b.isMine && !b.mineTossed && b.ownerEntityId == ownerId) ++n;
        return n;
    }

    /**
     * @brief One rocket, out of the nose.
     *
     * @param fast  the single-shot variant: same tracking, far more speed.
     *
     * All rounds leave from the centreline now. The old alternating off-axis
     * launch was there to stop one rocket eating the other's blast, but with a
     * 95px radius that no longer happens, and a volley that fans out of the
     * hull reads as a shotgun rather than as aimed fire.
     */
    void fireRocket(TransformComponent& tf, EnemyComponent& ec, uint32_t entityId,
        sf::Vector2f toPlayerN, sol::table& config, bool fast)
    {
        (void)ec;
        const float jitter = config["rocket_launch_spread"].get_or(7.f)
            * (((rand() % 200) - 100) / 100.f);
        const float r = jitter * 3.14159f / 180.f;
        const sf::Vector2f dir(toPlayerN.x * std::cos(r) - toPlayerN.y * std::sin(r),
            toPlayerN.x * std::sin(r) + toPlayerN.y * std::cos(r));

        const float angle = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        const sf::Vector2f spawn = tf.position + dir * 38.f;

        const float mult = fast ? config["rocket_fast_speed_mult"].get_or(2.1f) : 1.f;
        m_ef->createEnemyRocket(*m_em, spawn, angle, entityId,
            m_playerEntityId, m_worldId, config, mult);

        // ---- Launch smoke ----
        // Thrown BACKWARD out of the tube and spread wide, so the plume hangs
        // where the rocket was rather than chasing it. A fast rocket outruns
        // its own launch cloud, which is most of what sells the speed.
        const sf::Vector2f side(-dir.y, dir.x);
        const int puffs = fast ? 16 : 11;
        for (int k = 0; k < puffs; ++k) {
            const float lat = ((rand() % 200) - 100) / 100.f;
            const float back = 40.f + rand() % 130;
            const float life = 0.30f + (rand() % 40) / 100.f;
            m_em->particles.push_back({
                spawn + side * (lat * 6.f),
                -dir * back + side * (lat * 70.f),
                sf::Color(190, 170, 160, 190), life, life, 4.f + rand() % 5 });
        }
        for (int k = 0; k < 5; ++k) {
            const float life = 0.16f + (rand() % 14) / 100.f;
            m_em->particles.push_back({ spawn,
                -dir * (110.f + rand() % 160), sf::Color(255, 200, 110, 235),
                life, life, 3.f });
        }
        m_em->spawnShockRing(spawn, 3.f, fast ? 40.f : 28.f, 0.16f,
            sf::Color(255, 190, 90), 2.f, 190.f);
    }

    // ========================================================================
    // BASH — the parriable melee lunge
    // ========================================================================
    //
    // Windup -> Lunge -> Recover. The inverse of the ram in every respect the
    // player can see:
    //
    //                 RAM (dodge it)            BASH (parry it)
    //   range         long-mid, lane line       point-blank, crescent at prow
    //   tell colour   amber                     cyan -- the parry's own colour
    //   body          locks, glows white-hot    coils BACK, then snaps forward
    //   parry         whiffs, you eat it        stuns + staggers the Berserker
    //
    // Aim tracks through the windup and locks at lunge start. That is honest:
    // what you see at the last frame of the coil is the lane it strikes down.
    // Stepping out of reach during the lunge makes it whiff -- parry is the
    // reward answer, not the only one.
    //
    // Returns true while it owns the ship this frame.
    bool updateBash(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, sol::table& config, const enemyarch::ArchetypeDef& adef,
        float dist, sf::Vector2f toPlayerN)
    {
        switch (ec.bashState) {

        case BashState::None: {
            if (!config["bash_enabled"].get_or(false)) return false;
            if (ec.bashCooldown > 0.f) return false;
            if (ec.burstBusy()) return false;   // charged burst runs to its end
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.microRecover > 0.f) return false;
            if (ec.ramState != RamState::None) return false;
            if (dist > config["bash_trigger_range"].get_or(150.f)) return false;
            // Own rounds still in the air: wait. A lunge arriving alongside
            // its own bullets asks for a dodge and a parry in the same beat.
            if (ec.shotClearTimer > 0.f) return false;
            {
                const float w = config["bash_windup"].get_or(0.38f);
                if (!squadGrant(w + config["bash_lunge_time"].get_or(0.15f)
                    + config["bash_recover"].get_or(0.35f), true, false, w)) return false;
            }

            ec.bashState = BashState::Windup;
            ec.bashDuration = config["bash_windup"].get_or(0.38f);
            ec.bashTimer = ec.bashDuration;
            ec.bashDir = toPlayerN;
            ec.bashConnected = false;
            ec.bashStrikePending = false;

            // ---- Feint roll (note 29) ----
            // Never on a bash a dive or ram cancel promised, and half as
            // likely straight after a feint, so it stays a mix-up rather
            // than becoming the pattern.
            ec.bashFeint = false;
            if (ec.bashHonest) ec.bashHonest = false;
            else if (config["feint_enabled"].get_or(false)) {
                float c = config["feint_chance"].get_or(0.3f);
                if (ec.lastBashFeinted) c *= 0.5f;
                ec.bashFeint = (rand() % 1000) / 1000.f < c;
            }
            if (!ec.bashFeint) ec.lastBashFeinted = false;

            // Drop anything that would compete for the ship or the read.
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            ai.dodgeBurstTimer = 0.f;
            ai.flinchTimer = 0.f;
            return true;
        }

        case BashState::Windup: {
            ec.bashTimer -= dt;
            const float u = std::clamp(1.f - ec.bashTimer / std::max(0.01f, ec.bashDuration), 0.f, 1.f);

            // ---- FEINT: the windup stops halfway (note 29) ----
            // bashState drops to None this frame, so the cyan crescent is
            // gone on the frame -- RenderSystem only draws it in Windup and
            // Lunge. The hesitation that follows is the honest tell.
            if (ec.bashFeint && u >= config["feint_break"].get_or(0.5f)) {
                ec.bashState = BashState::None;
                ec.bashFeint = false;
                ec.lastBashFeinted = true;
                ec.duelShift = DuelShift::FeintBreak;
                ec.duelShiftDuration = config["feint_hesitate"].get_or(0.16f);
                ec.duelShiftTimer = ec.duelShiftDuration;
                ec.duelTrick = true;       // inside MELEE: keep the mode timer
                for (int k = 0; k < 3; ++k) emitRetro(tf, adef, 1.f);
                return true;
            }

            ec.bashDir = toPlayerN;
            turnToward(tf, bodyId, toPlayerN, config["bash_turn_rate"].get_or(12.f), dt);

            // Coil: bleed off approach speed and ease BACKWARDS. Anticipation
            // is the oldest melee tell there is -- a fist goes back before it
            // goes forward.
            {
                const float coil = config["bash_coil_speed"].get_or(70.f) * std::sin(u * 1.5708f);
                const float k = 1.f - std::exp(-12.f * dt);
                const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
                const b2Vec2 target = { -toPlayerN.x * coil / SCALE, -toPlayerN.y * coil / SCALE };
                b2Body_SetLinearVelocity(bodyId, { v.x + (target.x - v.x) * k,
                                                   v.y + (target.y - v.y) * k });
                b2Body_SetAngularVelocity(bodyId, 0.f);
            }

            // Squash toward the tail: the hull visibly loads up.
            const float e = u * u * (3.f - 2.f * u);
            tf.visualPivot = { 0.f, adef.radius * 0.45f };
            tf.visualScale.y *= 1.f - 0.17f * e;
            tf.visualScale.x *= 1.f + 0.08f * e;

            // Late sparks off the prow, in the tell colour.
            if (u > 0.45f && (rand() % 100) < 40) {
                const float r = tf.rotation * 3.14159f / 180.f;
                const sf::Vector2f fwd(std::sin(r), -std::cos(r));
                const sf::Vector2f rgt(std::cos(r), std::sin(r));
                const float side = ((rand() % 200) - 100) / 100.f;
                const sf::Vector2f at = tf.position + fwd * (adef.radius * 0.85f)
                    + rgt * (side * adef.radius * 0.45f);
                m_em->particles.push_back({ at,
                    fwd * (60.f + rand() % 80) + rgt * (side * 50.f),
                    sf::Color(140, 255, 235, 230),
                    0.18f, 0.20f, 2.f + rand() % 2 });
            }

            if (ec.bashTimer <= 0.f) {
                ec.bashState = BashState::Lunge;
                ec.bashDuration = config["bash_lunge_time"].get_or(0.16f);
                ec.bashTimer = ec.bashDuration;
                ec.bashDir = toPlayerN;                 // LOCKED from here
                lockHeading(tf, bodyId, ec.bashDir);

                const float spd = config["bash_lunge_speed"].get_or(950.f);
                b2Body_SetLinearVelocity(bodyId,
                    { ec.bashDir.x * spd / SCALE, ec.bashDir.y * spd / SCALE });

                m_em->spawnShockRing(tf.position - ec.bashDir * (adef.radius * 0.5f),
                    6.f, 60.f, 0.16f, sf::Color(200, 255, 245), 3.f, 200.f);
            }
            return true;
        }

        case BashState::Lunge: {
            ec.bashTimer -= dt;

            lockHeading(tf, bodyId, ec.bashDir);
            const float spd = config["bash_lunge_speed"].get_or(950.f);
            b2Body_SetLinearVelocity(bodyId,
                { ec.bashDir.x * spd / SCALE, ec.bashDir.y * spd / SCALE });

            tf.visualPivot = { 0.f, adef.radius * 0.45f };
            tf.visualScale.y *= 1.16f;
            tf.visualScale.x *= 0.92f;

            // ---- Strike: once, on reach, inside the lunge arc ----
            const float reach = config["bash_reach"].get_or(90.f);
            const float arcCos = config["bash_arc_cos"].get_or(0.30f);
            const float facing = toPlayerN.x * ec.bashDir.x + toPlayerN.y * ec.bashDir.y;

            if (!ec.bashConnected && dist <= reach && facing >= arcCos) {
                ec.bashConnected = true;
                ec.bashStrikePending = true;     // DamageSystem resolves next frame
                ec.bashTargetId = m_vsShip ? ec.feralTargetId : 0u;

                // Recoil off the impact. Without it the hull keeps pushing
                // into the player and Box2D shoves them around after the hit
                // already threw them -- two knockbacks that disagree.
                const float recoil = config["bash_recoil"].get_or(160.f);
                b2Body_SetLinearVelocity(bodyId,
                    { -ec.bashDir.x * recoil / SCALE, -ec.bashDir.y * recoil / SCALE });

                ec.bashState = BashState::Recover;
                ec.bashDuration = config["bash_recover"].get_or(0.35f);
                ec.bashTimer = ec.bashDuration;
                return true;
            }

            if (ec.bashTimer <= 0.f) {
                // Whiff: longer recovery. Getting out of reach should pay.
                ec.bashState = BashState::Recover;
                ec.bashDuration = config["bash_whiff_recover"].get_or(0.60f);
                ec.bashTimer = ec.bashDuration;
            }
            return true;
        }

        case BashState::Recover: {
            ec.bashTimer -= dt;
            const float u = std::clamp(ec.bashTimer / std::max(0.01f, ec.bashDuration), 0.f, 1.f);

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-5.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });
            b2Body_SetAngularVelocity(bodyId, 0.f);

            tf.visualOffsetAngle += 7.f * u * std::sin(ec.bashTimer * 24.f);
            tf.visualPivot = { 0.f, -18.f };

            if (ec.bashTimer <= 0.f) {
                ec.bashState = BashState::None;
                // DamageSystem may already have set a LONGER cooldown on a
                // connect; never shorten it.
                ec.bashCooldown = std::max(ec.bashCooldown,
                    config["bash_cooldown"].get_or(1.1f) + (rand() % 40) / 100.f);
            }
            return true;
        }
        }
        return false;
    }

    // ========================================================================
    // DUELIST — the Bloodseeker's two modes and the moves between them
    // ========================================================================
    //
    // Mode is a config swap (note 22): MELEE reads the archetype table, RANGE
    // reads `duel_range`. The shifts between them are owned here and return
    // true while they hold the ship, exactly like the ram and the bash.
    //
    //   MELEE --(timer / parried / pressured)--> DISENGAGE --> RANGE
    //   RANGE --(timer / player too far)-------> DIVE WINDUP --> DIVE
    //         DIVE --(bash range)--> bash windup, same frame   --> MELEE
    //         DIVE --(overshoot / out of rush)--> DIVE RECOVER --> MELEE

    /// Bone-white: the Bloodseeker's own colour for every duel effect. Not
    /// cyan (parry), not amber (ram) -- a third voice for a third meaning.
    static sf::Color duelBone(uint8_t a = 230) { return sf::Color(232, 222, 196, a); }

    /// The table a mode fights from. RANGE falls back to the archetype if
    /// `duel_range` is missing, so a typo degrades to "one mode" rather than
    /// to a unit reading defaults for everything.
    static sol::table duelTable(const sol::table& base, DuelMode m) {
        if (m == DuelMode::Range) {
            sol::object rt = base["duel_range"];
            if (rt.valid() && rt.is<sol::table>()) return rt.as<sol::table>();
        }
        return base;
    }

    /// `key = { lo, hi }` rolled uniformly; a missing key uses the fallback.
    static float rollSpan(const sol::table& t, const char* key, float lo, float hi) {
        sol::optional<sol::table> r = t[key];
        if (r) { lo = (*r)[1].get_or(lo); hi = (*r)[2].get_or(hi); }
        if (hi < lo) hi = lo;
        return lo + (hi - lo) * ((rand() % 1000) / 1000.f);
    }

    /// Drop out of any shift into the mode it was heading for, with a fresh
    /// timer. Used when something breaks a dive (stun, stagger via
    /// DamageSystem, a freeze) and at the natural end of every shift.
    void endDuelShift(EnemyComponent& ec, AIState& ai, const sol::table& base) {
        ec.duelShift = DuelShift::None;
        ec.duelShiftTimer = 0.f;
        ec.feintShotsLeft = 0;
        // The mode clock is set when a mode is ENTERED (enterModeClock), so a
        // shift ending -- trick or not -- never resets the roll.
        (void)base;
        ec.duelTrick = false;
        ai.maneuverTimer = 0.f;
    }

    /// A fresh mode: the roll clock restarts and the chance goes back to base.
    static void enterModeClock(EnemyComponent& ec, const sol::table& base) {
        ec.duelModeTimer = base["duel_roll_interval"].get_or(5.f);
        ec.duelSwitchChance = base["duel_switch_chance"].get_or(0.5f);
    }

    bool updateDuel(float dt, TransformComponent& tf, EnemyComponent& ec, HealthComponent& health,
        AIState& ai, b2BodyId bodyId, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f toPlayerN, float dist)
    {
        switch (ec.duelShift) {
        case DuelShift::Disengage:
            return updateDisengage(dt, tf, ec, ai, bodyId, adef, toPlayerN, dist);
        case DuelShift::DiveWindup:
        case DuelShift::Dive:
        case DuelShift::DiveRecover:
            return updateDive(dt, tf, ec, ai, bodyId, adef, toPlayerN, dist);
        case DuelShift::FeintBreak:
        case DuelShift::FeintFallback:
            return updateFeint(dt, tf, ec, ai, bodyId, adef, toPlayerN);
        case DuelShift::ExecApproach:
        case DuelShift::ExecStrike:
            return updateExecution(dt, tf, ec, ai, bodyId, adef);
        case DuelShift::None:
        default:
            break;
        }

        // ---- A RANGE attack in progress owns the ship (notes 27-28) ----
        // Ahead of the COMBAT check and the mode timer: once committed, a
        // cone finishes (or is broken by the player), it is never abandoned
        // because a timer ran out.
        if (ec.duelAttack != DuelAttack::None)
            return updateRangeAttack(dt, tf, ec, bodyId, adef, toPlayerN, dist);

        // ---- Free: tick the mode clock and roll for a switch ----
        (void)health;
        if (ai.currentState != EnemyState::COMBAT) return false;
        // Never cut into a committed attack. The ram already returned before
        // this point while it runs; the bash runs after, so check it here.
        if (ec.bashState != BashState::None || ec.ramState != RamState::None) return false;

        const sol::table& base = adef.config;
        const sol::table mt = duelTable(base, ec.duelMode);

        // ---- Post-stun evade window: the visible half (note 31) ----
        if (ec.duelEvadeTimer > 0.f) evadeKick(tf, ec, bodyId, adef, mt);

        // ---- Execution (note 33): checked before the switch roll ----
        if (base["exec_enabled"].get_or(false) && !ec.burstBusy() && tryStartExecution(dt, tf, ec, base))
            return true;

        // ---- THE ROLL (note 23) ----
        // Every duel_roll_interval: switch with duel_switch_chance. A miss
        // raises the next roll by duel_switch_step; a switch resets it (in
        // startDisengage / startDive, via enterModeClock). Nothing else
        // switches him -- not damage, not a parry, not distance.
        if (ec.duelModeTimer < 0.f) enterModeClock(ec, base);
        ec.duelModeTimer -= dt;
        // A charged burst finishes before the mode can flip under it.
        if (ec.duelModeTimer <= 0.f && !ec.burstBusy()) {
            const bool flip = (rand() % 1000) / 1000.f < ec.duelSwitchChance;
            if (flip) {
                if (ec.duelMode == DuelMode::Melee) startDisengage(tf, ec, ai, adef);
                else startDive(ec, ai, adef, toPlayerN, dist);
                return true;   // next frame runs on the new mode's table
            }
            ec.duelSwitchChance = std::min(1.f,
                ec.duelSwitchChance + base["duel_switch_step"].get_or(0.1f));
            ec.duelModeTimer = base["duel_roll_interval"].get_or(5.f);
        }

        if (ec.duelMode == DuelMode::Melee) return false;

        // ---- The RANGE kit ----
        if (ec.duelLancerCd > 0.f) ec.duelLancerCd -= dt;
        if (ec.duelConeCd > 0.f)   ec.duelConeCd -= dt;

        // One read at a time: never start over a filler shot's windup.
        if (!ec.telegraphActive) {
            if (mt["cone_enabled"].get_or(false) && ec.duelConeCd <= 0.f &&
                dist >= mt["cone_min_range"].get_or(180.f) &&
                dist <= mt["cone_max_range"].get_or(620.f))
            {
                if ((rand() % 1000) / 1000.f < mt["cone_chance"].get_or(0.5f)) {
                    startCone(tf, ec, mt);
                    return true;
                }
                ec.duelConeCd = mt["cone_reroll"].get_or(1.0f);
            }
            if (mt["lancer_enabled"].get_or(false) && ec.duelLancerCd <= 0.f &&
                dist >= mt["lancer_min_range"].get_or(200.f) &&
                dist <= mt["lancer_max_range"].get_or(950.f))
            {
                startLancer(tf, ec, mt, toPlayerN, dist);
                return true;
            }
        }

        // Mode tell, part two (the guttered aft flame is RenderSystem's):
        // retro plumes off the prow. Heavier while he is actually backing off.
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        const float away = -(v.x * SCALE * toPlayerN.x + v.y * SCALE * toPlayerN.y);
        emitRetro(tf, adef, away > 40.f ? 0.45f : 0.14f);
        return false;
    }

    // ---- MELEE -> RANGE --------------------------------------------------
    void startDisengage(TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        const enemyarch::ArchetypeDef& adef)
    {
        const sol::table rt = duelTable(adef.config, DuelMode::Range);

        ec.duelMode = DuelMode::Range;
        ec.duelShift = DuelShift::Disengage;
        ec.duelShiftDuration = rt["disengage_max_time"].get_or(3.0f);
        ec.duelShiftTimer = ec.duelShiftDuration;
        ec.duelEvadeKick = 0.f;
        enterModeClock(ec, adef.config);

        // Whatever the gun was doing is over: the disengage is silent.
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.shotsInBurst = 0;

        const float pref = rt["preferred_range"].get_or(480.f);
        if (pref > 0.f) ai.preferredRange = pref;
        ai.maneuver = Maneuver::FALLBACK;
        ai.maneuverTimer = 0.f;
        ai.dodgeBurstTimer = 0.f;
        ai.flinchTimer = 0.f;

        // RANGE opens with a lancer soon after he settles; the cone never
        // comes straight out of the disengage.
        ec.duelAttack = DuelAttack::None;
        ec.duelLancerCd = 0.6f;
        ec.duelConeCd = rollSpan(rt, "cone_first_delay", 1.5f, 3.f);

        // The announcement: he leaves a ghost of himself where he stood.
        m_em->spawnShockRing(tf.position, adef.radius * 0.5f, adef.radius * 1.9f,
            0.24f, duelBone(), 3.f, 210.f);
        fx::afterimage(*m_em, adef, tf.position, tf.rotation, { 0.f, 0.f }, duelBone(), 0.34f);
    }

    /**
     * @brief Backpedal to range. Owns the ship.
     *
     * Nose stays on the player -- he is flying backwards on his retros, which
     * is the whole mode tell made physical. The gun is silent: one read at a
     * time, and the read here is "he is untouchable, close the gap".
     *
     * The sidestep scan only looks at the PLAYER's ordinary rounds. Reflected
     * (parried) rounds are excluded on purpose: they are the parry's reward,
     * and the spec says parry beats this. DamageSystem applies the same rule.
     */
    bool updateDisengage(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, const enemyarch::ArchetypeDef& adef, sf::Vector2f toPlayerN, float dist)
    {
        const sol::table rt = duelTable(adef.config, DuelMode::Range);
        ec.duelShiftTimer -= dt;

        // ---- Exit: at range AND past the minimum, or out of time ----
        // Playtest: exiting the moment he reached range made the dodge window
        // almost nothing whenever the fight was already at mid range.
        const float elapsed = ec.duelShiftDuration - ec.duelShiftTimer;
        const float want = ai.preferredRange * rt["disengage_exit_fraction"].get_or(0.9f);
        const bool atRange = dist >= want;
        if (ec.duelShiftTimer <= 0.f ||
            (atRange && elapsed >= rt["disengage_min_time"].get_or(1.5f))) {
            endDuelShift(ec, ai, adef.config);
            ai.maneuver = Maneuver::STRAFE;
            return true;
        }

        // ---- Movement: back off while short of range, then hold and weave ----
        // Once at range he stops running away (or the minimum time would carry
        // him half an arena off) and slides sideways instead, still dodging.
        const sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
        const float weave = std::sin(elapsed
            * rt["disengage_weave_hz"].get_or(2.2f) * 6.28318f + ai.jitterPhase);
        sf::Vector2f dir = atRange
            ? perp * (weave >= 0.f ? 1.f : -1.f) + (-toPlayerN) * 0.15f
            : -toPlayerN + perp * (weave * 0.6f);
        const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (dl > 0.01f) dir /= dl;

        const float spd = rt["disengage_speed"].get_or(640.f);
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        const float k = 1.f - std::exp(-7.f * dt);
        b2Body_SetLinearVelocity(bodyId, { v.x + (dir.x * spd / SCALE - v.x) * k,
                                           v.y + (dir.y * spd / SCALE - v.y) * k });

        turnToward(tf, bodyId, toPlayerN, 10.f, dt);
        b2Body_SetAngularVelocity(bodyId, 0.f);

        // ---- The visible half of the perfect dodge ----
        evadeKick(tf, ec, bodyId, adef, rt);

        emitRetro(tf, adef, 0.9f);

        ec.telegraphActive = false;   // silent, every frame
        ec.telegraphTimer = 0.f;
        return true;
    }

    /**
     * @brief The VISIBLE half of a perfect dodge: sidestep a round about to hit.
     *
     * Used by the disengage and by the post-stun evade window. DamageSystem
     * is the guarantee (anything that still touches him passes through); this
     * is what makes it read as a dodge rather than as a hitbox bug. Kicks are
     * spaced by `disengage_dodge_gap` so a stream of fire produces a steady
     * dance, not a single launch off the screen.
     */
    void evadeKick(TransformComponent& tf, EnemyComponent& ec, b2BodyId bodyId,
        const enemyarch::ArchetypeDef& adef, const sol::table& rt)
    {
        if (ec.duelEvadeKick > 0.f) return;   // ticked in tickTimers
        sf::Vector2f side;
        if (!findIncomingRound(tf.position, adef.radius * 1.25f,
            rt["disengage_dodge_horizon"].get_or(0.30f), side)) return;

        const sf::Vector2f before = tf.position;
        const float kick = rt["disengage_dodge_kick"].get_or(460.f);
        const b2Vec2 cv = b2Body_GetLinearVelocity(bodyId);
        b2Body_SetLinearVelocity(bodyId, { cv.x + side.x * kick / SCALE,
                                           cv.y + side.y * kick / SCALE });
        fx::afterimage(*m_em, adef, before, tf.rotation, { 0.f, 0.f }, duelBone(), 0.24f);
        ec.duelEvadeKick = rt["disengage_dodge_gap"].get_or(0.12f);

        // Lean into the sidestep, the way an ordinary dodge banks.
        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f rgt(std::cos(r), std::sin(r));
        const float sgn = side.x * rgt.x + side.y * rgt.y;
        tf.visualOffsetAngle += -sgn * 22.f;
        tf.visualPivot = { 0.f, -18.f };
    }

    /**
     * @brief Is a PLAYER round about to hit this point?
     *
     * Closest approach within `horizon` seconds, inside `reach` px. On a hit,
     * `sideOut` is the way to step: away from the round's line, toward
     * whichever side the ship already sits on, so the sidestep never crosses
     * the path it is avoiding.
     */
    bool findIncomingRound(sf::Vector2f pos, float reach, float horizon, sf::Vector2f& sideOut) const {
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j >= m_em->bullets.size()) break;
            if (!b2Body_IsValid(m_em->physics[j].bodyId)) continue;
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Bullet) continue;

            const auto& b = m_em->bullets[j];
            if (b.isEnemyBullet || b.isReflected || b.isRocket || b.isMine || b.isWild) continue;
            if (b.markedForDestroy) continue;

            const b2Vec2 bv = b2Body_GetLinearVelocity(m_em->physics[j].bodyId);
            const sf::Vector2f vel(bv.x * SCALE, bv.y * SCALE);
            const float v2 = vel.x * vel.x + vel.y * vel.y;
            if (v2 < 1.f) continue;

            const sf::Vector2f rel = pos - m_em->transforms[j].position;
            const float t = (rel.x * vel.x + rel.y * vel.y) / v2;
            if (t < 0.f || t > horizon) continue;

            sf::Vector2f miss = rel - vel * t;     // me, relative to the closest point
            const float ml = std::sqrt(miss.x * miss.x + miss.y * miss.y);
            if (ml > reach) continue;

            if (ml > 0.5f) sideOut = miss / ml;
            else {
                const float vl = std::sqrt(v2);
                sideOut = sf::Vector2f(-vel.y / vl, vel.x / vl) * ((rand() % 2) ? 1.f : -1.f);
            }
            return true;
        }
        return false;
    }

    // ---- RANGE -> MELEE --------------------------------------------------
    void startDive(EnemyComponent& ec, AIState& ai, const enemyarch::ArchetypeDef& adef,
        sf::Vector2f toPlayerN, float dist)
    {
        const sol::table& mt = adef.config;   // the dive belongs to MELEE
        squadGrant(1.6f, true, false, mt["dive_windup"].get_or(0.4f));   // elite: reserves + melee read

        ec.duelMode = DuelMode::Melee;
        enterModeClock(ec, mt);

        // The range gun stops here, and its shot-clear must not hold up the
        // hand-off: the dive replaces the "wait for my rounds" beat with its
        // own, longer, windup.
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.shotsInBurst = 0;
        ec.shotPauseTimer = 0.f;
        ec.shotClearTimer = 0.f;

        const float pref = mt["preferred_range"].get_or(0.f);
        if (pref > 0.f) ai.preferredRange = pref;
        ai.dodgeBurstTimer = 0.f;
        ai.flinchTimer = 0.f;
        ai.maneuver = Maneuver::ATTACK_RUN;
        ai.maneuverTimer = 0.3f;

        // Already inside bash range: no rush to make. The transition IS the
        // bash -- normal trigger, normal windup -- starting next frame.
        if (dist <= mt["bash_trigger_range"].get_or(150.f)) {
            ec.bashCooldown = 0.f;
            ec.bashHonest = true;          // a promised bash never feints
            ec.duelShift = DuelShift::None;
            return;
        }

        ec.duelShift = DuelShift::DiveWindup;
        ec.duelShiftDuration = mt["dive_windup"].get_or(0.38f);
        ec.duelShiftTimer = ec.duelShiftDuration;
        ec.duelDir = toPlayerN;
    }

    bool updateDive(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, const enemyarch::ArchetypeDef& adef, sf::Vector2f toPlayerN, float dist)
    {
        const sol::table& mt = adef.config;
        ec.duelShiftTimer -= dt;

        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f fwd(std::sin(r), -std::cos(r));
        const sf::Vector2f rgt(std::cos(r), std::sin(r));

        switch (ec.duelShift) {

        case DuelShift::DiveWindup: {
            const float u = std::clamp(1.f - ec.duelShiftTimer /
                std::max(0.01f, ec.duelShiftDuration), 0.f, 1.f);

            ec.duelDir = toPlayerN;
            turnToward(tf, bodyId, toPlayerN, mt["dive_windup_turn"].get_or(12.f), dt);
            b2Body_SetAngularVelocity(bodyId, 0.f);

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-6.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });

            // The read: the hull NARROWS (streamlining) where a bash coil
            // squashes WIDE, and the flare is bone-white at the prow, where a
            // bash is cyan. Aft flame guttered by RenderSystem.
            const float e = u * u * (3.f - 2.f * u);
            tf.visualPivot = { 0.f, adef.radius * 0.4f };
            tf.visualScale.x *= 1.f - 0.14f * e;
            tf.visualScale.y *= 1.f + 0.10f * e;

            if ((rand() % 100) < static_cast<int>(35 + 55 * u)) {
                const float side = ((rand() % 200) - 100) / 100.f;
                const float life = 0.14f + (rand() % 10) / 100.f;
                m_em->particles.push_back({
                    tf.position + fwd * (adef.radius * 0.9f) + rgt * (side * adef.radius * 0.18f),
                    fwd * (40.f + rand() % 70) + rgt * (side * 90.f),
                    duelBone(235), life, life, 2.f + rand() % 2 });
            }

            if (ec.duelShiftTimer <= 0.f) {
                ec.duelShift = DuelShift::Dive;
                ec.duelShiftDuration = mt["dive_max_time"].get_or(0.85f);
                ec.duelShiftTimer = ec.duelShiftDuration;
                ec.duelDir = toPlayerN;
                lockHeading(tf, bodyId, ec.duelDir);

                const float spd = mt["dive_speed"].get_or(1350.f);
                b2Body_SetLinearVelocity(bodyId,
                    { ec.duelDir.x * spd / SCALE, ec.duelDir.y * spd / SCALE });
                m_em->spawnShockRing(tf.position - ec.duelDir * (adef.radius * 0.6f),
                    8.f, 90.f, 0.20f, duelBone(), 4.f, 230.f);
            }
            return true;
        }

        case DuelShift::Dive: {
            // ---- Steered, not railed ----
            // Bends toward you at dive_turn rad/s. Enough to punish standing
            // still or drifting; a committed sidestep late in the rush still
            // makes him overshoot.
            {
                const float cur = std::atan2(ec.duelDir.y, ec.duelDir.x);
                const float tgt = std::atan2(toPlayerN.y, toPlayerN.x);
                float d = tgt - cur;
                while (d > 3.14159f) d -= 6.28318f;
                while (d < -3.14159f) d += 6.28318f;
                const float maxTurn = mt["dive_turn"].get_or(2.4f) * dt;
                const float a = cur + std::clamp(d, -maxTurn, maxTurn);
                ec.duelDir = { std::cos(a), std::sin(a) };
            }
            lockHeading(tf, bodyId, ec.duelDir);

            const float spd = mt["dive_speed"].get_or(1350.f);
            b2Body_SetLinearVelocity(bodyId,
                { ec.duelDir.x * spd / SCALE, ec.duelDir.y * spd / SCALE });

            tf.visualPivot = { 0.f, adef.radius * 0.4f };
            tf.visualScale.x *= 0.88f;
            tf.visualScale.y *= 1.10f;

            // Bone streaks off the hull edges: a dive, not a ram (no amber
            // lane, no white-hot hull).
            for (int k = 0; k < 2; ++k) {
                const float side = (k == 0) ? -1.f : 1.f;
                const float life = 0.16f + (rand() % 10) / 100.f;
                m_em->particles.push_back({
                    tf.position + rgt * (side * adef.radius * 0.45f),
                    -ec.duelDir * (160.f + rand() % 140) + rgt * (side * 30.f),
                    duelBone(210), life, life, 2.f + rand() % 2 });
            }

            const float facing = ec.duelDir.x * toPlayerN.x + ec.duelDir.y * toPlayerN.y;
            const float reach = mt["bash_trigger_range"].get_or(150.f);

            // ---- Hand-off: the bash takes over THIS frame ----
            // Returning false falls through to updateBash, which opens its
            // windup at full length. Clearing the gates here is what makes
            // that certain rather than likely.
            if (dist <= reach && facing > 0.3f) {
                endDuelShift(ec, ai, mt);
                ec.bashCooldown = 0.f;
                ec.shotClearTimer = 0.f;
                ec.microRecover = 0.f;
                ec.bashHonest = true;      // the dive promised this bash: no feint
                return false;
            }

            // ---- Overshoot, or out of rush: the punish window ----
            if ((facing < -0.2f && dist > reach) || ec.duelShiftTimer <= 0.f) {
                ec.duelShift = DuelShift::DiveRecover;
                ec.duelShiftDuration = mt["dive_recover"].get_or(0.6f);
                ec.duelShiftTimer = ec.duelShiftDuration;
            }
            return true;
        }

        case DuelShift::DiveRecover: {
            const float u = std::clamp(ec.duelShiftTimer /
                std::max(0.01f, ec.duelShiftDuration), 0.f, 1.f);

            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float brake = std::exp(-4.5f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });
            b2Body_SetAngularVelocity(bodyId, 0.f);

            // Same language as a bash whiff: a sway that dies down.
            tf.visualOffsetAngle += 8.f * u * std::sin(ec.duelShiftTimer * 22.f);
            tf.visualPivot = { 0.f, -18.f };

            if (ec.duelShiftTimer <= 0.f) {
                endDuelShift(ec, ai, mt);
                ai.maneuver = Maneuver::ATTACK_RUN;
                ai.maneuverTimer = 0.3f;
            }
            return true;
        }

        default:
            return false;
        }
    }

    // ========================================================================
    // MELEE TRICKS — feint and ram cancel (notes 29-30)
    // ========================================================================

    /**
     * @brief After a feint breaks: the hesitation, then the backstep spray.
     *
     * Both beats keep the nose on the player, so the read is consistent with
     * the disengage: "backing off on retros". The spray is three rounds, no
     * telegraph beyond the hesitation itself -- it is there to punish a parry
     * thrown early (the whiff recovery is ~0.5s), not to out-damage anything.
     */
    bool updateFeint(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, const enemyarch::ArchetypeDef& adef, sf::Vector2f toPlayerN)
    {
        const sol::table& mt = adef.config;
        ec.duelShiftTimer -= dt;
        turnToward(tf, bodyId, toPlayerN, 12.f, dt);
        b2Body_SetAngularVelocity(bodyId, 0.f);

        if (ec.duelShift == DuelShift::FeintBreak) {
            // Dead stop. Whatever the coil was doing is over.
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = std::exp(-14.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
            emitRetro(tf, adef, 1.f);
            tf.visualOffsetAngle += 3.f * std::sin(ec.duelShiftTimer * 60.f);

            if (ec.duelShiftTimer <= 0.f) {
                ec.duelShift = DuelShift::FeintFallback;
                ec.duelShiftDuration = mt["feint_fallback_time"].get_or(0.6f);
                ec.duelShiftTimer = ec.duelShiftDuration;
                ec.feintShotsLeft = std::max(0, mt["feint_spray_count"].get_or(3));
                ec.feintShotTimer = 0.04f;   // first round almost at once
            }
            return true;
        }

        // ---- FeintFallback ----
        const float spd = mt["feint_fallback_speed"].get_or(520.f);
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        const float k = 1.f - std::exp(-8.f * dt);
        b2Body_SetLinearVelocity(bodyId, { v.x + (-toPlayerN.x * spd / SCALE - v.x) * k,
                                           v.y + (-toPlayerN.y * spd / SCALE - v.y) * k });
        emitRetro(tf, adef, 0.6f);

        ec.feintShotTimer -= dt;
        if (ec.feintShotsLeft > 0 && ec.feintShotTimer <= 0.f) {
            const float spread = mt["feint_spray_spread"].get_or(6.f) * 3.14159f / 180.f
                * (((rand() % 2001) - 1000) / 1000.f);
            const sf::Vector2f dir(toPlayerN.x * std::cos(spread) - toPlayerN.y * std::sin(spread),
                toPlayerN.x * std::sin(spread) + toPlayerN.y * std::cos(spread));
            const sf::Vector2f muzzle = tf.position + dir * (adef.radius * 0.9f);
            fireDuelRound(tf.entityId, muzzle, dir,
                mt["bullet_speed"].get_or(660.f), mt["feint_spray_damage"].get_or(6.f),
                mt["bullet_iframes"].get_or(0.15f), mt["bullet_lifetime"].get_or(1.0f),
                false, mt);
            m_em->spawnImpact(muzzle, sf::Color(255, 140, 60), dir * 160.f);
            --ec.feintShotsLeft;
            ec.feintShotTimer = mt["feint_spray_interval"].get_or(0.11f);
        }

        if (ec.duelShiftTimer <= 0.f) {
            endDuelShift(ec, ai, mt);    // duelTrick set: the mode timer is kept
            // No bash straight over his own rounds, and the feint costs the
            // bash its cooldown like a whiff would.
            ec.bashCooldown = std::max(ec.bashCooldown, mt["bash_cooldown"].get_or(1.1f));
            ec.shotClearTimer = mt["melee_shot_clear"].get_or(0.f);
            ai.maneuver = Maneuver::ATTACK_RUN;
            ai.maneuverTimer = 0.25f;
        }
        return true;
    }

    /**
     * @brief Stop a ram link on the frame and turn it into the Blood Dive.
     *
     * Everything that says "ram" goes at once: the state (so DamageSystem's
     * invulnerability ends too), the chain, the glow. What follows is the
     * dive's own read -- a short brake with a bone prow flare, the rush, and
     * an honest bash at the end -- so the player is never asked to learn a
     * third melee tell.
     */
    void cancelRamIntoRush(TransformComponent& tf, EnemyComponent& ec, b2BodyId bodyId,
        const sol::table& config, sf::Vector2f toPlayerN, float dist)
    {
        ec.ramState = RamState::None;
        ec.ramChainLeft = 0;
        ec.ramGlow = 0.f;
        ec.ramCancelAt = -1.f;
        ec.ramCooldown = config["ram_cooldown"].get_or(5.f);

        // The snap: most of the speed bled off at once, sparks off the prow.
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        b2Body_SetLinearVelocity(bodyId, { v.x * 0.3f, v.y * 0.3f });
        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f fwd(std::sin(r), -std::cos(r));
        for (int k = 0; k < 10; ++k) {
            const float a = ((rand() % 120) - 60) * 3.14159f / 180.f;
            const sf::Vector2f d(fwd.x * std::cos(a) - fwd.y * std::sin(a),
                fwd.x * std::sin(a) + fwd.y * std::cos(a));
            const float life = 0.16f + (rand() % 10) / 100.f;
            m_em->particles.push_back({ tf.position + fwd * 30.f, d * (120.f + rand() % 160),
                duelBone(235), life, life, 2.f + rand() % 2 });
        }

        // Already in reach: no rush to make, the honest bash just starts.
        if (dist <= config["bash_trigger_range"].get_or(150.f)) {
            ec.bashCooldown = 0.f;
            ec.shotClearTimer = 0.f;
            ec.microRecover = 0.f;
            ec.bashHonest = true;
            return;
        }

        ec.duelTrick = true;   // inside MELEE: keep the mode timer
        ec.duelShift = DuelShift::DiveWindup;
        ec.duelShiftDuration = config["ram_cancel_brake"].get_or(0.14f);
        ec.duelShiftTimer = ec.duelShiftDuration;
        ec.duelDir = toPlayerN;
    }

    // ========================================================================
    // SQUAD -- attack turns, stalking, flanking, pack sight, chase (notes 42-47)
    // ========================================================================
    //
    // 3.2 (playtest): the 3.1 threat budget made a pack too polite -- three
    // Berserkers hovered for seconds doing nothing, open, then came in one at
    // a time. It also gated units that were never the problem. Now:
    //
    //   ATTACK TURNS -- only for the heavy hitters (`squad_role = "assault"`,
    //   the Berserker; and the Maniac when his mood says so). At most 1 or 2
    //   hold a TURN at once -- the cap is re-rolled every time a turn ends
    //   (`two_turn_chance`), so the rhythm never settles. A turn lasts
    //   turn_min..turn_max s and the holder uses its WHOLE kit at its normal
    //   pace (bash, ram chain, charged burst). It is never cut mid-attack.
    //   Then it rests (`turn_rest`) and the next one is picked: weighted by
    //   how long each has waited, but random -- not a queue you can count.
    //   Alone, a Berserker simply always has the turn.
    //
    //   WAITING IS NOT HOVERING. A heavy without a turn STALKS: it circles
    //   just outside its strike range (`stalk_ring`) on its flank bearing,
    //   jinking, dodging as normal -- a threat in your peripheral vision,
    //   not a target parked in front of you.
    //
    //   EVERYONE ELSE IS FREE. Wardogs (small harassers), Raiders (support),
    //   the Barge and the Bloodseeker (elites) are never gated -- the cheap
    //   pressure stays constant; the big swings take turns.
    //
    //   THE MANIAC (`wild`) rolls a MOOD every mood_min..mood_max s:
    //     assault  presses in close and takes turns like a Berserker
    //     support  hangs back at range on rockets and mines, never waits
    //     berserk  tears straight into the middle, never waits
    //   so he drifts between the roles mid-fight -- the one you cannot read.
    //
    //   FLANKING -- non-elites fan round the player from the pack's mean
    //   bearing, in standing order (as 3.1).
    //
    //   PACK SIGHT + CHASE -- if any member of the fight can see you, every
    //   member within share_sight_radius keeps fresh memory of you: nobody
    //   de-aggros because YOU left HIS cone while his packmate is staring at
    //   you. ALERT units within call_radius of one who sees you join in. A
    //   unit in COMBAT that falls behind (past chase_distance, or 1.6x its
    //   preferred range) burns to catch up: up to chase_mult speed and thrust.
    //
    //   BREATHER -- you go into a tumble: no NEW turn starts for
    //   breather_time, and turn holders cannot start a new attack.
    //
    // Feral units and units fighting another ship are outside all of it.

    enum class SquadRole : uint8_t { None, Support, Harasser, Assault, Elite, Wild };
    enum class ManiacMood : uint8_t { Assault, Support, Berserk };

    struct SquadCfg {
        bool  enabled = true;
        float squadRadius = 1800.f;
        float twoTurnChance = 0.6f;
        float turnMin = 3.0f, turnMax = 5.0f, turnRest = 1.0f;
        float stalkRing = 380.f;
        float stalkPush = 6.f;
        float flankSpacing = 55.f;      // deg
        float flankMaxArc = 240.f;      // deg
        float flankStrength = 4.f;
        float breatherTime = 1.3f;
        float retryDelay = 0.25f;
        float shareSightRadius = 2400.f;
        float callRadius = 900.f;
        float chaseDistance = 750.f;
        float chaseMult = 1.5f;
        float moodMin = 3.5f, moodMax = 8.f;
        float moodAssault = 0.40f, moodSupport = 0.35f, moodBerserk = 0.25f;
        float rangeAssault = 240.f, rangeSupport = 540.f, rangeBerserk = 120.f;
    };

    struct SquadSlot { float bearing = 0.f; bool hasSlot = false; };
    struct SquadMem { uint32_t id; float dist; float bearing; };
    struct TurnInfo { float left = 0.f; };
    struct MoodInfo { ManiacMood mood = ManiacMood::Assault; float timer = 0.f; };

    static SquadRole roleFrom(const sol::table& c) {
        const std::string r = c["squad_role"].get_or<std::string>("");
        if (r == "support")  return SquadRole::Support;
        if (r == "harasser") return SquadRole::Harasser;
        if (r == "assault")  return SquadRole::Assault;
        if (r == "elite")    return SquadRole::Elite;
        if (r == "wild")     return SquadRole::Wild;
        return SquadRole::None;
    }

    static float wrapPi(float a) {
        while (a > 3.14159265f)  a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        return a;
    }

    static float frand() { return static_cast<float>(rand()) / static_cast<float>(RAND_MAX); }

    /// A role as it acts right now: a Maniac's mood picks his.
    SquadRole effectiveRole(uint32_t id, SquadRole base) const {
        if (base != SquadRole::Wild) return base;
        auto it = m_sqMood.find(id);
        const ManiacMood m = (it != m_sqMood.end()) ? it->second.mood : ManiacMood::Assault;
        return m == ManiacMood::Assault ? SquadRole::Assault : SquadRole::Wild;
    }

    /// Mid-attack: never end a turn under it.
    static bool unitBusy(const EnemyComponent& e) {
        return e.bashState != BashState::None || e.ramState != RamState::None || e.burstBusy()
            || e.telegraphActive || e.mineRunState != MineRunState::None || e.tossState != 0
            || e.rocketsLeft != 0 || e.stormActive;
    }

    void readSquadCfg() {
        m_sqCfg = SquadCfg{};
        if (!m_lua) return;
        sol::optional<sol::table> t = (*m_lua)["squad_ai"];
        if (!t) return;
        auto& c = m_sqCfg; auto& s = *t;
        c.enabled = s["enabled"].get_or(true);
        c.squadRadius = s["squad_radius"].get_or(c.squadRadius);
        c.twoTurnChance = s["two_turn_chance"].get_or(c.twoTurnChance);
        c.turnMin = s["turn_min"].get_or(c.turnMin);
        c.turnMax = s["turn_max"].get_or(c.turnMax);
        c.turnRest = s["turn_rest"].get_or(c.turnRest);
        c.stalkRing = s["stalk_ring"].get_or(c.stalkRing);
        c.stalkPush = s["stalk_push"].get_or(c.stalkPush);
        c.flankSpacing = s["flank_spacing"].get_or(c.flankSpacing);
        c.flankMaxArc = s["flank_max_arc"].get_or(c.flankMaxArc);
        c.flankStrength = s["flank_strength"].get_or(c.flankStrength);
        c.breatherTime = s["breather_time"].get_or(c.breatherTime);
        c.retryDelay = s["retry_delay"].get_or(c.retryDelay);
        c.shareSightRadius = s["share_sight_radius"].get_or(c.shareSightRadius);
        c.callRadius = s["call_radius"].get_or(c.callRadius);
        c.chaseDistance = s["chase_distance"].get_or(c.chaseDistance);
        c.chaseMult = s["chase_mult"].get_or(c.chaseMult);
        c.moodMin = s["mood_min"].get_or(c.moodMin);
        c.moodMax = s["mood_max"].get_or(c.moodMax);
        c.moodAssault = s["mood_assault"].get_or(c.moodAssault);
        c.moodSupport = s["mood_support"].get_or(c.moodSupport);
        c.moodBerserk = s["mood_berserk"].get_or(c.moodBerserk);
        c.rangeAssault = s["mood_range_assault"].get_or(c.rangeAssault);
        c.rangeSupport = s["mood_range_support"].get_or(c.rangeSupport);
        c.rangeBerserk = s["mood_range_berserk"].get_or(c.rangeBerserk);
    }

    /// Once a frame, before any unit: moods, turns, flank slots, pack sight.
    void updateSquad(float dt, size_t playerIdx, sf::Vector2f playerPos) {
        readSquadCfg();
        if (m_sqBreather > 0.f) m_sqBreather -= dt;

        // ---- Breather: the frame the player starts tumbling ----
        const bool tumbling = m_em->players[playerIdx].staggerTimer > 0.f;
        if (tumbling && !m_sqPlayerTumbling) m_sqBreather = m_sqCfg.breatherTime;
        m_sqPlayerTumbling = tumbling;

        m_sqSlots.clear();
        m_sqSeers.clear();
        if (!m_sqCfg.enabled) { m_sqTurns.clear(); return; }

        // Bookkeeping for the dead never grows without bound.
        if (m_sqWaited.size() + m_sqRest.size() + m_sqMood.size() > 192) {
            const auto gone = [&](uint32_t id) { return m_em->getEntityIndex(id) == (size_t)-1; };
            for (auto it = m_sqWaited.begin(); it != m_sqWaited.end(); ) it = gone(it->first) ? m_sqWaited.erase(it) : std::next(it);
            for (auto it = m_sqRest.begin(); it != m_sqRest.end(); ) it = gone(it->first) ? m_sqRest.erase(it) : std::next(it);
            for (auto it = m_sqMood.begin(); it != m_sqMood.end(); ) it = gone(it->first) ? m_sqMood.erase(it) : std::next(it);
        }

        // ---- Gather the fight ----
        std::vector<SquadMem>& mem = m_sqScratch;
        mem.clear();
        std::vector<uint32_t>& heavies = m_sqHeavyScratch;
        heavies.clear();
        m_sqHeavyDist.clear();
        const float R2 = m_sqCfg.squadRadius * m_sqCfg.squadRadius;
        for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            const auto& ej = m_em->enemies[j];
            if (!ej.powered() || ej.feralTimer > 0.f || m_em->healths[j].currentHp <= 0.f) continue;
            const sol::table& cj = m_registry->resolve(ej.archetype).config;
            const SquadRole base = roleFrom(cj);
            if (base == SquadRole::None) continue;
            const uint32_t id = m_em->transforms[j].entityId;
            auto it = m_aiCache.find(id);
            if (it == m_aiCache.end()) continue;
            const AIState& aj = it->second;
            const sf::Vector2f d = m_em->transforms[j].position - playerPos;
            const float d2 = d.x * d.x + d.y * d.y;

            // Pack sight: anyone in the fight who has eyes on you right now.
            if (aj.currentState == EnemyState::COMBAT && aj.timeSinceSeen < 0.25f)
                m_sqSeers.push_back(m_em->transforms[j].position);

            if (aj.currentState != EnemyState::COMBAT || d2 > R2) continue;

            // ---- The Maniac's mood ----
            if (base == SquadRole::Wild) {
                MoodInfo& mi = m_sqMood[id];
                mi.timer -= dt;
                if (mi.timer <= 0.f) {
                    const float tot = m_sqCfg.moodAssault + m_sqCfg.moodSupport + m_sqCfg.moodBerserk;
                    const float r = frand() * std::max(0.001f, tot);
                    mi.mood = r < m_sqCfg.moodAssault ? ManiacMood::Assault
                        : (r < m_sqCfg.moodAssault + m_sqCfg.moodSupport ? ManiacMood::Support : ManiacMood::Berserk);
                    mi.timer = m_sqCfg.moodMin + frand() * std::max(0.f, m_sqCfg.moodMax - m_sqCfg.moodMin);
                }
            }

            const SquadRole role = effectiveRole(id, base);
            if (role == SquadRole::Assault) { heavies.push_back(id); m_sqHeavyDist[id] = std::sqrt(d2); }
            if (role != SquadRole::Elite) mem.push_back({ id, std::sqrt(d2), std::atan2(d.y, d.x) });
        }

        // ---- Turns ----
        // Tick and end the live ones; an absent holder (dead, left, calmed
        // down, mood changed) just loses it.
        int live = 0;
        for (auto it = m_sqTurns.begin(); it != m_sqTurns.end(); ) {
            const bool present = std::find(heavies.begin(), heavies.end(), it->first) != heavies.end();
            if (!present) { it = m_sqTurns.erase(it); m_sqCap = rollCap(); continue; }
            it->second.left -= dt;
            const size_t idx = m_em->getEntityIndex(it->first);
            const bool busy = idx != (size_t)-1 && idx < m_em->enemies.size() && unitBusy(m_em->enemies[idx]);
            if (it->second.left <= 0.f && !busy) {
                m_sqRest[it->first] = m_sqCfg.turnRest;
                it = m_sqTurns.erase(it);
                m_sqCap = rollCap();
                continue;
            }
            ++live;
            ++it;
        }
        for (auto& r : m_sqRest) r.second -= dt;
        for (uint32_t id : heavies)
            if (!m_sqTurns.count(id)) m_sqWaited[id] += dt;

        // Hand out free turns: weighted by time waited, still a dice roll.
        if (m_sqBreather <= 0.f) {
            while (live < m_sqCap) {
                float tot = 0.f;
                for (uint32_t id : heavies) {
                    if (m_sqTurns.count(id)) continue;
                    auto rr = m_sqRest.find(id);
                    if (rr != m_sqRest.end() && rr->second > 0.f && heavies.size() > 1) continue;
                    tot += turnWeight(id);
                }
                if (tot <= 0.f) break;
                float roll = frand() * tot;
                uint32_t pick = 0;
                for (uint32_t id : heavies) {
                    if (m_sqTurns.count(id)) continue;
                    auto rr = m_sqRest.find(id);
                    if (rr != m_sqRest.end() && rr->second > 0.f && heavies.size() > 1) continue;
                    roll -= turnWeight(id);
                    pick = id;
                    if (roll <= 0.f) break;
                }
                if (!pick) break;
                m_sqTurns[pick].left = m_sqCfg.turnMin + frand() * std::max(0.f, m_sqCfg.turnMax - m_sqCfg.turnMin);
                m_sqWaited[pick] = 0.f;
                ++live;
            }
        }

        // ---- Flank fan ----
        if (mem.empty()) return;
        float sx = 0.f, sy = 0.f;
        for (const auto& m : mem) { sx += std::cos(m.bearing); sy += std::sin(m.bearing); }
        const float mean = std::atan2(sy, sx);
        std::sort(mem.begin(), mem.end(), [mean](const SquadMem& a, const SquadMem& b) {
            return wrapPi(a.bearing - mean) < wrapPi(b.bearing - mean); });
        const int n = static_cast<int>(mem.size());
        const float deg = 3.14159265f / 180.f;
        const float spacing = (n > 1)
            ? std::min(m_sqCfg.flankSpacing * deg, m_sqCfg.flankMaxArc * deg / (n - 1)) : 0.f;
        for (int k = 0; k < n; ++k) {
            auto& s = m_sqSlots[mem[k].id];
            s.bearing = mean + (k - (n - 1) * 0.5f) * spacing;
            s.hasSlot = true;
        }
    }

    int rollCap() const { return frand() < m_sqCfg.twoTurnChance ? 2 : 1; }

    /// Who gets the next turn: the longer it waited the likelier, and one
    /// that is actually close enough to use it beats one still chasing.
    float turnWeight(uint32_t id) {
        const float w = m_sqWaited[id] + 0.5f;
        auto d = m_sqHeavyDist.find(id);
        const float near = (d != m_sqHeavyDist.end() && d->second > 800.f) ? 0.3f : 1.f;
        return w * w * near;
    }

    /// Per unit, at the top of its update: its role, turn and slot.
    void beginSquadUnit(uint32_t id, const sol::table& cfg, bool feral) {
        const SquadRole base = roleFrom(cfg);
        m_sqRole = effectiveRole(id, base);
        m_sqBaseRole = base;
        m_sqOn = m_sqCfg.enabled && !feral && !m_vsShip && base != SquadRole::None;
        m_sqSelf = id;
        m_sqHasTurn = m_sqTurns.count(id) > 0;
        auto it = m_sqSlots.find(id);
        m_sqSlot = (it != m_sqSlots.end()) ? it->second : SquadSlot{};
    }

    /// The Maniac's preferred range for his current mood.
    float maniacMoodRange(uint32_t id) const {
        auto it = m_sqMood.find(id);
        if (it == m_sqMood.end()) return 0.f;
        switch (it->second.mood) {
        case ManiacMood::Support: return m_sqCfg.rangeSupport;
        case ManiacMood::Berserk: return m_sqCfg.rangeBerserk;
        default:                  return m_sqCfg.rangeAssault;
        }
    }

    /**
     * @brief May this unit start an attack now?
     * Only an ASSAULT unit (Berserker, or a Maniac in his assault mood) is
     * ever refused: it needs a turn, and no breather running. The arguments
     * are kept from the 3.1 budget so every call site stays as it was.
     */
    bool squadGrant(float hold = 0.f, bool melee = false, bool telegraph = false, float windup = 0.f) {
        (void)hold; (void)melee; (void)telegraph; (void)windup;
        if (!m_sqOn || m_sqRole != SquadRole::Assault) return true;
        if (m_sqBreather > 0.f) return false;
        return m_sqHasTurn;
    }

    /**
     * @brief Where a unit stands while it is not attacking.
     * Everyone with a slot drifts toward its flank bearing. A heavy WITHOUT
     * a turn stalks: no closing inside stalk_ring, pushed back out to it, and
     * circling (the flank drift plus its own strafe) -- a moving threat at
     * the edge of its reach, never a parked target.
     */
    sf::Vector2f squadSteer(sf::Vector2f enemyPos, sf::Vector2f playerPos, float maxSpeed,
        sf::Vector2f desired) const
    {
        if (!m_sqOn || !m_sqSlot.hasSlot) return desired;
        const sf::Vector2f rel = enemyPos - playerPos;
        const float r = std::sqrt(rel.x * rel.x + rel.y * rel.y);
        if (r < 1.f) return desired;
        const sf::Vector2f out = rel / r;
        const sf::Vector2f tan(-out.y, out.x);   // direction of increasing bearing
        const float err = wrapPi(m_sqSlot.bearing - std::atan2(rel.y, rel.x));
        sf::Vector2f v = desired + tan * (std::clamp(err / 0.8f, -1.f, 1.f) * maxSpeed * m_sqCfg.flankStrength);
        if (m_sqRole == SquadRole::Assault && !m_sqHasTurn) {
            const float ring = m_sqCfg.stalkRing;
            if (r < ring * 1.15f) {
                const float inward = -(v.x * out.x + v.y * out.y);
                if (inward > 0.f) v += out * inward;             // no closing in
            }
            if (r < ring)
                v += out * (maxSpeed * m_sqCfg.stalkPush * std::clamp((ring - r) / 150.f, 0.f, 1.f));
            // Circle: never sit still on the ring.
            const float side = (std::fmod(static_cast<float>(m_sqSelf) * 0.618f, 1.f) < 0.5f) ? 1.f : -1.f;
            v += tan * (side * maxSpeed * 3.f);
        }
        return v;
    }

    /// Is any member of the fight looking at the player, near `pos`?
    bool packSeesNear(sf::Vector2f pos, float radius) const {
        for (const auto& s : m_sqSeers) {
            const sf::Vector2f d = s - pos;
            if (d.x * d.x + d.y * d.y <= radius * radius) return true;
        }
        return false;
    }

    // ========================================================================
    // PACK — aura, execution, death-chaos targeting, escort (notes 32-35)
    // ========================================================================

    static sf::Color packGold(uint8_t a = 235) { return sf::Color(214, 172, 92, a); }

    /// Every live, powered unit with `aura_radius` > 0 or `escort_radius` > 0,
    /// gathered once a frame so the per-unit test is a short loop rather than
    /// a Lua read per pair. An escort-only source (the Barge) buffs nobody:
    /// its radius is 0, it only gives the units it `escort_takes` a leader.
    void collectPackSources() {
        m_packSources.clear();
        for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            const auto& ej = m_em->enemies[j];
            if (!ej.powered() || m_em->healths[j].currentHp <= 0.f) continue;
            const sol::table& cfg = m_registry->resolve(ej.archetype).config;
            const float r = cfg["aura_radius"].get_or(0.f);
            const float er = cfg["escort_radius"].get_or(0.f);
            if (r <= 0.f && er <= 0.f) continue;
            PackSource ps{ m_em->transforms[j].entityId, m_em->transforms[j].position,
                std::max(0.f, r), cfg["aura_bonus"].get_or(0.1f), er,
                cfg["escort_leash"].get_or(0.f), { 0, 0, 0, 0 }, 0 };
            if (sol::optional<sol::table> tk = cfg["escort_takes"]) {
                for (size_t k = 1; k <= tk->size() && ps.takeCount < 4; ++k) {
                    const uint8_t id = m_registry->idOf((*tk)[k].get_or<std::string>(""));
                    if (id != enemyarch::INVALID_ARCHETYPE) ps.takes[ps.takeCount++] = id;
                }
            }
            m_packSources.push_back(ps);
        }
    }

    /**
     * @brief packMult / packTier for one unit, and its escort anchor.
     *
     * Feral beats execution beats aura; none of them stack. Maniacs
     * (`pack_immune`) and the sources themselves are never buffed.
     */
    void updatePack(EnemyComponent& ec, const sol::table& base, sf::Vector2f pos,
        uint32_t selfId, bool feral)
    {
        float bonus = 0.f;
        uint8_t tier = 0;
        m_hasEscort = false;
        m_escortLeash = 0.f;

        const bool immune = base["pack_immune"].get_or(false)
            || base["aura_radius"].get_or(0.f) > 0.f;
        // Leaders never follow anyone: a Barge near a Bloodseeker holds its own line.
        const bool leader = base["escort_radius"].get_or(0.f) > 0.f;
        if (!immune) {
            float bestEscort = 1e18f;
            for (const auto& src : m_packSources) {
                if (src.id == selfId) continue;
                const sf::Vector2f d = src.pos - pos;
                const float d2 = d.x * d.x + d.y * d.y;
                if (d2 < src.radius * src.radius && src.bonus > bonus) { bonus = src.bonus; tier = 1; }
                if (leader || d2 >= src.escort * src.escort || d2 >= bestEscort) continue;
                bool takes = src.takeCount == 0;
                for (uint8_t k = 0; k < src.takeCount; ++k) if (src.takes[k] == ec.archetype) takes = true;
                if (!takes) continue;
                bestEscort = d2;
                m_hasEscort = true;
                m_escortPos = src.pos;
                m_escortLeash = src.leash;
            }
            if (ec.execBuffTimer > 0.f && ec.execBonus >= bonus) { bonus = ec.execBonus; tier = 2; }
        }
        if (feral) { bonus = ec.feralFrenzy; tier = 3; m_hasEscort = false; }
        if (!ec.powered()) { bonus = 0.f; tier = 0; m_hasEscort = false; }

        ec.packMult = 1.f + bonus;
        ec.packTier = tier;
    }

    /**
     * @brief A feral unit's target: the nearest ship, the player included.
     *
     * Re-picked every `feralRetarget` (set from the Bloodseeker's
     * death_chaos_retarget), or at once if the target died. Writes the
     * target's position and velocity into tgtPos / tgtVel.
     * @return true when the target is another SHIP (melee goes off).
     */
    bool feralTarget(float dt, size_t self, EnemyComponent& ec, sf::Vector2f pos,
        sf::Vector2f playerPos, sf::Vector2f playerVel, sf::Vector2f& tgtPos, sf::Vector2f& tgtVel)
    {
        ec.feralRetarget -= dt;
        size_t ti = (ec.feralTargetId != 0) ? m_em->getEntityIndex(ec.feralTargetId) : (size_t)-1;
        const bool lost = ec.feralTargetId != 0 &&
            (ti == (size_t)-1 || m_em->healths[ti].currentHp <= 0.f || !m_em->enemies[ti].powered());

        if (ec.feralRetarget <= 0.f || lost) {
            ec.feralRetarget = std::max(0.2f, ec.feralRetargetTime);
            const sf::Vector2f dp = playerPos - pos;
            float best = dp.x * dp.x + dp.y * dp.y;
            ec.feralTargetId = 0;
            for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
                if (j == self) continue;
                BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
                if (!ud || ud->type != BodyType::Enemy) continue;
                if (!m_em->enemies[j].powered() || m_em->healths[j].currentHp <= 0.f) continue;
                const sf::Vector2f d = m_em->transforms[j].position - pos;
                const float d2 = d.x * d.x + d.y * d.y;
                if (d2 < best) { best = d2; ec.feralTargetId = m_em->transforms[j].entityId; }
            }
            ti = (ec.feralTargetId != 0) ? m_em->getEntityIndex(ec.feralTargetId) : (size_t)-1;
        }

        if (ec.feralTargetId == 0 || ti == (size_t)-1) {
            tgtPos = playerPos; tgtVel = playerVel;
            return false;
        }
        tgtPos = m_em->transforms[ti].position;
        const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[ti].bodyId);
        tgtVel = { v.x * SCALE, v.y * SCALE };
        return true;
    }

    /**
     * @brief Find the ally a Bloodseeker would execute, or (size_t)-1.
     * Lowest `execute_rank` first, then lowest HP fraction. Wardogs, Maniacs
     * and Barges carry no rank and are never picked.
     */
    size_t findExecTarget(size_t self, sf::Vector2f pos, const sol::table& base) const {
        const float range = base["exec_range"].get_or(550.f);
        const float hpFrac = base["exec_hp_fraction"].get_or(0.3f);
        size_t best = (size_t)-1;
        int bestRank = 1 << 20;
        float bestFrac = 2.f;
        for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
            if (j == self) continue;
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            const auto& ej = m_em->enemies[j];
            if (!ej.powered() || ej.executed) continue;
            const auto& hj = m_em->healths[j];
            if (hj.currentHp <= 0.f) continue;
            const int rank = m_registry->resolve(ej.archetype).config["execute_rank"].get_or(0);
            if (rank <= 0) continue;
            const float frac = hj.currentHp / std::max(1.f, hj.maxHp);
            if (frac > hpFrac) continue;
            const sf::Vector2f d = m_em->transforms[j].position - pos;
            if (d.x * d.x + d.y * d.y > range * range) continue;
            if (rank < bestRank || (rank == bestRank && frac < bestFrac)) {
                best = j; bestRank = rank; bestFrac = frac;
            }
        }
        return best;
    }

    bool tryStartExecution(float dt, TransformComponent& tf, EnemyComponent& ec, const sol::table& base) {
        if (ec.execCd <= -999.f) ec.execCd = base["exec_first_delay"].get_or(6.f);
        if (ec.execCd > 0.f) { ec.execCd -= dt; return false; }
        ec.execRoll -= dt;
        if (ec.execRoll > 0.f) return false;
        ec.execRoll = base["exec_check"].get_or(1.f);
        if ((rand() % 1000) / 1000.f >= base["exec_chance"].get_or(0.5f)) return false;

        const size_t self = m_em->getEntityIndex(tf.entityId);
        const size_t t = findExecTarget(self, tf.position, base);
        if (t == (size_t)-1) return false;

        ec.execTargetId = m_em->transforms[t].entityId;
        ec.duelShift = DuelShift::ExecApproach;
        ec.duelShiftDuration = base["exec_max_time"].get_or(12.f);
        ec.duelShiftTimer = ec.duelShiftDuration;
        ec.duelTrick = true;            // inside a mode: the switch clock is kept
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;

        // The turn: a gold flash on him and on the one he has picked.
        m_em->spawnShockRing(tf.position, 10.f, 70.f, 0.20f, packGold(), 3.f, 220.f);
        m_em->spawnShockRing(m_em->transforms[t].position, 30.f, 6.f, 0.30f, packGold(), 2.5f, 220.f);
        return true;
    }

    void abortExecution(EnemyComponent& ec, AIState& ai, const sol::table& base) {
        endDuelShift(ec, ai, base);
        ec.execTargetId = 0;
        ec.execCd = base["exec_fail_cooldown"].get_or(4.f);
    }

    /**
     * @brief Fly at the chosen ally, gold windup, kill. Owns the ship.
     *
     * ExecApproach  steered rush at the target. His back is to you: this is
     *               the window the counterplay is built on.
     * ExecStrike    stops, swings onto it, gold crescent (RenderSystem) for
     *               exec_windup, then the kill. Gone out of reach by then:
     *               back to ExecApproach and he chases it down again.
     *
     * COMMITTED (3.0, playtest). Once he has picked a victim only two
     * things end it: the victim dies (anyone's kill -- the player denying
     * it is the counterplay) or he is broken out of it (stagger, stun or
     * parry: DamageSystem::breakDive / the stun handler). No timeout, no
     * giving up on a target that drifted out of reach, no turning back to
     * the player half way. exec_max_time is only a runaway guard now.
     */
    bool updateExecution(float dt, TransformComponent& tf, EnemyComponent& ec, AIState& ai,
        b2BodyId bodyId, const enemyarch::ArchetypeDef& adef)
    {
        const sol::table& base = adef.config;
        ec.duelShiftTimer -= dt;

        const size_t t = (ec.execTargetId != 0) ? m_em->getEntityIndex(ec.execTargetId) : (size_t)-1;
        if (t == (size_t)-1 || m_em->healths[t].currentHp <= 0.f || !m_em->enemies[t].powered()) {
            // Someone finished it first. Usually the player: the denial worked.
            abortExecution(ec, ai, base);
            return true;
        }
        sf::Vector2f to = m_em->transforms[t].position - tf.position;
        const float dist = std::sqrt(to.x * to.x + to.y * to.y);
        const sf::Vector2f dir = (dist > 0.01f) ? to / dist : sf::Vector2f(0.f, -1.f);
        const float reach = base["exec_reach"].get_or(105.f);

        if (ec.duelShift == DuelShift::ExecApproach) {
            const float spd = base["exec_speed"].get_or(950.f);
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = 1.f - std::exp(-6.f * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x + (dir.x * spd / SCALE - v.x) * k,
                                               v.y + (dir.y * spd / SCALE - v.y) * k });
            turnToward(tf, bodyId, dir, base["exec_turn"].get_or(9.f), dt);
            b2Body_SetAngularVelocity(bodyId, 0.f);

            if ((rand() % 100) < 45) {
                const float r = tf.rotation * 3.14159f / 180.f;
                const sf::Vector2f aft(-std::sin(r), std::cos(r));
                const float life = 0.18f + (rand() % 10) / 100.f;
                m_em->particles.push_back({ tf.position + aft * (adef.radius * 0.6f),
                    aft * (120.f + rand() % 120), packGold(210), life, life, 2.f + rand() % 2 });
            }

            if (dist <= reach) {
                ec.duelShift = DuelShift::ExecStrike;
                ec.duelShiftDuration = base["exec_windup"].get_or(0.32f);
                ec.duelShiftTimer = ec.duelShiftDuration;
                return true;
            }
            // Runaway guard only (default 12s): something pinned him.
            if (ec.duelShiftTimer <= 0.f) abortExecution(ec, ai, base);
            return true;
        }

        // ---- ExecStrike ----
        const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
        const float brake = std::exp(-9.f * dt);
        b2Body_SetLinearVelocity(bodyId, { v.x * brake, v.y * brake });
        turnToward(tf, bodyId, dir, 16.f, dt);
        b2Body_SetAngularVelocity(bodyId, 0.f);
        const float u = std::clamp(1.f - ec.duelShiftTimer / std::max(0.01f, ec.duelShiftDuration), 0.f, 1.f);
        const float e = u * u * (3.f - 2.f * u);
        tf.visualPivot = { 0.f, adef.radius * 0.45f };
        tf.visualScale.y *= 1.f - 0.15f * e;
        tf.visualScale.x *= 1.f + 0.07f * e;

        if (ec.duelShiftTimer > 0.f) return true;

        if (dist > reach * 1.6f) {          // it got away: chase it down again
            ec.duelShift = DuelShift::ExecApproach;
            ec.duelShiftDuration = base["exec_max_time"].get_or(12.f);
            ec.duelShiftTimer = ec.duelShiftDuration;
            return true;
        }

        // ---- The kill ----
        const sf::Vector2f at = m_em->transforms[t].position;
        m_em->healths[t].currentHp = 0.f;
        m_em->enemies[t].executed = true;  // DamageSystem: no scrap from this one
        b2Body_SetLinearVelocity(bodyId, { dir.x * 420.f / SCALE, dir.y * 420.f / SCALE });

        m_em->spawnShockRing(at, 10.f, base["exec_radius"].get_or(520.f) * 0.5f, 0.40f,
            packGold(), 5.f, 245.f);
        m_em->spawnShockRing(at, 6.f, 120.f, 0.22f, sf::Color(255, 250, 235), 4.f, 255.f);
        m_em->spawnExplosion(at, packGold(), 24, 3.4f);
        m_em->addTrauma(0.32f);
        m_em->requestHitstop(0.03f, 0.10f, 0.40f);

        buffPack(m_em->getEntityIndex(tf.entityId), tf.position, base);

        endDuelShift(ec, ai, base);
        ec.execTargetId = 0;
        ec.execCd = base["exec_cooldown"].get_or(18.f);
        return true;
    }

    /**
     * @brief The execution's payoff: +exec_bonus for exec_time to every ally
     * within exec_radius, and every recovery or cooldown they are sitting in
     * ends now. Recoveries end; windups and tells are untouched.
     */
    void buffPack(size_t self, sf::Vector2f pos, const sol::table& base) {
        const float r = base["exec_radius"].get_or(520.f);
        const float time = base["exec_time"].get_or(5.f);
        const float bonus = base["exec_bonus"].get_or(0.2f);
        for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
            if (j == self) continue;
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            auto& ej = m_em->enemies[j];
            if (!ej.powered() || ej.executed || m_em->healths[j].currentHp <= 0.f) continue;
            const sol::table& cj = m_registry->resolve(ej.archetype).config;
            if (cj["pack_immune"].get_or(false) || cj["aura_radius"].get_or(0.f) > 0.f) continue;
            const sf::Vector2f d = m_em->transforms[j].position - pos;
            if (d.x * d.x + d.y * d.y > r * r) continue;

            ej.execBuffTimer = time;
            ej.execBonus = bonus;

            // Back in it, now.
            if (ej.ramState == RamState::Recover) { ej.ramState = RamState::None; ej.ramGlow = 0.f; }
            if (ej.bashState == BashState::Recover) ej.bashState = BashState::None;
            if (ej.mineRunState == MineRunState::Recover) ej.mineRunState = MineRunState::None;
            ej.stormRecoverTimer = 0.f;
            ej.staggerRecoverTimer = 0.f;
            ej.microRecover = 0.f;
            ej.shotPauseTimer = 0.f;
            ej.shotsInBurst = 0;
            ej.bashCooldown = 0.f;
            ej.ramCooldown = 0.f;
            ej.fireTimer = 0.f;

            m_em->spawnShockRing(m_em->transforms[j].position, 6.f, 46.f, 0.24f, packGold(), 2.5f, 230.f);
        }
    }

    /// Retro plumes: short bone-white puffs thrown FORWARD off both prow
    /// shoulders. `rate` is the chance per frame of a puff.
    void emitRetro(const TransformComponent& tf, const enemyarch::ArchetypeDef& adef, float rate) {
        if ((rand() % 1000) / 1000.f > rate) return;
        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f fwd(std::sin(r), -std::cos(r));
        const sf::Vector2f rgt(std::cos(r), std::sin(r));
        for (int k = 0; k < 2; ++k) {
            const float side = (k == 0) ? -1.f : 1.f;
            const float life = 0.12f + (rand() % 8) / 100.f;
            m_em->particles.push_back({
                tf.position + fwd * (adef.radius * 0.45f) + rgt * (side * adef.radius * 0.22f),
                fwd * (130.f + rand() % 110) + rgt * (side * 45.f),
                duelBone(200), life, life, 2.f + rand() % 2 });
        }
    }

    // ========================================================================
    // RANGE KIT — lancer shot and suppression cone (notes 27-28)
    // ========================================================================

    /// The shot telegraph's red, shared with every enemy aim line, so "a round
    /// is coming down this line" stays one colour across the roster.
    static sf::Color shotRed(uint8_t a = 220) { return sf::Color(255, 90, 60, a); }

    /**
     * @brief Direction to fire a round of speed `s` from S so it meets a
     *        target at P moving at V.
     *
     * Solves |P + V t - S| = s t for the smallest t > 0. With `lead` < 1 the
     * velocity is scaled first, which is a deliberate under-lead, not an
     * approximation. When there is no solution (target outrunning the round)
     * it aims at the target itself.
     */
    static sf::Vector2f leadAim(sf::Vector2f S, sf::Vector2f P, sf::Vector2f V, float s, float lead) {
        V *= lead;
        const sf::Vector2f r = P - S;
        const float a = V.x * V.x + V.y * V.y - s * s;
        const float b = 2.f * (r.x * V.x + r.y * V.y);
        const float c = r.x * r.x + r.y * r.y;

        float t = -1.f;
        if (std::fabs(a) < 1e-3f) {
            if (std::fabs(b) > 1e-3f) t = -c / b;
        }
        else {
            const float disc = b * b - 4.f * a * c;
            if (disc >= 0.f) {
                const float sq = std::sqrt(disc);
                const float t1 = (-b - sq) / (2.f * a);
                const float t2 = (-b + sq) / (2.f * a);
                const float lo = std::min(t1, t2), hi = std::max(t1, t2);
                t = (lo > 0.f) ? lo : hi;
            }
        }
        sf::Vector2f aim = (t > 0.f) ? r + V * std::min(t, 1.6f) : r;
        const float l = std::sqrt(aim.x * aim.x + aim.y * aim.y);
        return (l > 0.01f) ? aim / l : sf::Vector2f(0.f, -1.f);
    }

    void cancelRangeAttack(EnemyComponent& ec) {
        if (ec.duelAttack == DuelAttack::None) return;
        const bool cone = ec.duelAttack >= DuelAttack::ConeWindup;
        ec.duelAttack = DuelAttack::None;
        ec.duelAtkTimer = 0.f;
        // Broken out of it, not finished: he does not get to try again at
        // once. The cone in particular must not resume after the stun ends.
        if (cone) ec.duelConeCd = std::max(ec.duelConeCd, 4.0f);
        else      ec.duelLancerCd = std::max(ec.duelLancerCd, 1.2f);
    }

    void startLancer(TransformComponent& tf, EnemyComponent& ec, const sol::table& rt,
        sf::Vector2f toPlayerN, float dist)
    {
        (void)tf; (void)dist;
        squadGrant(rt["lancer_charge"].get_or(0.55f) + rt["lancer_lock"].get_or(0.18f) + 0.3f);   // elite: reserves, never waits
        ec.duelAttack = DuelAttack::LancerCharge;
        ec.duelAtkDuration = rt["lancer_charge"].get_or(0.55f);
        ec.duelAtkTimer = ec.duelAtkDuration;
        ec.duelAtkDir = toPlayerN;
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
    }

    void startCone(TransformComponent& tf, EnemyComponent& ec, const sol::table& rt) {
        squadGrant(rt["cone_windup"].get_or(0.75f) + 1.5f);   // elite: reserves, never waits
        ec.duelAttack = DuelAttack::ConeWindup;
        ec.duelAtkDuration = rt["cone_windup"].get_or(0.75f);
        ec.duelAtkTimer = ec.duelAtkDuration;
        const float r = tf.rotation * 3.14159f / 180.f;
        ec.duelAtkDir = { std::sin(r), -std::cos(r) };
        ec.duelConeHalf = rt["cone_half_angle"].get_or(22.f);
        ec.duelConeRange = rt["cone_speed"].get_or(640.f) * rt["cone_lifetime"].get_or(1.15f);
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
    }

    /**
     * @brief One custom round: the factory's enemy bullet, then re-tuned.
     *
     * createEnemyBullet sizes everything from the archetype's ordinary gun
     * (speed, damage, i-frames). The lancer and the cone each need their own,
     * so the round is made normally and then overwritten -- the body's
     * velocity included, since the factory derives it from the angle.
     *
     * @param lancer  true = the lancer's long bone needle; false = the
     *                ordinary orange diamond, slightly smaller.
     */
    /// The lancer's three points in world space: returns the central (maw)
    /// spike tip; writes the two side spike tips. Authored as
    /// `lancer_maw` and `lancer_spike` (starboard, mirrored), x scale.
    sf::Vector2f lancerPoints(const TransformComponent& tf, const sol::table& rt,
        sf::Vector2f& left, sf::Vector2f& right) const
    {
        const float sc = rt["scale"].get_or(1.f);
        float mx = 0.f, my = -50.f, sx = 22.f, sy = -48.f;
        if (sol::optional<sol::table> m = rt["lancer_maw"]) { mx = (*m)["x"].get_or(mx); my = (*m)["y"].get_or(my); }
        if (sol::optional<sol::table> s = rt["lancer_spike"]) { sx = (*s)["x"].get_or(sx); sy = (*s)["y"].get_or(sy); }
        const float r = tf.rotation * 3.14159265f / 180.f;
        const float c = std::cos(r), s = std::sin(r);
        const auto w = [&](float x, float y) {
            x *= sc; y *= sc;
            return tf.position + sf::Vector2f(x * c - y * s, x * s + y * c);
            };
        left = w(-sx, sy);
        right = w(sx, sy);
        return w(mx, my);
    }

    void fireDuelRound(uint32_t ownerId, sf::Vector2f spawn, sf::Vector2f dir, float speed,
        float damage, float iframes, float lifetime, bool lancer, const sol::table& rt)
    {
        const float angle = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        const uint32_t id = m_ef->createEnemyBullet(*m_em, spawn, dir * speed, angle,
            ownerId, *m_lua, m_worldId, rt);
        const size_t bi = m_em->getEntityIndex(id);
        if (bi == (size_t)-1) return;

        const sf::Vector2f vel = dir * speed;
        if (b2Body_IsValid(m_em->physics[bi].bodyId))
            b2Body_SetLinearVelocity(m_em->physics[bi].bodyId, { vel.x / SCALE, vel.y / SCALE });
        m_em->transforms[bi].velocity = vel;

        auto& b = m_em->bullets[bi];
        b.damage = damage;
        b.playerIframes = iframes;
        b.lifetime = lifetime;

        auto& sh = m_em->renders[bi].shape;
        if (lancer) {
            // Longer and paler than anything else in the air: a needle you
            // can pick out of the filler at a glance.
            sh.setPoint(0, { 0.f, -22.f });
            sh.setPoint(1, { 3.2f, 0.f });
            sh.setPoint(2, { 0.f, 22.f });
            sh.setPoint(3, { -3.2f, 0.f });
            sh.setFillColor(sf::Color(250, 240, 222));
            sh.setOutlineThickness(2.0f);
            sh.setOutlineColor(shotRed(235));
        }
        else {
            sh.setPoint(0, { 0.f, -9.f });
            sh.setPoint(1, { 2.2f, 0.f });
            sh.setPoint(2, { 0.f, 9.f });
            sh.setPoint(3, { -2.2f, 0.f });
        }
    }

    bool updateRangeAttack(float dt, TransformComponent& tf, EnemyComponent& ec, b2BodyId bodyId,
        const enemyarch::ArchetypeDef& adef, sf::Vector2f toPlayerN, float dist)
    {
        const sol::table rt = duelTable(adef.config, DuelMode::Range);
        ec.duelAtkTimer -= dt;
        const float u = std::clamp(1.f - ec.duelAtkTimer /
            std::max(0.01f, ec.duelAtkDuration), 0.f, 1.f);

        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f fwd(std::sin(r), -std::cos(r));
        const sf::Vector2f rgt(std::cos(r), std::sin(r));
        const sf::Vector2f nose = tf.position + fwd * (adef.radius * 0.95f);

        const auto brake = [&](float rate) {
            const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
            const float k = std::exp(-rate * dt);
            b2Body_SetLinearVelocity(bodyId, { v.x * k, v.y * k });
            b2Body_SetAngularVelocity(bodyId, 0.f);
        };

        switch (ec.duelAttack) {

        // ---------------------------------------------------------------- LANCER
        case DuelAttack::LancerCharge: {
            brake(3.f);   // eases off rather than stopping: a shot, not a stance

            // Solve from the moment the round actually LEAVES, not from now:
            // what is left of the charge plus the whole lock. Solving from
            // now missed a player holding course by exactly their drift
            // during the lock (~54px at 300px/s) -- the one player the shot
            // exists to catch.
            // 3.0: the lancer is a BEAM (light: no travel time), so the
            // only lead is the player's drift until the fire moment.
            const float lead = rt["lancer_lead"].get_or(1.f);
            const float untilFire = std::max(0.f, ec.duelAtkTimer) + rt["lancer_lock"].get_or(0.18f);
            const sf::Vector2f playerPos = tf.position + toPlayerN * dist
                + m_playerVel * (lead * untilFire);
            {
                const sf::Vector2f d = playerPos - nose;
                const float l = std::sqrt(d.x * d.x + d.y * d.y);
                if (l > 0.01f) ec.duelAtkDir = d / l;
            }
            turnToward(tf, bodyId, ec.duelAtkDir, 16.f, dt);

            // ---- Charge: the side spikes pour into the central one ----
            // Bone-white motes stream from both spike tips to the maw tip
            // (RenderSystem draws the arcing lines and the swelling core).
            sf::Vector2f spikeL, spikeR;
            const sf::Vector2f maw = lancerPoints(tf, rt, spikeL, spikeR);
            for (const sf::Vector2f sp : { spikeL, spikeR }) {
                if ((rand() % 100) >= static_cast<int>(30 + 60 * u)) continue;
                const sf::Vector2f d = maw - sp;
                const float jitter = ((rand() % 100) - 50) / 100.f;
                const float flight = 0.10f + (rand() % 8) / 100.f;
                m_em->particles.push_back({ sp + sf::Vector2f(-d.y, d.x) * (0.08f * jitter),
                    d / flight, duelBone(230), flight, flight, 2.f + u });
            }

            if (ec.duelAtkTimer <= 0.f) {
                ec.duelAttack = DuelAttack::LancerLock;
                ec.duelAtkDuration = rt["lancer_lock"].get_or(0.18f);
                ec.duelAtkTimer = ec.duelAtkDuration;
                lockHeading(tf, bodyId, ec.duelAtkDir);
                m_em->spawnShockRing(nose, 4.f, 34.f, 0.14f, duelBone(), 2.f, 240.f);
            }
            return true;
        }

        case DuelAttack::LancerLock: {
            brake(9.f);
            lockHeading(tf, bodyId, ec.duelAtkDir);   // frozen: this is the honest beat
            tf.visualPivot = { 0.f, adef.radius * 0.4f };
            tf.visualScale.y *= 1.f - 0.10f * u;      // pulls back against the shot

            if (ec.duelAtkTimer <= 0.f) {
                const sf::Vector2f dir = ec.duelAtkDir;
                sf::Vector2f sl, sr;
                const sf::Vector2f muzzle = lancerPoints(tf, rt, sl, sr);

                // ---- THE BEAM: light, not a round (DamageSystem::traceBeam) ----
                // Everything on the line at once, through rocks and ships;
                // a parry mirrors it back off the shield.
                BeamShot shot;
                shot.origin = muzzle;
                shot.dir = dir;
                shot.range = rt["lancer_range"].get_or(1300.f);
                shot.width = rt["lancer_width"].get_or(14.f);
                shot.damage = rt["lancer_damage"].get_or(45.f);
                shot.iframes = rt["lancer_iframes"].get_or(0.6f);
                shot.knockback = rt["lancer_knockback"].get_or(1000.f);
                shot.pierceDamage = rt["lancer_pierce_damage"].get_or(600.f);
                shot.killHp = rt["lancer_kill_hp"].get_or(500.f);
                shot.objectDamage = rt["lancer_object_damage"].get_or(9999.f);
                shot.ownerId = tf.entityId;
                m_em->beamShots.push_back(shot);

                // Kick: hard recoil, a white flash at the maw.
                const b2Vec2 v = b2Body_GetLinearVelocity(bodyId);
                b2Body_SetLinearVelocity(bodyId, { v.x - dir.x * 320.f / SCALE,
                                                   v.y - dir.y * 320.f / SCALE });
                m_em->spawnShockRing(muzzle, 4.f, 70.f, 0.18f, duelBone(), 4.f, 255.f);
                m_em->spawnShockRing(muzzle, 2.f, 30.f, 0.12f, shotRed(240), 3.f, 255.f);
                m_em->spawnImpact(muzzle, duelBone(240), dir * 260.f);
                m_em->spawnScreenFlash(sf::Color(250, 244, 230), 0.10f, 55.f);

                ec.duelAttack = DuelAttack::LancerRecover;
                ec.duelAtkDuration = rt["lancer_recover"].get_or(0.25f);
                ec.duelAtkTimer = ec.duelAtkDuration;
            }
            return true;
        }

        case DuelAttack::LancerRecover: {
            brake(4.f);
            tf.visualOffsetAngle += 6.f * (1.f - u) * std::sin(ec.duelAtkTimer * 30.f);
            tf.visualPivot = { 0.f, -18.f };
            if (ec.duelAtkTimer <= 0.f) {
                ec.duelAttack = DuelAttack::None;
                ec.duelLancerCd = rollSpan(rt, "lancer_cooldown", 3.2f, 4.6f);
                ec.fireTimer = std::max(ec.fireTimer, 0.7f);   // filler waits its turn
            }
            return true;
        }

        // ------------------------------------------------------------------ CONE
        case DuelAttack::ConeWindup: {
            brake(7.f);   // coming to a full stop IS part of the read

            // Swings on through the windup; the wedge follows the hull, so
            // what is drawn is exactly what will fire.
            turnToward(tf, bodyId, toPlayerN, rt["cone_windup_turn"].get_or(6.f), dt);
            ec.duelAtkDir = fwd;

            // Spin-up: barrels squat and sparks at the prow.
            const float e = u * u * (3.f - 2.f * u);
            tf.visualPivot = { 0.f, adef.radius * 0.45f };
            tf.visualScale.x *= 1.f + 0.10f * e;
            tf.visualScale.y *= 1.f - 0.12f * e;
            if ((rand() % 100) < static_cast<int>(25 + 55 * u)) {
                const float side = ((rand() % 200) - 100) / 100.f;
                const float life = 0.14f + (rand() % 8) / 100.f;
                m_em->particles.push_back({ nose + rgt * (side * adef.radius * 0.3f),
                    fwd * (50.f + rand() % 80) + rgt * (side * 60.f),
                    shotRed(225), life, life, 2.f + rand() % 2 });
            }

            if (ec.duelAtkTimer <= 0.f) {
                ec.duelAttack = DuelAttack::ConeFire;
                ec.duelAtkDuration = rt["cone_fire_time"].get_or(2.2f);
                ec.duelAtkTimer = ec.duelAtkDuration;
                ec.duelAtkFire = 0.f;
                lockHeading(tf, bodyId, ec.duelAtkDir);   // LOCKED from here
                m_em->spawnShockRing(nose, 6.f, 70.f, 0.18f, shotRed(240), 3.f, 230.f);
            }
            return true;
        }

        case DuelAttack::ConeFire: {
            // Rooted: no drift, no turn. The whole point of the attack.
            b2Body_SetLinearVelocity(bodyId, { 0.f, 0.f });
            b2Body_SetAngularVelocity(bodyId, 0.f);
            lockHeading(tf, bodyId, ec.duelAtkDir);

            const float half = ec.duelConeHalf * 3.14159f / 180.f;
            const float base = std::atan2(ec.duelAtkDir.y, ec.duelAtkDir.x);
            const float interval = std::max(0.01f, rt["cone_interval"].get_or(0.05f));

            ec.duelAtkFire -= dt;
            int guard = 4;   // a long frame fires a few, never a flood
            while (ec.duelAtkFire <= 0.f && guard-- > 0) {
                ec.duelAtkFire += interval;
                const float a = base + half * (((rand() % 2001) - 1000) / 1000.f);
                const sf::Vector2f dir(std::cos(a), std::sin(a));
                fireDuelRound(tf.entityId, tf.position + dir * (adef.radius * 0.9f), dir,
                    rt["cone_speed"].get_or(640.f), rt["cone_damage"].get_or(6.f),
                    rt["cone_iframes"].get_or(0.10f), rt["cone_lifetime"].get_or(1.15f),
                    false, rt);
                m_em->spawnImpact(tf.position + dir * (adef.radius * 0.9f),
                    sf::Color(255, 140, 60), dir * 140.f);
            }
            if (ec.duelAtkFire < -interval) ec.duelAtkFire = 0.f;

            // Shudder under the recoil.
            tf.visualOffsetAngle += 2.5f * std::sin(ec.duelAtkTimer * 70.f);
            tf.visualPivot = { 0.f, adef.radius * 0.3f };

            if (ec.duelAtkTimer <= 0.f) {
                ec.duelAttack = DuelAttack::ConeRecover;
                ec.duelAtkDuration = rt["cone_recover"].get_or(1.1f);
                ec.duelAtkTimer = ec.duelAtkDuration;
            }
            return true;
        }

        case DuelAttack::ConeRecover: {
            // Still rooted, barrels venting. This is where he gets hit.
            brake(10.f);
            lockHeading(tf, bodyId, ec.duelAtkDir);
            const float left = 1.f - u;
            tf.visualOffsetAngle += 9.f * left * std::sin(ec.duelAtkTimer * 18.f);
            tf.visualPivot = { 0.f, -18.f };

            if ((rand() % 100) < static_cast<int>(45 * left)) {
                const float side = ((rand() % 200) - 100) / 100.f;
                m_em->particles.push_back({ nose + rgt * (side * adef.radius * 0.3f),
                    fwd * (20.f + rand() % 30) + rgt * (side * 25.f),
                    sf::Color(110, 100, 95, 170), 0.6f, 0.6f, 4.f + rand() % 3 });
            }

            if (ec.duelAtkTimer <= 0.f) {
                ec.duelAttack = DuelAttack::None;
                ec.duelConeCd = rollSpan(rt, "cone_cooldown", 9.f, 12.f);
                ec.duelLancerCd = std::max(ec.duelLancerCd, 1.0f);
                ec.fireTimer = std::max(ec.fireTimer, 0.8f);
            }
            return true;
        }

        default:
            ec.duelAttack = DuelAttack::None;
            return false;
        }
    }

    /// Rotate the hull toward a direction at `rate` (1/s, exponential).
    void turnToward(TransformComponent& tf, b2BodyId bodyId, sf::Vector2f dir,
        float rate, float dt) {
        const float target = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        float d = target - tf.rotation;
        while (d > 180.f) d -= 360.f;
        while (d < -180.f) d += 360.f;
        tf.rotation += d * std::min(1.f, rate * dt);
        b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
            b2MakeRot(tf.rotation * 3.14159f / 180.f));
    }

    static bool isMelee(const sol::table& config) {
        return config["maneuver_profile"].get_or<std::string>("standard") == "melee";
    }

    // ========================================================================
    // AVOIDANCE + EVASIVE BURSTS
    // ========================================================================
    sf::Vector2f computeAvoidance(float dt, size_t i, sf::Vector2f enemyPos,
        uint32_t entityId, AIState& ai, EnemyComponent& ec,
        float maxSpeed, sol::table& config)
    {
        sf::Vector2f avoidance(0.f, 0.f);
        if (ai.dodgeCooldown > 0.f)        ai.dodgeCooldown -= dt;
        if (ai.threatSeenTimer > 0.f)      ai.threatSeenTimer -= dt;
        if (ai.dodgePunishCooldown > 0.f)  ai.dodgePunishCooldown -= dt;
        if (ai.flinchTimer > 0.f)          ai.flinchTimer -= dt;

        const b2Vec2 myV = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
        const sf::Vector2f myVel(myV.x * SCALE, myV.y * SCALE);

        // Tier 1: ordinary asteroids
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = bodyUD(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;
            if (m_em->healths[j].isHoming) continue;

            sf::Vector2f diff = enemyPos - m_em->transforms[j].position;
            const float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
            if (d >= 300.f || d <= 0.01f) continue;

            const b2Vec2 av = b2Body_GetLinearVelocity(m_em->physics[j].bodyId);
            const sf::Vector2f astVel(av.x * SCALE, av.y * SCALE);

            const sf::Vector2f toMe = diff / d;
            const sf::Vector2f rel(astVel.x - myVel.x, astVel.y - myVel.y);
            const float closing = -(rel.x * toMe.x + rel.y * toMe.y);

            if (closing < 5.f) continue;

            ai.threatSeenTimer = std::max(ai.threatSeenTimer, 0.8f);

            const float tti = d / closing;
            const float react = m_dodgeManoeuvreTime;
            const float comp = std::clamp((tti - react * 0.5f) / (react * 1.8f), 0.f, 1.f);
            if (comp <= 0.01f) continue;

            avoidance += toMe * config["avoid_force"].get_or(380.f)
                * (1.f - d / 300.f) * comp;
        }

        // Tier 2: homing / kinetic rocks
        {
            float bestDist = FLT_MAX;
            uint32_t bestId = 0;
            sf::Vector2f bestPos, bestVel;

            for (size_t j = 0; j < m_em->physics.size(); ++j) {
                BodyUserData* ud2 = bodyUD(m_em->physics[j].bodyId);
                if (!ud2 || ud2->type != BodyType::Asteroid) continue;
                if (!m_em->healths[j].isHoming) continue;
                if (m_em->healths[j].homingTargetEntityId != entityId) continue;

                sf::Vector2f diff = enemyPos - m_em->transforms[j].position;
                const float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
                if (d < bestDist) {
                    bestDist = d; bestId = ud2->entityId;
                    bestPos = m_em->transforms[j].position;
                    const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[j].bodyId);
                    bestVel = { v.x * SCALE, v.y * SCALE };
                }
            }

            const float notice = config["homing_notice_range"].get_or(450.f);
            if (bestId != 0 && bestDist < notice) {
                ai.threatSeenTimer = std::max(ai.threatSeenTimer, 1.2f);
                if (!ai.threatNoticed || ai.trackedThreatId != bestId) {
                    ai.threatReactionDelay = config["threat_reaction_min"].get_or(0.35f)
                        + (rand() % 45) / 100.f;
                    ai.threatNoticed = true;
                    ai.trackedThreatId = bestId;
                    ai.dodgeCommitTimer = 0.f;
                }

                if (ai.threatReactionDelay > 0.f) {
                    ai.threatReactionDelay -= dt;
                }
                else if (ai.dodgeCommitTimer <= 0.f) {
                    sf::Vector2f away = enemyPos - bestPos;
                    const float l = std::sqrt(away.x * away.x + away.y * away.y);
                    if (l > 0.01f) away /= l;

                    const sf::Vector2f rel(bestVel.x - myVel.x, bestVel.y - myVel.y);
                    const float closing = std::max(1.f,
                        (rel.x * away.x + rel.y * away.y) * -1.f);
                    const float tti = bestDist / closing;

                    const sf::Vector2f perp(-away.y, away.x);
                    const int choice = rand() % 10;
                    if (choice < 4)      ai.pendingDodgeDir = perp * ((rand() % 2) ? 1.f : -1.f);
                    else if (choice < 7) ai.pendingDodgeDir = away;
                    else                 ai.pendingDodgeDir = -perp * ((rand() % 2) ? 1.f : -1.f);

                    ai.dodgeCommitTimer = 0.45f + (rand() % 30) / 100.f;

                    const float homingPenalty = config["homing_dodge_penalty"].get_or(2.5f);
                    triggerDodgeBurst(ai, ec, ai.pendingDodgeDir, 0.9f, tti / homingPenalty);
                }

                if (ai.dodgeCommitTimer > 0.f) {
                    ai.dodgeCommitTimer -= dt;
                }
            }
            else {
                ai.threatNoticed = false;
                ai.trackedThreatId = 0;
            }
        }

        // Tier 3: incoming bullets
        {
            const float dodgeChance = config["bullet_dodge_chance"].get_or(0.45f);

            if (ai.bulletReactionDelay <= 0.f && ai.bulletDodgeTimer <= 0.f
                && ai.dodgeCooldown <= 0.f && ai.flinchTimer <= 0.f) {

                for (size_t j = 0; j < m_em->physics.size(); ++j) {
                    BodyUserData* ud2 = bodyUD(m_em->physics[j].bodyId);
                    if (!ud2 || ud2->type != BodyType::Bullet) continue;
                    if (m_em->bullets[j].isEnemyBullet) continue;

                    sf::Vector2f diff = enemyPos - m_em->transforms[j].position;
                    const float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
                    if (d > config["bullet_notice_range"].get_or(520.f) || d < 0.01f) continue;

                    const b2Vec2 bv = b2Body_GetLinearVelocity(m_em->physics[j].bodyId);
                    sf::Vector2f bdir(bv.x, bv.y);
                    const float bl = std::sqrt(bdir.x * bdir.x + bdir.y * bdir.y);
                    if (bl < 0.01f) continue;
                    bdir /= bl;

                    const sf::Vector2f toMe = diff / d;
                    if (bdir.x * toMe.x + bdir.y * toMe.y < 0.75f) continue;

                    ai.threatSeenTimer = std::max(ai.threatSeenTimer, 1.0f);

                    ai.bulletReactionDelay = config["bullet_reaction_min"].get_or(0.28f)
                        + (rand() % 28) / 100.f;

                    const sf::Vector2f perp(-bdir.y, bdir.x);
                    const bool good = ((rand() % 100) / 100.f) < dodgeChance;
                    ai.bulletDodgeDir = good
                        ? perp * ((rand() % 2) ? 1.f : -1.f)
                        : bdir * 0.7f;

                    ai.pendingDodgeDir = ai.bulletDodgeDir;
                    m_pendingTTI[entityId] = d / (bl * SCALE);
                    break;
                }
            }

            if (ai.bulletReactionDelay > 0.f) {
                ai.bulletReactionDelay -= dt;
                if (ai.bulletReactionDelay <= 0.f) {
                    float tti = 99.f;
                    auto it = m_pendingTTI.find(entityId);
                    if (it != m_pendingTTI.end()) {
                        tti = it->second - config["bullet_reaction_min"].get_or(0.28f);
                        m_pendingTTI.erase(it);
                    }
                    if (triggerDodgeBurstChecked(ai, ec, ai.bulletDodgeDir, tti)) {
                        ai.bulletDodgeTimer = ai.dodgeBurstDuration;
                    }
                }
            }

            if (ai.bulletDodgeTimer > 0.f) {
                ai.bulletDodgeTimer -= dt;
            }
        }

        if (ai.flinchTimer > 0.f) {
            avoidance += ai.flinchDir * (maxSpeed * 0.8f);
        }

        return avoidance;
    }

    bool triggerDodgeBurstChecked(AIState& ai, EnemyComponent& ec,
        sf::Vector2f dir, float tti) {
        const float before = ai.dodgeBurstTimer;
        triggerDodgeBurst(ai, ec, dir, 1.f, tti);
        return ai.dodgeBurstTimer > before;
    }

    void triggerDodgeBurst(AIState& ai, EnemyComponent& ec, sf::Vector2f dir,
        float strength, float ttiSeconds = 99.f)
    {
        if (ai.dodgeCooldown > 0.f) return;
        if (ai.dodgeBurstTimer > 0.f) return;

        const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (l < 0.01f) return;
        dir /= l;

        const float manoeuvreTime = m_dodgeManoeuvreTime;
        if (ttiSeconds < manoeuvreTime) {
            triggerFlinch(ai, ec, dir, ttiSeconds);
            return;
        }

        ai.dodgeBurstDir = dir;
        ai.dodgeBurstDuration = m_dodgeDuration * (0.85f + (rand() % 30) / 100.f);
        ai.dodgeBurstTimer = ai.dodgeBurstDuration;
        ai.dodgeCooldown = m_dodgeCooldown + (rand() % 120) / 100.f;

        ai.dodgeBurstSide = 0.f;
        ec.dodgeFlashTimer = ai.dodgeBurstDuration;

        if (b2Body_IsValid(m_dodgeBodyId)) {
            const float speed = m_dodgeSpeed * std::clamp(strength, 0.4f, 1.4f);
            const b2Vec2 cur = b2Body_GetLinearVelocity(m_dodgeBodyId);
            b2Body_SetLinearVelocity(m_dodgeBodyId, {
                cur.x * 0.35f + dir.x * speed / SCALE,
                cur.y * 0.35f + dir.y * speed / SCALE
                });

            for (int n = 0; n < 8; ++n) {
                const float a = ((rand() % 90) - 45) * 3.14159f / 180.f;
                const sf::Vector2f d(
                    -dir.x * std::cos(a) + dir.y * std::sin(a),
                    -dir.x * std::sin(a) - dir.y * std::cos(a));
                m_em->particles.push_back({
                    m_dodgePos + d * 12.f,
                    d * (140.f + rand() % 120),
                    sf::Color(255, static_cast<uint8_t>(180 + rand() % 60), 100, 225),
                    0.24f, 0.28f,
                    2.5f + rand() % 3
                    });
            }
        }
    }

    void triggerFlinch(AIState& ai, EnemyComponent& ec, sf::Vector2f dir, float tti) {
        if (ai.flinchTimer > 0.f) return;
        ai.flinchDuration = std::min(0.30f, std::max(0.12f, tti));
        ai.flinchTimer = ai.flinchDuration;
        ai.flinchDir = dir;
        ec.dodgeFlashTimer = 0.f;
    }

    // ========================================================================
    // SHOOTING
    // ========================================================================
    void updateShooting(float dt, size_t i, const TransformComponent& tf,
        EnemyComponent& ec, AIState& ai, uint32_t entityId,
        sf::Vector2f playerPos, sf::Vector2f playerVel,
        float dist, sol::table& config)
    {
        ec.fireTimer -= dt * ec.packMult;

        if (ai.currentState != EnemyState::COMBAT) {
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            ec.burstCharging = false;
            ec.volleyLeft = 0;
            return;
        }

        const float attackRange = config["attack_range"].get_or(480.f);
        const float fireRate = config["fire_rate"].get_or(1.8f);
        const float telegraph = config["telegraph_time"].get_or(0.32f);

        // ---- CHARGED BURST (note 39, rebuilt in 3.0) ----
        // Units with burst_windup run their own small machine instead of the
        // per-round telegraph below. 2.9 bolted the charge onto round one of
        // the old loop, and every interrupt in between (hold-fire band,
        // leaving range, a bash, a mode switch) split it: a charge with no
        // shots, or shots with no charge.
        const float bw = config["burst_windup"].get_or(0.f);
        const int   burstN = config["burst_count"].get_or(0);
        if (bw > 0.f && burstN > 0) {
            updateChargedBurst(dt, i, tf, ec, entityId, playerPos, playerVel, dist, config, bw, burstN);
            return;
        }
        if (ec.burstBusy()) { ec.burstCharging = false; ec.volleyLeft = 0; ec.telegraphActive = false; }

        // ---- HOLD FIRE ----
        // Inside this radius the gun is simply off. A melee unit that keeps
        // spraying while it closes forces the player to dodge a bullet and
        // parry a lunge on the same beat, and neither read survives that.
        // One threat at a time is the whole point of the unit.
        const float holdFire = config["hold_fire_range"].get_or(0.f);
        if (holdFire > 0.f && dist < holdFire && !m_vsShip) {
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            ec.shotsInBurst = 0;
            return;
        }

        if (ec.telegraphActive) {
            ec.telegraphTimer -= dt;
            if (ec.telegraphTimer <= 0.f) {
                ec.telegraphActive = false;
                fireShot(i, tf, ec, entityId, ec.telegraphDir, config);
                ec.fireTimer = fireRate + ((rand() % 40) - 20) / 100.f;
                ec.shotClearTimer = config["melee_shot_clear"].get_or(0.f);

                // ---- BURST ----
                const int burst = config["burst_count"].get_or(0);
                if (burst > 0 && ++ec.shotsInBurst >= burst) {
                    ec.shotsInBurst = 0;
                    ec.shotPauseTimer = config["burst_pause"].get_or(1.0f)
                        * (0.85f + (rand() % 30) / 100.f);
                    ec.fireTimer = ec.shotPauseTimer;
                }
            }
            return;
        }

        if (ec.fireTimer > 0.f || dist > attackRange) return;
        if (ec.shotPauseTimer > 0.f) return;
        if (ec.microRecover > 0.f) return;     // reloading, not shooting
        if (ec.rocketsLeft > 0) return;        // mid-volley: one weapon at a time

        const float bulletSpeed = config["bullet_speed"].get_or(550.f);
        const float travelTime = dist / bulletSpeed;

        const float leadFactor = 0.55f + (rand() % 35) / 100.f;
        const sf::Vector2f predicted = playerPos + playerVel * (travelTime * leadFactor);

        sf::Vector2f aim = predicted - tf.position;
        const float al = std::sqrt(aim.x * aim.x + aim.y * aim.y);
        if (al > 0.01f) aim /= al;

        const float spreadDeg = config["aim_spread"].get_or(18.f);
        const float spread = ((rand() % 200) - 100) / 100.f * (spreadDeg * 3.14159f / 180.f);

        ec.telegraphDir = {
            aim.x * std::cos(spread) - aim.y * std::sin(spread),
            aim.x * std::sin(spread) + aim.y * std::cos(spread)
        };

        ec.telegraphDuration = telegraph * (0.85f + (rand() % 30) / 100.f);
        // Squad (note 43): a telegraphed round is a commit. Untelegraphed
        // spray (Wardogs) is the harassment itself and never waits.
        if (ec.telegraphDuration > 0.f && !squadGrant(ec.telegraphDuration + 0.25f, false, true)) {
            ec.fireTimer = m_sqCfg.retryDelay * (0.6f + (rand() % 80) / 100.f);
            return;
        }
        ec.telegraphTimer = ec.telegraphDuration;
        ec.telegraphActive = true;
    }

    /**
     * @brief CHARGE -> VOLLEY -> PAUSE (note 39).
     *
     * CHARGE  burst_windup s. The telegraph IS the charge (hull outline
     *         glows, aim line grows, sparks pull in to the nose), so every
     *         existing interrupt -- stun, stagger, bash, dive, frozen --
     *         that clears telegraphActive also cancels it, cleanly, with
     *         nothing fired. Tracks the target. The ship keeps flying.
     * VOLLEY  burst_count rounds, burst_interval apart, re-aimed per round,
     *         +-aim_spread/2. COMMITTED: crossing the hold-fire band or
     *         leaving attack_range does not cut it off half way -- that was
     *         the "charged and never shot" bug. Bash and ram cannot start
     *         while it runs (burstBusy), and melee_shot_clear runs from the
     *         last round as before, so a lunge still never arrives on top
     *         of its own bullets.
     * PAUSE   burst_pause.
     *
     * Hold-fire and range only gate the START of a charge.
     */
    void updateChargedBurst(float dt, size_t i, const TransformComponent& tf, EnemyComponent& ec,
        uint32_t entityId, sf::Vector2f playerPos, sf::Vector2f playerVel, float dist,
        sol::table& config, float windup, int count)
    {
        // Something cleared the telegraph from outside: the charge is broken.
        if (ec.burstBusy() && !ec.telegraphActive) {
            ec.burstCharging = false;
            ec.volleyLeft = 0;
            ec.fireTimer = std::max(ec.fireTimer, 0.35f);
        }

        const auto aimNow = [&]() {
            const float tt = dist / std::max(1.f, config["bullet_speed"].get_or(550.f));
            sf::Vector2f aim = playerPos + playerVel * (tt * 0.7f) - tf.position;
            const float al = std::sqrt(aim.x * aim.x + aim.y * aim.y);
            return al > 0.01f ? aim / al : ec.telegraphDir;
            };

        // ---- VOLLEY ----
        if (ec.volleyLeft > 0) {
            ec.telegraphDir = aimNow();
            ec.volleyTimer -= dt;
            if (ec.volleyTimer <= 0.f) {
                const float sp = config["aim_spread"].get_or(10.f) * 0.5f * 3.14159f / 180.f
                    * (((rand() % 200) - 100) / 100.f);
                const sf::Vector2f d = ec.telegraphDir;
                fireShot(i, tf, ec, entityId,
                    { d.x * std::cos(sp) - d.y * std::sin(sp), d.x * std::sin(sp) + d.y * std::cos(sp) }, config);
                ec.shotClearTimer = config["melee_shot_clear"].get_or(0.f);
                ec.volleyTimer = config["burst_interval"].get_or(0.07f);
                if (--ec.volleyLeft <= 0) {
                    ec.telegraphActive = false;
                    ec.shotPauseTimer = config["burst_pause"].get_or(1.0f) * (0.85f + (rand() % 30) / 100.f);
                    ec.fireTimer = ec.shotPauseTimer;
                }
            }
            return;
        }

        // ---- CHARGE ----
        if (ec.burstCharging) {
            ec.telegraphDir = aimNow();
            ec.telegraphTimer -= dt;
            const float u = 1.f - ec.telegraphTimer / std::max(0.01f, ec.telegraphDuration);
            if ((rand() % 100) < static_cast<int>(35 + 50 * u)) {
                const sf::Vector2f nose = tf.position + ec.telegraphDir * std::max(35.f, m_curRadius + 8.f);
                const float a = (rand() % 360) * 3.14159f / 180.f;
                const sf::Vector2f off(std::cos(a) * 22.f, std::sin(a) * 22.f);
                m_em->particles.push_back({ nose + off, -off * 5.f,
                    sf::Color(255, static_cast<uint8_t>(150 + 80 * u), 90, 235),
                    0.18f, 0.18f, 2.f + 1.5f * u });
            }
            if (ec.telegraphTimer <= 0.f) {
                ec.burstCharging = false;
                ec.telegraphTimer = 0.f;          // stays lit, at full, through the volley
                ec.volleyLeft = count;
                ec.volleyTimer = 0.f;
            }
            return;
        }

        // ---- START (only here do hold-fire and range apply) ----
        const float holdFire = config["hold_fire_range"].get_or(0.f);
        if (holdFire > 0.f && dist < holdFire && !m_vsShip) return;
        if (ec.fireTimer > 0.f || dist > config["attack_range"].get_or(480.f)) return;
        if (ec.shotPauseTimer > 0.f || ec.microRecover > 0.f || ec.rocketsLeft > 0) return;
        if (ec.bashState != BashState::None || ec.ramState != RamState::None) return;
        if (!squadGrant(windup + count * config["burst_interval"].get_or(0.07f) + 0.2f, false, true)) {
            ec.fireTimer = m_sqCfg.retryDelay * (0.6f + (rand() % 80) / 100.f);
            return;
        }

        ec.burstCharging = true;
        ec.telegraphDir = aimNow();
        ec.telegraphDuration = windup;            // fixed: telegraphs are never scaled
        ec.telegraphTimer = windup;
        ec.telegraphActive = true;
    }

    void fireShot(size_t i, const TransformComponent& tf, EnemyComponent& ec,
        uint32_t entityId, sf::Vector2f dir, sol::table& config)
    {
        const float bulletSpeed = config["bullet_speed"].get_or(550.f);
        const float angle = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        // Clear of the hull whatever the size: 35 was tuned on the old Raider
        // (r 29); the +30% hull (r 38) would have spawned rounds inside it.
        const sf::Vector2f spawnPos = tf.position + dir * std::max(35.f, m_curRadius + 8.f);

        const uint32_t bid = m_ef->createEnemyBullet(*m_em, spawnPos, dir * bulletSpeed,
            angle, entityId, *m_lua, m_worldId, config);

        // Feral fire hurts enemy hulls too (DamageSystem reads feralMult).
        if (ec.feralTimer > 0.f) {
            const size_t bi = m_em->getEntityIndex(bid);
            if (bi != (size_t)-1) m_em->bullets[bi].feralMult = ec.feralDamage;
        }

        m_em->spawnImpact(spawnPos, sf::Color(255, 120, 0), dir * -200.f);

        b2Body_ApplyLinearImpulseToCenter(m_em->physics[i].bodyId,
            { -dir.x * 2.5f, -dir.y * 2.5f }, true);
    }

    void opportunisticAsteroidShot(float dt, size_t i, const TransformComponent& tf,
        EnemyComponent& ec, uint32_t entityId,
        b2BodyId bodyId, sol::table& config)
    {
        ec.asteroidShotTimer -= dt;
        if (ec.asteroidShotTimer > 0.f) return;
        if (ec.telegraphActive) return;

        const sf::Vector2f enemyPos = tf.position;
        const b2Vec2 ev = b2Body_GetLinearVelocity(bodyId);
        sf::Vector2f movDir(ev.x, ev.y);
        const float ml = std::sqrt(movDir.x * movDir.x + movDir.y * movDir.y);
        if (ml < 0.5f) return;
        movDir /= ml;

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = bodyUD(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;

            sf::Vector2f toAst = m_em->transforms[j].position - enemyPos;
            const float d = std::sqrt(toAst.x * toAst.x + toAst.y * toAst.y);
            if (d > 200.f || d < 0.01f) continue;

            const sf::Vector2f n = toAst / d;
            if (movDir.x * n.x + movDir.y * n.y < 0.75f) continue;

            ec.asteroidShotTimer = config["asteroid_shot_cooldown"].get_or(2.2f)
                + (rand() % 100) / 100.f;

            if (rand() % 100 >= static_cast<int>(config["asteroid_shot_chance"].get_or(35.f)))
                return;

            const float bulletSpeed = config["bullet_speed"].get_or(550.f);
            const float angle = std::atan2(n.y, n.x) * 180.f / 3.14159f + 90.f;
            m_ef->createEnemyBullet(*m_em, enemyPos + n * 35.f, n * bulletSpeed,
                angle, entityId, *m_lua, m_worldId, config);
            return;
        }
    }

    // ========================================================================
    // BULLET STORM
    // ========================================================================
    void updateBulletStorm(float dt, size_t i, TransformComponent& tf,
        EnemyComponent& ec, AIState& ai, b2BodyId bodyId,
        sol::table& config)
    {
        if (ec.stormActive) {
            ec.stormTimer -= dt;
            const float u = 1.f - std::clamp(ec.stormTimer / std::max(0.01f, ec.stormDuration), 0.f, 1.f);

            const float peak = config["storm_spin_speed"].get_or(760.f);
            ec.stormSpin = peak * (0.35f + 0.65f * std::sin(std::clamp(u, 0.f, 1.f) * 3.14159f));

            tf.rotation += ec.stormSpin * dt;
            while (tf.rotation > 360.f) tf.rotation -= 360.f;
            while (tf.rotation < 0.f)   tf.rotation += 360.f;
            b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
                b2MakeRot(tf.rotation * 3.14159f / 180.f));

            {
                const b2Vec2 sv = b2Body_GetLinearVelocity(bodyId);
                const float brake = std::exp(-config["storm_brake_rate"].get_or(7.0f) * dt);
                b2Body_SetLinearVelocity(bodyId, { sv.x * brake, sv.y * brake });
            }

            ec.stormFireTimer -= dt;
            if (ec.stormFireTimer <= 0.f) {
                ec.stormFireTimer = config["storm_fire_interval"].get_or(0.13f);

                const float r = (tf.rotation - 90.f) * 3.14159f / 180.f;
                const float scatter = ((rand() % 40) - 20) * 3.14159f / 180.f;
                const sf::Vector2f dir(std::cos(r + scatter), std::sin(r + scatter));

                fireShot(i, tf, ec, tf.entityId, dir, config);
            }

            const float w = 0.10f * std::sin(u * 40.f);
            tf.visualScale.x *= 1.f + w;
            tf.visualScale.y *= 1.f - w;

            if (ec.stormTimer <= 0.f) {
                ec.stormActive = false;
                ec.stormRecoverTimer = config["storm_recover_time"].get_or(1.6f);
                m_em->spawnShockRing(tf.position, 10.f, 130.f, 0.35f,
                    sf::Color(255, 160, 60), 4.f, 220.f);
            }
            return;
        }

        ec.stormRecoverTimer -= dt;
        ec.telegraphActive = false;

        const float u = std::clamp(ec.stormRecoverTimer /
            std::max(0.01f, config["storm_recover_time"].get_or(1.6f)), 0.f, 1.f);

        tf.visualOffsetAngle += 14.f * u * std::sin(ec.stormRecoverTimer * 16.f);

        b2Body_SetAngularVelocity(bodyId, 0.f);

        {
            const b2Vec2 rv = b2Body_GetLinearVelocity(bodyId);
            const float drift = std::exp(-1.2f * dt);
            b2Body_SetLinearVelocity(bodyId, { rv.x * drift, rv.y * drift });
        }

        if (ec.stormRecoverTimer <= 0.f) {
            ec.stormRecoverTimer = 0.f;
            ai.stormCooldown = config["storm_cooldown"].get_or(12.f);
            ai.stormUrge = 0.f;
        }
    }

    void considerBulletStorm(float dt, size_t i, const sf::Vector2f& enemyPos,
        AIState& ai, EnemyComponent& ec, sol::table& config)
    {
        (void)i;

        if (ai.stormCooldown > 0.f) {
            ai.stormCooldown -= dt;
            ai.stormUrge = 0.f;
            return;
        }
        if (ai.currentState != EnemyState::COMBAT) {
            ai.stormUrge = std::max(0.f, ai.stormUrge - dt);
            return;
        }

        int nearby = 0;
        const float r = config["storm_asteroid_radius"].get_or(340.f);
        const float r2 = r * r;
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = bodyUD(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;
            const sf::Vector2f d = m_em->transforms[j].position - enemyPos;
            if (d.x * d.x + d.y * d.y < r2) ++nearby;
        }

        const float base = config["storm_base_rate"].get_or(0.25f);
        const float perRock = config["storm_rock_rate"].get_or(0.22f);
        const float rockBonus = std::min(perRock * nearby, config["storm_rock_cap"].get_or(1.1f));

        ai.stormUrge += dt * (base + rockBonus);

        if (nearby >= 2) {
            ai.panicLevel = std::min(1.f, ai.panicLevel + dt * (0.25f + 0.12f * nearby));
        }

        const float threshold = config["storm_urge_threshold"].get_or(2.2f)
            * (0.7f + ai.aggression * 0.6f);

        if (ai.stormUrge >= threshold && squadGrant(config["storm_duration"].get_or(1.7f))) {
            ec.stormActive = true;
            ec.stormDuration = config["storm_duration"].get_or(1.7f);
            ec.stormTimer = ec.stormDuration;
            ec.stormFireTimer = 0.f;
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            ai.stormUrge = 0.f;
            ai.panicLevel = 1.f;

            ec.alertIcon = AlertIcon::Spotted;
            ec.alertIconDuration = 0.7f;
            ec.alertIconTimer = 0.7f;

            m_em->spawnShockRing(enemyPos, 8.f, 95.f, 0.30f,
                sf::Color(255, 90, 40), 3.f, 235.f);
            m_em->addTrauma(0.14f);
        }
    }

    // ========================================================================
    // STAGGER
    // ========================================================================
    /// A stagger or stun breaks a summon charge (nothing comes out) and a
    /// mine toss (mines already thrown stay thrown).
    void breakSummonAndToss(EnemyComponent& ec, const sol::table& cfg) {
        if (ec.summonState == 1)
            ec.summonCooldown = std::max(ec.summonCooldown, cfg["summon_fail_cooldown"].get_or(5.f));
        if (ec.summonState != 0) { ec.summonState = 0; ec.summonTimer = 0.f; }
        if (ec.tossState != 0) {
            ec.tossState = 0;
            ec.tossCooldown = std::max(ec.tossCooldown, 3.f);
        }
    }

    bool updateStagger(float dt, size_t i, TransformComponent& tf, EnemyComponent& ec) {
        if (ec.staggerTimer > 0.f) {
            if (ec.summonState != 0 || ec.tossState != 0)
                breakSummonAndToss(ec, m_registry->resolve(ec.archetype).config);
            ec.staggerTimer = std::max(0.f, ec.staggerTimer - dt);
            const float u = 1.f - (ec.staggerTimer / std::max(0.0001f, ec.staggerDuration));

            const float decay = std::exp(-2.6f * u);
            tf.rotation += ec.staggerSpinSpeed * decay * dt;
            while (tf.rotation > 360.f) tf.rotation -= 360.f;
            while (tf.rotation < 0.f)   tf.rotation += 360.f;

            const b2BodyId body = m_em->physics[i].bodyId;
            b2Body_SetTransform(body, b2Body_GetPosition(body),
                b2MakeRot(tf.rotation * 3.14159f / 180.f));

            const float w = 0.07f * std::sin(u * 34.f);
            tf.visualScale.x *= 1.f + w;
            tf.visualScale.y *= 1.f - w;

            if ((rand() % 100) < 40) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({
                    tf.position + sf::Vector2f(std::cos(a), std::sin(a)) * 12.f,
                    sf::Vector2f(std::cos(a), std::sin(a)) * 40.f,
                    sf::Color(255, static_cast<uint8_t>(110 + rand() % 60), 40, 190),
                    0.32f, 0.55f,
                    2.f + rand() % 3
                    });
            }

            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;

            if (ec.staggerTimer <= 0.f) {
                ec.staggerRecoverTimer = ec.staggerRecoverDuration;
            }
            return true;
        }

        if (ec.staggerRecoverTimer > 0.f) {
            ec.staggerRecoverTimer = std::max(0.f, ec.staggerRecoverTimer - dt);
            const float u = ec.staggerRecoverTimer /
                std::max(0.0001f, ec.staggerRecoverDuration);
            tf.visualOffsetAngle += 9.f * u * std::sin(ec.staggerRecoverTimer * 18.f);
        }
        return false;
    }

    // ========================================================================
    // ROTATION + IDLE ANIMATION (with broadside support)
    // ========================================================================
    void updateRotation(float dt, TransformComponent& tf, EnemyComponent& ec,
        AIState& ai, sf::Vector2f desiredVel, sf::Vector2f toPlayer,
        b2BodyId bodyId, sol::table& config)
    {
        float targetAngle = tf.rotation;

        // Start with velocity‑based heading (used for naval units)
        if (std::abs(desiredVel.x) > 10.f || std::abs(desiredVel.y) > 10.f) {
            targetAngle = std::atan2(desiredVel.y, desiredVel.x) * 180.f / 3.14159f + 90.f;
        }

        const bool faceTarget =
            config["facing_mode"].get_or<std::string>("target") != "velocity";

        if (ai.currentState == EnemyState::COMBAT && faceTarget) {
            targetAngle = std::atan2(toPlayer.y, toPlayer.x) * 180.f / 3.14159f + 90.f;
            targetAngle += std::sin(m_noiseTime * 4.f + ai.jitterPhase) * 6.f;
        }
        else if (ai.currentState == EnemyState::ALERT) {
            ai.lookAroundTimer -= dt;
            if (ai.lookAroundTimer <= 0.f) {
                ai.lookAroundTimer = 0.7f + (rand() % 60) / 100.f;
                ai.lookAroundAngle = ((rand() % 2) ? 1.f : -1.f) *
                    (35.f + rand() % 30);
            }
            targetAngle += ai.lookAroundAngle *
                std::sin(ai.lookAroundTimer * 3.2f);
        }

        float rotSpeed = config["rotation_speed"].get_or(4.0f);
        if (ai.currentState == EnemyState::ALERT)  rotSpeed *= 1.5f;
        if (ai.currentState == EnemyState::COMBAT && faceTarget) rotSpeed *= 1.8f;

        float delta = targetAngle - tf.rotation;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;

        tf.rotation += delta * rotSpeed * dt;
        b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
            b2MakeRot(tf.rotation * 3.14159f / 180.f));

        // Idle bob
        const float pivotY = -18.f;
        if (ai.currentState == EnemyState::PATROL) {
            tf.visualOffsetAngle += 3.f * std::sin(m_noiseTime * 1.6f + ai.jitterPhase);
            tf.visualPivot = { 0.f, pivotY };
        }
        else if (ai.currentState == EnemyState::ALERT) {
            tf.visualOffsetAngle += 6.f * std::sin(m_noiseTime * 6.5f + ai.jitterPhase);
            tf.visualPivot = { 0.f, pivotY };
        }

        // Dodge burst bank
        if (ai.dodgeBurstTimer > 0.f && ai.dodgeBurstDuration > 0.f) {
            const float u = 1.f - (ai.dodgeBurstTimer / ai.dodgeBurstDuration);

            const float r = tf.rotation * 3.14159f / 180.f;
            const sf::Vector2f right(std::cos(r), std::sin(r));
            const sf::Vector2f fwd(std::sin(r), -std::cos(r));
            const float side = ai.dodgeBurstDir.x * right.x + ai.dodgeBurstDir.y * right.y;
            const float ahead = ai.dodgeBurstDir.x * fwd.x + ai.dodgeBurstDir.y * fwd.y;

            float k;
            if (u < 0.22f) {
                const float t = u / 0.22f;
                k = t * t * (3.f - 2.f * t);
            }
            else {
                const float t = (u - 0.22f) / 0.78f;
                k = (1.f - t) * std::cos(t * 3.14159f * 1.4f);
            }

            const float bankAngle = config["dodge_bank_angle"].get_or(34.f);
            tf.visualOffsetAngle += -side * bankAngle * k;
            tf.visualPivot = { 0.f, -18.f };

            tf.visualScale.y *= 1.f + 0.14f * std::max(0.f, k) * std::abs(ahead);
            tf.visualScale.x *= 1.f + 0.12f * std::max(0.f, k) * std::abs(side);

            if (u < 0.5f && (rand() % 100) < 55) {
                const sf::Vector2f vent = tf.position - ai.dodgeBurstDir * 16.f;
                m_em->particles.push_back({
                    vent,
                    -ai.dodgeBurstDir * (110.f + rand() % 90),
                    sf::Color(255, static_cast<uint8_t>(170 + rand() % 60), 90, 210),
                    0.22f, 0.28f,
                    2.f + rand() % 3
                    });
            }
        }

        // Flinch twitch
        if (ai.flinchTimer > 0.f && ai.flinchDuration > 0.f) {
            const float u = ai.flinchTimer / ai.flinchDuration;
            tf.visualOffsetAngle += 13.f * std::sin(u * 3.14159f * 3.f) * u;
            tf.visualPivot = { 0.f, -18.f };
        }

        // Burst pause: a visible breather, so the gap in the fire reads as the
        // unit recovering rather than as the AI losing interest.
        if (ec.shotPauseTimer > 0.f) {
            tf.visualOffsetAngle += 5.f * std::sin(ec.shotPauseTimer * 13.f);
            tf.visualPivot = { 0.f, pivotY };
        }

        // Telegraph
        if (ec.telegraphActive && ec.telegraphDuration > 0.f) {
            const float u = 1.f - (ec.telegraphTimer / ec.telegraphDuration);
            const float pull = std::sin(u * 3.14159f * 0.5f);
            tf.visualScale.y *= 1.f - 0.14f * pull;
            tf.visualScale.x *= 1.f + 0.12f * pull;
        }
    }

    // ========================================================================
    // HOMING ASTEROIDS (unchanged)
    // ========================================================================
    void updateHomingAsteroids(float dt) {
        (void)dt;
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Asteroid) continue;

            auto& health = m_em->healths[i];
            if (!health.isHoming || health.homingTargetEntityId == 0) continue;

            const size_t targetIdx = m_em->getEntityIndex(health.homingTargetEntityId);
            if (targetIdx == (size_t)-1) { health.isHoming = false; continue; }

            BodyUserData* tud = bodyUD(m_em->physics[targetIdx].bodyId);
            if (!tud || tud->type != BodyType::Enemy) { health.isHoming = false; continue; }

            const sf::Vector2f aPos = m_em->transforms[i].position;
            sf::Vector2f toTarget = m_em->transforms[targetIdx].position - aPos;
            const float len = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
            if (len > 0.01f) toTarget /= len;

            const float turnRate = (*m_lua)["homing_turn_rate"].get_or(3.0f);
            const b2Vec2 cv = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
            const sf::Vector2f desired(toTarget.x * 800.f, toTarget.y * 800.f);

            b2Body_ApplyForceToCenter(m_em->physics[i].bodyId,
                { (desired.x / SCALE - cv.x) * turnRate,
                  (desired.y / SCALE - cv.y) * turnRate }, true);

            if (rand() % 3 == 0) {
                m_em->spawnImpact(aPos, homingColor(150),
                    sf::Vector2f(-toTarget.x * 200.f, -toTarget.y * 200.f));
            }
        }
    }

    // ========================================================================
    // CHAOS DODGE (unchanged)
    // ========================================================================
    void updateChaosDodge(float dt, AIState& ai, EnemyComponent& ec,
        sf::Vector2f toPlayerN, float dist, sol::table& config)
    {
        if (ai.currentState != EnemyState::COMBAT) {
            ai.chaosDodgeTimer = 0.f;
            return;
        }

        ai.panicLevel = std::max(0.f, ai.panicLevel - dt * 0.35f);

        ai.chaosDodgeTimer -= dt;
        if (ai.chaosDodgeTimer > 0.f) return;

        if (ai.threatSeenTimer > 0.f) return;
        if (ai.dodgeCooldown > 0.f) return;
        if (ai.dodgeBurstTimer > 0.f || ai.flinchTimer > 0.f) return;

        const float baseInterval = config["chaos_dodge_interval"].get_or(5.5f);
        const float rate = baseInterval / (0.75f + ai.aggression * 0.35f + ai.panicLevel * 0.9f);
        ai.chaosDodgeTimer = rate * (0.7f + (rand() % 60) / 100.f);

        const float chance = config["chaos_dodge_chance"].get_or(0.40f) + ai.panicLevel * 0.3f;
        if ((rand() % 100) / 100.f > chance) return;

        const sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
        const int roll = rand() % 100;

        sf::Vector2f dir;
        if (roll < 60)      dir = perp * ((rand() % 2) ? 1.f : -1.f);
        else if (roll < 82) dir = -toPlayerN;
        else if (roll < 94) dir = toPlayerN;
        else {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            dir = { std::cos(a), std::sin(a) };
        }

        if (dist < 120.f && (dir.x * toPlayerN.x + dir.y * toPlayerN.y) > 0.5f) {
            dir = perp * ((rand() % 2) ? 1.f : -1.f);
        }

        triggerDodgeBurst(ai, ec, dir, 0.85f + ai.panicLevel * 0.3f);
    }

    // ========================================================================
    // CACHE MANAGEMENT
    // ========================================================================
    void pruneCache() {
        if (m_aiCache.size() < 64) return;
        for (auto it = m_aiCache.begin(); it != m_aiCache.end(); ) {
            it = (m_em->getEntityIndex(it->first) == (size_t)-1)
                ? m_aiCache.erase(it) : std::next(it);
        }
    }

    // ========================================================================
    // MEMBERS
    // ========================================================================
    float m_dodgeDuration = 0.42f;
    float m_dodgeCooldown = 2.0f;
    float m_dodgeManoeuvreTime = 0.40f;
    float m_dodgeSpeed = 620.f;

    b2BodyId m_dodgeBodyId = b2_nullBodyId;
    sf::Vector2f m_dodgePos;

    std::unordered_map<uint32_t, float> m_pendingTTI;

    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId m_worldId;
    /// A hijacked or parried rock is the PLAYER's projectile, so its trail
    /// wears the player's paint, not a hard-coded teal.
    sf::Color homingColor(std::uint8_t alpha) const {
        const size_t p = m_em ? m_em->getEntityIndex(m_playerEntityId) : (size_t)-1;
        if (p == (size_t)-1 || p >= m_em->players.size()) return sf::Color(0, 255, 200, alpha);
        const sf::Color c = m_em->players[p].livery.paint.homing;
        return sf::Color(c.r, c.g, c.b, alpha);
    }

    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    DevState* m_dev = nullptr;

    float m_noiseTime = 0.f;
    sf::Vector2f m_playerVel;            ///< This frame's player velocity, px/s
    bool m_playerDashing = false;        ///< Player dodge running / just spent

    // ---- Pack (notes 32-35) ----
    struct PackSource {
        uint32_t id; sf::Vector2f pos; float radius, bonus, escort, leash;
        uint8_t takes[4]; uint8_t takeCount;   ///< escort_takes filter; 0 = anyone
    };
    std::vector<PackSource> m_packSources;   ///< Aura units alive this frame
    bool         m_vsShip = false;       ///< This unit is feral and its target is a ship
    bool         m_hasEscort = false;    ///< This unit has a Bloodseeker to escort
    sf::Vector2f m_escortPos;
    float        m_escortLeash = 0.f;    ///< >0: stay this close to the leader in COMBAT too
    float        m_curRadius = 30.f;     ///< This unit's hull radius (muzzle offset)
    float        m_clock = 0.f;          ///< Seconds, for slow drifts (escort ring)

    // ---- Squad (notes 42-47) ----
    SquadCfg     m_sqCfg;
    std::unordered_map<uint32_t, SquadSlot> m_sqSlots;
    std::unordered_map<uint32_t, TurnInfo>  m_sqTurns;   ///< Who holds an attack turn
    std::unordered_map<uint32_t, float>     m_sqRest;    ///< Post-turn rest
    std::unordered_map<uint32_t, float>     m_sqWaited;  ///< Seconds since its last turn
    std::unordered_map<uint32_t, MoodInfo>  m_sqMood;    ///< Maniac moods
    std::vector<SquadMem> m_sqScratch;
    std::vector<uint32_t> m_sqHeavyScratch;
    std::unordered_map<uint32_t, float> m_sqHeavyDist;
    std::vector<sf::Vector2f> m_sqSeers;                 ///< Members with eyes on the player
    int          m_sqCap = 2;            ///< Turns allowed at once (re-rolled 1 / 2)
    float        m_sqBreather = 0.f;     ///< >0: the player just went down
    bool         m_sqPlayerTumbling = false;
    // this unit, this frame
    SquadRole    m_sqRole = SquadRole::None;
    SquadRole    m_sqBaseRole = SquadRole::None;
    bool         m_sqOn = false;
    bool         m_sqHasTurn = false;
    uint32_t     m_sqSelf = 0;
    SquadSlot    m_sqSlot;
    float        m_curDistToPlayer = 0.f;
    std::unordered_map<uint32_t, AIState> m_aiCache;
};