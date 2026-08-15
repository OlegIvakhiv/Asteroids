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
 * State split: this system owns AIState (decisions). Presentation and impact
 * state live on EnemyComponent so DamageSystem and RenderSystem can reach them.
 *
 * @author Oleg Ivakhiv
 * @version 2.0
 */

#pragma once

#include "ISystem.hpp"
#include "utils/components.hpp"
#include <unordered_map>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class AISystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
    }

    void update(float dt) override {
        if (!m_em || !m_lua) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;
        const b2Vec2 pvb = b2Body_GetLinearVelocity(m_em->physics[playerIdx].bodyId);
        const sf::Vector2f playerVel(pvb.x * SCALE, pvb.y * SCALE);

        sol::table config = (*m_lua)["enemy_config"];
        const float enginePower = config["engine_power"].get_or(200.0f);
        const float maxSpeed = config["max_speed"].get_or(20.0f);
        m_dodgeDuration = config["dodge_duration"].get_or(0.42f);
        // 1.1s, not 2.0s. The cooldown is shared by reactive dodges and idle
        // jukes; at 2.0-3.2s the pirate simply had no budget left to react with.
        // Idle jukes are now throttled by their own chaos_dodge_interval.
        m_dodgeCooldown = config["dodge_cooldown"].get_or(1.1f);
        // 0.25, not 0.40. At 0.40 a player bullet (0.52s of warning at the new
        // 420px notice range, minus a 0.28s reaction) could NEVER be dodged --
        // pirates could only ever flinch at gunfire. Kinetic rocks stay
        // undodgeable via homing_dodge_penalty instead, which is the honest
        // reason: they steer, so a sidestep doesn't shake them.
        m_dodgeManoeuvreTime = config["dodge_manoeuvre_time"].get_or(0.25f);
        m_dodgeSpeed = config["dodge_speed"].get_or(620.f);

        updateHomingAsteroids(dt);
        // updateRotation
         // ====================================================================
         // MAIN ENEMY LOOP
         // ====================================================================
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;

            auto& tf = m_em->transforms[i];
            auto& health = m_em->healths[i];
            auto& ec = m_em->enemies[i];
            const uint32_t entityId = tf.entityId;
            auto& ai = m_aiCache[entityId];
            const b2BodyId bodyId = m_em->physics[i].bodyId;

            if (!ai.initialised) rollPersonality(ai, entityId);

            // Reset per-frame visual offsets; everything below is additive.
            tf.visualOffsetAngle = 0.f;
            tf.visualPivot = { 0.f, 0.f };
            tf.visualScale = { 1.f, 1.f };

            tickTimers(dt, ec);

            // ================================================================
            // STAGGER — owns rotation completely, blocks everything
            // ================================================================
            if (updateStagger(dt, i, tf, ec)) continue;

            // Stun (from a rift overload or a parry) still freezes the brain,
            // but the visual state must keep updating or the enemy freezes
            // mid-telegraph with its wind-up glow stuck on.
            if (health.stunTimer > 0.f) {
                health.stunTimer -= dt;
                ec.telegraphActive = false;
                ec.telegraphTimer = 0.f;
                const float w = 0.06f * std::sin(health.stunTimer * 40.f);
                tf.visualScale.x *= 1.f + w;
                tf.visualScale.y *= 1.f - w;
                continue;
            }

            const sf::Vector2f enemyPos = tf.position;
            sf::Vector2f toPlayer = playerPos - enemyPos;
            const float distToPlayer = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);
            const sf::Vector2f toPlayerN = (distToPlayer > 0.01f)
                ? sf::Vector2f(toPlayer.x / distToPlayer, toPlayer.y / distToPlayer)
                : sf::Vector2f(0.f, -1.f);

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
            // BULLET STORM CONSIDERATION (throttled)
            //
            // considerBulletStorm does an O(n) asteroid scan, so running it for
            // every enemy every frame is wasteful — 6 pirates and 60 rocks is
            // 360 distance checks per frame for a decision that changes on a
            // multi-second timescale. The accumulated dt keeps the urge maths
            // correct at the slower cadence.
            // ================================================================
            ai.stormScanTimer -= dt;
            if (ai.stormScanTimer <= 0.f) {
                const float elapsed = 0.25f + (rand() % 10) / 100.f;
                considerBulletStorm(elapsed, i, enemyPos, ai, ec, config);
                ai.stormScanTimer = elapsed;
            }
            // NOTE: stormCooldown is decremented ONLY inside
            // considerBulletStorm, using the accumulated `elapsed` above. It
            // used to ALSO be decremented here every non-scan frame, so over
            // any 0.25s window it burned ~0.48s of cooldown -- draining roughly
            // twice as fast as configured. storm_cooldown = 12 behaved like ~6.

            // A storm may have just started this frame. Hand off immediately
            // rather than running one frame of normal behaviour on top of it.
            if (ec.stormActive) {
                updateBulletStorm(dt, i, tf, ec, ai, bodyId, config);
                continue;
            }

            // triggerDodgeBurst applies a real velocity change, so it needs
            // this enemy's body. Bind BEFORE anything that can trigger a dodge.
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

            // REACTIVE dodging runs FIRST and gets first claim on the dodge
            // cooldown. It used to run AFTER updateChaosDodge, so the proactive
            // juke -- which fired on roughly the same period as the cooldown --
            // consumed the budget every time and the enemy almost never got to
            // evade an actual bullet.
            sf::Vector2f avoidance = computeAvoidance(dt, i, enemyPos, entityId, ai, ec, maxSpeed, config);

            // ---- Proactive chaotic juke: only with the leftovers ----
            updateChaosDodge(dt, ai, ec, toPlayerN, distToPlayer, config);

            // ---- Evasive burst sustain ----
            // The dash velocity was already SET in triggerDodgeBurst. This is
            // only a decaying nudge so drag doesn't kill it instantly; a full
            // steering target here would fight the velocity set and produce
            // the mushy drift the burst is meant to replace.
            if (ai.dodgeBurstTimer > 0.f) {
                ai.dodgeBurstTimer -= dt;
                const float u = ai.dodgeBurstTimer / std::max(0.01f, ai.dodgeBurstDuration);
                avoidance += ai.dodgeBurstDir * (maxSpeed * 4.f * u);
            }

            const sf::Vector2f finalDesiredVel = ai.smoothedDesiredVel + avoidance;

            // ================================================================
            // STEERING
            // ================================================================
            const b2Vec2 currentVel = b2Body_GetLinearVelocity(bodyId);
            b2Vec2 impulse = { finalDesiredVel.x / SCALE - currentVel.x,
                               finalDesiredVel.y / SCALE - currentVel.y };

            const float impulseLen = std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y);
            const float maxForce = enginePower * dt;
            if (impulseLen > maxForce) {
                const float s = maxForce / impulseLen;
                impulse.x *= s; impulse.y *= s;
            }
            b2Body_ApplyForceToCenter(bodyId, { impulse.x * 50.f, impulse.y * 50.f }, true);

            // ================================================================
            // SHOOTING
            // ================================================================
            updateShooting(dt, i, tf, ec, ai, entityId, playerPos, playerVel,
                distToPlayer, config);

            opportunisticAsteroidShot(dt, i, tf, ec, entityId, bodyId, config);

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
    //
    // Seeded from entityId so a given pirate is consistent for its whole life,
    // but different from its neighbours. Without this, a squad of three moves
    // as one object and immediately reads as scripted.
    // ========================================================================
    void rollPersonality(AIState& ai, uint32_t entityId) {
        const uint32_t h = entityId * 2654435761u;
        auto frac = [&](int shift) {
            return static_cast<float>((h >> shift) & 0xFF) / 255.f;
        };

        ai.preferredRange = 220.f + frac(0) * 220.f;     // 220 .. 440 px
        ai.aggression = 0.25f + frac(8) * 0.65f;    // 0.25 .. 0.90
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
    }

    // ========================================================================
    // PERCEPTION — three senses feeding one signal
    // ========================================================================
    bool canSee(const TransformComponent& tf, sf::Vector2f enemyPos,
        sf::Vector2f toPlayerN, float dist,
        const HealthComponent& health, AIState& ai, sol::table& config)
    {
        // enemyPos and health are unused: perception works off the direction
        // and distance the caller already computed, and the damage sense lives
        // in updatePerception (it reads EnemyComponent::hitFlashTimer, not
        // HealthComponent). Kept in the signature so adding e.g. wounded-pilot
        // tunnel vision later needs no call-site changes.
        (void)enemyPos; (void)health;

        const float visionRange = config["vision_range"].get_or(620.f);
        const float fovDeg = config["vision_fov"].get_or(110.f);
        const float proximity = config["proximity_sense"].get_or(150.f);

        // 1. PROXIMITY — omnidirectional. Without this you can sit inside an
        //    enemy's blind spot indefinitely, which reads as a bug.
        if (dist < proximity) return true;

        if (dist > visionRange) return false;

        // 2. CONE. Forward is (sin r, -cos r): the ship's nose vertex is at
        //    local (0,-30), matching the player's convention.
        const float r = tf.rotation * 3.14159f / 180.f;
        const sf::Vector2f forward(std::sin(r), -std::cos(r));
        const float d = forward.x * toPlayerN.x + forward.y * toPlayerN.y;

        // Widen the cone once already alerted — someone actively looking for
        // you sweeps their attention wider than someone idly patrolling.
        float half = fovDeg * 0.5f;
        if (ai.currentState != EnemyState::PATROL) {
            half *= config["vision_fov_alert_mult"].get_or(1.45f);
        }
        half = std::min(half, 175.f);

        // Falloff: seeing something at the very edge of range takes longer.
        if (d < std::cos(half * 3.14159f / 180.f)) return false;

        return true;
    }

    void updatePerception(float dt, bool sees, sf::Vector2f playerPos, sf::Vector2f playerVel,
        float dist, AIState& ai, EnemyComponent& ec,
        const TransformComponent& tf, sol::table& config)
    {
        const float visionRange = config["vision_range"].get_or(620.f);

        // 3. DAMAGE SENSE — being shot alerts you instantly no matter where
        //    you're looking. hitFlashTimer is set by DamageSystem.
        const bool wasHit = (ec.hitFlashTimer > 0.f);

        // ---- SECOND HIT = COMBAT, no questions asked ----
        // One hit could be a stray rock. Two is someone shooting at you.
        // Deliberately placed before the suspicion maths so it cannot be
        // out-voted by a low suspicion score.
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

            // Suspicion builds faster the closer the player is.
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

                // Reached the last known spot and found nothing? Give up.
                sf::Vector2f toLast = ai.lastKnownPlayerPos - tf.position;
                const float dLast = std::sqrt(toLast.x * toLast.x + toLast.y * toLast.y);

                if (ai.searchTimer <= 0.f || (dLast < 90.f && !sees && ai.timeSinceSeen > 1.5f)) {
                    enterState(ai, ec, EnemyState::PATROL, AlertIcon::Lost, config);
                    // Forget everything, as specified.
                    ai.suspicion = 0.f;
                    ai.hasSeenPlayer = false;
                    ai.searchTimer = 0.f;
                }
            }
            break;

        case EnemyState::COMBAT: {
            // De-aggro is deliberately hard: you must break BOTH line of sight
            // for a sustained period AND open real distance. Either alone would
            // let the player shake a pursuer by clipping behind one asteroid.
            const float loseTime = config["combat_lose_time"].get_or(3.2f);
            const float loseDist = config["combat_lose_distance"].get_or(950.f);

            if (ai.timeSinceSeen > loseTime && dist > loseDist) {
                enterState(ai, ec, EnemyState::ALERT, AlertIcon::Lost, config);
                ai.searchTimer = config["combat_search_time"].get_or(9.0f);
                ai.suspicion = 0.55f;   // stays primed — re-spotting is instant
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

        // A state change resets the manoeuvre so the enemy visibly reacts
        // rather than continuing whatever it was doing.
        ai.maneuverTimer = 0.f;

        if (s == EnemyState::COMBAT) {
            // Startle: a brief backward flinch before committing. Cheap, and
            // it stops the transition from looking like a switch being flipped.
            ai.maneuver = Maneuver::FALLBACK;
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
            // Search AHEAD of the last known position using remembered velocity
            // — a pirate who saw you moving left expects you to still be moving
            // left. Searching the exact spot you vanished from looks stupid.
            const sf::Vector2f predicted = ai.lastKnownPlayerPos +
                ai.lastKnownPlayerVel * config["alert_lead_time"].get_or(0.7f);

            sf::Vector2f toTarget = predicted - enemyPos;
            const float d = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);

            if (d > 60.f) {
                // Approach in a slight zigzag rather than a straight line —
                // reads as searching rather than as pathing.
                const float sway = std::sin(ai.searchTimer * 2.6f + ai.jitterPhase) * 0.45f;
                sf::Vector2f dir = toTarget / d;
                sf::Vector2f perp(-dir.y, dir.x);
                dir += perp * sway;
                const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (dl > 0.01f) dir /= dl;
                return dir * (maxSpeed * 9.f);
            }

            // Arrived: circle the area slowly, scanning.
            sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
            return perp * ai.strafeDir * (maxSpeed * 3.5f);
        }

        // ---- PATROL: slow drift between waypoints ----
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

    /**
     * @brief Combat movement built from committed manoeuvres
     *
     * Each manoeuvre runs for a fixed duration, then a new one is picked with
     * weights that depend on distance band and personality. This is what
     * replaces the old converging orbit.
     */
    sf::Vector2f combatVelocity(AIState& ai, EnemyComponent& ec,
        sf::Vector2f enemyPos, sf::Vector2f playerPos,
        sf::Vector2f toPlayerN, float dist,
        float maxSpeed, sol::table& config)
    {
        ai.maneuverTimer -= 0.15f;   // called at ~10 Hz, so this is ~1.5x realtime

        if (ai.maneuverTimer <= 0.f) {
            pickManeuver(ai, ec, dist, config);
        }

        const sf::Vector2f perp(-toPlayerN.y, toPlayerN.x);
        const float band = ai.preferredRange;

        // Wander noise, decorrelated per enemy by jitterPhase. Small, but it
        // stops parallel paths from staying parallel.
        m_noiseTime += 0.0016f;
        const float nx = std::sin(m_noiseTime * 3.1f + ai.jitterPhase) * 0.22f;
        const float ny = std::cos(m_noiseTime * 2.3f + ai.jitterPhase * 1.7f) * 0.22f;
        const sf::Vector2f noise(nx, ny);

        switch (ai.maneuver) {

        case Maneuver::APPROACH: {
            // Come in at an ANGLE, not straight down the line of sight. A
            // head-on approach is both easy to shoot and looks robotic.
            sf::Vector2f dir = toPlayerN + perp * ai.strafeDir * 0.55f + noise;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (9.f + ai.aggression * 5.f));
        }

        case Maneuver::ATTACK_RUN: {
            // Fast, committed pass. Overshoots on purpose.
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
            // Break off toward a point offset from the player, so the enemy
            // crosses open space instead of hugging a radius.
            const float a = ai.jitterPhase + m_noiseTime * 0.8f;
            const sf::Vector2f target = playerPos +
                sf::Vector2f(std::cos(a), std::sin(a)) * (band * 1.35f);
            sf::Vector2f dir = target - enemyPos;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * 11.f);
        }

        case Maneuver::STRAFE:
        default: {
            // Hold the band. Radial term corrects distance error; tangential
            // term slides sideways. The correction is proportional, so an
            // enemy at the right range drifts almost purely sideways.
            const float err = (dist - band) / std::max(1.f, band);   // -1 .. +N
            const float radial = std::clamp(err, -0.9f, 0.9f);

            sf::Vector2f dir = toPlayerN * radial + perp * ai.strafeDir + noise;
            const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (l > 0.01f) dir /= l;
            return dir * (maxSpeed * (7.f + ai.aggression * 3.f));
        }
        }
    }

    void pickManeuver(AIState& ai, EnemyComponent& ec, float dist, sol::table& config) {
        const float band = ai.preferredRange;
        const int roll = rand() % 100;

        // Flip strafe handedness sometimes — a pirate that always circles one
        // way is trivially predictable.
        if (rand() % 100 < 35) ai.strafeDir = -ai.strafeDir;

        if (dist > band * 1.6f) {
            // Too far: close, sometimes with a committed run.
            ai.maneuver = (roll < 25 + static_cast<int>(ai.aggression * 35))
                ? Maneuver::ATTACK_RUN : Maneuver::APPROACH;
            ai.maneuverTimer = 0.7f + (rand() % 60) / 100.f;
        }
        else if (dist < band * 0.55f) {
            // Too close: back off, unless feeling reckless.
            ai.maneuver = (roll < 70 - static_cast<int>(ai.aggression * 40))
                ? Maneuver::FALLBACK : Maneuver::STRAFE;
            ai.maneuverTimer = 0.5f + (rand() % 50) / 100.f;
        }
        else {
            // In the band: mostly strafe, occasionally break the pattern.
            if (roll < 55)      ai.maneuver = Maneuver::STRAFE;
            else if (roll < 72) ai.maneuver = Maneuver::REPOSITION;
            else if (roll < 88) ai.maneuver = Maneuver::ATTACK_RUN;
            else                ai.maneuver = Maneuver::FALLBACK;
            ai.maneuverTimer = 0.6f + (rand() % 120) / 100.f;
        }

        // Wounded pirates lose their nerve — a readable behaviour shift that
        // rewards the player for pressing an advantage.
        if (ec.hitFlashTimer > 0.f && rand() % 100 < 45) {
            ai.maneuver = Maneuver::FALLBACK;
            ai.maneuverTimer = 0.5f;
        }
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

        // ========================================================================
        // Tier 1: ORDINARY ASTEROIDS
        //
        // Now gated by time-to-impact instead of being a distance force field.
        // A rock drifting in from 300px at 180px/s gives 1.7s — plenty, full
        // avoidance strength. The same rock at 600px/s gives 0.5s, less than the
        // pirate's manoeuvre time, so avoidance collapses toward zero and it just
        // gets hit. That IS the intended behaviour: fast rocks are lethal.
        // ========================================================================
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;
            if (m_em->healths[j].isHoming) continue;          // Tier 2 handles these

            sf::Vector2f diff = enemyPos - m_em->transforms[j].position;
            const float d = std::sqrt(diff.x * diff.x + diff.y * diff.y);
            if (d >= 300.f || d <= 0.01f) continue;

            const b2Vec2 av = b2Body_GetLinearVelocity(m_em->physics[j].bodyId);
            const sf::Vector2f astVel(av.x * SCALE, av.y * SCALE);

            // Closing speed along the line between us — the only component that
            // actually threatens. A rock moving sideways at 600px/s isn't a threat
            // at all, and the old code treated it as one.
            const sf::Vector2f toMe = diff / d;
            const sf::Vector2f rel(astVel.x - myVel.x, astVel.y - myVel.y);
            const float closing = -(rel.x * toMe.x + rel.y * toMe.y);

            if (closing < 5.f) continue;   // not actually coming at us

            // Something is genuinely bearing down: suppress idle jukes for a beat.
            ai.threatSeenTimer = std::max(ai.threatSeenTimer, 0.8f);

            const float tti = d / closing;

            // Competence ramps from 0 (no time) to 1 (plenty of time).
            const float react = m_dodgeManoeuvreTime;
            const float comp = std::clamp((tti - react * 0.5f) / (react * 1.8f), 0.f, 1.f);
            if (comp <= 0.01f) continue;

            avoidance += toMe * config["avoid_force"].get_or(380.f)
                * (1.f - d / 300.f) * comp;
        }

        // ========================================================================
        // Tier 2: HOMING / KINETIC ROCKS
        //
        // These are the parry-launched and rift-hijacked ones. At 800-1000px/s
        // they give 0.45-0.56s from notice to impact, which is BELOW the pirate's
        // manoeuvre time — so triggerDodgeBurst refuses and they flinch instead.
        // Exactly the "they see it but can't get out of the way" you wanted, and
        // it falls out of the model rather than being hardcoded.
        // ========================================================================
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

                    // Time left AFTER the reaction delay has already burned.
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

                    // HOMING PENALTY: a parry-launched or rift-hijacked rock
                    // STEERS. A sidestep that clears a dumb projectile does
                    // nothing against one that corrects, so evading it needs far
                    // more margin. Dividing the available time by this factor is
                    // what keeps kinetic rocks unavoidable while leaving ordinary
                    // bullets dodgeable -- the two travel at similar speeds, so
                    // speed alone cannot separate them.
                    const float homingPenalty = config["homing_dodge_penalty"].get_or(2.5f);
                    triggerDodgeBurst(ai, ec, ai.pendingDodgeDir, 0.9f, tti / homingPenalty);
                }

                if (ai.dodgeCommitTimer > 0.f) {
                    ai.dodgeCommitTimer -= dt;
                    // Only steer if the dodge actually committed. A flinch must not
                    // produce movement, or it silently becomes a slow dodge.
                    // NOTE: no steering push here. triggerDodgeBurst already
                    // SET the velocity; adding a target on top fights it.
                }
            }
            else {
                ai.threatNoticed = false;
                ai.trackedThreatId = 0;
            }
        }

        // ========================================================================
        // Tier 3: INCOMING BULLETS
        //
        // Reaction delay raised to 0.28-0.55s (was 0.12-0.34s). Human visual
        // reaction floor is ~0.25s even for trained people, and these are supposed
        // to be BAD pirates. At 800px/s from 260px they get 0.33s of warning, so
        // most bullets now land — which is the point.
        // ========================================================================
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
                    // 520px, not 260. At 800px/s a bullet crosses 260px in 0.33s;
                // after a 0.28s reaction only 0.04s remains, far below any
                // manoeuvre time -- so bullets were ALWAYS unreactable and the
                // pirate could only ever flinch at gunfire.
                //
                // 520px buys 0.65s: 0.28s reaction leaves 0.37s against a 0.25s
                // manoeuvre time, so a dodge clears with ~50% margin. Tightening
                // this toward 400 makes pirates eat far more of your shots.
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
                        : bdir * 0.7f;              // bad read: dodge INTO the shot

                    // Remember how long we have, so the delay can be checked
                    // against it when it expires.
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
                        // Time already consumed by the reaction delay.
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
                // No steering push -- the velocity set in triggerDodgeBurst is
                // the dodge. See the Tier 2 note above.
            }
        }

        // ---- Flinch produces a tiny nudge only. Visible, not evasive. ----
        if (ai.flinchTimer > 0.f) {
            avoidance += ai.flinchDir * (maxSpeed * 0.8f);
        }

        return avoidance;
    }

    /// Wrapper so callers can tell whether the dodge actually committed.
    bool triggerDodgeBurstChecked(AIState& ai, EnemyComponent& ec,
        sf::Vector2f dir, float tti) {
        const float before = ai.dodgeBurstTimer;
        triggerDodgeBurst(ai, ec, dir, 1.f, tti);
        return ai.dodgeBurstTimer > before;
    }

    /**
     * @brief Commit to an evasive burst
     * @param ttiSeconds Time until the threat arrives. Pass a large number
     *                   (e.g. 99.f) for a proactive juke with no incoming threat.
     *
     * Returns silently without dodging if there isn't enough time — the caller
     * should have already triggered a flinch in that case.
     *
     * Bursts are now LONG (0.42s) and RARE (2.0-3.2s cooldown) rather than short
     * and constant. A long committed burst is readable, animatable, and punishable;
     * a short frequent one is just jitter.
     */
    void triggerDodgeBurst(AIState& ai, EnemyComponent& ec, sf::Vector2f dir,
        float strength, float ttiSeconds = 99.f)
    {
        if (ai.dodgeCooldown > 0.f) return;
        if (ai.dodgeBurstTimer > 0.f) return;   // already mid-dodge

        const float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (l < 0.01f) return;
        dir /= l;

        // ---- TIME CHECK: can this pirate physically make it? ----
        const float manoeuvreTime = m_dodgeManoeuvreTime;
        if (ttiSeconds < manoeuvreTime) {
            triggerFlinch(ai, ec, dir, ttiSeconds);
            return;
        }

        ai.dodgeBurstDir = dir;
        ai.dodgeBurstDuration = m_dodgeDuration * (0.85f + (rand() % 30) / 100.f);
        ai.dodgeBurstTimer = ai.dodgeBurstDuration;
        ai.dodgeCooldown = m_dodgeCooldown + (rand() % 120) / 100.f;

        // Bank side locked at commit, so the animation can't flip mid-dodge.
        ai.dodgeBurstSide = 0.f;   // resolved in updateRotation against facing
        ec.dodgeFlashTimer = ai.dodgeBurstDuration;

        // ================================================================
        // THE ACTUAL DASH
        //
        // This used to feed a steering TARGET into `avoidance`, which then
        // went through the force limiter:
        //
        //     if (|impulse| > enginePower * dt) scale it down
        //
        // At enginePower 200 and dt 1/60 that cap is 3.33, so the dodge was
        // squeezed through the exact same throttle as ordinary drifting. It
        // was PHYSICALLY INCAPABLE of being snappy no matter how large the
        // multiplier was -- which is why it read as slow stirring rather than
        // as a dash.
        //
        // The player's dash bypasses all of it with SetLinearVelocity. So does
        // this now. Blending 35% of the existing velocity keeps some momentum,
        // so it reads as a hard cut of the throttle rather than a teleport.
        // ================================================================
        if (b2Body_IsValid(m_dodgeBodyId)) {
            const float speed = m_dodgeSpeed * std::clamp(strength, 0.4f, 1.4f);
            const b2Vec2 cur = b2Body_GetLinearVelocity(m_dodgeBodyId);
            b2Body_SetLinearVelocity(m_dodgeBodyId, {
                cur.x * 0.35f + dir.x * speed / SCALE,
                cur.y * 0.35f + dir.y * speed / SCALE
                });

            // Burn flare venting opposite the dash, at the moment of commit.
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

    /**
     * @brief Visible but useless reaction to a threat noticed too late
     *
     * This is the behaviour you actually want from a weak pirate facing a
     * high-speed rock: they see it, they twitch, and it hits them anyway. Silence
     * would read as the AI not noticing; a successful dodge would read as the AI
     * being too good. The flinch is the honest middle.
     */
    void triggerFlinch(AIState& ai, EnemyComponent& ec, sf::Vector2f dir, float tti) {
        if (ai.flinchTimer > 0.f) return;
        ai.flinchDuration = std::min(0.30f, std::max(0.12f, tti));
        ai.flinchTimer = ai.flinchDuration;
        ai.flinchDir = dir;
        ec.dodgeFlashTimer = 0.f;   // NOT a real dodge — must not trigger the punish
    }

    // ========================================================================
    // SHOOTING — with an honest telegraph
    // ========================================================================
    void updateShooting(float dt, size_t i, const TransformComponent& tf,
        EnemyComponent& ec, AIState& ai, uint32_t entityId,
        sf::Vector2f playerPos, sf::Vector2f playerVel,
        float dist, sol::table& config)
    {
        // Tick ALWAYS, in every state. It used to sit below the COMBAT gate,
        // so outside combat fireTimer never drained -- which is what let
        // opportunisticAsteroidShot fire every single frame.
        ec.fireTimer -= dt;

        if (ai.currentState != EnemyState::COMBAT) {
            ec.telegraphActive = false;
            ec.telegraphTimer = 0.f;
            return;
        }

        const float attackRange = config["attack_range"].get_or(480.f);
        const float fireRate = config["fire_rate"].get_or(1.8f);
        const float telegraph = config["telegraph_time"].get_or(0.32f);

        // ---- Wind-up in progress ----
        if (ec.telegraphActive) {
            ec.telegraphTimer -= dt;
            if (ec.telegraphTimer <= 0.f) {
                ec.telegraphActive = false;
                fireShot(i, tf, ec, entityId, ec.telegraphDir, config);
                ec.fireTimer = fireRate + ((rand() % 40) - 20) / 100.f;
            }
            return;
        }

        if (ec.fireTimer > 0.f || dist > attackRange) return;

        // ---- Begin wind-up. Aim is LOCKED NOW, not at the moment of firing.
        //      This is what makes the telegraph honest: the player can read the
        //      wind-up direction and move out of it. Re-aiming at fire time
        //      would make the wind-up pure decoration.
        const float bulletSpeed = config["bullet_speed"].get_or(550.f);
        const float travelTime = dist / bulletSpeed;

        // Pirates lead badly on purpose.
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
            angle, entityId, *m_lua, m_worldId);

        m_em->spawnImpact(spawnPos, sf::Color(255, 120, 0), dir * -200.f);

        // Recoil kick, so firing visibly costs the shooter something.
        b2Body_ApplyLinearImpulseToCenter(m_em->physics[i].bodyId,
            { -dir.x * 2.5f, -dir.y * 2.5f }, true);
    }

    /**
     * @brief Fire at an asteroid blocking the path
     *
     * THIS FUNCTION HAD AN INVERTED GATE.
     *
     *   old:  if (ec.fireTimer <= 0.3f) return;      // fire only when HIGH
     *         ...
     *         ec.fireTimer = fire_rate;              // ...which sets it HIGH
     *
     * So firing made the condition MORE true. Combined with ec.fireTimer only
     * being decremented inside updateShooting() -- which returns early outside
     * COMBAT -- a patrolling pirate never drained the timer and fired at rocks
     * EVERY FRAME. 60 shots/sec, indefinitely, the moment a rock was ahead.
     * That was the wall of projectiles.
     *
     * Now it owns a timer that always drains, and the cooldown is set whether
     * or not the chance roll succeeds -- otherwise a failed roll simply retries
     * next frame, which is the same bug wearing a hat.
     */
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
        if (ml < 0.5f) return;              // must actually be going somewhere
        movDir /= ml;

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;

            sf::Vector2f toAst = m_em->transforms[j].position - enemyPos;
            const float d = std::sqrt(toAst.x * toAst.x + toAst.y * toAst.y);
            if (d > 200.f || d < 0.01f) continue;

            const sf::Vector2f n = toAst / d;
            if (movDir.x * n.x + movDir.y * n.y < 0.75f) continue;   // tighter cone

            // Set the cooldown BEFORE the roll, so a failed roll can't retry
            // on the very next frame.
            ec.asteroidShotTimer = config["asteroid_shot_cooldown"].get_or(2.2f)
                + (rand() % 100) / 100.f;

            if (rand() % 100 >= static_cast<int>(config["asteroid_shot_chance"].get_or(35.f)))
                return;

            const float bulletSpeed = config["bullet_speed"].get_or(550.f);
            const float angle = std::atan2(n.y, n.x) * 180.f / 3.14159f + 90.f;
            m_ef->createEnemyBullet(*m_em, enemyPos + n * 35.f, n * bulletSpeed,
                angle, entityId, *m_lua, m_worldId);
            return;
        }
    }

    // ========================================================================
    // BULLET STORM
    //
    // Triggered when the pirate is boxed in by asteroids DURING combat: he
    // panics, spins up, and sprays. The recovery phase is the point — it's a
    // long, obvious punish window, which is what makes the move a gift to the
    // player rather than an unfair burst of damage.
    // ========================================================================
    void updateBulletStorm(float dt, size_t i, TransformComponent& tf,
        EnemyComponent& ec, AIState& ai, b2BodyId bodyId,
        sol::table& config)
    {
        if (ec.stormActive) {
            ec.stormTimer -= dt;
            const float u = 1.f - std::clamp(ec.stormTimer / std::max(0.01f, ec.stormDuration), 0.f, 1.f);

            // Spin ramps up then eases off — a flat spin rate looks mechanical.
            const float peak = config["storm_spin_speed"].get_or(760.f);
            // Floored at 35%: sin(u*pi) is ZERO at BOTH ends, so the opening
            // and closing volleys used to fire with no rotation at all -- which
            // is why the first burst all went one direction.
            ec.stormSpin = peak * (0.35f + 0.65f * std::sin(std::clamp(u, 0.f, 1.f) * 3.14159f));

            tf.rotation += ec.stormSpin * dt;
            while (tf.rotation > 360.f) tf.rotation -= 360.f;
            while (tf.rotation < 0.f)   tf.rotation += 360.f;
            b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
                b2MakeRot(tf.rotation * 3.14159f / 180.f));

            // ---- BRAKE HARD: plant your feet, then spin ----
            // Nothing used to touch linear velocity here, so the enemy kept its
            // combat momentum and spun while sailing across the screen.
            // Exponential decay rather than a hard zero: an instant stop reads
            // as the game freezing, a ~0.25s brake reads as slamming the retros.
            {
                const b2Vec2 sv = b2Body_GetLinearVelocity(bodyId);
                const float brake = std::exp(-config["storm_brake_rate"].get_or(7.0f) * dt);
                b2Body_SetLinearVelocity(bodyId, { sv.x * brake, sv.y * brake });
            }

            // Fire outward along the current facing, plus scatter.
            ec.stormFireTimer -= dt;
            if (ec.stormFireTimer <= 0.f) {
                ec.stormFireTimer = config["storm_fire_interval"].get_or(0.13f);

                const float r = (tf.rotation - 90.f) * 3.14159f / 180.f;
                // +/-20 deg, not +/-60. The spin is already doing the
                // spreading; extra scatter on top just muddies the pattern.
                const float scatter = ((rand() % 40) - 20) * 3.14159f / 180.f;
                const sf::Vector2f dir(std::cos(r + scatter), std::sin(r + scatter));

                fireShot(i, tf, ec, tf.entityId, dir, config);
            }

            // Stress visual
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

        // ---- RECOVERY: dizzy, drifting, defenceless ----
        ec.stormRecoverTimer -= dt;
        ec.telegraphActive = false;

        const float u = std::clamp(ec.stormRecoverTimer /
            std::max(0.01f, config["storm_recover_time"].get_or(1.6f)), 0.f, 1.f);

        // Residual wobble that decays — reads as "shaking it off".
        tf.visualOffsetAngle += 14.f * u * std::sin(ec.stormRecoverTimer * 16.f);

        b2Body_SetAngularVelocity(bodyId, 0.f);

        // Keep him near-stationary through the punish window. Drifting away
        // while dizzy would undo the whole point of the recovery phase.
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

    /// Called from the main loop's asteroid scan; decides whether to panic.
    /**
     * @brief Decide whether the pirate cracks and starts a Bullet Storm
     *
     * WHY THIS NEVER FIRED BEFORE:
     *   Urge only accumulated while >=4 asteroids sat inside a 260px circle,
     *   at 0.15 per 0.25s scan against a threshold of 1.54-2.73. That needed
     *   2.6-4.5 SECONDS of unbroken combat with a dense cluster that also had
     *   to persist. In practice that combination essentially never happened.
     *
     * WHAT IT IS NOW:
     *   Urge builds during ANY combat, and asteroid pressure ACCELERATES it
     *   rather than gating it. That matches the original intent -- "he decided
     *   he wants to do this right now in a fight" -- with crowding as the thing
     *   that pushes him over the edge, not a prerequisite.
     *
     *   At the defaults: ~9s of clean combat, or ~2s when boxed in.
     */
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

        // Count nearby rocks. Radius widened from 260 to 340 -- 260px is barely
        // more than the ship's own engagement bubble.
        int nearby = 0;
        const float r = config["storm_asteroid_radius"].get_or(340.f);
        const float r2 = r * r;
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type != BodyType::Asteroid) continue;
            const sf::Vector2f d = m_em->transforms[j].position - enemyPos;
            if (d.x * d.x + d.y * d.y < r2) ++nearby;
        }

        // Base rate applies in ANY combat; rocks add on top (capped so a huge
        // field doesn't trigger it instantly).
        const float base = config["storm_base_rate"].get_or(0.25f);
        const float perRock = config["storm_rock_rate"].get_or(0.22f);
        const float rockBonus = std::min(perRock * nearby, config["storm_rock_cap"].get_or(1.1f));

        ai.stormUrge += dt * (base + rockBonus);

        // Crowding also feeds panic, so the pirate visibly gets twitchier for
        // seconds BEFORE the storm -- the player can read it coming.
        if (nearby >= 2) {
            ai.panicLevel = std::min(1.f, ai.panicLevel + dt * (0.25f + 0.12f * nearby));
        }

        // Timid pirates crack sooner than reckless ones.
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

            // Wind-up tell -- the storm must never start unannounced.
            ec.alertIcon = AlertIcon::Spotted;
            ec.alertIconDuration = 0.7f;
            ec.alertIconTimer = 0.7f;

            m_em->spawnShockRing(enemyPos, 8.f, 95.f, 0.30f,
                sf::Color(255, 90, 40), 3.f, 235.f);
            m_em->addTrauma(0.14f);
        }
    }

    // ========================================================================
    // STAGGER — mirrors the player's tumble/recovery
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

            // Damage smoke
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
    // ROTATION + IDLE ANIMATION
    // ========================================================================
    void updateRotation(float dt, TransformComponent& tf, EnemyComponent& ec,
        AIState& ai, sf::Vector2f desiredVel, sf::Vector2f toPlayer,
        b2BodyId bodyId, sol::table& config)
    {
        float targetAngle = tf.rotation;

        if (std::abs(desiredVel.x) > 10.f || std::abs(desiredVel.y) > 10.f) {
            targetAngle = std::atan2(desiredVel.y, desiredVel.x) * 180.f / 3.14159f + 90.f;
        }

        if (ai.currentState == EnemyState::COMBAT) {
            // Face the player, but not perfectly — a small tracking error keeps
            // it from looking like a turret.
            targetAngle = std::atan2(toPlayer.y, toPlayer.x) * 180.f / 3.14159f + 90.f;
            targetAngle += std::sin(m_noiseTime * 4.f + ai.jitterPhase) * 6.f;
        }
        else if (ai.currentState == EnemyState::ALERT) {
            // ---- SCANNING: sweep the nose side to side while moving ----
            // This is the main visual tell for ALERT. A ship that moves the
            // same way it does in PATROL but is internally "suspicious"
            // communicates nothing.
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
        if (ai.currentState == EnemyState::ALERT)  rotSpeed *= 1.5f;   // twitchy
        if (ai.currentState == EnemyState::COMBAT) rotSpeed *= 1.8f;

        float delta = targetAngle - tf.rotation;
        while (delta > 180.f) delta -= 360.f;
        while (delta < -180.f) delta += 360.f;

        tf.rotation += delta * rotSpeed * dt;
        b2Body_SetTransform(bodyId, b2Body_GetPosition(bodyId),
            b2MakeRot(tf.rotation * 3.14159f / 180.f));

        // ---- Idle bob, per-state. Uses the same pivot trick as the player:
        //      offsetting rotation about a point near the nose swings the TAIL,
        //      which reads as banking rather than as spinning. ----
        const float pivotY = -18.f;
        if (ai.currentState == EnemyState::PATROL) {
            tf.visualOffsetAngle += 3.f * std::sin(m_noiseTime * 1.6f + ai.jitterPhase);
            tf.visualPivot = { 0.f, pivotY };
        }
        else if (ai.currentState == EnemyState::ALERT) {
            // Faster, more agitated
            tf.visualOffsetAngle += 6.f * std::sin(m_noiseTime * 6.5f + ai.jitterPhase);
            tf.visualPivot = { 0.f, pivotY };
        }

        // ---- DODGE BURST: banked roll with attack/decay envelope ----
        if (ai.dodgeBurstTimer > 0.f && ai.dodgeBurstDuration > 0.f) {
            const float u = 1.f - (ai.dodgeBurstTimer / ai.dodgeBurstDuration); // 0->1

            // Which side of the hull is leading, in the ship's local frame.
            const float r = tf.rotation * 3.14159f / 180.f;
            const sf::Vector2f right(std::cos(r), std::sin(r));
            const sf::Vector2f fwd(std::sin(r), -std::cos(r));
            const float side = ai.dodgeBurstDir.x * right.x + ai.dodgeBurstDir.y * right.y;
            const float ahead = ai.dodgeBurstDir.x * fwd.x + ai.dodgeBurstDir.y * fwd.y;

            // Envelope: snap into the bank over the first 22%, hold, then
            // recover with a small counter-overshoot. Same shape as the
            // player's dash bank, so both ships share a movement language.
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
            tf.visualPivot = { 0.f, -18.f };   // nose pivot: swings the TAIL

            // Stretch along the direction of travel.
            tf.visualScale.y *= 1.f + 0.14f * std::max(0.f, k) * std::abs(ahead);
            tf.visualScale.x *= 1.f + 0.12f * std::max(0.f, k) * std::abs(side);

            // Thruster burn on the side doing the work.
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

        // ---- FLINCH: a sharp twitch that accomplishes nothing ----
        if (ai.flinchTimer > 0.f && ai.flinchDuration > 0.f) {
            const float u = ai.flinchTimer / ai.flinchDuration;   // 1 -> 0
            tf.visualOffsetAngle += 13.f * std::sin(u * 3.14159f * 3.f) * u;
            tf.visualPivot = { 0.f, -18.f };
        }

        // ---- Telegraph: rear back, then snap forward as the shot lands ----
        if (ec.telegraphActive && ec.telegraphDuration > 0.f) {
            const float u = 1.f - (ec.telegraphTimer / ec.telegraphDuration);   // 0 -> 1
            const float pull = std::sin(u * 3.14159f * 0.5f);
            tf.visualScale.y *= 1.f - 0.14f * pull;   // compress
            tf.visualScale.x *= 1.f + 0.12f * pull;
        }
    }

    // ========================================================================
    // HOMING ASTEROIDS (unchanged behaviour)
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
    /**
 * @brief Unprompted evasive jukes during combat
 *
 * The reactive dodges (bullets, homing rocks) only fire when something is
 * already incoming. That produces a pirate who flies in perfectly smooth arcs
 * until the exact moment a bullet arrives, which reads as a machine waiting for
 * input.
 *
 * Real evasive flying is mostly PRE-emptive and mostly unnecessary. So this
 * fires on a timer with no threat required, and the direction is deliberately
 * unreliable:
 *
 *   - 55% sideways relative to the player  (a sensible juke)
 *   - 20% backwards                        (disengage)
 *   - 15% straight at the player           (aggressive, often a mistake)
 *   - 10% a fully random direction         (pure panic)
 *
 * That 25% of clearly-bad choices is the point. These are weak pirates, and
 * the mistakes are what make them read as people rather than as solvers.
 *
 * Rate scales with panicLevel, so a pirate boxed in by rocks jukes far more
 * often — which also visually foreshadows the Bullet Storm.
 */
 /**
  * @brief Unprompted evasive juke -- the "human factor" wobble
  *
  * SUPPRESSED WHENEVER A REAL THREAT IS AROUND.
  *
  * This used to fire every 1.1-3.7s while sharing ai.dodgeCooldown (2.0-3.2s)
  * with the reactive dodges. Since the periods matched, the juke consumed the
  * budget almost every cycle and the pirate effectively STOPPED evading
  * bullets and asteroids -- it just sidestepped at random forever.
  *
  * Two changes fix that:
  *   1. Its own long timer (chaos_dodge_interval, now ~5.5s baseline).
  *   2. It bails entirely if computeAvoidance saw a threat recently. A pilot
  *      with a rock bearing down on him is not idly juking; he's evading.
  */
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

        // ---- Real threats always win ----
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
        if (roll < 60)      dir = perp * ((rand() % 2) ? 1.f : -1.f);   // sensible juke
        else if (roll < 82) dir = -toPlayerN;                            // disengage
        else if (roll < 94) dir = toPlayerN;                             // aggressive
        else {                                                           // pure panic
            const float a = (rand() % 360) * 3.14159f / 180.f;
            dir = { std::cos(a), std::sin(a) };
        }

        // Even bad AI shouldn't ram you on purpose.
        if (dist < 120.f && (dir.x * toPlayerN.x + dir.y * toPlayerN.y) > 0.5f) {
            dir = perp * ((rand() % 2) ? 1.f : -1.f);
        }

        triggerDodgeBurst(ai, ec, dir, 0.85f + ai.panicLevel * 0.3f);
    }


    /// AI cache is keyed by entityId; nothing else removes dead pirates.
    void pruneCache() {
        if (m_aiCache.size() < 64) return;
        for (auto it = m_aiCache.begin(); it != m_aiCache.end(); ) {
            it = (m_em->getEntityIndex(it->first) == (size_t)-1)
                ? m_aiCache.erase(it) : std::next(it);
        }
    }

    // Dodge tuning, cached once per frame in update() from Lua.
    float m_dodgeDuration = 0.42f;   ///< LONG bursts: readable and punishable
    float m_dodgeCooldown = 2.0f;    ///< RARE: was 0.55, which meant ~20% uptime
    float m_dodgeManoeuvreTime = 0.40f;   ///< Time needed to actually clear a threat
    float m_dodgeSpeed = 620.f;           ///< px/sec, SET directly (bypasses steering)

    // Set per-enemy each iteration so triggerDodgeBurst can apply a real
    // velocity change without threading the body through every call site.
    b2BodyId m_dodgeBodyId = b2_nullBodyId;
    sf::Vector2f m_dodgePos;

    std::unordered_map<uint32_t, float> m_pendingTTI;   ///< TTI captured at notice

    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;

    float m_noiseTime = 0.f;
    std::unordered_map<uint32_t, AIState> m_aiCache;
};