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
 * CHANGED in 1.4 — MELEE CONTRACTS (Berserker)
 *
 *  - BASH STRIKES. AISystem raises EnemyComponent::bashStrikePending on the
 *    frame a lunge reaches the player; resolveBashStrikes() runs FIRST each
 *    frame and routes a parrying player into parryEnemy() -- the very same
 *    function a contact parry uses -- so "what a parried ship does" has one
 *    definition. Contact events from a bash-committed enemy are ignored: the
 *    strike owns the outcome, and the hull scraping you is not a second hit.
 *
 *  - RAM DAMAGE WAS NEVER APPLIED. `ram_damage` has sat in enemy.lua since
 *    the Barge shipped, but nothing read it: a charge landed as ordinary
 *    collision damage, a flat 15. applyRamHit() now reads it.
 *
 *  - PARRYING A RAM MADE YOU IMMUNE. The "parry whiffs against a charge"
 *    branch did `continue`, which skipped the collision damage below it. The
 *    whiff FX played and the player took nothing -- so the one tool the ram
 *    contract says does NOT work was the safest answer to it. The whiff now
 *    falls through into the hit. For the Berserker this is load-bearing: its
 *    entire skill test is "parry the bash, dodge the charge", and that test
 *    does not exist if parry beats both.
 *
 *  - PER-BULLET I-FRAMES (BulletComponent::playerIframes).
 *
 *  - PER-ARCHETYPE DEATH (`death_style = "visceral"`): the hull splits into
 *    its own triangles.
 *
 * @author Oleg Ivakhiv
 * @version 1.4 (melee contracts)
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"        // added for archetype registry
#include <cfloat>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

class DamageSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_ef = ctx.ef;
        m_worldId = ctx.worldId;
        m_playerEntityId = ctx.playerEntityId;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;    // store registry pointer
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
        if (m_em->players[playerIdx].perfectParryFlash > 0.f) {
            m_em->players[playerIdx].perfectParryFlash -= dt;
            if (m_em->players[playerIdx].perfectParryFlash <= 0.f)
                m_em->players[playerIdx].perfectParryChain = 0;
        }
        // ====================================================================
        // 1b. BASH STRIKES raised by AISystem last frame
        // ====================================================================
        // Before contacts, so a lunge that also produced a begin-touch this
        // frame is already settled when that contact is looked at.
        resolveBashStrikes(playerIdx);
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
                        // ---- RAM IMMUNITY: charging enemies ignore all damage ----
                        if (isRamInvulnerable(targetIdx)) {
                            // Sparks off the prow. The player must SEE that the
                            // shot connected and did nothing, or they will read
                            // it as a miss and keep shooting.
                            m_em->spawnImpact(hitPos, sf::Color(255, 230, 160), hitVel);
                            blt.markedForDestroy = true;
                            continue;
                        }

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

                // ---- ENEMY MID-COMMIT: the attack owns the outcome ----
                if (otherType == BodyType::Enemy && otherIdx != (size_t)-1) {
                    if (isRamInvulnerable(otherIdx)) {
                        if (isParryActive) {
                            // Parry whiffs against a charge. Loud, so it reads
                            // as "wrong tool" rather than "the parry is buggy"
                            // -- and then the charge lands anyway. That second
                            // half was missing: this used to `continue` here.
                            const sf::Vector2f op = m_em->transforms[otherIdx].position;
                            m_em->spawnExplosion(op, sf::Color(255, 120, 60), 14, 2.4f);
                            m_em->spawnShockRing(op, 14.f, 120.f, 0.25f,
                                sf::Color(255, 120, 60), 3.f, 180.f);
                        }
                        applyRamHit(playerIdx, otherIdx);
                        continue;
                    }
                    if (isBashCommitted(otherIdx)) continue;   // resolveBashStrikes owns it
                }

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
                        onParrySuccess(playerIdx, (playerPos + otherPos) * 0.5f);
                        continue;
                    }

                    // --- PARRY ENEMY ---
                    // (A ram in Charge never reaches here -- handled above.)
                    else if (otherType == BodyType::Enemy) {
                        parryEnemy(playerIdx, otherIdx);
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
                            onParrySuccess(playerIdx, otherPos);
                            // ---- Perfect parry on bullet ----
                            if (isPerfectParry(playerIdx))
                                onPerfectParry(playerIdx, otherPos);
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
                            m_em->healths[playerIdx].invulTimer = blt.playerIframes;
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
                            // ---- RAM IMMUNITY: charging enemies ignore kinetic damage ----
                            if (!isRamInvulnerable(enIdx)) {
                                // Damage scales with SIZE and IMPACT SPEED.
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

                                // The rock is SPENT.
                                astHp.currentHp = -1.f;

                                // Feedback scaled to the size of the hit.
                                m_em->addTrauma(0.25f + 0.30f * tierMult);
                                m_em->requestHitstop(0.03f, 0.12f + 0.06f * tierMult, 0.38f);
                                m_em->spawnShockRing(astPos, 15.f, 120.f + 110.f * tierMult,
                                    0.40f, sf::Color(0, 255, 200), 6.f, 245.f);
                                m_em->spawnExplosion(astPos, sf::Color(0, 255, 200),
                                    20 + static_cast<int>(18 * tierMult), 3.5f);
                            }
                            else {
                                // Charging enemy: the rock shatters harmlessly.
                                astHp.currentHp = -1.f;
                                m_em->spawnImpact(astPos, sf::Color(200, 200, 200),
                                    sf::Vector2f(vA2.x * SCALE, vA2.y * SCALE) * 0.5f);
                            }
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
                            if (!isRamInvulnerable(enIdx)) {
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
                    spawnEnemyDeath(i, deathPos);
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
    const enemyarch::EnemyRegistry* m_registry = nullptr;   // added for archetype access

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
     * @brief Is this enemy currently untouchable?
     *
     * THE RAM CONTRACT, in one place so it cannot drift between call sites:
     * during Charge the unit takes zero damage from any source, cannot be
     * staggered, cannot be stunned, and cannot be parried.
     *
     * This is a hard rule rather than a big number, because "very tanky during
     * the charge" and "you cannot stop the charge" teach different lessons. The
     * first invites the player to try trading; the second teaches them to move.
     * Only the second is readable at a glance, and the recovery window is where
     * the damage they wanted to deal is meant to go.
     */
    bool isRamInvulnerable(size_t idx) const {
        return idx < m_em->enemies.size() &&
            m_em->enemies[idx].ramState == RamState::Charge;
    }

    /// Mid-lunge, or recoiling from a strike that already resolved. Contact
    /// begin-events from this ship are ignored -- the strike was the hit.
    bool isBashCommitted(size_t idx) const {
        if (idx >= m_em->enemies.size()) return false;
        const auto& ec = m_em->enemies[idx];
        return ec.bashState == BashState::Lunge ||
            (ec.bashState == BashState::Recover && ec.bashConnected);
    }

    /// Per-archetype float, with a fallback if the registry is missing.
    float acfg(size_t idx, const char* key, float def) const {
        if (!m_registry || idx >= m_em->enemies.size()) return def;
        return m_registry->resolve(m_em->enemies[idx].archetype).config[key].get_or(def);
    }

    // ========================================================================
    // PARRY
    // ========================================================================

    /// Shared parry-success reaction (was a lambda inside update()).
    void onParrySuccess(size_t playerIdx, sf::Vector2f contactPos) {
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
    }

    /**
     * @brief A parrying player meets an enemy hull -- by contact OR by bash.
     *
     * Moved out of the contact loop verbatim so the bash can call it. If the
     * two paths each had their own copy, the first tuning pass on one would
     * quietly make "parry a Berserker's bash" and "parry a Berserker you
     * bumped into" feel different for no reason the player could see.
     */
    void parryEnemy(size_t playerIdx, size_t otherIdx) {
        const sf::Vector2f playerPos = m_em->transforms[playerIdx].position;
        const sf::Vector2f otherPos = m_em->transforms[otherIdx].position;

        sf::Vector2f away = otherPos - playerPos;
        float len = std::sqrt(away.x * away.x + away.y * away.y);
        if (len > 0.01f) away /= len;

        float stunDuration = (*m_lua)["parry_stun_duration"].get_or(1.5f);
        float reflectDamage = (*m_lua)["parry_reflect_damage"].get_or(50.f);

        // ---- Stun resistance, separate from stagger resistance ----
        const float stunResist = std::clamp(acfg(otherIdx, "stun_resist", 0.f), 0.f, 1.f);

        m_em->healths[otherIdx].currentHp -= reflectDamage;
        m_em->healths[otherIdx].stunTimer = stunDuration * (1.f - stunResist);

        // ---- FULL STAGGER, not just a shove ----
        // A melee parry is the highest-risk thing the player can do, so it
        // gets the loudest reaction available.
        staggerEnemy(otherIdx, away, wcfg("parry_melee_knockback", 1100.f), 1.0f);

        m_em->enemies[otherIdx].hitFlashTimer = 0.22f;
        m_em->spawnExplosion(otherPos, sf::Color(0, 255, 200), 22, 3.0f);

        const sf::Vector2f mid = (playerPos + otherPos) * 0.5f;
        onParrySuccess(playerIdx, mid);
        if (isPerfectParry(playerIdx))
            onPerfectParry(playerIdx, mid);
    }

    // ========================================================================
    // MELEE HITS
    // ========================================================================

    /**
     * @brief Settle every bash that reached the player last frame.
     *
     * Parrying -> parryEnemy(): the Berserker is stunned and thrown, the
     * player gets the full parry reaction. Not parrying -> a heavy hit and a
     * stagger along the lunge.
     *
     * HIT-CONFIRM COOLDOWNS. A landed bash throws the player into a ~1.65s
     * tumble+recover. Without a lockout the Berserker is back in range and
     * winding up again before control returns -- bash, tumble, bash, tumble,
     * with no input that answers it. bash_hit_cooldown and
     * hit_confirm_cooldown (for the ram) guarantee a window of real control
     * after every connect. Applied even if i-frames ate the damage, so that
     * rule never depends on what else just happened.
     */
    void resolveBashStrikes(size_t playerIdx) {
        for (size_t i = 0; i < m_em->enemies.size(); ++i) {
            auto& ec = m_em->enemies[i];
            if (!ec.bashStrikePending) continue;
            ec.bashStrikePending = false;
            if (i == playerIdx) continue;
            if (m_em->healths[i].currentHp <= 0.f) continue;   // died mid-swing

            // ---- PARRIED: the reward ----
            if (m_em->players[playerIdx].parryTimer > 0.f) {
                parryEnemy(playerIdx, i);
                continue;
            }

            ec.bashCooldown = std::max(ec.bashCooldown, acfg(i, "bash_hit_cooldown", 2.0f));
            ec.ramCooldown = std::max(ec.ramCooldown, acfg(i, "hit_confirm_cooldown", 0.f));

            const sf::Vector2f ePos = m_em->transforms[i].position;
            const sf::Vector2f pPos = m_em->transforms[playerIdx].position;
            const sf::Vector2f contact = ePos + (pPos - ePos) * 0.6f;

            auto& php = m_em->healths[playerIdx];
            if (php.invulTimer > 0.f) {
                // Landed on i-frames. Show it connected with nothing.
                m_em->spawnImpact(contact, sf::Color(255, 230, 190), ec.bashDir * -300.f);
                continue;
            }

            php.currentHp -= acfg(i, "bash_damage", 30.f);
            php.invulTimer = acfg(i, "bash_iframes", 0.5f);

            m_em->staggerPlayer(playerIdx, ec.bashDir,
                acfg(i, "bash_knockback", 950.f),
                vcfg("stagger_tumble_duration", 1.1f),
                vcfg("stagger_recover_duration", 0.55f),
                vcfg("stagger_spin_speed", 620.f));

            // staggerPlayer already brings the shake, flash and hitstop. This
            // is just the crack at the point of contact.
            m_em->spawnShockRing(contact, 6.f, 85.f, 0.18f,
                sf::Color(255, 235, 200), 5.f, 255.f);
            m_em->spawnImpact(contact, sf::Color(255, 200, 140), ec.bashDir * -500.f);
        }
    }

    /**
     * @brief A charging ship reached the player.
     *
     * Reads `ram_damage` -- which, until 1.4, nothing did. The throw is mostly
     * SIDEWAYS out of the lane: straight along it would leave the player
     * sitting in the path of the next link of a chain.
     */
    void applyRamHit(size_t playerIdx, size_t enIdx) {
        auto& ec = m_em->enemies[enIdx];

        // A chain that lands, stops. See AISystem 2.1, note 11.
        ec.ramChainLeft = 0;
        ec.bashCooldown = std::max(ec.bashCooldown, acfg(enIdx, "hit_confirm_cooldown", 0.f));

        auto& php = m_em->healths[playerIdx];
        if (php.invulTimer > 0.f) return;

        php.currentHp -= acfg(enIdx, "ram_damage", 95.f);
        php.invulTimer = acfg(enIdx, "ram_iframes", 1.0f);

        const sf::Vector2f pPos = m_em->transforms[playerIdx].position;
        const sf::Vector2f ePos = m_em->transforms[enIdx].position;
        const sf::Vector2f lane = ec.ramDir;

        sf::Vector2f away = pPos - ePos;
        const float along = away.x * lane.x + away.y * lane.y;
        sf::Vector2f side = away - lane * along;
        float sl = std::sqrt(side.x * side.x + side.y * side.y);
        if (sl < 0.5f) {   // dead centre: pick a side
            side = ((rand() % 2) ? 1.f : -1.f) * sf::Vector2f(-lane.y, lane.x);
            sl = 1.f;
        }
        side /= sl;

        m_em->staggerPlayer(playerIdx, lane * 0.55f + side * 0.85f,
            acfg(enIdx, "ram_knockback", 1100.f),
            vcfg("stagger_tumble_duration", 1.1f),
            vcfg("stagger_recover_duration", 0.55f),
            vcfg("stagger_spin_speed", 620.f));

        m_em->spawnExplosion((pPos + ePos) * 0.5f, sf::Color(255, 190, 110), 18, 3.5f);
    }

    // ========================================================================
    // ENEMY DEATH
    // ========================================================================

    void spawnEnemyDeath(size_t i, sf::Vector2f deathPos) {
        if (m_registry) {
            const enemyarch::ArchetypeDef& adef = m_registry->resolve(m_em->enemies[i].archetype);
            if (adef.config["death_style"].get_or<std::string>("standard") == "visceral") {
                spawnVisceralDeath(i, deathPos, adef);
                return;
            }
        }

        // ---- Standard: unchanged from 1.3 ----
        m_em->spawnExplosion(deathPos, sf::Color::Red, 35, 5.0f);
        m_em->spawnExplosion(deathPos, sf::Color::Yellow, 15, 2.5f);
        m_em->spawnShockRing(deathPos, 15.f, 190.f, 0.40f,
            sf::Color(255, 90, 40), 5.f, 230.f);
        m_em->addTrauma(0.30f);
    }

    /**
     * @brief Close-range, dirty, and it comes apart.
     *
     * Where the standard death is a round blast, this one is TIGHT (small
     * fast ring, not a big slow one), carries the ship's MOMENTUM (a
     * Berserker is almost always dying at speed, pointed at you), and breaks
     * the hull into its own triangles -- the silhouette the player has been
     * reading all fight is what flies apart. No new art, no new system: the
     * shards are visualTris fed to the existing DebrisSystem.
     */
    void spawnVisceralDeath(size_t i, sf::Vector2f pos, const enemyarch::ArchetypeDef& adef) {
        const auto& tf = m_em->transforms[i];

        sf::Vector2f shipVel(0.f, 0.f);
        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
            shipVel = { v.x * SCALE, v.y * SCALE };
        }
        const float rr = tf.rotation * 3.14159f / 180.f;
        const float cs = std::cos(rr), sn = std::sin(rr);

        // 1. The frame the hull splits: one fat, very short white core.
        m_em->particles.push_back({ m_em->nextEntityId++, pos, shipVel * 0.3f,
            sf::Color(255, 245, 225, 255), 0.10f, 0.10f, 34.f });

        // 2. Tight, fast pressure ring. Small radius on purpose -- this is a
        //    point-blank death, not an area event like the magma rock.
        m_em->spawnShockRing(pos, 10.f, 125.f, 0.22f, sf::Color(255, 120, 60), 7.f, 255.f);

        // 3. Dense, dirty sparks thrown WITH the ship's momentum.
        for (int k = 0; k < 28; ++k) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = 180.f + rand() % 340;
            const float life = 0.25f + (rand() % 30) / 100.f;
            m_em->particles.push_back({ m_em->nextEntityId++, pos,
                sf::Vector2f(std::cos(a), std::sin(a)) * sp + shipVel * 0.5f,
                sf::Color(255, static_cast<uint8_t>(110 + rand() % 120), 50, 235),
                life, life, 2.f + rand() % 4 });
        }

        // 4. The hull, in pieces.
        const size_t triCount = adef.visualTris.size() / 3;
        const int maxShards = adef.config["death_shards"].get_or(7);
        if (triCount > 0 && maxShards > 0) {
            const size_t step = std::max<size_t>(1, triCount / static_cast<size_t>(maxShards));
            const sf::Color shard(
                static_cast<uint8_t>(adef.color.r * 0.8f),
                static_cast<uint8_t>(adef.color.g * 0.8f),
                static_cast<uint8_t>(adef.color.b * 0.8f), 255);

            int made = 0;
            for (size_t t = 0; t < triCount && made < maxShards; t += step, ++made) {
                const sf::Vector2f a = adef.visualTris[t * 3];
                const sf::Vector2f b = adef.visualTris[t * 3 + 1];
                const sf::Vector2f c = adef.visualTris[t * 3 + 2];
                const sf::Vector2f cc = (a + b + c) / 3.f;
                const sf::Vector2f pts[3] = { a - cc, b - cc, c - cc };

                const sf::Vector2f wc(cc.x * cs - cc.y * sn, cc.x * sn + cc.y * cs);
                sf::Vector2f out = wc;
                const float ol = std::sqrt(out.x * out.x + out.y * out.y);
                if (ol > 0.5f) out /= ol;
                else {
                    const float ra = (rand() % 360) * 3.14159f / 180.f;
                    out = { std::cos(ra), std::sin(ra) };
                }

                m_em->spawnDebris(pos + wc,
                    out * (140.f + rand() % 220) + shipVel * 0.6f,
                    ((rand() % 2) ? 1.f : -1.f) * (180.f + rand() % 360),
                    pts, 3, shard, 0.9f + (rand() % 50) / 100.f);
            }
        }

        // 5. A little smoke that stays behind while the rest flies on.
        for (int k = 0; k < 6; ++k) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            m_em->particles.push_back({ m_em->nextEntityId++, pos,
                sf::Vector2f(std::cos(a), std::sin(a)) * (20.f + rand() % 40),
                sf::Color(70, 55, 50, 170), 0.9f, 0.9f, 7.f + rand() % 5 });
        }

        m_em->addTrauma(adef.config["death_trauma"].get_or(0.38f));
        m_em->requestHitstop(0.02f, 0.07f, 0.45f);
    }


    /**
 * @brief Was this parry connect within the perfect timing slice?
 *
 * parryTimer counts DOWN from parry_window, so a HIGH remaining value means
 * little time has elapsed since the press -- the player reacted to the
 * incoming attack rather than pressing early and waiting for it.
 */
    bool isPerfectParry(size_t playerIdx) const {
        const float window = (*m_lua)["parry_window"].get_or(0.3f);
        const float frac = (*m_lua)["parry_perfect_fraction"].get_or(0.45f);
        return m_em->players[playerIdx].parryTimer >= window * (1.f - frac);
    }

    /**
     * @brief Cancel parry recovery and grant brief i-frames.
     *
     * Called only from the BULLET and SHIP parry branches -- never asteroids.
     *
     * Two separate effects, solving two separate problems:
     *   - Cancelling the animation lockout and cooldown fixes "I parried and
     *     then stood there spinning while the fight moved on."
     *   - The i-frames fix "I parried one bullet and the next one hit me
     *     anyway." Cancelling recovery alone does NOT fix that: the second
     *     shot was already in flight when the first connected, so there is no
     *     amount of reaction speed that covers it.
     *
     * Chained perfects get progressively fewer i-frames so a parry-lock cannot
     * be held forever against sustained fire.
     */
    void onPerfectParry(size_t playerIdx, sf::Vector2f at) {
        auto& ps = m_em->players[playerIdx];
        auto& hp = m_em->healths[playerIdx];

        ps.parryAnimTimer = 0.f;
        ps.parryWhiffRecovery = false;
        ps.parryWhiffTimer = 0.f;
        ps.parryCooldown = (*m_lua)["parry_perfect_cooldown"].get_or(0.12f);

        const float base = (*m_lua)["parry_perfect_iframes"].get_or(0.22f);
        const float falloff = (*m_lua)["parry_perfect_chain_falloff"].get_or(0.75f);
        const float grant = base * std::pow(falloff,
            static_cast<float>(ps.perfectParryChain));

        hp.invulTimer = std::max(hp.invulTimer, grant);
        ps.perfectParryChain++;
        ps.perfectParryFlash = 0.30f;

        m_em->spawnScreenFlash(sf::Color(190, 255, 235), 0.16f, 70.f);
        m_em->spawnShockRing(at, 10.f, 165.f, 0.30f,
            sf::Color(140, 255, 225), 4.f, 300.f);
    }


    /**
     * @brief Knock an enemy out of control (mirrors EntityManager::staggerPlayer)
     *
     * Now scaled by the archetype's `stagger_resist`. Before this, a melee
     * parry threw a 2600 HP cruiser across the arena exactly as far as it threw
     * a Wardog, which made "heavy ship" a claim the game never backed up.
     *
     * Resistance scales duration, spin AND knockback speed together. Scaling
     * only the knockback would give a cruiser that stays put but still spins
     * like a dropped coin -- worse than either extreme, because the visual and
     * the physics would be telling the player different stories.
     *
     * At resist >= 0.995 the stagger is refused outright and downgraded to a
     * hit flash. That is deliberately reachable: some units should read as
     * genuinely immovable rather than merely stubborn.
     */
    void staggerEnemy(size_t idx, sf::Vector2f knockDir, float knockSpeed, float severity) {
        if (idx >= m_em->enemies.size()) return;
        auto& ec = m_em->enemies[idx];
        if (ec.staggerTimer > 0.f) return;

        // A charging ship cannot be staggered. See the ram contract below.
        if (ec.ramState == RamState::Charge) return;

        float resist = 0.f;
        if (m_registry) {
            resist = std::clamp(
                m_registry->resolve(ec.archetype).config["stagger_resist"].get_or(0.f),
                0.f, 1.f);
        }
        const float k = 1.f - resist;

        if (k < 0.005f) {
            // Immovable: acknowledge the hit, refuse the ragdoll.
            ec.hitFlashTimer = std::max(ec.hitFlashTimer, 0.18f);
            m_em->spawnShockRing(m_em->transforms[idx].position,
                10.f, 90.f, 0.22f, sf::Color(255, 200, 120), 3.f, 140.f);
            return;
        }

        severity = std::clamp(severity, 0.f, 1.f);

        ec.staggerDuration = wcfg("enemy_stagger_tumble", 0.85f) * (0.6f + severity * 0.7f) * k;
        ec.staggerTimer = ec.staggerDuration;
        ec.staggerRecoverDuration = wcfg("enemy_stagger_recover", 0.5f) * k;
        ec.staggerRecoverTimer = 0.f;
        ec.staggerSpinSpeed = ((rand() % 2) ? 1.f : -1.f) *
            wcfg("enemy_stagger_spin", 540.f) * (0.6f + severity * 0.8f) * k;

        ec.telegraphActive = false;
        ec.telegraphTimer = 0.f;
        ec.stormActive = false;
        ec.stormTimer = 0.f;
        ec.bashState = BashState::None;      // A tumbling ship is not mid-swing
        ec.bashStrikePending = false;

        float len = std::sqrt(knockDir.x * knockDir.x + knockDir.y * knockDir.y);
        if (len > 0.001f) {
            knockDir /= len;
            b2Body_SetLinearVelocity(m_em->physics[idx].bodyId,
                { knockDir.x * knockSpeed * k / SCALE,
                  knockDir.y * knockSpeed * k / SCALE });
        }

        const sf::Vector2f p = m_em->transforms[idx].position;
        m_em->spawnShockRing(p, 12.f, 150.f, 0.32f, sf::Color(255, 170, 60), 4.f, 220.f);
        m_em->spawnExplosion(p, sf::Color(255, 160, 80), 16, 3.f);
    }
};