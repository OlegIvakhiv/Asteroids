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
 *
 * 1.1 -- THE BARGE PASS
 * ---------------------
 * SEARCH, THEN TRACK. Out of COMBAT the gun no longer tracks the player (a
 * perfect tell, and nothing for an eye to do): it sweeps `turret_scan_arc`
 * about the hull's heading on patrol, and a tighter, quicker arc about the
 * brain's last sighting on alert (EnemyComponent::turretLook).
 *
 * TURRET VISION. `turret_vision_range` / `turret_vision_fov`: a cone along
 * the barrels. Writes EnemyComponent::turretSees; AISystem ORs it into the
 * hull's own sight, so either eye spotting you triggers the whole ship.
 *
 * SHOTGUN BLAST. Inside `shotgun_trigger_range`, on its own cooldown: the
 * gun winds up (`shotgun_windup`), tracking at `shotgun_track_mult`, then
 * LOCKS for the last `shotgun_lock`. A red wedge from the muzzles shows the
 * exact cone (`shotgun_range`, `shotgun_half_angle`); the hit test IS that
 * wedge, resolved on the frame the lock runs out. Parry negates it,
 * i-frames dodge it, standing in it costs `shotgun_damage` (half at the far
 * edge) and a tumble. It also breaks rocks in the wedge.
 *
 * No fire while the hull is SUMMONING (EnemyComponent::summonState): the
 * charge is the tell. Rounds now leave alternating barrels of the shared
 * TurretModel.
 *
 *
 * 1.2 -- PLAYTEST
 * ---------------
 * The aimed shot is one HEAVY SLUG out of both barrels (turret_slug_*: big,
 * pale, a hot trace, faster and harder). The barrage is 6 rounds. The
 * shotgun fires shotgun_pellets real rounds across the wedge instead of an
 * instant AoE test: how many hit depends on how much of the wedge you fill.
 *
 * @author Oleg Ivakhiv
 * @version 1.2 (playtest)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"
#include "utils/TurretModel.hpp"
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
        m_dev = ctx.dev;
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
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;

            auto& ec = m_em->enemies[i];
            ec.turretSees = false;   // set again below if the gun is live and sees you
            // Dev freeze: a frozen hull with a live turret still shoots you.
            if (m_dev && m_dev->isAIFrozen(ud->entityId)) continue;
            // Dormant or rebooting: the gun is as dead as the hull looks.
            // It holds whatever angle it had; the reboot is the hull's beat.
            if (!ec.powered()) continue;

            const auto& def = m_registry->resolve(ec.archetype);
            if (def.turrets.empty()) continue;

            sol::table cfg = def.config;
            if (!cfg["turret_enabled"].get_or(false)) continue;

            updateTurret(dt, i, ec, def, cfg, playerPos, playerVel, playerIdx);
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
        sf::Vector2f playerPos, sf::Vector2f playerVel, size_t playerIdx)
    {
        const auto& tf = m_em->transforms[i];
        const sf::Vector2f muzzle = mountWorld(tf, def.turrets[0]);

        const sf::Vector2f toPlayer = playerPos - muzzle;
        const float dist = std::sqrt(toPlayer.x * toPlayer.x + toPlayer.y * toPlayer.y);
        const bool combat = ec.visualState == EnemyState::COMBAT;

        // An aimed shot winding up leads with the SLUG's speed, not the burst's.
        const float bulletSpeed = (ec.turretTelegraphActive && ec.turretMode == 0)
            ? slugSpeed(cfg) : cfg["bullet_speed"].get_or(660.f);
        const bool  lead = cfg["turret_lead_target"].get_or(true);

        // ---- Aim: COMBAT tracks you; anything else SEARCHES ----
        //
        // Until 1.1 the gun tracked the player in every state, which made it
        // a perfect tell (a "dormant-looking" patrol whose gun follows you)
        // and left nothing for a turret EYE to do. Now out of combat it
        // sweeps: a slow arc about the hull's heading on patrol, a tighter,
        // quicker one about where the brain last saw you on alert.
        float desiredAngle = 0.f;
        if (combat) {
            const sf::Vector2f aimPoint = lead
                ? solveIntercept(muzzle, playerPos, playerVel, bulletSpeed)
                : playerPos;
            const sf::Vector2f toAim = aimPoint - muzzle;
            desiredAngle = std::atan2(toAim.y, toAim.x) * 180.f / 3.14159265f + 90.f;
        }
        else {
            ec.turretScanPhase += dt;
            float arc = cfg["turret_scan_arc"].get_or(140.f);
            float hz = cfg["turret_scan_rate"].get_or(0.14f);
            float centre = tf.rotation;
            if (ec.visualState == EnemyState::ALERT && ec.turretHasLook) {
                const sf::Vector2f tl = ec.turretLook - muzzle;
                centre = std::atan2(tl.y, tl.x) * 180.f / 3.14159265f + 90.f;
                arc *= cfg["turret_scan_alert_arc"].get_or(0.55f);
                hz *= 1.8f;
            }
            desiredAngle = centre + std::sin(ec.turretScanPhase * 6.2831853f * hz) * arc * 0.5f;
        }

        // ---- Traverse ----
        //
        // The turret keeps tracking through stagger and stun. A crew does not
        // stop aiming because the deck shook, and visually it reads as the ship
        // being rattled but the gun staying on you -- which is the intimidating
        // read a capital ship should have.
        //
        // A shotgun windup tracks FASTER (it is a close-range swing, and the
        // traverse that is counterplay at 600px is a free pass at 200), then
        // LOCKS for the last beat: that is the window to get out of the wedge.
        float traverse = cfg["turret_traverse"].get_or(90.f);
        if (ec.shotgunState == 1) traverse *= cfg["shotgun_track_mult"].get_or(1.8f);
        if (ec.shotgunState == 2) traverse = 0.f;
        const float delta = wrap180(desiredAngle - ec.turretAngle);
        const float step = traverse * dt;
        ec.turretAngle += std::clamp(delta, -step, step);
        ec.turretAngle = wrap180(ec.turretAngle);

        if (ec.turretMuzzleFlash > 0.f) ec.turretMuzzleFlash -= dt;
        if (ec.shotgunFlash > 0.f) ec.shotgunFlash -= dt;
        if (ec.shotgunCooldown > 0.f) ec.shotgunCooldown -= dt * ec.packMult;

        // ---- Vision: the gun's own eye ----
        // A narrow cone along the barrels. AISystem ORs this into the hull's
        // sight next frame -- one brain, two eyes -- so the turret sweeping
        // across you is as good as the bow pointing at you.
        {
            const float vr = cfg["turret_vision_range"].get_or(0.f);
            float half = cfg["turret_vision_fov"].get_or(60.f) * 0.5f;
            if (ec.visualState != EnemyState::PATROL)
                half *= cfg["vision_fov_alert_mult"].get_or(1.45f);
            ec.turretSees = false;
            if (vr > 0.f && dist <= vr && dist > 0.01f) {
                const float r = ec.turretAngle * 3.14159265f / 180.f;
                const sf::Vector2f fwd(std::sin(r), -std::cos(r));
                const float c = (fwd.x * toPlayer.x + fwd.y * toPlayer.y) / dist;
                ec.turretSees = c >= std::cos(std::min(half, 175.f) * 3.14159265f / 180.f);
            }
        }

        // ---- Firing gates ----
        //
        // No firing during a ram: the charge is a movement commitment, and
        // shooting out of it would muddy the one attack that is supposed to
        // read as a single unambiguous "get out of the way." Same for a
        // summon: the hull is charging to spit Wardogs, and that charge is
        // the tell -- a gun going off over it would bury it.
        const bool canFire =
            combat &&
            ec.ramState == RamState::None &&
            ec.summonState == 0 &&
            dist <= cfg["turret_range"].get_or(800.f);

        if (!canFire) {
            ec.turretTelegraphActive = false;
            ec.turretBurstLeft = 0;
            if (ec.shotgunState != 0) {
                ec.shotgunState = 0;
                ec.shotgunCooldown = std::max(ec.shotgunCooldown, 1.5f);
            }
            return;
        }

        // ---- SHOTGUN in progress: owns the gun ----
        if (ec.shotgunState != 0) {
            ec.shotgunTimer -= dt;
            if (ec.shotgunState == 1 && ec.shotgunTimer <= cfg["shotgun_lock"].get_or(0.28f))
                ec.shotgunState = 2;
            if (ec.shotgunTimer <= 0.f) {
                fireShotgun(i, ec, def, cfg, muzzle, playerIdx);
                ec.shotgunState = 0;
                ec.shotgunCooldown = cfg["shotgun_cooldown"].get_or(7.f);
                ec.turretCooldown = std::max(ec.turretCooldown, cfg["shotgun_recover"].get_or(0.9f));
            }
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

        // ---- Close in: SHOTGUN BLAST ----
        // Its own cooldown, not the main gun's: this is the answer to a
        // player who parked under the guns, and it must be there when they do.
        if (cfg["shotgun_enabled"].get_or(false) && ec.shotgunCooldown <= 0.f &&
            dist <= cfg["shotgun_trigger_range"].get_or(260.f) &&
            std::fabs(delta) <= cfg["shotgun_trigger_arc"].get_or(35.f))
        {
            ec.shotgunState = 1;
            ec.shotgunDuration = cfg["shotgun_windup"].get_or(0.85f);
            ec.shotgunTimer = ec.shotgunDuration;
            // A chamber with a charge in it groans: sparks off the breech.
            const float r = ec.turretAngle * 3.14159265f / 180.f;
            const sf::Vector2f fwd(std::sin(r), -std::cos(r));
            m_em->spawnShockRing(muzzle + fwd * (turretmodel::kMuzzle * turretSize(cfg)),
                4.f, 34.f, 0.22f, sf::Color(255, 90, 60), 2.5f, 220.f);
            return;
        }

        // ---- Idle: wind up the next attack ----
        // Pack aura / execution buff: cadence only, never the telegraph.
        ec.turretCooldown -= dt * ec.packMult;
        if (ec.turretCooldown > 0.f) return;

        // Do not commit to a shot while still swinging onto target. Without
        // this the turret fires at the wall it happens to be pointing at when
        // the cooldown expires, which looks broken rather than menacing.
        if (std::fabs(delta) > 12.f) return;

        const float bias = cfg["turret_burst_bias"].get_or(0.45f);
        const bool wantBurst = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) < bias;

        ec.turretMode = wantBurst ? 1 : 0;
        ec.turretTelegraphDuration = wantBurst
            ? cfg["turret_burst_telegraph"].get_or(0.30f)
            : cfg["turret_aimed_telegraph"].get_or(0.42f);
        ec.turretTelegraphTimer = ec.turretTelegraphDuration;
        ec.turretTelegraphActive = true;
    }

    // ========================================================================
    // SHOTGUN BLAST
    // ========================================================================
    //
    // Real rounds since 1.2 (playtest): shotgun_pellets ordinary rounds out
    // of the muzzles at once, spread evenly across the wedge the windup drew
    // with a little jitter, living exactly shotgun_range. The wedge is still
    // where they will go; what lands now depends on how many pellets your
    // hull is actually in front of -- point-blank most of them, the far edge
    // one or two. Parry reflects them like any round; dodge i-frames pass.
    void fireShotgun(size_t i, EnemyComponent& ec, const enemyarch::ArchetypeDef& def,
        sol::table& cfg, sf::Vector2f mount, size_t playerIdx)
    {
        (void)def;
        const float s = turretSize(cfg);
        const float r = ec.turretAngle * 3.14159265f / 180.f;
        const sf::Vector2f dir(std::sin(r), -std::cos(r));
        const sf::Vector2f side(-dir.y, dir.x);
        const sf::Vector2f apex = mount + dir * (turretmodel::kMuzzle * s);
        const float R = cfg["shotgun_range"].get_or(320.f);
        const float half = cfg["shotgun_half_angle"].get_or(30.f);
        const int   n = std::max(1, cfg["shotgun_pellets"].get_or(11));
        const float spd = cfg["shotgun_pellet_speed"].get_or(950.f);
        const float dmg = cfg["shotgun_pellet_damage"].get_or(7.f);
        const float ifr = cfg["shotgun_pellet_iframes"].get_or(0.02f);

        ec.shotgunFlash = 0.30f;
        ec.turretMuzzleFlash = 0.14f;

        for (int k = 0; k < n; ++k) {
            const float t = (n > 1) ? static_cast<float>(k) / (n - 1) : 0.5f;
            const float jitter = (((rand() % 200) - 100) / 100.f) * (half / std::max(1, n - 1)) * 0.6f;
            const float a = ec.turretAngle + (t - 0.5f) * 2.f * half + jitter;
            const float v = spd * (0.88f + (rand() % 24) / 100.f);
            Round rd;
            rd.speed = v; rd.damage = dmg; rd.iframes = ifr; rd.lifetime = R / v;
            rd.size = 0.8f;
            fireRound(i, cfg, apex + side * (((k % 2) ? 1.f : -1.f) * turretmodel::kBarrelX * s), a, rd);
        }

        for (const auto& mz : turretmodel::muzzles(s)) {
            const sf::Vector2f w = mount + sf::Vector2f(
                mz.x * std::cos(r) - mz.y * std::sin(r), mz.x * std::sin(r) + mz.y * std::cos(r));
            m_em->spawnExplosion(w, sf::Color(255, 210, 120), 8, 2.6f);
        }
        for (int k = 0; k < 12; ++k) {   // a short muzzle cloud, nothing more
            const float a = std::atan2(dir.y, dir.x) + ((rand() % 200) - 100) / 100.f * 0.6f;
            const float sp = 160.f + rand() % 220;
            m_em->particles.push_back({ apex, sf::Vector2f(std::cos(a), std::sin(a)) * sp,
                sf::Color(200, 180, 165, 190), 0.28f, 0.28f, 4.f + rand() % 4 });
        }
        m_em->spawnShockRing(apex, 6.f, 60.f, 0.18f, sf::Color(255, 120, 70), 4.f, 230.f);
        const sf::Vector2f pd = (playerIdx != (size_t)-1)
            ? m_em->transforms[playerIdx].position - apex : sf::Vector2f(1e4f, 0.f);
        m_em->addTrauma((pd.x * pd.x + pd.y * pd.y) < R * R * 2.25f ? 0.24f : 0.08f);
    }

    static float turretSize(sol::table& cfg) { return cfg["turret_size"].get_or(10.f); }

    // ========================================================================
    // FIRING
    // ========================================================================

    void spawn(size_t i, const enemyarch::ArchetypeDef& def, sol::table& cfg,
        sf::Vector2f muzzle, float angleDeg)
    {
        (void)def;
        const float speed = cfg["bullet_speed"].get_or(660.f);
        const float rad = (angleDeg - 90.f) * 3.14159265f / 180.f;
        const sf::Vector2f dir(std::cos(rad), std::sin(rad));

        // Out of alternating barrels, past the muzzle brake, so the round is
        // not born inside the hull where the enemy-vs-enemy collision filter
        // would eat it.
        const float s = turretSize(cfg);
        m_barrel = !m_barrel;
        const sf::Vector2f side(-dir.y, dir.x);
        const sf::Vector2f origin = muzzle + dir * (turretmodel::kMuzzle * s + 8.f)
            + side * ((m_barrel ? 1.f : -1.f) * turretmodel::kBarrelX * s);

        m_ef->createEnemyBullet(*m_em, origin, dir * speed, angleDeg,
            m_em->transforms[i].entityId, *m_lua, m_worldId, cfg);

        m_em->spawnExplosion(origin, sf::Color(255, 190, 90), 4, 2.0f);
    }

    /**
     * @brief The aimed shot: one HEAVY SLUG from both barrels at once.
     *
     * Playtest 1.2: a single ordinary round out of one barrel of a twin gun
     * read wrong. Now both barrels fire together into one big pale slug --
     * faster than the burst rounds (turret_slug_speed), harder
     * (turret_slug_damage), with a hot trace behind it so you can see the
     * lane it is cutting. It is the shot that punishes a straight line.
     */
    void fireAimedShot(size_t i, EnemyComponent& ec,
        const enemyarch::ArchetypeDef& def, sol::table& cfg, sf::Vector2f muzzle)
    {
        (void)def;
        const float spread = cfg["turret_aimed_spread"].get_or(3.f);
        const float jitter = ((rand() % 200) / 100.f - 1.f) * spread;
        const float a = ec.turretAngle + jitter;
        const float rad = (a - 90.f) * 3.14159265f / 180.f;
        const sf::Vector2f dir(std::cos(rad), std::sin(rad));
        const float s = turretSize(cfg);
        Round rd;
        rd.speed = slugSpeed(cfg);
        rd.damage = cfg["turret_slug_damage"].get_or(40.f);
        rd.iframes = cfg["turret_slug_iframes"].get_or(0.8f);
        rd.lifetime = cfg["turret_range"].get_or(800.f) * 1.25f / rd.speed;
        rd.size = 1.9f;
        rd.heavy = true;
        fireRound(i, cfg, muzzle + dir * (turretmodel::kMuzzle * s + 10.f), a, rd);
        ec.turretMuzzleFlash = 0.14f;
        const sf::Vector2f side(-dir.y, dir.x);
        for (float sd : { -1.f, 1.f })
            m_em->spawnExplosion(muzzle + dir * (turretmodel::kMuzzle * s) + side * (sd * turretmodel::kBarrelX * s),
                sf::Color(255, 220, 160), 6, 2.4f);
        m_em->spawnShockRing(muzzle + dir * (turretmodel::kMuzzle * s), 4.f, 40.f, 0.16f,
            sf::Color(255, 200, 140), 3.f, 220.f);
        m_em->addTrauma(0.05f);
    }

    static float slugSpeed(sol::table& cfg) {
        return cfg["bullet_speed"].get_or(660.f) * cfg["turret_slug_speed_mult"].get_or(1.6f);
    }

    /// Per-round overrides on top of the archetype's ordinary gun.
    struct Round { float speed = 0.f, damage = 0.f, iframes = 0.f, lifetime = 0.f, size = 1.f; bool heavy = false; };

    /**
     * @brief The factory's enemy bullet, then re-tuned (as AISystem's
     * fireDuelRound does): speed, damage, i-frames, life, and the shape.
     * `heavy` = the slug: white-hot core, thick red rim, a trace behind it
     * (WeaponSystem emits it for BulletComponent::trail).
     */
    void fireRound(size_t i, sol::table& cfg, sf::Vector2f origin, float angleDeg, const Round& rd) {
        const float rad = (angleDeg - 90.f) * 3.14159265f / 180.f;
        const sf::Vector2f vel(std::cos(rad) * rd.speed, std::sin(rad) * rd.speed);
        const uint32_t id = m_ef->createEnemyBullet(*m_em, origin, vel, angleDeg,
            m_em->transforms[i].entityId, *m_lua, m_worldId, cfg);
        const size_t bi = m_em->getEntityIndex(id);
        if (bi == (size_t)-1) return;
        if (b2Body_IsValid(m_em->physics[bi].bodyId))
            b2Body_SetLinearVelocity(m_em->physics[bi].bodyId, { vel.x / SCALE, vel.y / SCALE });
        m_em->transforms[bi].velocity = vel;
        auto& b = m_em->bullets[bi];
        b.damage = rd.damage;
        b.playerIframes = rd.iframes;
        b.lifetime = rd.lifetime;
        b.trail = rd.heavy ? 1 : 0;
        auto& sh = m_em->renders[bi].shape;
        const float k = rd.size;
        sh.setPoint(0, { 0.f, -12.f * k });
        sh.setPoint(1, { 2.5f * k, 0.f });
        sh.setPoint(2, { 0.f, 12.f * k });
        sh.setPoint(3, { -2.5f * k, 0.f });
        if (rd.heavy) {
            sh.setFillColor(sf::Color(255, 236, 200));
            sh.setOutlineThickness(2.6f);
            sh.setOutlineColor(sf::Color(255, 80, 40, 235));
        }
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
    DevState* m_dev = nullptr;
    bool m_barrel = false;   ///< Which of the twin barrels fires next
};