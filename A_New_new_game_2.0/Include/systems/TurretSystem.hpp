/**
 * @file TurretSystem.hpp
 * @brief Independently-aiming turrets mounted on enemy hulls.
 *
 * WHY THIS IS A SEPARATE SYSTEM
 * -----------------------------
 * A turret's aim has nothing to do with where its hull is pointing. That is
 * the entire design point of the Barge: it holds a naval broadside orbit,
 * never turning to face the player, while the gun tracks independently. Trying
 * to express that inside AISystem's shooting code would mean threading "but
 * ignore the hull rotation" through every aim path in a file that is already
 * 1600 lines. Turret logic is genuinely separable, so it is separate.
 *
 * ORDERING: runs immediately AFTER AISystem, so the hull transform for this
 * frame is final before mount points are resolved into world space. Running it
 * before would put the muzzle one frame behind the ship, which is visible as a
 * detached, lagging gun on anything that moves.
 *
 *
 * THE TWO FIRE MODES
 * ------------------
 * AIMED  -- one predictive shot. Solves the intercept against the player's
 *           current velocity, so standing still or holding a straight line
 *           gets punished. Long telegraph: this is the shot you dodge.
 *
 * BURST  -- 3-4 rounds fanned across an arc. Deliberately NOT aimed at the
 *           player: it is aimed at the region the player could move into. You
 *           cannot sidestep a burst the way you sidestep an aimed shot, which
 *           is what stops the counterplay from collapsing into "always strafe."
 *           Shorter telegraph, because the fan itself is the warning.
 *
 * Alternating between them is what makes the Barge feel like it is fighting
 * rather than metronoming. Bias is per-archetype (`turret_burst_bias`).
 *
 *
 * THE TRAVERSE RATE IS THE COUNTERPLAY
 * ------------------------------------
 * The turret has full 360 degrees of rotation, so there is no safe angle. What
 * there IS, is a swing speed. At the Barge's 78 deg/sec a full rotation takes
 * ~4.6 seconds, so cutting hard across the gun's arc genuinely outruns it. A
 * turret that snapped to target instantly would have no counterplay at all --
 * it would just be an unavoidable damage tick with extra steps.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"
#include <cmath>
#include <cstdlib>

class TurretSystem : public ISystem {
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
        if (!m_em || !m_ef || !m_lua) return;
        if (!m_registry || m_registry->empty()) return;

        const size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;
        const b2Vec2 pvb = b2Body_GetLinearVelocity(m_em->physics[playerIdx].bodyId);
        const sf::Vector2f playerVel(pvb.x * SCALE, pvb.y * SCALE);

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;

            auto& ec = m_em->enemies[i];
            const auto& def = m_registry->resolve(ec.archetype);
            if (def.turrets.empty()) continue;

            sol::table cfg = def.config;
            if (!cfg["turret_enabled"].get_or(false)) continue;

            updateTurret(dt, i, ec, def, cfg, playerPos, playerVel);
        }
    }

private:

    // ========================================================================
    // MOUNT RESOLUTION
    // ========================================================================

    /// Local mount point -> world position, using the hull's CURRENT rotation.
    static sf::Vector2f mountWorld(const TransformComponent& tf, sf::Vector2f local) {
        const float r = tf.rotation * 3.14159265f / 180.f;
        const float c = std::cos(r), s = std::sin(r);
        return { tf.position.x + (local.x * c - local.y * s),
                 tf.position.y + (local.x * s + local.y * c) };
    }

    static float wrap180(float d) {
        while (d > 180.f)  d -= 360.f;
        while (d < -180.f) d += 360.f;
        return d;
    }

    /**
     * @brief Solve where to shoot so the round and the target arrive together.
     *
     * Two fixed-point iterations rather than the closed-form quadratic. The
     * quadratic has no real solution when the target outruns the projectile,
     * and handling that case is more code than it is worth for a result that
     * only needs to look intentional. Two passes converge closely enough that
     * a player holding a straight line gets hit, which is the whole point.
     */
    static sf::Vector2f solveIntercept(sf::Vector2f from, sf::Vector2f targetPos,
        sf::Vector2f targetVel, float projSpeed) {
        sf::Vector2f aim = targetPos;
        for (int pass = 0; pass < 2; ++pass) {
            const sf::Vector2f d = aim - from;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y);
            const float t = (projSpeed > 1.f) ? dist / projSpeed : 0.f;
            aim = targetPos + targetVel * t;
        }
        return aim;
    }

    // ========================================================================
    // PER-TURRET UPDATE
    // ========================================================================

    void updateTurret(float dt, size_t i, EnemyComponent& ec,
        const enemyarch::ArchetypeDef& def, sol::table& cfg,
        sf::Vector2f playerPos, sf::Vector2f playerVel)
    {
        const auto& tf = m_em->transforms[i];
        const sf::Vector2f muzzle = mountWorld(tf, def.turrets[0]);

        const sf::Vector2f toPlayer = playerPos - muzzle;
        const float dist = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);

        const float bulletSpeed = cfg["bullet_speed"].get_or(660.f);
        const bool  lead = cfg["turret_lead_target"].get_or(true);

        const sf::Vector2f aimPoint = lead
            ? solveIntercept(muzzle, playerPos, playerVel, bulletSpeed)
            : playerPos;

        const sf::Vector2f toAim = aimPoint - muzzle;
        const float desiredAngle =
            std::atan2(toAim.y, toAim.x) * 180.f / 3.14159265f + 90.f;

        // ---- Traverse ----
        //
        // The turret keeps tracking through stagger and stun. A crew does not
        // stop aiming because the deck shook, and visually it reads as the ship
        // being rattled but the gun staying on you -- which is the intimidating
        // read a capital ship should have.
        const float traverse = cfg["turret_traverse"].get_or(90.f);
        const float delta = wrap180(desiredAngle - ec.turretAngle);
        const float step = traverse * dt;
        ec.turretAngle += std::clamp(delta, -step, step);
        ec.turretAngle = wrap180(ec.turretAngle);

        if (ec.turretMuzzleFlash > 0.f) ec.turretMuzzleFlash -= dt;

        // ---- Firing gates ----
        //
        // No firing during a ram: the charge is a movement commitment, and
        // shooting out of it would muddy the one attack that is supposed to
        // read as a single unambiguous "get out of the way."
        const bool canFire =
            ec.visualState == EnemyState::COMBAT &&
            ec.ramState == RamState::None &&
            dist <= cfg["turret_range"].get_or(800.f);

        if (!canFire) {
            ec.turretTelegraphActive = false;
            ec.turretBurstLeft = 0;
            return;
        }

        // ---- Burst in progress ----
        if (ec.turretBurstLeft > 0) {
            ec.turretBurstTimer -= dt;
            if (ec.turretBurstTimer <= 0.f) {
                fireBurstRound(i, ec, def, cfg, muzzle);
                ec.turretBurstLeft--;
                ec.turretBurstTimer = cfg["turret_burst_interval"].get_or(0.09f);
                if (ec.turretBurstLeft <= 0)
                    ec.turretCooldown = cfg["turret_burst_cooldown"].get_or(3.6f);
            }
            return;
        }

        // ---- Telegraph running down ----
        if (ec.turretTelegraphActive) {
            ec.turretTelegraphTimer -= dt;
            if (ec.turretTelegraphTimer <= 0.f) {
                ec.turretTelegraphActive = false;

                if (ec.turretMode == 1) {
                    // Fan is anchored to the aim at the END of the wind-up, and
                    // centred on where the player is heading rather than where
                    // they are. Suppression, not marksmanship.
                    ec.turretBurstBaseAngle = ec.turretAngle;
                    ec.turretBurstLeft = cfg["turret_burst_count"].get_or(4);
                    ec.turretBurstTimer = 0.f;
                }
                else {
                    fireAimedShot(i, ec, def, cfg, muzzle);
                    ec.turretCooldown = cfg["turret_aimed_cooldown"].get_or(2.4f);
                }
            }
            return;
        }

        // ---- Idle: wind up the next attack ----
        ec.turretCooldown -= dt;
        if (ec.turretCooldown > 0.f) return;

        // Do not commit to a shot while still swinging onto target. Without
        // this the turret fires at the wall it happens to be pointing at when
        // the cooldown expires, which looks broken rather than menacing.
        if (std::fabs(delta) > 12.f) return;

        const float bias = cfg["turret_burst_bias"].get_or(0.45f);
        const bool wantBurst = (static_cast<float>(rand()) / RAND_MAX) < bias;

        ec.turretMode = wantBurst ? 1 : 0;
        ec.turretTelegraphDuration = wantBurst
            ? cfg["turret_burst_telegraph"].get_or(0.30f)
            : cfg["turret_aimed_telegraph"].get_or(0.42f);
        ec.turretTelegraphTimer = ec.turretTelegraphDuration;
        ec.turretTelegraphActive = true;
    }

    // ========================================================================
    // FIRING
    // ========================================================================

    void spawn(size_t i, const enemyarch::ArchetypeDef& def, sol::table& cfg,
        sf::Vector2f muzzle, float angleDeg)
    {
        const float speed = cfg["bullet_speed"].get_or(660.f);
        const float rad = (angleDeg - 90.f) * 3.14159265f / 180.f;
        const sf::Vector2f dir(std::cos(rad), std::sin(rad));

        // Offset past the barrel so the round is not born inside the hull,
        // where the enemy-vs-enemy collision filter would eat it.
        const sf::Vector2f origin = muzzle + dir * (cfg["turret_size"].get_or(10.f) + 14.f);

        m_ef->createEnemyBullet(*m_em, origin, dir * speed, angleDeg,
            m_em->transforms[i].entityId, *m_lua, m_worldId, cfg);

        m_em->spawnExplosion(origin, sf::Color(255, 190, 90), 4, 2.0f);
    }

    void fireAimedShot(size_t i, EnemyComponent& ec,
        const enemyarch::ArchetypeDef& def, sol::table& cfg, sf::Vector2f muzzle)
    {
        const float spread = cfg["turret_aimed_spread"].get_or(3.f);
        const float jitter = ((rand() % 200) / 100.f - 1.f) * spread;
        spawn(i, def, cfg, muzzle, ec.turretAngle + jitter);
        ec.turretMuzzleFlash = 0.11f;
    }

    void fireBurstRound(size_t i, EnemyComponent& ec,
        const enemyarch::ArchetypeDef& def, sol::table& cfg, sf::Vector2f muzzle)
    {
        const int   total = std::max(1, cfg["turret_burst_count"].get_or(4));
        const float fan = cfg["turret_burst_spread"].get_or(15.f);
        const int   shot = total - ec.turretBurstLeft;   // 0-based index

        // Sweep across the fan rather than firing random angles inside it. A
        // sweep is a shape the player can read and run ahead of; random spray
        // in the same cone is just noise with the same dps.
        const float t = (total > 1) ? (static_cast<float>(shot) / (total - 1)) : 0.5f;
        const float offset = (t - 0.5f) * fan;

        spawn(i, def, cfg, muzzle, ec.turretBurstBaseAngle + offset);
        ec.turretMuzzleFlash = 0.09f;
    }

    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId      m_worldId;
    uint32_t       m_playerEntityId = 0;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
};