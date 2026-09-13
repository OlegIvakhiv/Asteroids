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
 * State split: this system owns AIState (decisions). Presentation and impact
 * state live on EnemyComponent so DamageSystem and RenderSystem can reach them.
 *
 * @author Oleg Ivakhiv
 * @version 2.1
 */

#pragma once

#include "ISystem.hpp"
#include "utils/components.hpp"
#include "core/EnemyArchetypes.hpp"
#include <unordered_map>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <string>

class AISystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;
        if (!m_registry || m_registry->empty()) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;
        const b2Vec2 pvb = b2Body_GetLinearVelocity(m_em->physics[playerIdx].bodyId);
        const sf::Vector2f playerVel(pvb.x * SCALE, pvb.y * SCALE);
        m_playerVel = playerVel;   // pickManeuver needs it to choose a side

        updateHomingAsteroids(dt);

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
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

            // ---- Personality rolled AFTER config is loaded ----
            if (!ai.initialised) rollPersonality(ai, entityId, config);

            const float enginePower = config["engine_power"].get_or(200.0f);
            const float maxSpeed = config["max_speed"].get_or(20.0f);
            m_dodgeDuration = config["dodge_duration"].get_or(0.42f);
            m_dodgeCooldown = config["dodge_cooldown"].get_or(1.1f);
            m_dodgeManoeuvreTime = config["dodge_manoeuvre_time"].get_or(0.25f);
            m_dodgeSpeed = config["dodge_speed"].get_or(620.f);

            tf.visualOffsetAngle = 0.f;
            tf.visualPivot = { 0.f, 0.f };
            tf.visualScale = { 1.f, 1.f };

            tickTimers(dt, ec);

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

            // ================================================================
            // RAM CHARGE — owns movement completely while active
            // ================================================================
            if (updateRam(dt, i, tf, ec, ai, bodyId, config,
                playerPos, distToPlayer, toPlayerN)) continue;

            // ---- Stun (after ram, because ram cannot be stunned) ----
            if (health.stunTimer > 0.f) {
                health.stunTimer -= dt;
                ec.telegraphActive = false;
                ec.telegraphTimer = 0.f;
                ec.bashState = BashState::None;     // Stun breaks any melee commit
                ec.bashStrikePending = false;
                const float w = 0.06f * std::sin(health.stunTimer * 40.f);
                tf.visualScale.x *= 1.f + w;
                tf.visualScale.y *= 1.f - w;
                continue;
            }

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
            const bool sees = canSee(tf, enemyPos, toPlayerN, distToPlayer, health, ai, config);
            updatePerception(dt, sees, playerPos, playerVel, distToPlayer, ai, ec, tf, config);

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

            const sf::Vector2f finalDesiredVel = ai.smoothedDesiredVel + avoidance;

            // ================================================================
            // STEERING — mass‑compensated
            // ================================================================
            const b2Vec2 currentVel = b2Body_GetLinearVelocity(bodyId);
            b2Vec2 impulse = { finalDesiredVel.x / SCALE - currentVel.x,
                               finalDesiredVel.y / SCALE - currentVel.y };

            float gain = 50.f;
            float maxForce = enginePower * dt;

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
        if (ec.bashCooldown > 0.f) ec.bashCooldown = std::max(0.f, ec.bashCooldown - dt);
        if (ec.shotPauseTimer > 0.f) ec.shotPauseTimer = std::max(0.f, ec.shotPauseTimer - dt);
        if (ec.shotClearTimer > 0.f) ec.shotClearTimer = std::max(0.f, ec.shotClearTimer - dt);

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
        if (ec.ramCooldown > 0.f) ec.ramCooldown -= dt;

        switch (ec.ramState) {

        case RamState::None: {
            if (!config["ram_enabled"].get_or(false)) return false;
            if (ec.ramCooldown > 0.f) return false;
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.bashState != BashState::None) return false;   // one commit at a time
            if (ec.shotClearTimer > 0.f) return false;           // see updateBash

            // Barge: fires when you are too close OR too far.
            // Berserker: near = 0 (the bash owns close range) and a max, so it
            // charges across the MID band -- "long-mid range" per the roster.
            const float farT = config["ram_far_trigger"].get_or(700.f);
            const float nearT = config["ram_near_trigger"].get_or(210.f);
            const float maxT = config["ram_max_trigger"].get_or(1.0e9f);
            if (dist < nearT || (dist > farT && dist < maxT)) {
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
                m_em->particles.push_back({
                    m_em->nextEntityId++, jet,
                    ec.ramDir * -(60.f + rand() % 120),
                    sf::Color(255, static_cast<uint8_t>(120 + rand() % 80), 60, 230),
                    0.30f, 0.34f, 3.f + rand() % 4 });
            }

            if (ec.ramTimer <= 0.f) {
                ec.ramState = RamState::Charge;
                ec.ramDuration = config["ram_charge_duration"].get_or(1.25f);
                ec.ramTimer = ec.ramDuration;
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
                m_em->particles.push_back({
                    m_em->nextEntityId++, tf.position + side,
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
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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
            if (ai.currentState != EnemyState::COMBAT) return false;
            if (ec.ramState != RamState::None) return false;
            if (dist > config["bash_trigger_range"].get_or(150.f)) return false;
            // Own rounds still in the air: wait. A lunge arriving alongside
            // its own bullets asks for a dodge and a parry in the same beat.
            if (ec.shotClearTimer > 0.f) return false;

            ec.bashState = BashState::Windup;
            ec.bashDuration = config["bash_windup"].get_or(0.38f);
            ec.bashTimer = ec.bashDuration;
            ec.bashDir = toPlayerN;
            ec.bashConnected = false;
            ec.bashStrikePending = false;

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
                m_em->particles.push_back({
                    m_em->nextEntityId++, at,
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
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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
                BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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
                    BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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
                    m_em->nextEntityId++,
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
        ec.fireTimer -= dt;

        if (ai.currentState != EnemyState::COMBAT) {
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            return;
        }

        const float attackRange = config["attack_range"].get_or(480.f);
        const float fireRate = config["fire_rate"].get_or(1.8f);
        const float telegraph = config["telegraph_time"].get_or(0.32f);

        // ---- HOLD FIRE ----
        // Inside this radius the gun is simply off. A melee unit that keeps
        // spraying while it closes forces the player to dodge a bullet and
        // parry a lunge on the same beat, and neither read survives that.
        // One threat at a time is the whole point of the unit.
        const float holdFire = config["hold_fire_range"].get_or(0.f);
        if (holdFire > 0.f && dist < holdFire) {
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
        ec.telegraphTimer = ec.telegraphDuration;
        ec.telegraphActive = true;
    }

    void fireShot(size_t i, const TransformComponent& tf, EnemyComponent& ec,
        uint32_t entityId, sf::Vector2f dir, sol::table& config)
    {
        const float bulletSpeed = config["bullet_speed"].get_or(550.f);
        const float angle = std::atan2(dir.y, dir.x) * 180.f / 3.14159f + 90.f;
        const sf::Vector2f spawnPos = tf.position + dir * 35.f;

        m_ef->createEnemyBullet(*m_em, spawnPos, dir * bulletSpeed,
            angle, entityId, *m_lua, m_worldId, config);

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
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
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

        if (ai.stormUrge >= threshold) {
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
    bool updateStagger(float dt, size_t i, TransformComponent& tf, EnemyComponent& ec) {
        if (ec.staggerTimer > 0.f) {
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
                    m_em->nextEntityId++,
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
                    m_em->nextEntityId++,
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
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Asteroid) continue;

            auto& health = m_em->healths[i];
            if (!health.isHoming || health.homingTargetEntityId == 0) continue;

            const size_t targetIdx = m_em->getEntityIndex(health.homingTargetEntityId);
            if (targetIdx == (size_t)-1) { health.isHoming = false; continue; }

            BodyUserData* tud = (BodyUserData*)b2Body_GetUserData(m_em->physics[targetIdx].bodyId);
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
                m_em->spawnImpact(aPos, sf::Color(0, 255, 200, 150),
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
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;

    float m_noiseTime = 0.f;
    sf::Vector2f m_playerVel;            ///< This frame's player velocity, px/s
    std::unordered_map<uint32_t, AIState> m_aiCache;
};