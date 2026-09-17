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
 * CHANGED in 1.5 -- MANIAC
 *
 *  - detonate(): one AoE helper for rockets, thrown Maniacs and suicide
 *    blasts. Friendly fire is ON in all three cases, which is the entire
 *    reason to redirect any of them.
 *
 *  - Rockets never despawn quietly: fuse end or contact both route through
 *    the blast. Parrying one does not destroy it -- it keeps the entity and
 *    goes wild, spinning off in the parried direction on the fuse it has
 *    left, now dangerous to the squad that fired it.
 *
 *  - Suicide contact: parried -> FrenzyState::Thrown (a spinning bomb with
 *    the player credited for wherever it lands); not parried -> full blast on
 *    the player. Killed mid-charge -> a smaller blast, so shooting him down
 *    early stays the safe answer and therefore a real choice.
 *
 * @author Oleg Ivakhiv
 * @version 1.5 (maniac)
 */

#pragma once

#include "utils/ClassTuning.hpp"
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

        // Anti-stunlock rules for EntityManager::staggerPlayer, hot-reloadable.
        m_em->staggerGrace = vcfg("stagger_grace", 0.8f);
        m_em->staggerTumbleIframes = vcfg("stagger_tumble_iframes", 1.0f);
        m_em->staggerImmuneShove = vcfg("stagger_immune_shove", 0.35f);
        m_em->poisePerKnockback = vcfg("poise_per_knockback", 0.1f);
        m_em->poiseAbsorbShove = vcfg("poise_absorb_shove", 0.30f);

        // Class feel for this frame: armour, shoulder bash, ramming.
        m_feel = ship::classFeelFor(*m_lua, m_em->players[playerIdx].kit);
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
        // 1c. THROWN MANIACS — fuse and impact
        // ====================================================================
        updateThrown(dt);
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
            // 2a-pre0. A THROWN MANIAC hitting ANYTHING
            // ================================================================
            // Not just the player: the entire point of throwing him is to put
            // him into something else. Checked before ordnance so a thrown
            // Maniac meeting a mine resolves as his blast, not the mine's.
            {
                bool done = false;
                for (int pass = 0; pass < 2 && !done; ++pass) {
                    BodyUserData* ud = pass ? udB : udA;
                    if (!ud || (pass ? typeB : typeA) != BodyType::Enemy) continue;
                    const size_t idx = m_em->getEntityIndex(ud->entityId);
                    if (idx == (size_t)-1) continue;
                    if (m_em->enemies[idx].frenzyState != FrenzyState::Thrown) continue;
                    blowUpManiac(idx, true);
                    done = true;
                }
                if (done) continue;
            }

            // ================================================================
            // 2a-pre. ORDNANCE vs ANYTHING
            // ================================================================
            // Rockets and mines resolve here rather than in the player-centric
            // block below, because most of their interesting contacts do not
            // involve the player at all: a parried rocket meeting a Raider, a
            // Rakshari drifting into his own squadmate's minefield. Handling
            // them only where the player was one of the two bodies is exactly
            // why parried rockets appeared to pass through enemies.
            {
                size_t ordIdx = (size_t)-1, otherIdx2 = (size_t)-1;
                BodyType otherType2 = BodyType::Asteroid;

                for (int pass = 0; pass < 2; ++pass) {
                    BodyUserData* ud = pass ? udB : udA;
                    BodyType t = pass ? typeB : typeA;
                    if (t != BodyType::Bullet || !ud) continue;
                    const size_t idx = m_em->getEntityIndex(ud->entityId);
                    if (idx == (size_t)-1) continue;
                    if (!m_em->bullets[idx].isRocket && !m_em->bullets[idx].isMine) continue;

                    BodyUserData* oud = pass ? udA : udB;
                    ordIdx = idx;
                    otherIdx2 = oud ? m_em->getEntityIndex(oud->entityId) : (size_t)-1;
                    otherType2 = pass ? typeA : typeB;
                    break;
                }

                if (ordIdx != (size_t)-1) {
                    auto& ord = m_em->bullets[ordIdx];

                    const bool ownerHit = (otherIdx2 != (size_t)-1) &&
                        (m_em->transforms[otherIdx2].entityId == ord.ownerEntityId);
                    if (ord.armTimer > 0.f || ownerHit) continue;

                    // ---- MINES ----
                    if (ord.isMine) {
                        // Hull contact is NOT a trigger, with one exception.
                        // A Rakshari brushing past a mine leaves it sitting
                        // there -- otherwise a pack clears its own field in
                        // seconds. An ASTEROID counts as a damaging event:
                        // rock hitting armed explosive should set it off, and
                        // it gives the player a second way to clear a lane.
                        if (otherType2 == BodyType::Asteroid) {
                            m_em->lightMineFuse(ord, m_em->transforms[ordIdx].position);
                        }
                        else if (otherType2 == BodyType::Bullet && otherIdx2 != (size_t)-1) {
                            // A round -- anyone's -- lights it and dies on it,
                            // the same way a bullet dies on a rock. No health
                            // involved: shooting a mine is a decision, not a
                            // damage race.
                            m_em->lightMineFuse(ord, m_em->transforms[ordIdx].position);
                            m_em->bullets[otherIdx2].markedForDestroy = true;
                            m_em->spawnImpact(m_em->transforms[ordIdx].position,
                                sf::Color(255, 160, 70),
                                m_em->transforms[otherIdx2].velocity);
                        }
                        else if (otherIdx2 == playerIdx) {
                            // The player physically touching one is inside the
                            // zone by definition, so this is just the zone
                            // trigger arriving early.
                            m_em->lightMineFuse(ord, m_em->transforms[ordIdx].position);
                        }
                        continue;
                    }

                    // ---- ROCKETS ----
                    if (otherType2 == BodyType::Bullet) continue;   // handled above

                    const bool isPlayerHit = (otherIdx2 == playerIdx);
                    if (isPlayerHit && m_em->players[playerIdx].parryTimer > 0.f) {
                        parryRocket(playerIdx, ordIdx);
                        continue;
                    }

                    ord.markedForDestroy = true;
                    continue;
                }
            }

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

                    if (targetType == BodyType::Bullet) {
                        // Ordnance is settled in the block above; nothing to
                        // do here. Kept explicit so a future projectile type
                        // does not fall through into the asteroid branch.
                    }
                    else if (targetType == BodyType::Asteroid) {
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

                // ---- ROCKETS ----
                // Any contact ends the flight; the blast is applied on
                // destruction so there is exactly one code path into it.
                if (otherType == BodyType::Bullet && otherIdx != (size_t)-1 &&
                    m_em->bullets[otherIdx].isRocket) {
                    if (m_em->bullets[otherIdx].armTimer <= 0.f) {
                        // A parry beats the blast, so check it before arming
                        // the detonation -- otherwise the correct answer to a
                        // rocket would be to never touch it.
                        if (m_em->players[playerIdx].parryTimer > 0.f) {
                            parryRocket(playerIdx, otherIdx);
                            continue;
                        }
                        m_em->bullets[otherIdx].markedForDestroy = true;
                    }
                    continue;
                }

                // ---- ENEMY MID-COMMIT: the attack owns the outcome ----
                if (otherType == BodyType::Enemy && otherIdx != (size_t)-1) {
                    // Suicide charge: parry flips him into a thrown bomb,
                    // anything else eats the full blast.
                    if (m_em->enemies[otherIdx].frenzyState == FrenzyState::Charge ||
                        m_em->enemies[otherIdx].frenzyState == FrenzyState::Thrown) {
                        resolveFrenzyContact(playerIdx, otherIdx);
                        continue;
                    }
                    // Gunship dodge into a ship: the dodge IS the attack. Runs
                    // before the ram contract so it can break a charge.
                    if (tryShoulderBash(playerIdx, otherIdx)) continue;
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

                const bool isRock = (otherType == BodyType::Asteroid && otherIdx != (size_t)-1);
                const int tier = isRock ? std::min<int>(3, m_em->healths[otherIdx].asteroidTier) : -1;
                const bool ramming = m_feel.ramming > 0.5f && relativeSpeed >= m_feel.ramMinSpeed
                    && m_em->players[playerIdx].kit.valid;

                // ---- GUNSHIP RAMMING ----
                if (ramming && otherIdx != (size_t)-1) {
                    const float over = relativeSpeed - m_feel.ramMinSpeed;
                    const sf::Vector2f op = m_em->transforms[otherIdx].position;
                    if (tier == 0) {
                        // Small rock: gone, no damage, no poise, no stop.
                        m_em->healths[otherIdx].currentHp = -1.f;
                        m_em->spawnImpact(op, sf::Color(220, 210, 190),
                            (op - m_em->transforms[playerIdx].position) * 6.f);
                        m_em->addTrauma(0.08f);
                        continue;
                    }
                    const float dealt = m_feel.ramBaseDamage + over * m_feel.ramDamagePerSpeed;
                    if (tier == 1) {
                        m_em->healths[otherIdx].currentHp -= dealt * 2.f;   // medium rocks crack
                    }
                    else if (otherType == BodyType::Enemy && !isRamInvulnerable(otherIdx)
                        && !isBashCommitted(otherIdx)) {
                        m_em->healths[otherIdx].currentHp -= dealt;
                        m_em->enemies[otherIdx].hitFlashTimer = 0.2f;
                        m_em->enemies[otherIdx].timesHit += 1;
                        sf::Vector2f dir = op - m_em->transforms[playerIdx].position;
                        staggerEnemy(otherIdx, dir, 380.f + over * 25.f,
                            std::clamp(over / 20.f, 0.15f, 0.5f));
                    }
                }

                if (relativeSpeed > 12.0f && m_em->healths[playerIdx].invulTimer <= 0) {
                    // Hull damage by what you hit. Rocks by size; anything else as before.
                    float hullDamage = isRock ? rockCfg(tier, "damage", 15.f) : 15.f;
                    if (ramming && tier == 1) hullDamage *= m_feel.ramMediumTaken;
                    m_em->damagePlayer(playerIdx, hullDamage);
                    m_em->healths[playerIdx].invulTimer = 1.0f;

                    // ---- IMPACT -> POISE (or, with no poise, the old stagger rule) ----
                    // Poise cost scales with SIZE and SPEED. A small rock clipped
                    // during a dodge costs a sliver; a big one at full burn breaks
                    // a medium ship. Legacy ships keep the speed threshold.
                    if (otherIdx != (size_t)-1) {
                        const auto& ps = m_em->players[playerIdx];
                        const float speedK = 0.5f + 0.5f * std::clamp((relativeSpeed - 12.f) / 18.f, 0.f, 1.f);
                        float poiseDmg = (isRock ? rockCfg(tier, "poise", 90.f) : 90.f) * speedK;
                        float knock = (isRock ? rockCfg(tier, "knockback", 900.f) : vcfg("stagger_knockback", 900.f)) * speedK;
                        if (ramming && tier == 1) poiseDmg *= 0.5f;

                        const bool legacyStagger = relativeSpeed > vcfg("stagger_speed_threshold", 20.0f);
                        if (ps.poiseMax > 0.f || legacyStagger) {
                            sf::Vector2f away = m_em->transforms[playerIdx].position
                                - m_em->transforms[otherIdx].position;
                            m_em->staggerPlayer(playerIdx, away, knock,
                                vcfg("stagger_tumble_duration", 1.1f),
                                vcfg("stagger_recover_duration", 0.55f),
                                vcfg("stagger_spin_speed", 620.f),
                                poiseDmg);
                        }
                    }
                }
                else if (relativeSpeed > 1.5f && m_em->healths[playerIdx].invulTimer <= 0 &&
                    m_em->healths[playerIdx].cheapInvulTimer <= 0) {
                    m_em->damagePlayer(playerIdx, 1.0f);
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
                            m_em->damagePlayer(playerIdx, blt.damage);
                            // Chip: wears poise down, can never break it alone.
                            m_em->chipPoise(playerIdx, blt.damage * vcfg("bullet_poise_per_damage", 0.6f));
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
                                if (j == playerIdx) m_em->damagePlayer(j, damage * falloff);
                                else                m_em->healths[j].currentHp -= damage * falloff;

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
                // A rocket always goes off -- on contact, or when its fuse
                // runs out mid-air. Never a silent despawn: a live fuse the
                // player has walked away from is a hazard they earned.
                const auto& b = m_em->bullets[i];
                if (b.isRocket || (b.isMine && b.armTimer <= 0.f)) {
                    detonate(m_em->transforms[i].position, b.blastRadius, b.blastDamage,
                        i, b.isWild ? playerIdx : (size_t)-1,
                        b.isWild ? sf::Color(255, 240, 180) : sf::Color(255, 130, 40));
                }
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

    ship::ClassFeel m_feel;   ///< This frame's class feel for the player

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

    /**
     * @brief Gunship dodge connects with a ship: a cheap bash.
     *
     * First enemy per dodge only. Damage, a real stagger and a short stun --
     * the opening for a follow-up. Against a CHARGING ship it is the counter:
     * the charge (and the rest of its chain) is broken, the hit is worth
     * shoulder_counter times more, and the gunship still eats the ram --
     * through hyperarmor, so no tumble and a fraction of the damage.
     *
     * @return true if it bashed, so the caller skips the normal contact rules.
     */
    bool tryShoulderBash(size_t playerIdx, size_t enIdx) {
        auto& ps = m_em->players[playerIdx];
        if (m_feel.shoulderBash < 0.5f || !ps.kit.valid || ps.shoulderUsed) return false;
        const bool dashing = ps.dashTimer > 0.f
            || (ps.dashDriftDuration > 0.f && ps.dashDriftTimer > ps.dashDriftDuration - 0.12f);
        if (!dashing || enIdx >= m_em->enemies.size()) return false;

        ps.shoulderUsed = true;
        auto& ec = m_em->enemies[enIdx];
        const bool breaksCharge = (ec.ramState == RamState::Charge);

        if (breaksCharge) {
            // Take the ram through hyperarmor: damage, but never a tumble.
            const bool had = ps.hyperarmor;
            ps.hyperarmor = true;
            m_em->damagePlayer(playerIdx, acfg(enIdx, "ram_damage", 95.f));
            ps.hyperarmor = had;

            ec.ramChainLeft = 0;
            ec.ramState = RamState::Recover;
            ec.ramDuration = acfg(enIdx, "ram_recover", 1.9f);
            ec.ramTimer = ec.ramDuration;
            ec.ramCooldown = std::max(ec.ramCooldown, acfg(enIdx, "ram_cooldown", 15.f));
        }

        m_em->healths[enIdx].currentHp -= m_feel.shoulderDamage * (breaksCharge ? m_feel.shoulderCounter : 1.f);
        ec.hitFlashTimer = 0.25f;
        ec.timesHit += 2;
        staggerEnemy(enIdx, ps.dashDir, m_feel.shoulderKnock, breaksCharge ? 1.f : 0.75f);
        m_em->healths[enIdx].stunTimer = std::max(m_em->healths[enIdx].stunTimer, m_feel.shoulderStun);

        // The gunship stops on the hit instead of sailing through: a bash, not a pass.
        const b2BodyId pb = m_em->physics[playerIdx].bodyId;
        ps.dashTimer = 0.f;
        ps.dashDriftTimer = 0.f;
        b2Body_SetBullet(pb, false);
        b2Body_SetLinearVelocity(pb, { -ps.dashDir.x * 140.f / SCALE, -ps.dashDir.y * 140.f / SCALE });

        const sf::Vector2f ep = m_em->transforms[enIdx].position;
        const sf::Vector2f contact = m_em->transforms[playerIdx].position + (ep - m_em->transforms[playerIdx].position) * 0.55f;
        m_em->addTrauma(breaksCharge ? 0.60f : 0.42f);
        m_em->requestHitstop(breaksCharge ? 0.07f : 0.04f, 0.18f, 0.35f);
        m_em->spawnShockRing(contact, 8.f, breaksCharge ? 150.f : 95.f, 0.24f,
            sf::Color(255, 205, 90), 6.f, 255.f);
        m_em->spawnImpact(contact, sf::Color(255, 225, 160), ps.dashDir * 520.f);
        if (breaksCharge) m_em->spawnScreenFlash(sf::Color(255, 210, 140), 0.14f, 60.f);
        return true;
    }

    /// Rock contact tuning by tier, from visuals.rock_<small|medium|large|magma>_<key>.
    float rockCfg(int tier, const char* key, float fallback) const {
        static const float kDamage[4] = { 8.f, 15.f, 22.f, 18.f };
        static const float kPoise[4] = { 12.f, 45.f, 90.f, 70.f };
        static const float kKnock[4] = { 260.f, 600.f, 900.f, 750.f };
        static const char* kName[4] = { "small", "medium", "large", "magma" };
        if (tier < 0 || tier > 3) return fallback;
        const std::string k = std::string(key);
        const float def = (k == "damage") ? kDamage[tier] : (k == "poise") ? kPoise[tier]
            : (k == "knockback") ? kKnock[tier] : fallback;
        return vcfg((std::string("rock_") + kName[tier] + "_" + k).c_str(), def);
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

            m_em->damagePlayer(playerIdx, acfg(i, "bash_damage", 30.f));
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

        m_em->damagePlayer(playerIdx, acfg(enIdx, "ram_damage", 95.f));
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
    // AREA BLASTS
    // ========================================================================

    /**
     * @brief One radial blast: damage with falloff, stagger, FX.
     *
     * @param skipIdx    the source entity (never damages itself)
     * @param creditIdx  index whose stagger is suppressed -- the player when
     *                   the blast is theirs. A redirected payload that still
     *                   knocked the player around would punish the parry.
     *
     * Friendly fire is unconditional. Every hazard the Maniac produces is
     * worth redirecting precisely BECAUSE it does not care whose hull it
     * touches; an "enemies are immune" rule would quietly delete the reason
     * to parry any of it.
     */
    void detonate(sf::Vector2f pos, float radius, float damage,
        size_t skipIdx, size_t creditIdx, sf::Color tint)
    {
        if (radius <= 1.f || damage <= 0.f) return;

        const size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);

        m_em->addDebugAoE(pos, radius, tint, 0.35f);
        m_em->spawnShockRing(pos, 12.f, radius, 0.30f, tint, 6.f, 240.f);
        m_em->addTrauma(std::clamp(radius / 420.f, 0.12f, 0.45f));

        // ---- Debris field ----
        // Particle count scales with the blast instead of being fixed, so
        // shrinking a radius for balance does not also make the explosion look
        // cheap -- a small blast should be DENSE, not sparse.
        const int burst = 34 + static_cast<int>(radius * 0.30f);
        m_em->spawnExplosion(pos, tint, burst, 4.2f);
        m_em->spawnExplosion(pos, sf::Color(255, 245, 215), burst / 3, 2.6f);

        // NOTE: no big "core" particle here. Particles are axis-aligned
        // squares, so anything above ~40px reads as a literal cube sitting in
        // the middle of the explosion. The centre is carried by the shock ring
        // and the dense small stuff instead.

        // Radial streaks out to the actual damage edge: the ring says WHERE
        // the blast ends, and these make that edge legible mid-fight.
        for (int a = 0; a < 360; a += 14) {
            const float r = a * 3.14159f / 180.f;
            const sf::Vector2f d(std::cos(r), std::sin(r));
            const float sp = radius * (2.6f + (rand() % 90) / 100.f);
            const float life = 0.22f + (rand() % 26) / 100.f;
            m_em->particles.push_back({ m_em->nextEntityId++,
                pos + d * (radius * 0.18f), d * sp,
                sf::Color(255, static_cast<uint8_t>(150 + rand() % 90), 60, 240),
                life, life, 2.f + rand() % 3 });
        }

        // Slow smoke that outlives the flash and marks the spot.
        for (int k = 0; k < 8; ++k) {
            const float r = (rand() % 360) * 3.14159f / 180.f;
            m_em->particles.push_back({ m_em->nextEntityId++, pos,
                sf::Vector2f(std::cos(r), std::sin(r)) * (25.f + rand() % 55),
                sf::Color(80, 62, 55, 165), 0.85f, 0.85f, 6.f + rand() % 6 });
        }

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j == skipIdx) continue;

            // Bullets in the blast are ignored on purpose: chaining every
            // round in the air turns two rockets into an arena-wide cascade.
            BodyUserData* ud = b2Body_IsValid(m_em->physics[j].bodyId)
                ? (BodyUserData*)b2Body_GetUserData(m_em->physics[j].bodyId) : nullptr;
            if (ud && ud->type == BodyType::Bullet) continue;

            const sf::Vector2f o = m_em->transforms[j].position;
            const float dx = pos.x - o.x, dy = pos.y - o.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= radius) continue;

            const float falloff = 1.f - (d / radius);
            if (j == playerIdx) m_em->damagePlayer(j, damage * falloff);
            else                m_em->healths[j].currentHp -= damage * falloff;

            if (ud && ud->type == BodyType::Enemy && j < m_em->enemies.size()) {
                m_em->enemies[j].hitFlashTimer = 0.2f;
                m_em->enemies[j].timesHit += 2;   // an AoE is unambiguous
            }

            if (j == playerIdx && j != creditIdx &&
                falloff > vcfg("stagger_blast_falloff", 0.45f)) {
                m_em->staggerPlayer(playerIdx, o - pos,
                    vcfg("stagger_knockback", 900.f) * falloff,
                    vcfg("stagger_tumble_duration", 1.1f),
                    vcfg("stagger_recover_duration", 0.55f),
                    vcfg("stagger_spin_speed", 620.f));
            }
        }
    }

    // ========================================================================
    // ROCKETS
    // ========================================================================

    /**
     * @brief Parry a skid rocket: it survives, and changes sides.
     *
     * Destroying it would make the parry a defensive button identical to
     * shooting the rocket down. Keeping the entity alive -- same fuse, no
     * guidance, spinning off the way it was hit -- is what turns a defensive
     * read into an offensive one, and what lets the player aim it at the
     * squad that fired it.
     */
    void parryRocket(size_t playerIdx, size_t rocketIdx) {
        auto& b = m_em->bullets[rocketIdx];
        const sf::Vector2f rPos = m_em->transforms[rocketIdx].position;
        const sf::Vector2f pPos = m_em->transforms[playerIdx].position;

        sf::Vector2f dir = rPos - pPos;
        float l = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (l > 0.01f) dir /= l;
        else dir = { 0.f, -1.f };

        b.isWild = true;
        b.homingTargetEntityId = 0;     // no guidance, ever again
        b.homingTurnRate = 0.f;
        b.trackTimer = 0.f;
        b.skidTurnRate = 0.f;
        // Tumble hard around its own centre -- the same language as a
        // parry-launched asteroid or a staggered ship, so "this is loose now"
        // reads instantly. The PATH stays straight: it goes where you sent it.
        b.spin = ((rand() % 2) ? 1.f : -1.f) * (620.f + rand() % 560);
        b.wanderAmp = 0.f;
        b.wildDrag = wcfg("parry_rocket_drag", 0.75f);
        b.wildStallSpeed = wcfg("parry_rocket_stall", 170.f);
        b.ownerEntityId = m_playerEntityId;
        b.isEnemyBullet = false;        // it belongs to the player now
        b.armTimer = wcfg("parry_rocket_arm", 0.10f);
        b.blastDamage *= wcfg("parry_rocket_damage_mult", 1.6f);
        b.blastRadius *= wcfg("parry_rocket_radius_mult", 1.15f);

        // Fuse untouched but floored, so a rocket parried in its last moments
        // still has time to travel somewhere worth travelling to.
        b.lifetime = std::max(b.lifetime, wcfg("parry_rocket_min_fuse", 1.6f));

        // Fast enough to run down the Maniac who fired it. A parried rocket
        // that the sender simply out-flies is a parry with no target, and the
        // sender is the target the player actually wants.
        const float speed = wcfg("parry_rocket_speed", 1150.f);
        if (b2Body_IsValid(m_em->physics[rocketIdx].bodyId)) {
            b2Body_SetLinearVelocity(m_em->physics[rocketIdx].bodyId,
                { dir.x * speed / SCALE, dir.y * speed / SCALE });
        }

        // YELLOW: the game's existing "parried, now yours" colour, the same
        // one a reflected bullet wears. Live rockets are orange/red, so the
        // switch is unmistakable even in a crowded frame.
        // Full-saturation yellow with a white-hot rim, and a thicker outline:
        // at a 10px shape a subtle recolour is invisible in a busy frame. Same
        // colour a reflected bullet wears.
        auto& sh = m_em->renders[rocketIdx].shape;
        sh.setFillColor(sf::Color(255, 238, 0));
        sh.setOutlineColor(sf::Color(255, 255, 210, 255));
        sh.setOutlineThickness(4.0f);

        m_em->spawnExplosion(rPos, sf::Color(0, 255, 200), 14, 2.2f);
        m_em->spawnShockRing(rPos, 5.f, 52.f, 0.20f, sf::Color(255, 240, 90), 3.f, 240.f);
        onParrySuccess(playerIdx, rPos);
        if (isPerfectParry(playerIdx)) onPerfectParry(playerIdx, rPos);
    }

    // ========================================================================
    // FRENZY
    // ========================================================================

    /// Tick every thrown Maniac's fuse. Impact is handled by the contact loop;
    /// this is the timeout, and the safety net for one thrown into open space.
    void updateThrown(float dt) {
        for (size_t i = 0; i < m_em->enemies.size(); ++i) {
            auto& ec = m_em->enemies[i];
            if (ec.frenzyState != FrenzyState::Thrown) continue;
            if (ec.detonated) continue;

            ec.frenzyTimer -= dt;

            // Trailing sparks: a thrown Maniac must be legible as a hazard in
            // flight, not just as a ragdoll.
            if ((rand() % 100) < 70) {
                const float a = (rand() % 360) * 3.14159f / 180.f;
                m_em->particles.push_back({ m_em->nextEntityId++,
                    m_em->transforms[i].position +
                        sf::Vector2f(std::cos(a), std::sin(a)) * 18.f,
                    sf::Vector2f(std::cos(a), std::sin(a)) * (70.f + rand() % 140),
                    sf::Color(255, static_cast<uint8_t>(120 + rand() % 110), 50, 230),
                    0.26f, 0.26f, 3.f });
            }

            // Shake is drawn by AISystem; this is the countdown itself.
            if (ec.frenzyTimer <= 0.f) blowUpManiac(i, true);
        }
    }

    /**
     * @brief The player's hull met a charging (or thrown) Maniac.
     *
     * Parried mid-charge, he becomes ordnance: same body, no AI, thrown along
     * the parry with a hard spin and a short fuse. Unparried, the charge does
     * what it promised.
     */
    void resolveFrenzyContact(size_t playerIdx, size_t enIdx) {
        auto& ec = m_em->enemies[enIdx];
        if (ec.detonated) return;

        // Already thrown: touching anything is impact, including the player.
        if (ec.frenzyState == FrenzyState::Thrown) { blowUpManiac(enIdx, true); return; }

        const sf::Vector2f ePos = m_em->transforms[enIdx].position;
        const sf::Vector2f pPos = m_em->transforms[playerIdx].position;

        // ---- PARRIED ----
        if (m_em->players[playerIdx].parryTimer > 0.f) {
            sf::Vector2f away = ePos - pPos;
            float l = std::sqrt(away.x * away.x + away.y * away.y);
            if (l > 0.01f) away /= l; else away = { 0.f, -1.f };

            ec.frenzyState = FrenzyState::Thrown;
            ec.frenzyTimer = acfg(enIdx, "thrown_fuse", 1.5f);
            ec.frenzyDir = away;
            ec.frenzyBlinkHz = 16.f;   // frantic: the fuse is short now
            ec.bashState = BashState::None;
            ec.ramState = RamState::None;

            // Stunned for longer than the fuse. He must not steer, and an
            // AI that woke up mid-flight would undo the whole trick.
            m_em->healths[enIdx].stunTimer = ec.frenzyTimer + 1.f;

            if (b2Body_IsValid(m_em->physics[enIdx].bodyId)) {
                // Thrown hard enough to clear his own blast. The reward for a
                // parry cannot be "he explodes on top of you anyway": speed x
                // fuse has to exceed the radius, so this is sized from the
                // blast rather than picked by feel.
                const float need = acfg(enIdx, "suicide_blast_radius", 270.f)
                    * acfg(enIdx, "thrown_clearance", 1.35f)
                    / std::max(0.2f, ec.frenzyTimer);
                const float spd = std::max(acfg(enIdx, "thrown_speed", 1150.f), need);
                b2Body_SetLinearVelocity(m_em->physics[enIdx].bodyId,
                    { away.x * spd / SCALE, away.y * spd / SCALE });
                b2Body_SetAngularVelocity(m_em->physics[enIdx].bodyId,
                    ((rand() % 2) ? 1.f : -1.f) * acfg(enIdx, "thrown_spin", 16.f));
            }

            m_em->spawnExplosion(ePos, sf::Color(0, 255, 200), 26, 3.4f);
            m_em->spawnShockRing(ePos, 14.f, 150.f, 0.28f, sf::Color(120, 255, 230), 4.f, 240.f);
            onParrySuccess(playerIdx, (pPos + ePos) * 0.5f);
            if (isPerfectParry(playerIdx)) onPerfectParry(playerIdx, ePos);
            return;
        }

        // ---- NOT PARRIED ----
        blowUpManiac(enIdx, true);
    }

    /**
     * @brief Blow him up where he stands and mark him dead.
     *
     * @param full  true for a completed charge or a thrown impact; false for a
     *              charge that was shot down. The smaller blast is what keeps
     *              "kill him early" a genuinely safer answer than "parry him",
     *              rather than a strictly worse one.
     */
    void blowUpManiac(size_t i, bool full) {
        auto& ec = m_em->enemies[i];
        if (ec.detonated) return;
        ec.detonated = true;

        const sf::Vector2f pos = m_em->transforms[i].position;

        // Three sizes, and the ordering is the design:
        //   thrown  > full > shot down
        // Parrying him is the highest-risk answer, so it has to produce the
        // biggest bang -- otherwise the safe play (shoot him early) would also
        // be the strongest one, and the parry would be a stunt with no payoff.
        float scale = full ? 1.f : 0.55f;
        float dmgScale = full ? 1.f : 0.45f;
        if (ec.frenzyState == FrenzyState::Thrown) {
            scale = acfg(i, "thrown_blast_mult", 1.35f);
            dmgScale = acfg(i, "thrown_damage_mult", 1.4f);
        }

        const float radius = acfg(i, "suicide_blast_radius", 260.f) * scale;
        const float damage = acfg(i, "suicide_blast_damage", 75.f) * dmgScale;

        // A Maniac thrown by the player should not then stagger the player.
        const size_t credit = (ec.frenzyState == FrenzyState::Thrown)
            ? m_em->getEntityIndex(m_playerEntityId) : (size_t)-1;

        detonate(pos, radius, damage, i, credit, sf::Color(255, 170, 60));
        m_em->spawnExplosion(pos, sf::Color(255, 240, 190), 30, 5.0f);

        // ---- FLASH ----
        // Screen-level only. The previous version also dropped a huge white
        // particle at the epicentre, which -- particles being squares -- drew
        // a white cube in the middle of the blast.
        const float white = full ? 1.f : 0.55f;
        m_em->spawnScreenFlash(sf::Color(255, 245, 235), 0.26f * white, 165.f * white);
        m_em->spawnShockRing(pos, 8.f, radius * 0.55f, 0.14f,
            sf::Color(255, 250, 240), 10.f, 255.f);
        m_em->requestHitstop(0.04f * white, 0.11f, 0.38f);

        m_em->healths[i].currentHp = 0.f;
    }

    // ========================================================================
    // ENEMY DEATH
    // ========================================================================

    void spawnEnemyDeath(size_t i, sf::Vector2f deathPos) {
        // Already blew himself up -- the blast WAS the death. Stacking a
        // standard explosion on top of it would read as two separate events.
        if (m_em->enemies[i].detonated) return;

        // Shot down mid-charge: it still goes off, just smaller. A Maniac who
        // simply vanished when killed during the charge would make the whole
        // ignition beat retroactively meaningless.
        if (m_em->enemies[i].frenzyState == FrenzyState::Ignite ||
            m_em->enemies[i].frenzyState == FrenzyState::Charge) {
            blowUpManiac(i, false);
            return;
        }

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
        // The window this parry actually opened with -- class-scaled by
        // InputSystem. Reading raw Lua here made a light ship's early presses
        // count as "perfect" and a heavy's never could.
        const auto& ps = m_em->players[playerIdx];
        const float window = (ps.parryWindowTotal > 0.f)
            ? ps.parryWindowTotal : (*m_lua)["parry_window"].get_or(0.3f);
        const float frac = (*m_lua)["parry_perfect_fraction"].get_or(0.45f);
        return ps.parryTimer >= window * (1.f - frac);
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