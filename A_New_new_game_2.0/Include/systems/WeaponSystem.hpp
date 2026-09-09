/**
 * @file WeaponSystem.hpp
 * @brief Player shooting, weapon heat, Rift Shot, and projectile steering
 *
 * CHANGED in 1.3:
 *  - Homing steering for reflected (parried) bullets, in the bullet loop.
 *  - Projectile trails: rift bolts and reflected shots leave visible streaks.
 *  - EVERY trauma / hitstop / flash value is now a Lua lookup. They were
 *    partly hardcoded, which made the "too shaky" complaint untunable without
 *    a recompile.
 *
 * TWO RESOURCES (unchanged from 1.2):
 *   ENERGY — strategic budget, shared with dash and turbo.
 *   HEAT   — tactical rhythm, weapon-only, hard-locks the gun at max.
 *
 * @author Oleg Ivakhiv
 * @version 1.3
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "utils/InputRegistry.hpp"
#include "utils/GameConfig.hpp"
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class WeaponSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
    }

    void update(float dt) override {
        if (!m_em || !m_lua || !m_ef) return;

        sol::table binds = (*m_lua)["key_bindings"];
        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& playerStats = m_em->players[playerIdx];
        auto& playerTf = m_em->transforms[playerIdx];

        playerStats.shootTimer -= dt;

        // ====================================================================
        // 0. WEAPON HEAT
        //
        // Runs BEFORE the stagger gate: the gun should keep cooling while
        // you're knocked out of control, or a stagger silently extends an
        // overheat lockout for no reason the player can see.
        // ====================================================================
        updateHeat(dt, playerStats, playerTf);

        if (playerStats.staggerTimer > 0.f || playerStats.staggerRecoverTimer > 0.f) {
            playerStats.riftCharging = false;
            updateProjectiles(dt, playerIdx);
            return;
        }

        bool fireHeld = InputRegistry::isPressed(binds["fire"].get<std::string>());
        bool detonateKey = InputRegistry::isPressed(
            (*m_lua)["rift_detonate_key"].get_or(std::string("MouseRight"))
        );

        const float shotEnergy = wcfg("shot_energy_cost", 6.f);
        const float shotHeat = wcfg("shot_heat", 5.0f);
        const float riftEnergy = wcfg("rift_energy_cost", 40.f);
        const float riftHeat = wcfg("rift_heat", 52.f);
        const float riftChargeDrain = wcfg("rift_charge_drain", 14.f);

        // ====================================================================
        // 1. RIFT SHOT CHARGING
        // ====================================================================
        const bool canStartRift =
            !playerStats.riftBoltInFlight &&
            !playerStats.riftCharging &&
            !playerStats.weaponOverheated &&
            playerStats.energyDrive >= riftEnergy &&
            // Under overdrive the Rift generates no heat, so gating it on a
            // heat budget it will never spend would lock out the one weapon
            // the buff most exists to enable.
            (playerStats.overdriveTimer > 0.f ||
                playerStats.weaponHeat + riftHeat < playerStats.maxWeaponHeat);

        if (detonateKey && canStartRift) {
            playerStats.riftCharging = true;
            playerStats.riftChargeTimer = 0.f;
        }
        else if (detonateKey && !playerStats.riftCharging && !playerStats.riftBoltInFlight) {
            if (m_denyCooldown <= 0.f) {
                m_denyCooldown = 0.35f;
                spawnDenyPuff(playerTf);
            }
        }
        if (m_denyCooldown > 0.f) m_denyCooldown -= dt;

        if (playerStats.riftCharging) {
            playerStats.riftChargeTimer += dt;
            playerStats.energyDrive = std::max(0.f, playerStats.energyDrive - riftChargeDrain * dt);

            const float chargeTime = (*m_lua)["rift_charge_time"].get_or(0.6f);
            const float chargeT = std::clamp(playerStats.riftChargeTimer / chargeTime, 0.f, 1.f);

            // Sparks converge INWARD on the nose — reads as gathering energy.
            const int sparkCount = 1 + static_cast<int>(chargeT * 3.f);
            for (int n = 0; n < sparkCount; ++n) {
                float rotRad = (playerTf.rotation - 90.f) * 3.14159f / 180.f;
                sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
                sf::Vector2f nosePos = playerTf.position + fwd * 40.f;

                float a = (rand() % 360) * 3.14159f / 180.f;
                float dist = 60.f + rand() % 50;
                sf::Vector2f from = nosePos + sf::Vector2f(std::cos(a), std::sin(a)) * dist;
                sf::Vector2f toward = (nosePos - from);
                float tl = std::sqrt(toward.x * toward.x + toward.y * toward.y);
                if (tl > 0.01f) toward /= tl;

                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    from,
                    toward * (dist / 0.16f),
                    sf::Color(static_cast<uint8_t>(180 + rand() % 75), 80, 255,
                              static_cast<uint8_t>(150 + 105 * chargeT)),
                    0.16f, 0.16f,
                    2.5f + chargeT * 3.f
                    });
            }

            m_em->cameraTrauma = std::max(m_em->cameraTrauma,
                wcfg("rift_charge_trauma", 0.12f) * chargeT * chargeT);

            if (!detonateKey) {
                playerStats.riftCharging = false;
                playerStats.riftChargeTimer = 0.f;
            }
            else if (playerStats.riftChargeTimer >= chargeTime) {
                playerStats.riftCharging = false;
                playerStats.riftChargeTimer = 0.f;

                float rotRad = (playerTf.rotation - 90.f) * 3.14159f / 180.f;
                sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
                sf::Vector2f spawnPos = playerTf.position + fwd * 55.f;
                sf::Vector2f vel = fwd * (*m_lua)["rift_bullet_speed"].get_or(1100.f);

                playerStats.energyDrive = std::max(0.f, playerStats.energyDrive - riftEnergy);
                addHeat(playerStats, riftHeat, playerTf.position);

                // Recoil: impulse, so it ADDS to your momentum.
                const float recoil = wcfg("rift_recoil_impulse", 26.f);
                b2Body_ApplyLinearImpulseToCenter(m_em->physics[playerIdx].bodyId,
                    { -fwd.x * recoil, -fwd.y * recoil }, true);

                m_em->addTrauma(wcfg("rift_fire_trauma", 0.40f));
                m_em->cameraZoomKick = -wcfg("rift_fire_zoom_kick", 0.05f);
                m_em->spawnScreenFlash(sf::Color(170, 90, 255), 0.14f,
                    wcfg("rift_fire_flash_alpha", 45.f));
                m_em->spawnShockRing(spawnPos, 15.f, 150.f, 0.30f,
                    sf::Color(190, 100, 255), 5.f, 235.f);

                uint32_t boltId = m_ef->createRiftBolt(*m_em, spawnPos, vel,
                    playerTf.rotation, *m_lua, m_worldId);
                playerStats.riftBoltEntityId = boltId;
                playerStats.riftBoltInFlight = true;

                // Muzzle blast: cone backwards, not a ring.
                for (int n = 0; n < 22; ++n) {
                    float spread = ((rand() % 120) - 60) * 3.14159f / 180.f;
                    sf::Vector2f d(
                        -fwd.x * std::cos(spread) + fwd.y * std::sin(spread),
                        -fwd.x * std::sin(spread) - fwd.y * std::cos(spread)
                    );
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        spawnPos,
                        d * static_cast<float>(250 + rand() % 250),
                        sf::Color(160, 60, 255, 220),
                        0.22f, 0.22f,
                        4.f + rand() % 3
                        });
                }
            }
        }

        // ====================================================================
        // 2. RIFT BOLT DETONATION
        // ====================================================================
        if (playerStats.riftBoltInFlight) {
            size_t boltIdx = m_em->getEntityIndex(playerStats.riftBoltEntityId);

            if (boltIdx == (size_t)-1) {
                playerStats.riftBoltInFlight = false;
                playerStats.riftBoltEntityId = 0;
            }
            else if (fireHeld && playerStats.shootTimer <= 0 && !playerStats.isParrying) {
                detonate(boltIdx, playerIdx, playerStats);
            }
        }

        // ====================================================================
        // 3. NORMAL SHOOTING
        // ====================================================================
        const bool canFire =
            fireHeld &&
            playerStats.shootTimer <= 0 &&
            !playerStats.riftCharging &&
            !playerStats.riftBoltInFlight &&
            !playerStats.parryWhiffRecovery &&
            !playerStats.weaponOverheated &&
            playerStats.energyDrive >= shotEnergy;

        if (canFire) {
            float rotRad = (playerTf.rotation - 90.f) * 3.14159f / 180.f;
            sf::Vector2f direction(std::cos(rotRad), std::sin(rotRad));
            float bulletSpeed = (*m_lua)["bullet_speed"].get_or(800.f);
            sf::Vector2f spawnPos = playerTf.position + direction * 50.f;

            m_ef->createBullet(*m_em, spawnPos, direction * bulletSpeed,
                playerTf.rotation, *m_lua, m_worldId);
            playerStats.shootTimer = (*m_lua)["fire_rate"].get_or(0.2f);

            playerStats.energyDrive = std::max(0.f, playerStats.energyDrive - shotEnergy);
            addHeat(playerStats, shotHeat, playerTf.position);

            // Recoil scales with heat: a hot gun kicks harder.
            const float heatT = playerStats.weaponHeat / std::max(1.f, playerStats.maxWeaponHeat);
            const float kick = wcfg("shot_recoil_impulse", 3.0f) * (1.f + heatT * 0.8f);
            b2Body_ApplyLinearImpulseToCenter(m_em->physics[playerIdx].bodyId,
                { -direction.x * kick, -direction.y * kick }, true);

            m_em->addTrauma(wcfg("shot_trauma", 0.10f) * (1.f + heatT));

            // Muzzle flash shifts cyan -> orange -> white as heat rises.
            const int flashCount = 5 + static_cast<int>(heatT * 5.f);
            for (int n = 0; n < flashCount; ++n) {
                float spread = ((rand() % 80) - 40) * 3.14159f / 180.f;
                sf::Vector2f sparkDir(
                    direction.x * std::cos(spread) - direction.y * std::sin(spread),
                    direction.x * std::sin(spread) + direction.y * std::cos(spread)
                );
                float spd = 150.f + rand() % 200;
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    spawnPos,
                    sparkDir * spd,
                    heatColor(heatT, 230),
                    0.08f + (rand() % 6) / 100.f,
                    0.12f,
                    2.5f + rand() % 2 + heatT * 2.f
                    });
            }
        }
        else if (fireHeld && playerStats.weaponOverheated && m_denyCooldown <= 0.f) {
            m_denyCooldown = 0.25f;
            spawnDenyPuff(playerTf);
        }

        // ====================================================================
        // 4. PROJECTILE UPDATE (homing, trails, lifetime)
        // ====================================================================
        updateProjectiles(dt, playerIdx);
    }

private:
    // ========================================================================
    // PROJECTILES: homing steering, trails, lifetime
    // ========================================================================
    void updateProjectiles(float dt, size_t playerIdx) {
        (void)playerIdx;

        for (size_t i = m_em->bullets.size(); i-- > 0; ) {
            b2BodyId bodyId = m_em->physics[i].bodyId;
            if (!b2Body_IsValid(bodyId)) continue;

            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid;
            if (type != BodyType::Bullet) continue;

            auto& bullet = m_em->bullets[i];
            bullet.lifetime -= dt;

            // ================================================================
            // HOMING
            //
            // Steering by ROTATING the velocity vector, not by re-pointing it
            // at the target. A hard re-point makes the shot snap to the enemy
            // and look like a scripted hit; a bounded turn rate makes it curve,
            // and lets a fast enemy genuinely outrun a bad angle.
            // ================================================================
            if (bullet.homingTargetEntityId != 0 && bullet.homingTurnRate > 0.f) {
                size_t tIdx = m_em->getEntityIndex(bullet.homingTargetEntityId);

                if (tIdx == (size_t)-1) {
                    bullet.homingTargetEntityId = 0;   // target died; fly straight
                }
                else {
                    const sf::Vector2f myPos = m_em->transforms[i].position;
                    const b2Vec2 bv = b2Body_GetLinearVelocity(bodyId);
                    const sf::Vector2f vel(bv.x * SCALE, bv.y * SCALE);
                    const float speed = std::sqrt(vel.x * vel.x + vel.y * vel.y);

                    if (speed > 1.f) {
                        // Lead the target rather than chasing its current
                        // position — pure pursuit curves in behind a moving
                        // enemy and often never closes.
                        const b2Vec2 tvb = b2Body_GetLinearVelocity(m_em->physics[tIdx].bodyId);
                        const sf::Vector2f tVel(tvb.x * SCALE, tvb.y * SCALE);
                        sf::Vector2f toT = m_em->transforms[tIdx].position - myPos;
                        const float dist = std::sqrt(toT.x * toT.x + toT.y * toT.y);
                        const float lead = std::min(dist / speed, 0.6f);
                        toT += tVel * lead;

                        const float tl = std::sqrt(toT.x * toT.x + toT.y * toT.y);
                        if (tl > 0.01f) {
                            toT /= tl;
                            const sf::Vector2f dir = vel / speed;

                            // Signed angle between current heading and desired.
                            const float cross = dir.x * toT.y - dir.y * toT.x;
                            const float dot = std::clamp(dir.x * toT.x + dir.y * toT.y, -1.f, 1.f);
                            const float angle = std::atan2(cross, dot);   // radians

                            const float maxTurn = bullet.homingTurnRate * 3.14159f / 180.f * dt;
                            const float turn = std::clamp(angle, -maxTurn, maxTurn);

                            const float c = std::cos(turn), s = std::sin(turn);
                            const sf::Vector2f nd(dir.x * c - dir.y * s, dir.x * s + dir.y * c);

                            b2Body_SetLinearVelocity(bodyId,
                                { nd.x * speed / SCALE, nd.y * speed / SCALE });

                            m_em->transforms[i].velocity = nd * speed;
                        }
                    }
                }
            }

            // ---- Trails for the loud projectiles ----
            if (bullet.isRiftBolt && (rand() % 2 == 0)) {
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    m_em->transforms[i].position,
                    { 0.f, 0.f },
                    sf::Color(180, 60, 255, 180),
                    0.12f, 0.12f,
                    6.f
                    });
            }
            else if (bullet.isReflected) {
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    m_em->transforms[i].position,
                    { 0.f, 0.f },
                    sf::Color(255, 215, 90, 200),
                    0.16f, 0.16f,
                    5.f
                    });
            }

            // ---- Fade out for plain player bullets ----
            if (!bullet.isEnemyBullet && !bullet.isRiftBolt && !bullet.isReflected) {
                float maxLifetime = (*m_lua)["bullet_lifetime"].get_or(1.5f);
                float ratio = std::max(0.f, bullet.lifetime / maxLifetime);
                auto& shape = m_em->renders[i].shape;
                sf::Color outlineCol = shape.getOutlineColor();
                sf::Color fillCol = shape.getFillColor();
                outlineCol.a = static_cast<uint8_t>(ratio * 255);
                fillCol.a = static_cast<uint8_t>(ratio * 255);
                shape.setOutlineColor(outlineCol);
                shape.setFillColor(fillCol);
            }

            if (bullet.lifetime <= 0 || bullet.markedForDestroy) {
                m_em->destroyEntity(i);
            }
        }
    }

    // ========================================================================
    // RIFT DETONATION
    // ========================================================================
    void detonate(size_t boltIdx, size_t playerIdx, PlayerComponent& playerStats) {
        const sf::Vector2f boltPos = m_em->transforms[boltIdx].position;

        float nearestDist = FLT_MAX;
        size_t nearestIdx = (size_t)-1;
        BodyType nearestType = BodyType::Asteroid;

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j == playerIdx || j == boltIdx) continue;
            BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
            if (!ud2 || ud2->type == BodyType::Bullet) continue;

            sf::Vector2f diff = boltPos - m_em->transforms[j].position;
            float centerDist = std::sqrt(diff.x * diff.x + diff.y * diff.y);

            float targetRadius = 20.f;
            if (ud2->type == BodyType::Asteroid) {
                int reward = m_em->scoreRewards[j];
                if (reward >= 200)      targetRadius = 45.f;
                else if (reward == 75)  targetRadius = 40.f;
                else if (reward >= 50)  targetRadius = 30.f;
                else                    targetRadius = 15.f;
            }

            float surfaceDist = std::max(0.f, centerDist - targetRadius);
            if (surfaceDist < nearestDist) {
                nearestDist = surfaceDist;
                nearestIdx = j;
                nearestType = ud2->type;
            }
        }

        const float impactThreshold = 40.f;

        // ---- MODE 1: AIR BURST ----
        if (nearestIdx == (size_t)-1 || nearestDist > impactThreshold) {
            const float burstRadius = (*m_lua)["rift_burst_radius"].get_or(120.f);
            const float burstDamage = (*m_lua)["rift_burst_damage"].get_or(20.f);
            const float burstKnock = wcfg("rift_burst_knockback", 420.f);

            m_em->addDebugAoE(boltPos, impactThreshold, sf::Color(0, 255, 200, 255), 0.5f);
            m_em->addDebugAoE(boltPos, burstRadius, sf::Color(180, 60, 255, 180), 0.5f);

            m_em->addTrauma(wcfg("rift_detonate_trauma", 0.32f));
            m_em->requestHitstop(wcfg("rift_detonate_freeze", 0.03f),
                wcfg("rift_detonate_slomo", 0.10f), 0.45f);
            m_em->spawnScreenFlash(sf::Color(190, 120, 255), 0.18f,
                wcfg("rift_detonate_flash_alpha", 55.f));
            m_em->spawnShockRing(boltPos, 20.f, burstRadius * 1.15f, 0.42f,
                sf::Color(170, 70, 255), 8.f, 255.f);
            m_em->spawnShockRing(boltPos, 8.f, burstRadius * 0.5f, 0.22f,
                sf::Color::White, 4.f, 255.f);

            for (int n = 0; n < 36; ++n) {
                float a = n * 10.f * 3.14159f / 180.f;
                sf::Vector2f d(std::cos(a), std::sin(a));
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    boltPos + d * burstRadius * 0.3f,
                    d * 250.f,
                    sf::Color(120, 60, 255, 200),
                    0.25f, 0.25f,
                    4.f
                    });
            }
            m_em->spawnExplosion(boltPos, sf::Color(160, 80, 255), 28, 3.5f);

            for (size_t j = 0; j < m_em->physics.size(); ++j) {
                if (j == boltIdx) continue;
                BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
                if (!ud2) continue;

                sf::Vector2f diff = boltPos - m_em->transforms[j].position;
                float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y);
                if (dist > burstRadius) continue;

                const float falloff = 1.f - (dist / burstRadius);

                if (dist > 0.01f) {
                    sf::Vector2f away = -diff / dist;
                    float k = burstKnock * falloff * (j == playerIdx ? 0.55f : 1.f);
                    b2Body_ApplyLinearImpulseToCenter(m_em->physics[j].bodyId,
                        { away.x * k / SCALE, away.y * k / SCALE }, true);
                }
                if (j == playerIdx) continue;

                if (ud2->type == BodyType::Bullet) {
                    m_em->bullets[j].markedForDestroy = true;
                }
                else if (ud2->type == BodyType::Asteroid) {
                    int reward = m_em->scoreRewards[j];
                    if (reward < 50) m_em->healths[j].currentHp = -1.f;
                    else             m_em->healths[j].currentHp -= burstDamage * (0.5f + falloff);
                }
                else {
                    m_em->healths[j].currentHp -= burstDamage * (0.5f + falloff);

                    // Enemies notice being caught in an explosion. Without
                    // this, AoE damage silently bypassed the damage sense and
                    // a rift burst never alerted anyone.
                    if (ud2->type == BodyType::Enemy) {
                        m_em->enemies[j].hitFlashTimer = 0.22f;
                        m_em->enemies[j].timesHit += 2;   // an AoE is unambiguous
                    }
                }
            }
        }

        // ---- MODE 2: KINETIC HIJACK ----
        else if (nearestType == BodyType::Asteroid) {
            const float homingSpeed = (*m_lua)["homing_missile_speed"].get_or(800.f);

            size_t enemyIdx = (size_t)-1;
            float bestDist = FLT_MAX;
            for (size_t j = 0; j < m_em->physics.size(); ++j) {
                BodyUserData* ud2 = (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId);
                if (!ud2 || ud2->type != BodyType::Enemy) continue;
                sf::Vector2f d = boltPos - m_em->transforms[j].position;
                float dist = d.x * d.x + d.y * d.y;
                if (dist < bestDist) { bestDist = dist; enemyIdx = j; }
            }

            m_em->addTrauma(wcfg("rift_hijack_trauma", 0.28f));
            m_em->spawnShockRing(boltPos, 15.f, 170.f, 0.35f,
                sf::Color(0, 255, 200), 5.f, 240.f);
            m_em->healths[nearestIdx].isKineticWeapon = true;

            if (enemyIdx != (size_t)-1) {
                sf::Vector2f toEnemy = m_em->transforms[enemyIdx].position - boltPos;
                float len = std::sqrt(toEnemy.x * toEnemy.x + toEnemy.y * toEnemy.y);
                if (len > 0.01f) toEnemy /= len;

                b2Body_SetLinearVelocity(m_em->physics[nearestIdx].bodyId,
                    { toEnemy.x * homingSpeed / SCALE, toEnemy.y * homingSpeed / SCALE });
                m_em->healths[nearestIdx].isHoming = true;
                m_em->healths[nearestIdx].homingTargetEntityId =
                    m_em->transforms[enemyIdx].entityId;

                m_em->healths[nearestIdx].isKineticWeapon = true;

                for (int n = 0; n < 24; ++n) {
                    float a = n * 15.f * 3.14159f / 180.f;
                    sf::Vector2f d(std::cos(a), std::sin(a));
                    m_em->spawnImpact(boltPos + d * 30.f,
                        sf::Color(0, 255, 200, 200), d * 400.f);
                }
            }
        }

        // ---- MODE 3: SYSTEMS OVERLOAD ----
        else if (nearestType == BodyType::Enemy) {
            const float riftDamage = (*m_lua)["rift_damage"].get_or(45.f)
                * wcfg("rift_overload_multiplier", 4.f);
            m_em->healths[nearestIdx].currentHp -= riftDamage;
            m_em->healths[nearestIdx].stunTimer = wcfg("rift_overload_stun", 2.5f);
            m_em->enemies[nearestIdx].hitFlashTimer = 0.25f;
            m_em->enemies[nearestIdx].timesHit += 2;

            sf::Vector2f away = m_em->transforms[nearestIdx].position - boltPos;
            float al = std::sqrt(away.x * away.x + away.y * away.y);
            if (al > 0.01f) {
                away /= al;
                float k = wcfg("rift_overload_knockback", 900.f);
                b2Body_ApplyLinearImpulseToCenter(m_em->physics[nearestIdx].bodyId,
                    { away.x * k / SCALE, away.y * k / SCALE }, true);
                b2Body_SetAngularVelocity(m_em->physics[nearestIdx].bodyId,
                    ((rand() % 2) ? 1.f : -1.f) * 12.f);
            }

            const sf::Vector2f tp = m_em->transforms[nearestIdx].position;
            m_em->addTrauma(wcfg("rift_overload_trauma", 0.45f));
            m_em->requestHitstop(wcfg("rift_overload_freeze", 0.05f),
                wcfg("rift_overload_slomo", 0.18f), 0.35f);
            m_em->spawnScreenFlash(sf::Color(225, 190, 255), 0.24f,
                wcfg("rift_overload_flash_alpha", 80.f));
            m_em->spawnShockRing(tp, 20.f, 340.f, 0.50f,
                sf::Color(180, 80, 255), 9.f, 255.f);
            m_em->spawnShockRing(tp, 8.f, 150.f, 0.24f, sf::Color::White, 5.f, 255.f);

            m_em->spawnExplosion(tp, sf::Color(160, 60, 255), 48, 5.f);
            m_em->spawnExplosion(tp, sf::Color::White, 24, 3.f);

            for (int n = 0; n < 24; ++n) {
                float a = (rand() % 360) * 3.14159f / 180.f;
                sf::Vector2f d(std::cos(a), std::sin(a));
                m_em->spawnImpact(tp + d * 40.f, sf::Color(200, 100, 255), d * 300.f);
            }
        }

        m_em->bullets[boltIdx].markedForDestroy = true;
        playerStats.riftBoltInFlight = false;
        playerStats.riftBoltEntityId = 0;
        playerStats.shootTimer = 0.5f;
    }

    // ========================================================================
    // HEAT
    //
    // Cooling has a GRACE DELAY. Without it, tapping at exactly the fire rate
    // cools as fast as it heats and the overheat can never trigger — the delay
    // is what makes sustained fire different from paced fire.
    //
    // Recovery unlocks at a THRESHOLD, not zero. Waiting out a full cooldown
    // from 100 is dead air.
    // ========================================================================
    void updateHeat(float dt, PlayerComponent& ps, const TransformComponent& tf) {
        ps.maxWeaponHeat = wcfg("max_weapon_heat", 100.f);

        const float coolDelay = wcfg("heat_cool_delay", 0.40f);
        const float coolRate = wcfg("heat_cool_rate", 34.f);
        const float ventRate = wcfg("heat_vent_rate", 50.f);
        const float unlockAt = wcfg("heat_unlock_threshold", 30.f);
        (void)coolDelay;

        if (ps.heatCoolDelay > 0.f) ps.heatCoolDelay -= dt;

        if (ps.weaponOverheated) {
            // ---- QTE OPEN -> the gauge stops dead at the overheat line ----
            // This system owns the freeze because it owns cooling. Venting
            // through the minigame would tell the player they can simply wait
            // it out, which is the exact behaviour the QTE replaces. The bar
            // pinned at maximum is what turns a passive lockout into a prompt.
            if (ps.qteActive) {
                ps.weaponHeat = ps.maxWeaponHeat;

                // Keep the strain venting, so a frozen bar still looks like a
                // machine under load rather than a paused game.
                if ((rand() % 100) < 70) {
                    const float rotRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
                    const sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
                    const sf::Vector2f side(-fwd.y, fwd.x);
                    const float s = ((rand() % 2) ? 1.f : -1.f);
                    m_em->particles.push_back({
                        m_em->nextEntityId++,
                        tf.position + fwd * 18.f + side * (s * 12.f),
                        side * (s * (110.f + rand() % 90)) + fwd * float(rand() % 50),
                        sf::Color(255, 190, 150, 205),
                        0.26f, 0.42f, 3.f + rand() % 3 });
                }
                return;
            }

            // ---- Normal venting (when QTE is not active) ----
            ps.weaponHeat = std::max(0.f, ps.weaponHeat - ventRate * dt);

            if ((rand() % 100) < 60) {
                const float rotRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
                const sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
                const sf::Vector2f side(-fwd.y, fwd.x);
                const float s = ((rand() % 2) ? 1.f : -1.f);
                const sf::Vector2f from = tf.position + fwd * 18.f + side * (s * 12.f);
                m_em->particles.push_back({
                    m_em->nextEntityId++,
                    from,
                    side * (s * (90.f + rand() % 70)) + fwd * static_cast<float>(rand() % 40),
                    sf::Color(255, 200, 160, 190),
                    0.30f + (rand() % 20) / 100.f,
                    0.5f,
                    3.f + rand() % 3
                    });
            }

            if (ps.weaponHeat <= unlockAt) {
                ps.weaponOverheated = false;
                ps.heatCoolDelay = 0.f;
                m_em->spawnShockRing(tf.position, 12.f, 70.f, 0.25f,
                    sf::Color(120, 220, 255), 3.f, 200.f);
            }
            return;
        }

        if (ps.heatCoolDelay <= 0.f && ps.weaponHeat > 0.f) {
            const float t = ps.weaponHeat / std::max(1.f, ps.maxWeaponHeat);
            ps.weaponHeat = std::max(0.f, ps.weaponHeat - coolRate * (0.6f + t * 0.8f) * dt);
        }

        // Ambient shimmer once genuinely hot — the telegraph.
        const float heatT = ps.weaponHeat / std::max(1.f, ps.maxWeaponHeat);
        if (heatT > 0.55f && (rand() % 100) < static_cast<int>(heatT * 45.f)) {
            const float rotRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
            const sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
            const sf::Vector2f nose = tf.position + fwd * 28.f;
            const float a = (rand() % 360) * 3.14159f / 180.f;
            m_em->particles.push_back({
                m_em->nextEntityId++,
                nose + sf::Vector2f(std::cos(a), std::sin(a)) * 6.f,
                sf::Vector2f(std::cos(a), std::sin(a)) * (25.f + rand() % 35),
                heatColor(heatT, 200),
                0.22f, 0.3f,
                2.f + heatT * 2.f
                });
        }
    }

    /**
     * @brief Every heat source in the game goes through here.
     *
     * ONE funnel, deliberately. Plasma (line ~227) and Rift (line ~151) both
     * call this, so a single guard covers every weapon, present and future.
     * Any new weapon that adds heat by touching ps.weaponHeat directly will
     * silently ignore overdrive -- which is precisely how you get "it works
     * for plasma but not the Rift". Do not write to weaponHeat anywhere except
     * this function and the cooling code.
     */
    void addHeat(PlayerComponent& ps, float amount, sf::Vector2f pos) {
        // OVERDRIVE: earned by an amber vent. NOTHING generates heat.
        //
        // Early return rather than scaling the amount down, so the promise
        // stays literal: "no weapon generates heat" should mean the gauge does
        // not move, not that it moves slower.
        if (ps.overdriveTimer > 0.f) {
            ps.weaponHeat = 0.f;
            ps.heatCoolDelay = 0.f;
            return;
        }

        ps.weaponHeat = std::min(ps.maxWeaponHeat, ps.weaponHeat + amount);
        ps.heatCoolDelay = wcfg("heat_cool_delay", 0.40f);

        if (ps.weaponHeat >= ps.maxWeaponHeat && !ps.weaponOverheated) {
            ps.weaponOverheated = true;
            ps.riftCharging = false;
            m_em->addTrauma(wcfg("overheat_trauma", 0.45f));
            m_em->spawnScreenFlash(sf::Color(255, 140, 60), 0.25f, 85.f);
            m_em->spawnShockRing(pos, 14.f, 120.f, 0.35f,
                sf::Color(255, 130, 40), 5.f, 240.f);
        }
    }

    /// Barrel colour ramp: cyan (cold) -> orange -> white-hot.
    static sf::Color heatColor(float t, uint8_t alpha) {
        t = std::clamp(t, 0.f, 1.f);
        float r, g, b;
        if (t < 0.5f) {
            const float u = t / 0.5f;
            r = 255.f * u;
            g = 220.f + (150.f - 220.f) * u;
            b = 200.f + (40.f - 200.f) * u;
        }
        else {
            const float u = (t - 0.5f) / 0.5f;
            r = 255.f;
            g = 150.f + (255.f - 150.f) * u;
            b = 40.f + (235.f - 40.f) * u;
        }
        return sf::Color(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
            static_cast<uint8_t>(b), alpha);
    }

    void spawnDenyPuff(const TransformComponent& tf) {
        const float rotRad = (tf.rotation - 90.f) * 3.14159f / 180.f;
        const sf::Vector2f fwd(std::cos(rotRad), std::sin(rotRad));
        const sf::Vector2f nose = tf.position + fwd * 40.f;
        for (int n = 0; n < 6; ++n) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            m_em->particles.push_back({
                m_em->nextEntityId++,
                nose,
                sf::Vector2f(std::cos(a), std::sin(a)) * (40.f + rand() % 40),
                sf::Color(150, 150, 160, 160),
                0.18f, 0.18f,
                2.f + rand() % 2
                });
        }
    }

    float wcfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["weapon"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;

    float m_denyCooldown = 0.f;
};