/**
 * @file DamageSystem.hpp
 * @brief Handles collisions, damage application, and entity death
 *
 * CHANGED in 1.3 — DAMAGE IS NOW CARRIED BY THE PROJECTILE.
 *
 * Previously this system hardcoded `15.0f` for any bullet hitting an asteroid
 * and `25.0f` for any bullet hitting an enemy, regardless of what the bullet
 * actually was. That meant a fully-charged Rift Bolt landing a direct hit dealt
 * exactly as much as a basic shot — 25 against a 250 HP enemy, i.e. 10 hits
 * either way. The projectile type was invisible to the damage path.
 *
 * Now `BulletComponent` carries `damage`, `knockback` and `stunOnHit`, set at
 * creation time from Lua. This system just reads them. Adding a new weapon no
 * longer requires touching this file.
 *
 * Also new: reflected (parried) bullets are set up as homing seekers with a
 * real damage number and a stun, so a parry-into-reflect is a genuine punish
 * rather than a slightly-better basic shot.
 *
 * @author Oleg Ivakhiv
 * @version 1.3 (per-projectile damage)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include <cfloat>
#include <cmath>
#include <vector>
#include <algorithm>

class DamageSystem : public ISystem {
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

        // ====================================================================
        // 1. UPDATE INVULNERABILITY TIMERS
        // ====================================================================
        if (m_em->healths[playerIdx].invulTimer > 0)
            m_em->healths[playerIdx].invulTimer -= dt;
        if (m_em->healths[playerIdx].cheapInvulTimer > 0)
            m_em->healths[playerIdx].cheapInvulTimer -= dt;

        // ---- Shared parry-success reaction ----
        auto onParrySuccess = [&](sf::Vector2f contactPos) {
            // This flag was never being set before 1.3, so the whiff penalty
            // fired on every parry including the ones that connected.
            m_em->players[playerIdx].parryHitSomething = true;
            m_em->players[playerIdx].parryFlashTimer = 0.25f;

            m_em->triggerParrySuccess(
                contactPos,
                vcfg("parry_hitstop_freeze", 0.06f),
                vcfg("parry_hitstop_slomo", 0.35f),
                vcfg("parry_hitstop_min_scale", 0.25f),
                vcfg("parry_trauma", 0.75f),
                vcfg("parry_flash_alpha", 170.f));
        };

        // ====================================================================
        // 2. PROCESS BOX2D CONTACT EVENTS
        // ====================================================================
        b2ContactEvents events = b2World_GetContactEvents(m_worldId);
        b2BodyId playerBody = m_em->physics[playerIdx].bodyId;

        for (int i = 0; i < events.beginCount; ++i) {
            b2ContactBeginTouchEvent* event = events.beginEvents + i;
            b2BodyId bodyA = b2Shape_GetBody(event->shapeIdA);
            b2BodyId bodyB = b2Shape_GetBody(event->shapeIdB);

            BodyUserData* udA = (BodyUserData*)b2Body_GetUserData(bodyA);
            BodyUserData* udB = (BodyUserData*)b2Body_GetUserData(bodyB);

            BodyType typeA = udA ? udA->type : BodyType::Asteroid;
            BodyType typeB = udB ? udB->type : BodyType::Asteroid;

            // ================================================================
            // 2a. BULLET vs TARGET  (damage read from the projectile)
            // ================================================================
            b2BodyId bulletBody = b2_nullBodyId;
            b2BodyId targetBody = b2_nullBodyId;
            BodyType targetType = BodyType::Asteroid;

            if (typeA == BodyType::Bullet) {
                bulletBody = bodyA; targetBody = bodyB; targetType = typeB;
            }
            else if (typeB == BodyType::Bullet) {
                bulletBody = bodyB; targetBody = bodyA; targetType = typeA;
            }

            if (b2Body_IsValid(bulletBody) && b2Body_IsValid(targetBody)) {
                BodyUserData* bulletUD = (BodyUserData*)b2Body_GetUserData(bulletBody);
                BodyUserData* targetUD = (BodyUserData*)b2Body_GetUserData(targetBody);
                if (!bulletUD || !targetUD) continue;

                size_t bulletIdx = m_em->getEntityIndex(bulletUD->entityId);
                size_t targetIdx = m_em->getEntityIndex(targetUD->entityId);
                if (bulletIdx == (size_t)-1 || targetIdx == (size_t)-1) continue;

                auto& blt = m_em->bullets[bulletIdx];

                if (!blt.markedForDestroy && !blt.isEnemyBullet) {
                    sf::Vector2f hitPos = m_em->transforms[bulletIdx].position;
                    sf::Vector2f hitVel = m_em->transforms[bulletIdx].velocity;

                    // ---- THE FIX: damage comes from the projectile ----
                    const float dmg = blt.damage * blt.damageMultiplier;

                    if (targetType == BodyType::Asteroid) {
                        m_em->healths[targetIdx].currentHp -= dmg;
                        applyKnockback(targetIdx, hitVel, blt.knockback);
                        m_em->spawnImpact(hitPos, sf::Color(180, 180, 180), hitVel);
                        spawnHitFx(blt, hitPos, targetIdx, false);
                        blt.markedForDestroy = true;
                    }
                    else if (targetType == BodyType::Enemy) {
                        m_em->healths[targetIdx].currentHp -= dmg;
                        applyKnockback(targetIdx, hitVel, blt.knockback);
                        m_em->enemies[targetIdx].hitFlashTimer = 0.18f;
                        m_em->enemies[targetIdx].timesHit++;

                        // ---- CAUGHT MID-DODGE ----
                        // Landing a shot on an enemy that is committed to an
                        // evasive burst staggers it. This rewards leading the
                        // dodge rather than tracking it, which is the skill the
                        // dodge system is meant to teach.
                        auto& tec = m_em->enemies[targetIdx];
                        if (tec.dodgeFlashTimer > 0.f &&
                            tec.staggerTimer <= 0.f &&
                            blt.damage >= wcfg("dodge_punish_min_damage", 40.f) &&
                            (rand() % 100) < static_cast<int>(wcfg("dodge_punish_chance", 45.f)))
                        {
                            staggerEnemy(targetIdx, hitVel,
                                wcfg("dodge_punish_knockback", 320.f), 0.45f);
                            m_em->requestHitstop(0.02f, 0.09f, 0.45f);
                        }

                        if (blt.stunOnHit > 0.f) {
                            m_em->healths[targetIdx].stunTimer =
                                std::max(m_em->healths[targetIdx].stunTimer, blt.stunOnHit);
                        }

                        m_em->spawnImpact(hitPos, sf::Color::Yellow, hitVel);
                        m_em->spawnExplosion(hitPos, sf::Color::Red, 5, 2.0f);
                        spawnHitFx(blt, hitPos, targetIdx, true);
                        blt.markedForDestroy = true;
                    }
                }
            }

            // ================================================================
            // 2b. PLAYER COLLISION (Handles Parry & Deflections)
            // ================================================================
            bool isPlayerA = B2_ID_EQUALS(bodyA, playerBody);
            bool isPlayerB = B2_ID_EQUALS(bodyB, playerBody);

            if (isPlayerA || isPlayerB) {
                b2BodyId otherBody = isPlayerA ? bodyB : bodyA;
                BodyUserData* otherUD = (BodyUserData*)b2Body_GetUserData(otherBody);
                BodyType otherType = otherUD ? otherUD->type : BodyType::Asteroid;

                size_t otherIdx = (size_t)-1;
                for (size_t idx = 0; idx < m_em->physics.size(); ++idx) {
                    if (B2_ID_EQUALS(m_em->physics[idx].bodyId, otherBody)) {
                        otherIdx = idx;
                        break;
                    }
                }

                bool isParryActive = m_em->players[playerIdx].parryTimer > 0;

                // ---- PARRY ACTIVE ----
                if (isParryActive && otherIdx != (size_t)-1) {
                    sf::Vector2f playerPos = m_em->transforms[playerIdx].position;
                    sf::Vector2f otherPos = m_em->transforms[otherIdx].position;

                    // --- PARRY ASTEROID ---
                    if (otherType == BodyType::Asteroid) {
                        int reward = m_em->scoreRewards[otherIdx];
                        bool isLarge = (reward >= 200);
                        bool isMagmatic = (reward == 75);

                        m_em->spawnExplosion(otherPos, sf::Color(0, 255, 200), 15, 2.5f);

                        if (!isLarge && !isMagmatic) {
                            m_em->healths[otherIdx].currentHp = -1.f;
                        }
                        else {
                            sf::Vector2f dir = otherPos - playerPos;
                            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                            if (len > 0.01f) dir /= len;

                            float launchSpeed = (*m_lua)["parry_asteroid_launch_speed"].get_or(1000.f);
                            b2Body_SetLinearVelocity(m_em->physics[otherIdx].bodyId,
                                { dir.x * launchSpeed / SCALE, dir.y * launchSpeed / SCALE });

                            m_em->healths[otherIdx].wasParryLaunched = true;
                            m_em->healths[otherIdx].isHoming = true;
                        }
                        onParrySuccess((playerPos + otherPos) * 0.5f);
                        continue;
                    }

                    // --- PARRY ENEMY ---
                    else if (otherType == BodyType::Enemy) {
                        sf::Vector2f away = otherPos - playerPos;
                        float len = std::sqrt(away.x * away.x + away.y * away.y);
                        if (len > 0.01f) away /= len;

                        float stunDuration = (*m_lua)["parry_stun_duration"].get_or(1.5f);
                        float reflectDamage = (*m_lua)["parry_reflect_damage"].get_or(50.f);

                        m_em->healths[otherIdx].currentHp -= reflectDamage;
                        m_em->healths[otherIdx].stunTimer = stunDuration;

                        // ---- FULL STAGGER, not just a shove ----
                        // A melee parry is the highest-risk thing the player
                        // can do, so it gets the loudest reaction available.
                        staggerEnemy(otherIdx, away,
                            wcfg("parry_melee_knockback", 1100.f), 1.0f);

                        m_em->enemies[otherIdx].hitFlashTimer = 0.22f;
                        m_em->spawnExplosion(otherPos, sf::Color(0, 255, 200), 22, 3.0f);

                        onParrySuccess((playerPos + otherPos) * 0.5f);
                        continue;
                    }

                    // --- PARRY ENEMY BULLET -> HOMING COUNTER-SHOT ---
                    else if (otherType == BodyType::Bullet) {
                        if (m_em->bullets[otherIdx].isEnemyBullet) {
                            m_em->bullets[otherIdx].markedForDestroy = true;

                            size_t enemyIdx = findNearestEnemy(otherPos);
                            sf::Vector2f reflectDir = (enemyIdx != (size_t)-1) ?
                                (m_em->transforms[enemyIdx].position - otherPos)
                                : (otherPos - playerPos);

                            float len = std::sqrt(reflectDir.x * reflectDir.x + reflectDir.y * reflectDir.y);
                            if (len > 0.01f) reflectDir /= len;

                            float bulletSpeed = wcfg("reflect_speed", 1000.f);
                            sf::Vector2f reflectedVel(reflectDir.x * bulletSpeed, reflectDir.y * bulletSpeed);
                            float angle = std::atan2(reflectDir.y, reflectDir.x) * 180.f / 3.14159f + 90.f;

                            uint32_t newBulletId = m_ef->createBullet(*m_em, otherPos, reflectedVel,
                                angle, *m_lua, m_worldId);
                            size_t newBulletIdx = m_em->getEntityIndex(newBulletId);

                            if (newBulletIdx != (size_t)-1) {
                                auto& nb = m_em->bullets[newBulletIdx];
                                nb.isReflected = true;
                                nb.damageMultiplier = 1.0f;   // damage is absolute now

                                // ---- A parry-reflect should be a PUNISH ----
                                // Enemy HP is 250, so ~100 makes this a 3-shot
                                // kill (2 if you also land anything else).
                                nb.damage = wcfg("reflect_damage", 100.f);
                                nb.knockback = wcfg("reflect_knockback", 700.f);
                                nb.stunOnHit = wcfg("reflect_stun", 0.9f);
                                nb.lifetime = wcfg("reflect_lifetime", 3.0f);

                                // ---- Homing ----
                                // Reflects were previously fired at the nearest
                                // enemy's position at that instant, so any enemy
                                // that was moving simply wasn't there when the
                                // shot arrived. Now the bullet steers.
                                if (enemyIdx != (size_t)-1) {
                                    nb.homingTargetEntityId = m_em->transforms[enemyIdx].entityId;
                                    nb.homingTurnRate = wcfg("reflect_turn_rate", 420.f);
                                }

                                // Visual: reflected rounds are bigger and gold.
                                auto& sh = m_em->renders[newBulletIdx].shape;
                                sh.setPointCount(4);
                                sh.setPoint(0, { 0.f, -18.f });
                                sh.setPoint(1, { 4.5f,  0.f });
                                sh.setPoint(2, { 0.f,  18.f });
                                sh.setPoint(3, { -4.5f,  0.f });
                                sh.setFillColor(sf::Color(255, 245, 190));
                                sh.setOutlineThickness(2.2f);
                                sh.setOutlineColor(sf::Color(255, 190, 40, 235));
                            }

                            m_em->spawnExplosion(otherPos, sf::Color(0, 255, 200), 10, 1.5f);
                            onParrySuccess(otherPos);
                            continue;
                        }
                    }
                }

                // ---- NORMAL UNPARRIED COLLISION DAMAGE ----
                b2Vec2 vA = b2Body_GetLinearVelocity(bodyA);
                b2Vec2 vB = b2Body_GetLinearVelocity(bodyB);
                float relativeSpeed = std::sqrt(std::pow(vA.x - vB.x, 2) + std::pow(vA.y - vB.y, 2));

                if (relativeSpeed > 12.0f && m_em->healths[playerIdx].invulTimer <= 0) {
                    m_em->healths[playerIdx].currentHp -= 15.0f;
                    m_em->healths[playerIdx].invulTimer = 1.0f;

                    // ---- HARD IMPACT -> STAGGER ----
                    float staggerSpeed = vcfg("stagger_speed_threshold", 20.0f);
                    if (relativeSpeed > staggerSpeed && otherIdx != (size_t)-1) {
                        sf::Vector2f away = m_em->transforms[playerIdx].position
                            - m_em->transforms[otherIdx].position;
                        m_em->staggerPlayer(playerIdx, away,
                            vcfg("stagger_knockback", 900.f),
                            vcfg("stagger_tumble_duration", 1.1f),
                            vcfg("stagger_recover_duration", 0.55f),
                            vcfg("stagger_spin_speed", 620.f));
                    }
                }
                else if (relativeSpeed > 1.5f && m_em->healths[playerIdx].invulTimer <= 0 &&
                    m_em->healths[playerIdx].cheapInvulTimer <= 0) {
                    m_em->healths[playerIdx].currentHp -= 1.0f;
                    m_em->healths[playerIdx].cheapInvulTimer = 0.2f;
                }
            }

            // ================================================================
            // 2c. ENEMY BULLET hits Player / Asteroid
            // ================================================================
            if (b2Body_IsValid(bulletBody) && b2Body_IsValid(targetBody)) {
                BodyUserData* bulletUD = (BodyUserData*)b2Body_GetUserData(bulletBody);
                BodyUserData* targetUD = (BodyUserData*)b2Body_GetUserData(targetBody);
                if (bulletUD && targetUD) {
                    size_t bulletIdx = m_em->getEntityIndex(bulletUD->entityId);
                    size_t targetIdx = m_em->getEntityIndex(targetUD->entityId);

                    if (bulletIdx != (size_t)-1 && targetIdx != (size_t)-1 &&
                        !m_em->bullets[bulletIdx].markedForDestroy &&
                        m_em->bullets[bulletIdx].isEnemyBullet)
                    {
                        auto& blt = m_em->bullets[bulletIdx];
                        BodyType hitType = targetUD->type;
                        sf::Vector2f hitPos = m_em->transforms[bulletIdx].position;
                        sf::Vector2f hitVel = m_em->transforms[bulletIdx].velocity;

                        if (hitType == BodyType::Player && m_em->healths[playerIdx].invulTimer <= 0) {
                            m_em->healths[playerIdx].currentHp -= blt.damage;
                            m_em->healths[playerIdx].invulTimer = 0.8f;
                            m_em->spawnExplosion(hitPos, sf::Color(255, 100, 0), 8, 2.f);
                            m_em->spawnImpact(hitPos, sf::Color(255, 140, 0), hitVel);
                        }
                        else if (hitType == BodyType::Asteroid) {
                            m_em->healths[targetIdx].currentHp -= blt.damage * 0.6f;
                            m_em->spawnImpact(hitPos, sf::Color(255, 120, 0), hitVel);
                        }

                        blt.markedForDestroy = true;
                    }
                }
            }

            // ================================================================
            // 2d. ASTEROID-ASTEROID COLLISION (speed-based damage)
            // ================================================================
            if (typeA == BodyType::Asteroid && typeB == BodyType::Asteroid) {
                if (!udA || !udB) continue;
                size_t idxA = m_em->getEntityIndex(udA->entityId);
                size_t idxB = m_em->getEntityIndex(udB->entityId);
                if (idxA == (size_t)-1 || idxB == (size_t)-1) continue;

                b2Vec2 vA2 = b2Body_GetLinearVelocity(bodyA);
                b2Vec2 vB2 = b2Body_GetLinearVelocity(bodyB);
                float relSpd = std::sqrt(std::pow(vA2.x - vB2.x, 2) + std::pow(vA2.y - vB2.y, 2));

                if (relSpd > 6.f) {
                    float dmg = (relSpd - 6.f) * 3.f;
                    float multA = m_em->healths[idxA].isHoming ? 2.5f : 1.f;
                    float multB = m_em->healths[idxB].isHoming ? 2.5f : 1.f;

                    m_em->healths[idxA].currentHp -= dmg * multB;
                    m_em->healths[idxB].currentHp -= dmg * multA;

                    sf::Vector2f midPos = (m_em->transforms[idxA].position + m_em->transforms[idxB].position) * 0.5f;
                    m_em->spawnImpact(midPos, sf::Color(180, 180, 180),
                        sf::Vector2f((vA2.x - vB2.x) * SCALE * 0.5f,
                            (vA2.y - vB2.y) * SCALE * 0.5f));

                    if (m_em->healths[idxA].isHoming && m_em->healths[idxA].isExplosive)
                        m_em->healths[idxA].currentHp = -1.f;
                    if (m_em->healths[idxB].isHoming && m_em->healths[idxB].isExplosive)
                        m_em->healths[idxB].currentHp = -1.f;
                }
            }

            // ================================================================
            // 2e. ASTEROID-ENEMY COLLISION (speed-based damage)
            // ================================================================
            {
                b2BodyId astBody = b2_nullBodyId, enBody = b2_nullBodyId;
                BodyUserData* astUD = nullptr; BodyUserData* enUD = nullptr;

                if (typeA == BodyType::Asteroid && typeB == BodyType::Enemy) {
                    astBody = bodyA; astUD = udA; enBody = bodyB; enUD = udB;
                }
                else if (typeB == BodyType::Asteroid && typeA == BodyType::Enemy) {
                    astBody = bodyB; astUD = udB; enBody = bodyA; enUD = udA;
                }

                if (astUD && enUD) {
                    size_t astIdx = m_em->getEntityIndex(astUD->entityId);
                    size_t enIdx = m_em->getEntityIndex(enUD->entityId);

                    if (astIdx != (size_t)-1 && enIdx != (size_t)-1) {
                        b2Vec2 vA2 = b2Body_GetLinearVelocity(astBody);
                        b2Vec2 vB2 = b2Body_GetLinearVelocity(enBody);
                        float relSpd = std::sqrt(std::pow(vA2.x - vB2.x, 2) +
                            std::pow(vA2.y - vB2.y, 2));

                        auto& astHp = m_em->healths[astIdx];
                        const sf::Vector2f astPos = m_em->transforms[astIdx].position;
                        const sf::Vector2f enPos = m_em->transforms[enIdx].position;

                        // ---- Explosive kinetic rock: let the blast do the work ----
                        if (astHp.isHoming && astHp.isExplosive) {
                            astHp.currentHp = -1.f;
                        }
                        // ---- KINETIC WEAPON (parry-launched OR rift-hijacked) ----
                        else if (astHp.isKineticWeapon || astHp.wasParryLaunched) {

                            // Damage scales with SIZE and IMPACT SPEED.
                            //
                            // Size matters because the player chooses which rock
                            // to hijack — that choice should be meaningful. A
                            // LARGE rock is a committed setup and one-shots; a
                            // chip is an opportunistic poke.
                            //
                            // Speed is clamped so a glancing bump still hurts
                            // (0.4 floor) and a freak high-speed collision can't
                            // scale to absurdity (1.35 ceiling).
                            const float base = wcfg("kinetic_base_damage", 170.f);
                            const float speedF = std::clamp(relSpd / 22.f, 0.40f, 1.35f);

                            float tierMult;
                            switch (astHp.asteroidTier) {
                            case 2:  tierMult = wcfg("kinetic_tier_large", 1.50f); break;
                            case 3:  tierMult = wcfg("kinetic_tier_magma", 2.00f); break;
                            case 1:  tierMult = wcfg("kinetic_tier_medium", 0.78f); break;
                            default: tierMult = wcfg("kinetic_tier_small", 0.38f); break;
                            }

                            const float dmg = base * tierMult * speedF;
                            m_em->healths[enIdx].currentHp -= dmg;
                            m_em->enemies[enIdx].hitFlashTimer = 0.22f;

                            // ---- Knockback + stagger ----
                            sf::Vector2f push = enPos - astPos;
                            const float pl = std::sqrt(push.x * push.x + push.y * push.y);
                            if (pl > 0.01f) {
                                push /= pl;
                                const float k = wcfg("kinetic_knockback", 950.f) * tierMult;
                                b2Body_ApplyLinearImpulseToCenter(m_em->physics[enIdx].bodyId,
                                    { push.x * k / SCALE, push.y * k / SCALE }, true);
                            }
                            staggerEnemy(enIdx, push,
                                wcfg("kinetic_knockback", 950.f) * 0.8f,
                                std::clamp(tierMult, 0.f, 1.f));

                            // ---- The rock is SPENT ----
                            // It was a projectile. Letting it bounce off and
                            // remain a live hazard undercuts the whole play.
                            astHp.currentHp = -1.f;

                            // Feedback scaled to the size of the hit.
                            m_em->addTrauma(0.25f + 0.30f * tierMult);
                            m_em->requestHitstop(0.03f, 0.12f + 0.06f * tierMult, 0.38f);
                            m_em->spawnShockRing(astPos, 15.f, 120.f + 110.f * tierMult,
                                0.40f, sf::Color(0, 255, 200), 6.f, 245.f);
                            m_em->spawnExplosion(astPos, sf::Color(0, 255, 200),
                                20 + static_cast<int>(18 * tierMult), 3.5f);
                        }
                        // ---- Ordinary accidental collision ----
                        else if (relSpd > 8.f) {
                            float dmg = (relSpd - 8.f) * 2.5f;
                            m_em->healths[enIdx].currentHp -= dmg;
                            sf::Vector2f midPos = (astPos + enPos) * 0.5f;
                            m_em->spawnImpact(midPos, sf::Color(180, 120, 60),
                                sf::Vector2f(vA2.x * SCALE * 0.3f, vA2.y * SCALE * 0.3f));
                        }
                    
                        // ---- Heavy rock impact staggers the pirate ----
                        // Same threshold the player uses, so the rule is
                        // symmetric and the player can predict it.
                        if (relSpd > vcfg("stagger_speed_threshold", 20.f)) {
                            sf::Vector2f away = m_em->transforms[enIdx].position
                                - m_em->transforms[astIdx].position;
                            staggerEnemy(enIdx, away,
                                vcfg("stagger_knockback", 900.f) * 0.8f, 0.9f);
                            m_em->enemies[enIdx].hitFlashTimer = 0.2f;
                        }
                    }
                }
            }
        }

        // ====================================================================
        // 3. DESTROY DEAD ENTITIES
        // ====================================================================
        std::vector<size_t> indicesToDestroy;
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (i == playerIdx) continue;
            b2BodyId bodyId = m_em->physics[i].bodyId;
            if (!b2Body_IsValid(bodyId)) continue;

            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid;
            bool shouldDestroy = false;

            if (m_em->healths[i].currentHp <= 0) {
                shouldDestroy = true;
                sf::Vector2f deathPos = m_em->transforms[i].position;

                if (type == BodyType::Asteroid) {
                    int reward = m_em->scoreRewards[i];
                    m_em->totalScore += reward;
                    bool isExplosive = m_em->healths[i].isExplosive;

                    if (isExplosive) {
                        bool wasHoming = m_em->healths[i].isHoming;
                        float damageMult = wasHoming ? 5.f : 1.f;
                        float radiusMult = wasHoming ? 1.5f : 1.f;

                        float radius = m_em->healths[i].explosionRadius * radiusMult;
                        float damage = m_em->healths[i].explosionDamage * damageMult;

                        sf::Color ringColor = wasHoming
                            ? sf::Color(0, 255, 180, 220)
                            : sf::Color(255, 80, 0, 220);

                        m_em->addDebugAoE(deathPos, radius, ringColor, 0.4f);
                        m_em->spawnMagmaExplosion(deathPos, radius, wasHoming);

                        // Magma rocks throw shards too.
                        sf::Vector2f impactDir(0.f, 0.f);
                        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
                            b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
                            impactDir = { v.x, v.y };
                        }
                        m_em->fractureAsteroid(i, impactDir, 0, m_ef, m_lua, m_worldId);

                        if (wasHoming) {
                            m_em->spawnExplosion(deathPos, sf::Color(0, 255, 150), 20, 4.0f);
                        }

                        for (int angle = 0; angle < 360; angle += 15) {
                            float rad = angle * 3.14159f / 180.f;
                            sf::Vector2f dir(std::cos(rad), std::sin(rad));
                            m_em->spawnImpact(sf::Vector2f(deathPos.x + dir.x * 30, deathPos.y + dir.y * 30),
                                sf::Color(255, 100, 0), sf::Vector2f(dir.x * 500, dir.y * 500));
                        }

                        // Apply AoE damage (and stagger the player if close)
                        for (size_t j = 0; j < m_em->physics.size(); ++j) {
                            if (j == i) continue;
                            sf::Vector2f otherPos = m_em->transforms[j].position;
                            float ddx = deathPos.x - otherPos.x;
                            float ddy = deathPos.y - otherPos.y;
                            float dist = std::sqrt(ddx * ddx + ddy * ddy);
                            if (dist < radius) {
                                float falloff = 1.0f - (dist / radius);
                                m_em->healths[j].currentHp -= damage * falloff;

                                if (j == playerIdx && falloff > vcfg("stagger_blast_falloff", 0.45f)) {
                                    m_em->staggerPlayer(playerIdx, otherPos - deathPos,
                                        vcfg("stagger_knockback", 900.f) * falloff,
                                        vcfg("stagger_tumble_duration", 1.1f),
                                        vcfg("stagger_recover_duration", 0.55f),
                                        vcfg("stagger_spin_speed", 620.f));
                                }
                            }
                        }
                    }
                    else {
                        // ---- FRACTURE ----
                        // Direction of the killing blow. Fragments need it, or
                        // the break is radially symmetric and reads as a
                        // firework instead of an impact.
                        sf::Vector2f impactDir(0.f, 0.f);
                        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
                            b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
                            impactDir = { v.x, v.y };
                        }

                        int children = 0;
                        if (m_em->healths[i].asteroidTier >= 2)      children = 3;
                        else if (m_em->healths[i].asteroidTier == 1) children = 2;

                        m_em->fractureAsteroid(i, impactDir, children,
                            m_ef, m_lua, m_worldId);
                        m_em->addTrauma(0.10f + 0.10f * m_em->healths[i].asteroidTier);
                    }
                }
                else if (type == BodyType::Enemy) {
                    m_em->totalScore += m_em->scoreRewards[i];
                    m_em->spawnExplosion(deathPos, sf::Color::Red, 35, 5.0f);
                    m_em->spawnExplosion(deathPos, sf::Color::Yellow, 15, 2.5f);
                    m_em->spawnShockRing(deathPos, 15.f, 190.f, 0.40f,
                        sf::Color(255, 90, 40), 5.f, 230.f);
                    m_em->addTrauma(0.30f);
                }
            }
            else if (type == BodyType::Bullet &&
                (m_em->bullets[i].markedForDestroy || m_em->bullets[i].lifetime <= 0)) {
                shouldDestroy = true;
            }

            if (shouldDestroy) indicesToDestroy.push_back(i);
        }

        for (size_t i = indicesToDestroy.size(); i-- > 0; ) {
            m_em->destroyEntity(indicesToDestroy[i]);
        }
    }

private:
    EntityManager* m_em = nullptr;
    EntityFactory* m_ef = nullptr;
    b2WorldId m_worldId;
    uint32_t m_playerEntityId = 0;
    sol::state* m_lua = nullptr;

    /// Push a target along the projectile's travel direction.
    void applyKnockback(size_t targetIdx, sf::Vector2f bulletVel, float knockback) {
        if (knockback <= 0.f) return;
        float sp = std::sqrt(bulletVel.x * bulletVel.x + bulletVel.y * bulletVel.y);
        if (sp < 0.01f) return;
        b2Body_ApplyLinearImpulseToCenter(m_em->physics[targetIdx].bodyId,
            { (bulletVel.x / sp) * knockback / SCALE,
              (bulletVel.y / sp) * knockback / SCALE }, true);
    }

    /// Impact feedback scaled to how big the projectile actually was.
    void spawnHitFx(const BulletComponent& blt, sf::Vector2f pos, size_t targetIdx, bool isEnemy) {
        if (blt.isRiftBolt) {
            m_em->addTrauma(vcfg("rift_direct_hit_trauma", 0.40f));
            m_em->spawnShockRing(pos, 12.f, 170.f, 0.34f,
                sf::Color(180, 80, 255), 6.f, 240.f);
            m_em->spawnExplosion(pos, sf::Color(190, 110, 255), 26, 4.f);
            m_em->requestHitstop(0.03f, 0.10f, 0.45f);
            if (isEnemy) {
                b2Body_SetAngularVelocity(m_em->physics[targetIdx].bodyId,
                    ((rand() % 2) ? 1.f : -1.f) * 9.f);
            }
        }
        else if (blt.isReflected) {
            m_em->addTrauma(0.28f);
            m_em->spawnShockRing(pos, 8.f, 110.f, 0.26f,
                sf::Color(255, 200, 60), 4.f, 235.f);
            m_em->spawnExplosion(pos, sf::Color(255, 220, 120), 16, 3.f);
            m_em->requestHitstop(0.03f, 0.12f, 0.40f);
        }
    }

    float vcfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }
    float wcfg(const char* key, float def) const {
        sol::optional<sol::table> v = (*m_lua)["weapon"];
        if (!v) return def;
        return (*v)[key].get_or(def);
    }

    size_t findNearestEnemy(sf::Vector2f pos) {
        size_t nearestIdx = (size_t)-1;
        float nearestDistSq = FLT_MAX;

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId)) continue;
            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;

            sf::Vector2f enemyPos = m_em->transforms[i].position;
            float dx = pos.x - enemyPos.x;
            float dy = pos.y - enemyPos.y;
            float distSq = dx * dx + dy * dy;

            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearestIdx = i;
            }
        }
        return nearestIdx;
    }

    /**
 * @brief Knock an enemy out of control (mirrors EntityManager::staggerPlayer)
 * @param idx        Enemy index
 * @param knockDir   Direction to be thrown (need not be normalised)
 * @param knockSpeed Pixels/sec
 * @param severity   0..1, scales duration and spin
 *
 * Uses SetLinearVelocity rather than an impulse, for the same reason the
 * player's stagger does: a stagger should OVERRIDE momentum. An impulse gets
 * mostly cancelled when the target was already charging in, which is exactly
 * the moment a stagger needs to land hardest.
 *
 * Early-returns if already staggering. Without that guard, a pirate caught in
 * a magma cluster chains into a lockout it never escapes — the same problem
 * the player's version has.
 */
    void staggerEnemy(size_t idx, sf::Vector2f knockDir, float knockSpeed, float severity) {
        if (idx >= m_em->enemies.size()) return;
        auto& ec = m_em->enemies[idx];
        if (ec.staggerTimer > 0.f) return;

        severity = std::clamp(severity, 0.f, 1.f);

        ec.staggerDuration = wcfg("enemy_stagger_tumble", 0.85f) * (0.6f + severity * 0.7f);
        ec.staggerTimer = ec.staggerDuration;
        ec.staggerRecoverDuration = wcfg("enemy_stagger_recover", 0.5f);
        ec.staggerRecoverTimer = 0.f;
        ec.staggerSpinSpeed = ((rand() % 2) ? 1.f : -1.f) *
            wcfg("enemy_stagger_spin", 540.f) * (0.6f + severity * 0.8f);

        // Cancel competing states.
        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.stormActive = false;
        ec.stormTimer = 0.f;

        float len = std::sqrt(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
        if (len > 0.001f) {
            knockDir /= len;
            b2Body_SetLinearVelocity(m_em->physics[idx].bodyId,
                { knockDir.x * knockSpeed / SCALE, knockDir.y * knockSpeed / SCALE });
        }

        const sf::Vector2f p = m_em->transforms[idx].position;
        m_em->spawnShockRing(p, 12.f, 150.f, 0.32f, sf::Color(255, 170, 60), 4.f, 220.f);
        m_em->spawnExplosion(p, sf::Color(255, 160, 80), 16, 3.f);
    }

};