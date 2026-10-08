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
 * CHANGED in 1.6 -- BLOODSEEKER
 *
 *  - DISENGAGE DODGE. While a Bloodseeker disengages (DuelShift::Disengage)
 *    the player's plasma and Rift bolts PASS THROUGH him: the round is put
 *    back on its own line beyond the hull at its own speed, he is kicked off
 *    that line, and a hull-shaped ghost stays where he was. Reflected
 *    (parried) rounds still land -- the spec is "he cannot dodge the parry"
 *    -- and so do ramming, the shoulder bash, kinetic rocks and every blast,
 *    none of which come through this branch. A stunned or tumbling
 *    Bloodseeker dodges nothing.
 *
 *  - DIVE CONTACT. The Blood Dive is a rush, not a ram: touching him mid-dive
 *    deals no damage either way. Parrying it is a parry. The bash it hands
 *    off to is what hurts, and that keeps its full cyan windup.
 *
 *  - parryEnemy() and a reflected round's stun flag duelStunEvade: when that
 *    stun ends AISystem opens his post-stun evade window, during which
 *    isDuelEvading() is true exactly as in the disengage. Bullet stuns honour
 *    `stun_resist_ranged` (default 0, so only units that set it change).
 *    parryEnemy(), like staggerEnemy(), breaks a dive, a ram-cancel rush or a feint
 *    in progress (breakDive). staggerEnemy() also breaks a lancer or a cone
 *    (breakRangeAttack) and puts it on cooldown rather than letting it
 *    resume when he recovers.
 *
 *  - PACK. triggerDeathChaos() stamps feralTimer on the pack when a
 *    Bloodseeker dies (not on an executed ally). Enemy rounds that carry
 *    feralMult (fired while feral) now damage enemy hulls; every other enemy
 *    round still stops dead on one. An `executed` ally drops no scrap.
 *
 *  - A ram the Bloodseeker CANCELS stops being a ram on the frame: ramState
 *    goes to None, so isRamInvulnerable() is false for the rush that
 *    follows, and the rush is a dive (no contact damage, parriable).
 *
 *  1.8 -- ROSTER PASS
 *  - FERAL MELEE. A bash strike with bashTargetId set lands on that ship
 *    (bash_damage x death_chaos_damage, stagger), not the player. A feral
 *    ram in Charge hurts ANY hull it touches (feralRamHit: ram_damage x
 *    death_chaos_damage, thrown sideways out of its lane).
 *  - Enemy rounds chip poise x BulletComponent::poiseMult (Wardogs 0.1).
 *  - A mine that runs out of lifetime unlit FIZZLES: no blast.
 *
 *  1.9 -- PLAYTEST
 *  - BEAMS. resolveBeams() / traceBeam(): the lancer is hitscan. Pierces
 *    everything on the line (rocks gone, ships <= killHp destroyed, bigger
 *    ones pierceDamage + stagger); a parry stops it on the shield and
 *    REFLECTS it off the shield's curve as the player's beam.
 *  - PARRIED ROCKETS SEEK. parryRocket() picks a random victim (ships 3 :
 *    rocks 1, never the player), turns the rocket's collisions off and
 *    hands it to WeaponSystem, which always delivers it.
 *
 *  1.10 -- DODGE PASS
 *  - PARRY RE-ARM. The hidden "perfect timing" test is gone: every bullet /
 *    ship parry re-arms (onParryRearm, was onPerfectParry). Asteroids keep
 *    the full recovery. Lua: parry_rearm_* (old parry_perfect_* still read).
 *  - PERFECT DODGE. onDodgeAte(): a refit dodge whose i-frames ate an enemy
 *    attack (round, bash, ram, beam, blast) refunds its energy and re-arms,
 *    once per dodge, falling off over a chain. checkDodgePhase(): with enemy
 *    collisions off (InputSystem), a ram / dive / lunge passing through the
 *    ship counts. Dodge i-frames cover blasts.
 *    1.10c (playtest): a dodged bash or ram changes NOTHING for the
 *    attacker -- the chain continues, no hit-confirm lockout (those are for
 *    hits that land). Dodging well is the player's reward only.
 *    1.10d: PERFECT SHOULDER BASH (heavy). A shoulder bash inside a perfect
 *    dodge is one event, not two: no ram damage taken when it breaks a
 *    charge; HUD reads "PERFECT SHOULDER BASH".
 *    1.10b (playtest): a NEAR MISS counts too (dodge_graze_*), and the tell
 *    is loud -- a thick ghost where it met you, a ghost trail for the rest
 *    of the escape (updateDodgeTrail), a slow-mo dip, a double ring, and a
 *    "PERFECT DODGE" readout on the HUD (HudSystem).
 *
 * @author Oleg Ivakhiv
 * @version 1.10 (dodge pass)
 */

#pragma once

#include "utils/ClassTuning.hpp"
#include "ISystem.hpp"
#include "utils/LuaConfig.hpp"
#include "core/EntityManager.hpp"
#include "core/EntityFactory.hpp"
#include "core/EnemyArchetypes.hpp"        // added for archetype registry
#include "utils/ShipShatter.hpp"            // ship wreckage on death
#include "utils/Afterimage.hpp"             // Bloodseeker dodge ghosts
#include "utils/SeekTarget.hpp"             // parried rockets pick a victim
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
        // Damage flash decays for EVERYTHING that has health -- rocks, scrap,
        // wrecks, enemies, the player. Enemies keep their own
        // EnemyComponent timer as well, because AI reads it as "was I just
        // shot"; this one is purely what the renderer whitens by.
        for (auto& h : m_em->healths)
            if (h.hitFlash > 0.f) h.hitFlash = std::max(0.f, h.hitFlash - dt);

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
        if (m_em->players[playerIdx].dodgeVerdictFlash > 0.f)
            m_em->players[playerIdx].dodgeVerdictFlash -= dt;
        if (m_em->players[playerIdx].parryChainTimer > 0.f) {
            m_em->players[playerIdx].parryChainTimer -= dt;
            if (m_em->players[playerIdx].parryChainTimer <= 0.f)
                m_em->players[playerIdx].parryChain = 0;
        }
        // ====================================================================
        // 1b. BASH STRIKES raised by AISystem last frame
        // ====================================================================
        // Before contacts, so a lunge that also produced a begin-touch this
        // frame is already settled when that contact is looked at.
        resolveBashStrikes(playerIdx);
        resolveBeams(playerIdx);             // the lancer: hitscan, same frame-after rule
        checkDodgePhase(playerIdx);          // perfect dodge through a ram / dive / lunge
        updateDodgeTrail(playerIdx, dt);

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

            BodyUserData* udA = bodyUD(bodyA);
            BodyUserData* udB = bodyUD(bodyB);

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
            // 2a-pre1. FERAL RAM vs a SHIP (death-chaos)
            // ================================================================
            // A feral unit's charge hurts any hull it meets, not only the one
            // it picked. The player side of a ram is handled further down.
            if (typeA == BodyType::Enemy && typeB == BodyType::Enemy && udA && udB) {
                const size_t ia = m_em->getEntityIndex(udA->entityId);
                const size_t ib = m_em->getEntityIndex(udB->entityId);
                if (ia != (size_t)-1 && ib != (size_t)-1 &&
                    ia < m_em->enemies.size() && ib < m_em->enemies.size()) {
                    bool hit = false;
                    for (int pass = 0; pass < 2; ++pass) {
                        const size_t r = pass ? ib : ia, v = pass ? ia : ib;
                        if (feralRamHit(r, v)) hit = true;
                    }
                    if (hit) continue;
                }
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
                BodyUserData* bulletUD = bodyUD(bulletBody);
                BodyUserData* targetUD = bodyUD(targetBody);
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
                        m_em->healths[targetIdx].hitFlash = 0.16f;

                        // Plasma on hull plate is the same event as plasma on
                        // an enemy ship, so it throws the same sparks: yellow
                        // shower plus a small red burst. Stone keeps the dull
                        // grey chips -- that contrast is now the fastest way
                        // to tell salvage from rock mid-fight.
                        if (m_em->healths[targetIdx].metallic) {
                            m_em->spawnImpact(hitPos, sf::Color::Yellow, hitVel);
                            m_em->spawnExplosion(hitPos, sf::Color(220, 60, 30), 4, 1.8f);
                        }
                        else {
                            m_em->spawnImpact(hitPos, sf::Color(180, 180, 180), hitVel);
                        }

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

                        // ---- BLOODSEEKER DISENGAGE: the round MISSES ----
                        // The opposite read to the ram: there the shot lands
                        // and does nothing; here it never lands at all. So
                        // the round keeps flying and he leaves a ghost.
                        if (isDuelEvading(targetIdx) && !blt.isReflected) {
                            evadeRound(bulletIdx, targetIdx, hitVel);
                            continue;
                        }

                        m_em->healths[targetIdx].currentHp -= dmg;
                        applyKnockback(targetIdx, hitVel, blt.knockback);
                        m_em->enemies[targetIdx].hitFlashTimer = 0.18f;
                        m_em->healths[targetIdx].hitFlash = 0.18f;
                        m_em->enemies[targetIdx].timesHit++;
                        m_em->enemies[targetIdx].provoke();   // player's round, parried ones included

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
                            // Per-unit resist for stuns that arrive by round
                            // (reflected shots, Rift direct hits). Its own key,
                            // default 0: every unit that does not set it takes
                            // these stuns in full, exactly as before.
                            const float res = std::clamp(
                                acfg(targetIdx, "stun_resist_ranged", 0.f), 0.f, 1.f);
                            m_em->healths[targetIdx].stunTimer =
                                std::max(m_em->healths[targetIdx].stunTimer, blt.stunOnHit * (1.f - res));
                            // A PARRIED round's stun arms the Bloodseeker's
                            // post-stun evade window (a no-op on anyone whose
                            // post_stun_evade_time is 0).
                            if (blt.isReflected) m_em->enemies[targetIdx].duelStunEvade = true;
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
                BodyUserData* otherUD = bodyUD(otherBody);
                BodyType otherType = otherUD ? otherUD->type : BodyType::Asteroid;

                // Every entity body carries its id in BodyUserData, so this is
                // one hash lookup -- it used to be a linear scan of every
                // entity, once per player contact.
                const size_t otherIdx = otherUD
                    ? m_em->getEntityIndex(otherUD->entityId) : (size_t)-1;

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
                    // Blood Dive: a rush, not a ram. A parry still parries
                    // him; anything else is just two hulls meeting.
                    if (otherIdx < m_em->enemies.size() && m_em->enemies[otherIdx].duelDiving()) {
                        if (isParryActive) parryEnemy(playerIdx, otherIdx);
                        continue;
                    }
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
                        // Any explosive launches like a magmatic rock -- the
                        // unstable core is its Rakshari-yard counterpart.
                        bool isMagmatic = (reward == 75) || m_em->healths[otherIdx].isExplosive;

                        m_em->spawnExplosion(otherPos, parryColor(), 15, 2.5f);

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

                            m_em->spawnExplosion(otherPos, parryColor(), 10, 1.5f);
                            onParrySuccess(playerIdx, otherPos);
                            // ---- Parry re-arm (every bullet parry) ----
                            onParryRearm(playerIdx, otherPos);
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
                        m_em->enemies[otherIdx].provoke();
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
                BodyUserData* bulletUD = bodyUD(bulletBody);
                BodyUserData* targetUD = bodyUD(targetBody);
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
                            m_em->chipPoise(playerIdx, blt.damage * blt.poiseMult
                                * vcfg("bullet_poise_per_damage", 0.6f));
                            m_em->healths[playerIdx].invulTimer = blt.playerIframes;
                            m_em->spawnExplosion(hitPos, sf::Color(255, 100, 0), 8, 2.f);
                            m_em->spawnImpact(hitPos, sf::Color(255, 140, 0), hitVel);
                        }
                        else if (hitType == BodyType::Player) {
                            onDodgeAte(playerIdx, hitPos);   // only pays out inside a dodge
                        }
                        else if (hitType == BodyType::Asteroid) {
                            m_em->healths[targetIdx].currentHp -= blt.damage * 0.6f;
                            m_em->spawnImpact(hitPos, sf::Color(255, 120, 0), hitVel);
                        }
                        // ---- Death-chaos: a FERAL round hurts the pack too ----
                        // Ordinary enemy rounds still stop dead on an enemy
                        // hull and do nothing. Never its own shooter, never a
                        // charging ram, never a dormant hull (it must stay a
                        // wreck to look at).
                        else if (hitType == BodyType::Enemy && blt.feralMult > 0.f &&
                            targetIdx < m_em->enemies.size() &&
                            m_em->transforms[targetIdx].entityId != blt.ownerEntityId &&
                            !m_em->enemies[targetIdx].dormant && !isRamInvulnerable(targetIdx))
                        {
                            m_em->healths[targetIdx].currentHp -= blt.damage * blt.feralMult;
                            m_em->healths[targetIdx].hitFlash = 0.16f;
                            m_em->enemies[targetIdx].hitFlashTimer = 0.16f;
                            m_em->spawnImpact(hitPos, sf::Color(255, 70, 50), hitVel);
                            m_em->spawnExplosion(hitPos, sf::Color(220, 40, 30), 4, 1.8f);
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
                    m_em->healths[idxA].hitFlash = 0.12f;
                    m_em->healths[idxB].hitFlash = 0.12f;

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
                                m_em->enemies[enIdx].provoke();   // parried or rift-hijacked: the player threw it

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
                                    0.40f, parryColor(), 6.f, 245.f);
                                m_em->spawnExplosion(astPos, parryColor(),
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
                            // A stray rock does NOT wake a dormant hull -- but
                            // it must flash the way it would on a real wreck,
                            // or the missing flash is the tell.
                            if (m_em->enemies[enIdx].dormant) m_em->healths[enIdx].hitFlash = 0.12f;
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

            BodyUserData* ud = bodyUD(bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid;
            bool shouldDestroy = false;

            if (m_em->healths[i].currentHp <= 0) {
                shouldDestroy = true;
                sf::Vector2f deathPos = m_em->transforms[i].position;

                if (type == BodyType::Asteroid) {
                    // Salvage pays out in scrap; plain rock (scrapMax 0) does
                    // not. scoreRewards is no longer credited anywhere -- it
                    // survives only as the size tag parry and Rift read.
                    dropScrap(i, rollScrap(m_em->healths[i].scrapMin, m_em->healths[i].scrapMax));
                    bool isExplosive = m_em->healths[i].isExplosive;

                    if (isExplosive) {
                        bool wasHoming = m_em->healths[i].isHoming;
                        float damageMult = wasHoming ? 5.f : 1.f;
                        float radiusMult = wasHoming ? 1.5f : 1.f;

                        float radius = m_em->healths[i].explosionRadius * radiusMult;
                        float damage = m_em->healths[i].explosionDamage * damageMult;

                        const sf::Color homing = homingColor(220);
                        sf::Color ringColor = wasHoming ? homing : sf::Color(255, 80, 0, 220);

                        m_em->addDebugAoE(deathPos, radius, ringColor, 0.4f);
                        m_em->spawnMagmaExplosion(deathPos, radius, wasHoming, homingColor(255));

                        // Magma rocks throw shards too. The fracture itself
                        // runs AFTER the blast below: an unstable core throws
                        // physical scrap (burst_children), and spawning it
                        // first would let the blast erase it on the same frame.
                        sf::Vector2f impactDir(0.f, 0.f);
                        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
                            b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
                            impactDir = { v.x, v.y };
                        }

                        if (wasHoming) {
                            m_em->spawnExplosion(deathPos, homingColor(255), 20, 4.0f);
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
                                else {
                                    m_em->healths[j].currentHp -= damage * falloff;
                                    m_em->healths[j].hitFlash = 0.16f;
                                    // Only a rock the player LAUNCHED counts as
                                    // the player's blast. A magma rock that just
                                    // broke carries no record of who broke it.
                                    if (wasHoming && j < m_em->enemies.size()) m_em->enemies[j].provoke();
                                }

                                if (j == playerIdx && falloff > vcfg("stagger_blast_falloff", 0.45f)) {
                                    m_em->staggerPlayer(playerIdx, otherPos - deathPos,
                                        vcfg("stagger_knockback", 900.f) * falloff,
                                        vcfg("stagger_tumble_duration", 1.1f),
                                        vcfg("stagger_recover_duration", 0.55f),
                                        vcfg("stagger_spin_speed", 620.f));
                                }
                            }
                        }

                        // Shards always; physical pieces only for types that
                        // ask for them (0 for MAGMATIC -- unchanged).
                        m_em->fractureAsteroid(i, impactDir, m_em->healths[i].burstChildren,
                            m_ef, m_lua, m_worldId);
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
                    // Executed by a Bloodseeker: the Rakshari kept that
                    // salvage. Only kills the PLAYER's fight caused pay out.
                    const bool executed = i < m_em->enemies.size() && m_em->enemies[i].executed;
                    if (!executed) dropScrap(i, enemyScrap(i));
                    else m_em->spawnShockRing(deathPos, 8.f, 90.f, 0.30f,
                        sf::Color(214, 172, 92), 4.f, 240.f);
                    // A Bloodseeker's death turns his pack on itself.
                    if (!executed) triggerDeathChaos(i, deathPos);
                    spawnEnemyDeath(i, deathPos);
                }
            }
            else if (type == BodyType::Bullet &&
                (m_em->bullets[i].markedForDestroy || m_em->bullets[i].lifetime <= 0)) {
                // A rocket always goes off -- on contact, or when its fuse
                // runs out mid-air. Never a silent despawn: a live fuse the
                // player has walked away from is a hazard they earned.
                const auto& b = m_em->bullets[i];
                // A mine that simply ran out (decay, never lit) fizzles: a
                // puff and a dead click, no blast. See WeaponSystem.
                const bool fizzle = b.isMine && !b.markedForDestroy && b.mineFuse <= 0.f;
                if (fizzle) {
                    const sf::Vector2f mp = m_em->transforms[i].position;
                    m_em->spawnExplosion(mp, sf::Color(120, 112, 100), 6, 1.6f);
                    m_em->spawnShockRing(mp, 3.f, 22.f, 0.20f, sf::Color(150, 140, 125), 1.5f, 150.f);
                }
                else if (b.isRocket || (b.isMine && b.armTimer <= 0.f)) {
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

    /// Lua `visuals` table, cached per config epoch (see LuaConfig.hpp).
    luacfg::Table m_cfgVisuals{ "visuals" };
    float vcfg(const char* key, float def) const {
        return m_cfgVisuals.get(m_lua, key, def);
    }
    /// Lua `weapon` table, cached per config epoch (see LuaConfig.hpp).
    luacfg::Table m_cfgWeapon{ "weapon" };
    float wcfg(const char* key, float def) const {
        return m_cfgWeapon.get(m_lua, key, def);
    }

    size_t findNearestEnemy(sf::Vector2f pos) {
        size_t nearestIdx = (size_t)-1;
        float nearestDistSq = FLT_MAX;

        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId)) continue;
            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud || ud->type != BodyType::Enemy) continue;
            // Powered down = no signature to lock onto. A parried round that
            // curved toward a "wreck" would announce the ambush for free.
            if (i < m_em->enemies.size() && m_em->enemies[i].dormant) continue;

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

        // ---- PERFECT SHOULDER BASH (1.10d) ----
        // The perfect dodge and the shoulder bash used to be two separate
        // reactions to one contact: the dodge paid out, then the bash still
        // charged the ram's damage. Now they are one event. If this dodge's
        // i-frames are still up, or it already paid out as perfect, the bash
        // is PERFECT: no damage taken, and the payout happens here if the
        // graze check has not done it yet.
        if (ps.dodgeIframeTimer > 0.f && !ps.dodgeAte)
            onDodgeAte(playerIdx, m_em->transforms[enIdx].position);
        const bool perfect = ps.dodgeAte;
        if (perfect) ps.dodgeVerdictBash = true;

        if (breaksCharge) {
            // Late (no perfect): take the ram through hyperarmor -- damage,
            // but never a tumble. Perfect: take nothing.
            if (!perfect) {
                const bool had = ps.hyperarmor;
                ps.hyperarmor = true;
                m_em->damagePlayer(playerIdx, acfg(enIdx, "ram_damage", 95.f));
                ps.hyperarmor = had;
            }

            ec.ramChainLeft = 0;
            ec.ramState = RamState::Recover;
            ec.ramDuration = acfg(enIdx, "ram_recover", 1.9f);
            ec.ramTimer = ec.ramDuration;
            ec.ramCooldown = std::max(ec.ramCooldown, acfg(enIdx, "ram_cooldown", 15.f));
        }

        m_em->healths[enIdx].currentHp -= m_feel.shoulderDamage * (breaksCharge ? m_feel.shoulderCounter : 1.f);
        ec.hitFlashTimer = 0.25f;
        ec.timesHit += 2;
        ec.provoke();
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

    // ========================================================================
    // BLOODSEEKER
    // ========================================================================

    /// Disengaging or in the post-stun window, and actually in control of
    /// himself. A stun or a tumble switches the dodge off: a punished
    /// Bloodseeker takes the hits.
    bool isDuelEvading(size_t idx) const {
        if (idx >= m_em->enemies.size()) return false;
        const auto& ec = m_em->enemies[idx];
        return ec.duelEvading() && ec.staggerTimer <= 0.f && m_em->healths[idx].stunTimer <= 0.f;
    }

    /// End a Blood Dive, ram-cancel rush or feint in progress. He lands in
    /// MELEE. A real dive leaves the mode timer for AISystem to re-roll; a
    /// trick happened inside MELEE, so its timer is kept.
    void breakDive(size_t idx) {
        if (idx >= m_em->enemies.size()) return;
        auto& ec = m_em->enemies[idx];
        if (!ec.duelBreakable()) return;
        ec.duelShift = DuelShift::None;
        ec.duelShiftTimer = 0.f;
        ec.feintShotsLeft = 0;
        ec.duelTrick = false;   // the mode clock is not touched: see AISystem note 23
    }

    /**
     * @brief Death-chaos: every Rakshari near a dying Bloodseeker goes feral.
     *
     * Not Maniacs (`pack_immune`), not other aura units, not dormant hulls.
     * AISystem does the rest (target picking, melee off vs ships); this is
     * the stamp and the signal. The signal has to be screen-wide because it
     * changes how every enemy on screen should be read: a dark red flash and
     * a ring out to the exact radius now, red brackets on the screen edge
     * (RenderSystem) for as long as any of them is still feral.
     */
    void triggerDeathChaos(size_t dead, sf::Vector2f pos) {
        if (!m_registry || dead >= m_em->enemies.size()) return;
        const sol::table& cfg = m_registry->resolve(m_em->enemies[dead].archetype).config;
        const float radius = cfg["death_chaos_radius"].get_or(0.f);
        if (radius <= 0.f) return;
        const float time = cfg["death_chaos_time"].get_or(5.f);
        const float retarget = cfg["death_chaos_retarget"].get_or(1.f);
        const float dmg = cfg["death_chaos_damage"].get_or(1.5f);
        const float frenzy = cfg["death_chaos_frenzy"].get_or(0.25f);

        int turned = 0;
        for (size_t j = 0; j < m_em->physics.size() && j < m_em->enemies.size(); ++j) {
            if (j == dead) continue;
            BodyUserData* ud = b2Body_IsValid(m_em->physics[j].bodyId) ? bodyUD(m_em->physics[j].bodyId) : nullptr;
            if (!ud || ud->type != BodyType::Enemy) continue;
            auto& ej = m_em->enemies[j];
            if (!ej.powered() || m_em->healths[j].currentHp <= 0.f) continue;
            const sol::table& cj = m_registry->resolve(ej.archetype).config;
            if (cj["pack_immune"].get_or(false) || cj["aura_radius"].get_or(0.f) > 0.f) continue;
            const sf::Vector2f d = m_em->transforms[j].position - pos;
            if (d.x * d.x + d.y * d.y > radius * radius) continue;

            ej.feralTimer = time;
            ej.feralRetarget = 0.f;          // pick a target on the next frame
            ej.feralRetargetTime = retarget;
            ej.feralTargetId = 0;
            ej.feralFrenzy = frenzy;
            ej.feralDamage = dmg;
            ej.execBuffTimer = 0.f;          // the man who gave that is dead
            ej.telegraphActive = false;      // whatever it was lining up is off
            ej.telegraphTimer = 0.f;
            m_em->spawnShockRing(m_em->transforms[j].position, 8.f, 60.f, 0.26f,
                sf::Color(255, 40, 30), 3.f, 240.f);
            ++turned;
        }

        // The signal, even if nobody was close enough to turn: his death is
        // an event either way.
        m_em->spawnScreenFlash(sf::Color(150, 10, 10), 0.45f, turned > 0 ? 95.f : 55.f);
        m_em->spawnShockRing(pos, 20.f, radius, 0.70f, sf::Color(255, 40, 30), 6.f, 230.f);
        m_em->addTrauma(0.55f);
        m_em->requestHitstop(0.05f, 0.14f, 0.35f);
    }

    /// A tumbling Bloodseeker is not charging a lancer or holding a cone.
    /// Mirrors AISystem::cancelRangeAttack: broken, not finished, so the
    /// attack goes on cooldown instead of resuming when he recovers.
    void breakRangeAttack(size_t idx) {
        if (idx >= m_em->enemies.size()) return;
        auto& ec = m_em->enemies[idx];
        if (ec.duelAttack == DuelAttack::None) return;
        const bool cone = ec.duelAttack >= DuelAttack::ConeWindup;
        ec.duelAttack = DuelAttack::None;
        ec.duelAtkTimer = 0.f;
        if (cone) ec.duelConeCd = std::max(ec.duelConeCd, 4.0f);
        else      ec.duelLancerCd = std::max(ec.duelLancerCd, 1.2f);
    }

    /**
     * @brief A round that hit a disengaging Bloodseeker goes on as a miss.
     *
     * Bullets are solid bodies, so by the time this contact is reported the
     * round has already bounced off the hull. Undo that: put it back on its
     * own line, clear of the far side of the hull, at its own speed. Player
     * rounds fly straight (no homing -- reflected rounds never get here), so
     * the transform's spawn velocity IS its line.
     *
     * He gets kicked off the line the round was on, and his ghost stays on it,
     * so the player sees the shot go through where he used to be.
     */
    void evadeRound(size_t bulletIdx, size_t enIdx, sf::Vector2f vel) {
        const float sp = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (sp < 1.f) { m_em->bullets[bulletIdx].markedForDestroy = true; return; }
        const sf::Vector2f dir = vel / sp;

        const enemyarch::ArchetypeDef* adef = m_registry
            ? &m_registry->resolve(m_em->enemies[enIdx].archetype) : nullptr;
        const float radius = adef ? adef->radius : 40.f;

        const sf::Vector2f bp = m_em->transforms[bulletIdx].position;
        const sf::Vector2f ep = m_em->transforms[enIdx].position;
        const sf::Vector2f rel = ep - bp;
        const float along = rel.x * dir.x + rel.y * dir.y;

        // ---- The round: same line, past the hull, same speed ----
        const sf::Vector2f np = bp + dir * (std::max(0.f, along) + radius + 14.f);
        const b2BodyId bb = m_em->physics[bulletIdx].bodyId;
        if (b2Body_IsValid(bb)) {
            b2Body_SetTransform(bb, { np.x / SCALE, np.y / SCALE }, b2Body_GetRotation(bb));
            b2Body_SetLinearVelocity(bb, { vel.x / SCALE, vel.y / SCALE });
        }
        m_em->transforms[bulletIdx].position = np;

        // ---- Him: off the line, toward the side he was already on ----
        sf::Vector2f side = rel - dir * along;
        const float sl = std::sqrt(side.x * side.x + side.y * side.y);
        side = (sl > 0.5f) ? side / sl
            : sf::Vector2f(-dir.y, dir.x) * ((rand() % 2) ? 1.f : -1.f);

        const b2BodyId eb = m_em->physics[enIdx].bodyId;
        if (b2Body_IsValid(eb)) {
            const float kick = acfg(enIdx, "disengage_dodge_kick", 460.f) * 0.8f;
            const b2Vec2 v = b2Body_GetLinearVelocity(eb);
            b2Body_SetLinearVelocity(eb, { v.x + side.x * kick / SCALE, v.y + side.y * kick / SCALE });
        }

        const sf::Color bone(232, 222, 196);
        if (adef) fx::afterimage(*m_em, *adef, ep, m_em->transforms[enIdx].rotation,
            { 0.f, 0.f }, bone, 0.22f);
        // A flick of bone dust where the round went through the ghost.
        m_em->spawnImpact(bp, sf::Color(232, 222, 196, 200), dir * 120.f);
    }

    /// Mid-lunge, or recoiling from a strike that already resolved. Contact
    /// begin-events from this ship are ignored -- the strike was the hit.
    bool isBashCommitted(size_t idx) const {
        if (idx >= m_em->enemies.size()) return false;
        const auto& ec = m_em->enemies[idx];
        return ec.bashState == BashState::Lunge ||
            (ec.bashState == BashState::Recover && ec.bashConnected);
    }

    /// The player's painted parry colour. Every parry flash, ring and burst
    /// reads it, so a repaint is consistent across the whole mechanic.
    /// A rock the player hijacked or parried is the player's own weapon.
    sf::Color homingColor(std::uint8_t alpha) const {
        const size_t p = m_em ? m_em->getEntityIndex(m_playerEntityId) : (size_t)-1;
        const sf::Color c = (p == (size_t)-1 || p >= m_em->players.size())
            ? sf::Color(0, 255, 200) : m_em->players[p].livery.paint.homing;
        return sf::Color(c.r, c.g, c.b, alpha);
    }

    /// Mix toward white. Keeps a repainted effect's highlight in the family.
    static sf::Color lighten(sf::Color c, float k) {
        return sf::Color(static_cast<std::uint8_t>(c.r + (255 - c.r) * k),
            static_cast<std::uint8_t>(c.g + (255 - c.g) * k),
            static_cast<std::uint8_t>(c.b + (255 - c.b) * k), c.a);
    }

    sf::Color parryColor() const {
        const size_t p = m_em ? m_em->getEntityIndex(m_playerEntityId) : (size_t)-1;
        if (p == (size_t)-1 || p >= m_em->players.size()) return sf::Color(0, 255, 200);
        return m_em->players[p].livery.paint.parry;
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
            vcfg("parry_flash_alpha", 170.f),
            parryColor());
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
        m_em->healths[otherIdx].hitFlash = 0.2f;
        m_em->healths[otherIdx].stunTimer = stunDuration * (1.f - stunResist);
        m_em->enemies[otherIdx].provoke();

        // Bloodseeker: the stun ends in his post-stun evade window, and a
        // parry ends a dive, rush or feint outright.
        m_em->enemies[otherIdx].duelStunEvade = true;
        breakDive(otherIdx);

        // ---- FULL STAGGER, not just a shove ----
        // A melee parry is the highest-risk thing the player can do, so it
        // gets the loudest reaction available.
        staggerEnemy(otherIdx, away, wcfg("parry_melee_knockback", 1100.f), 1.0f);

        m_em->enemies[otherIdx].hitFlashTimer = 0.22f;
        m_em->spawnExplosion(otherPos, parryColor(), 22, 3.0f);

        const sf::Vector2f mid = (playerPos + otherPos) * 0.5f;
        onParrySuccess(playerIdx, mid);
        onParryRearm(playerIdx, mid);
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
     * after every connect. Only after a connect (1.10c): a bash the player's
     * i-frames ate never landed, there is no tumble to protect, and locking
     * the Berserker out made a perfect dodge read as "the enemy gave up".
     */
    void resolveBashStrikes(size_t playerIdx) {
        for (size_t i = 0; i < m_em->enemies.size(); ++i) {
            auto& ec = m_em->enemies[i];
            if (!ec.bashStrikePending) continue;
            ec.bashStrikePending = false;
            if (i == playerIdx) continue;
            if (m_em->healths[i].currentHp <= 0.f) continue;   // died mid-swing

            // ---- FERAL: the swing was at another ship ----
            if (ec.bashTargetId != 0) {
                const uint32_t tid = ec.bashTargetId;
                ec.bashTargetId = 0;
                const size_t t = m_em->getEntityIndex(tid);
                if (t == (size_t)-1 || t >= m_em->enemies.size()) continue;
                if (m_em->healths[t].currentHp <= 0.f || m_em->enemies[t].dormant) continue;
                ec.bashCooldown = std::max(ec.bashCooldown, acfg(i, "bash_hit_cooldown", 2.0f));
                const sf::Vector2f ePos = m_em->transforms[i].position;
                const sf::Vector2f tPos = m_em->transforms[t].position;
                const sf::Vector2f contact = ePos + (tPos - ePos) * 0.6f;
                if (isRamInvulnerable(t)) {   // a charging hull shrugs it off
                    m_em->spawnImpact(contact, sf::Color(255, 230, 190), ec.bashDir * -300.f);
                    continue;
                }
                const float mult = ec.feralDamage > 0.f ? ec.feralDamage : 1.f;
                hurtShip(t, acfg(i, "bash_damage", 30.f) * mult);
                staggerEnemy(t, ec.bashDir, acfg(i, "bash_knockback", 950.f) * 0.7f, 0.7f);
                m_em->spawnShockRing(contact, 6.f, 85.f, 0.18f, sf::Color(255, 90, 70), 5.f, 255.f);
                m_em->spawnImpact(contact, sf::Color(255, 90, 60), ec.bashDir * -500.f);
                m_em->addTrauma(0.10f);
                continue;
            }

            // ---- PARRIED: the reward ----
            if (m_em->players[playerIdx].parryTimer > 0.f) {
                parryEnemy(playerIdx, i);
                continue;
            }

            const sf::Vector2f ePos = m_em->transforms[i].position;
            const sf::Vector2f pPos = m_em->transforms[playerIdx].position;
            const sf::Vector2f contact = ePos + (pPos - ePos) * 0.6f;

            auto& php = m_em->healths[playerIdx];
            if (php.invulTimer > 0.f) {
                // Landed on i-frames. Show it connected with nothing -- and
                // leave the attacker's rhythm alone: no hit-confirm lockout
                // for a hit that never landed (1.10c).
                m_em->spawnImpact(contact, sf::Color(255, 230, 190), ec.bashDir * -300.f);
                onDodgeAte(playerIdx, contact);
                continue;
            }

            // Hit-confirm lockout: only for a bash that LANDED (anti tumble-lock).
            ec.bashCooldown = std::max(ec.bashCooldown, acfg(i, "bash_hit_cooldown", 2.0f));
            ec.ramCooldown = std::max(ec.ramCooldown, acfg(i, "hit_confirm_cooldown", 0.f));

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

    // ========================================================================
    // PERFECT DODGE (1.10)
    // ========================================================================

    /**
     * @brief This dodge's i-frames just ate an enemy attack: pay it out.
     *
     * Once per dodge. Energy refunded (falling off over a chain so sustained
     * fire cannot be dodged forever), the dodge re-armed on the spot, a ghost
     * of the ship left where the hit should have landed in the player's own
     * dodge paint, a short hitstop. Rocks and other non-attacks never call
     * this -- dodging into scenery is not a read.
     */
    void onDodgeAte(size_t playerIdx, sf::Vector2f at) {
        if (playerIdx >= m_em->players.size()) return;
        auto& ps = m_em->players[playerIdx];
        if (ps.dodgeIframeTimer <= 0.f || ps.dodgeAte) return;
        ps.dodgeAte = true;

        const float falloff = (*m_lua)["dodge_perfect_chain_falloff"].get_or(0.7f);
        const float k = std::pow(falloff, static_cast<float>(ps.dodgeChain));
        ps.energyDrive = std::min(ps.maxEnergyDrive,
            ps.energyDrive + ps.dashEnergyCost * (*m_lua)["dodge_perfect_refund"].get_or(1.f) * k);
        ps.dashCooldown = 0.f;                                   // re-armed
        ps.dodgeChain++;
        ps.dodgeChainTimer = (*m_lua)["dodge_perfect_chain_time"].get_or(1.5f);

        // ---- The tell (playtest: the first version drowned in the dodge's own
        // flash and shake). Three layers, all in the player's dodge paint:
        //   1. THE MISS -- a bright, thick ghost of the ship left exactly
        //      where the attack met it, lingering ~0.6s: "it hit THAT, not me".
        //   2. THE TRAIL -- the rest of the dodge sheds ghosts every ~35ms,
        //      the Bloodseeker's disengage read, so the escape itself is drawn.
        //   3. THE BEAT -- a real slow-motion dip and a double ring. Distinct
        //      from a normal dodge, which never slows time.
        const auto& tf = m_em->transforms[playerIdx];
        const sf::Color paint = ps.livery.paint.dodge;
        const float rot = tf.rotation + tf.visualOffsetAngle;
        if (ps.modelTris.size() >= 3 && ps.modelOutline.size() >= 3)
            fx::afterimageShape(*m_em, ps.modelOutline, ps.modelTris, tf.position, rot,
                { 0.f, 0.f }, lighten(paint, 0.55f), 0.60f, 3.0f, 110);
        ps.dodgeTrailTimer = (*m_lua)["dodge_perfect_trail_time"].get_or(0.35f);
        ps.dodgeTrailTick = 0.f;
        ps.dodgeVerdictFlash = 0.9f;
        ps.dodgeVerdictBash = false;

        m_em->spawnShockRing(tf.position, 10.f, 110.f, 0.32f, lighten(paint, 0.5f), 4.f, 255.f);
        m_em->spawnShockRing(tf.position, 4.f, 55.f, 0.20f, sf::Color(255, 255, 255), 2.5f, 230.f);
        m_em->spawnExplosion(at, lighten(paint, 0.5f), 14, 2.2f);
        m_em->spawnScreenFlash(lighten(paint, 0.6f), 0.14f, 55.f);
        m_em->requestHitstop(0.04f, 0.22f, 0.30f);
        m_em->addTrauma(0.15f);
    }

    /// Perfect-dodge trail: a ghost every few frames for the rest of the
    /// escape, fading along the line it took.
    void updateDodgeTrail(size_t playerIdx, float dt) {
        auto& ps = m_em->players[playerIdx];
        if (ps.dodgeTrailTimer <= 0.f) return;
        ps.dodgeTrailTimer -= dt;
        ps.dodgeTrailTick -= dt;
        if (ps.dodgeTrailTick > 0.f) return;
        ps.dodgeTrailTick = 0.035f;
        if (ps.modelTris.size() < 3 || ps.modelOutline.size() < 3) return;
        const auto& tf = m_em->transforms[playerIdx];
        fx::afterimageShape(*m_em, ps.modelOutline, ps.modelTris, tf.position,
            tf.rotation + tf.visualOffsetAngle, { 0.f, 0.f },
            lighten(ps.livery.paint.dodge, 0.3f), 0.30f, 1.6f, 45);
    }

    /**
     * @brief Phase-through and GRAZE detection, every frame of a dodge's
     * i-frames.
     *
     * With enemy collisions off, a ram, a Blood Dive or a bash lunge passing
     * over the ship never makes a contact event -- so look for it. And a
     * contact-only rule only ever paid out for dodging INTO an attack, which
     * nobody does on purpose (playtest: "no indication"). So a near miss
     * counts too: an attacking hull within dodge_graze_hull px of the ship's
     * edge, or an enemy round within dodge_graze_bullet px of its centre,
     * while the i-frames are up. The attacker is untouched: a perfect dodge
     * rewards the player, it never makes the enemy back off.
     */
    void checkDodgePhase(size_t playerIdx) {
        auto& ps = m_em->players[playerIdx];
        if (ps.dodgeIframeTimer <= 0.f || ps.dodgeAte || !m_registry) return;
        const sf::Vector2f pp = m_em->transforms[playerIdx].position;
        const float grazeHull = (*m_lua)["dodge_graze_hull"].get_or(55.f);
        const float grazeBullet = (*m_lua)["dodge_graze_bullet"].get_or(40.f);

        // ---- Rounds grazing past ----
        for (size_t j = 0; j < m_em->bullets.size() && j < m_em->transforms.size(); ++j) {
            const auto& b = m_em->bullets[j];
            if (!b.isEnemyBullet || b.isMine || b.markedForDestroy) continue;
            const sf::Vector2f d = m_em->transforms[j].position - pp;
            if (d.x * d.x + d.y * d.y > grazeBullet * grazeBullet) continue;
            onDodgeAte(playerIdx, pp + d);
            return;
        }

        // ---- Attacking hulls through or past ----
        for (size_t j = 0; j < m_em->enemies.size() && j < m_em->physics.size(); ++j) {
            auto& ej = m_em->enemies[j];
            const bool attacking = ej.ramState == RamState::Charge || ej.duelDiving()
                || ej.bashState == BashState::Lunge;
            if (!attacking || j == playerIdx) continue;
            const float r = m_registry->resolve(ej.archetype).radius * 0.9f + 18.f + grazeHull;
            const sf::Vector2f d = m_em->transforms[j].position - pp;
            if (d.x * d.x + d.y * d.y > r * r) continue;
            onDodgeAte(playerIdx, pp + d * 0.5f);
            return;
        }
    }

    // ========================================================================
    // BEAMS -- the Bloodseeker's lancer (hitscan, piercing, mirror parry)
    // ========================================================================

    /// Rough body radius for a beam hit test: archetype radius for ships,
    /// furthest vertex for polygons, the circle for circles.
    float beamBodyRadius(size_t j, BodyType t) const {
        if (t == BodyType::Enemy && m_registry && j < m_em->enemies.size())
            return m_registry->resolve(m_em->enemies[j].archetype).radius * 0.85f;
        if (j < m_em->physicsShapes.size()) {
            const auto& s = m_em->physicsShapes[j];
            if (s.type == PhysicsShapeData::Type::Circle) return s.radius;
            float r2 = 0.f;
            for (const auto& v : s.vertices) r2 = std::max(r2, v.x * v.x + v.y * v.y);
            if (r2 > 0.f) return std::sqrt(r2) * 0.9f;
        }
        return 16.f;
    }

    void resolveBeams(size_t playerIdx) {
        if (m_em->beamShots.empty()) return;
        std::vector<BeamShot> shots;
        shots.swap(m_em->beamShots);
        for (const auto& s : shots) traceBeam(playerIdx, s);
    }

    /**
     * @brief Everything on the line, at once.
     *
     * Pierces: rocks and wrecks take objectDamage (gone), ships at or under
     * killHp max HP are destroyed, bigger ones (Barge, Bloodseeker) take
     * pierceDamage and a stagger. The player takes damage + a tumble unless
     * on i-frames -- or PARRYING, in which case the beam stops at the
     * shield and reflects off its curve like light off a convex mirror:
     * dead centre sends it straight back down the lane at him, an edge hit
     * glances off at an angle. The reflection is the player's beam (same
     * pierce, reflect_* damage) and gets one bounce.
     */
    void traceBeam(size_t playerIdx, const BeamShot& s) {
        struct Hit { size_t j; float t; float perp; BodyType type; float r; };
        std::vector<Hit> hits;
        const float half = s.width * 0.5f;
        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (!b2Body_IsValid(m_em->physics[j].bodyId)) continue;
            BodyUserData* ud = bodyUD(m_em->physics[j].bodyId);
            if (!ud || ud->type == BodyType::Bullet) continue;
            if (m_em->transforms[j].entityId == s.ownerId) continue;
            if (m_em->healths[j].currentHp <= 0.f) continue;
            const float r = (j == playerIdx) ? 18.f : beamBodyRadius(j, ud->type);
            const sf::Vector2f rel = m_em->transforms[j].position - s.origin;
            const float t = rel.x * s.dir.x + rel.y * s.dir.y;
            if (t < -r || t > s.range + r) continue;
            const float perp = std::fabs(rel.x * s.dir.y - rel.y * s.dir.x);
            if (perp > r + half) continue;
            hits.push_back({ j, std::max(0.f, t), perp, ud->type, r });
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });

        float end = s.range;
        for (const auto& h : hits) {
            const size_t j = h.j;
            const sf::Vector2f at = s.origin + s.dir * h.t;

            if (j == playerIdx) {
                auto& ps = m_em->players[playerIdx];
                auto& ph = m_em->healths[playerIdx];
                if (ps.parryTimer > 0.f && s.bounce < 2) {
                    // ---- MIRROR ----
                    const sf::Vector2f pc = m_em->transforms[playerIdx].position;
                    const float R = h.r + half;
                    const float back = std::sqrt(std::max(0.f, R * R - h.perp * h.perp));
                    const sf::Vector2f entry = s.origin + s.dir * std::max(0.f, h.t - back);
                    sf::Vector2f n = entry - pc;
                    const float nl = std::sqrt(n.x * n.x + n.y * n.y);
                    n = (nl > 0.01f) ? n / nl : -s.dir;
                    const float dn = s.dir.x * n.x + s.dir.y * n.y;
                    sf::Vector2f refl = s.dir - n * (2.f * dn);
                    const float rl = std::sqrt(refl.x * refl.x + refl.y * refl.y);
                    refl = (rl > 0.01f) ? refl / rl : -s.dir;
                    end = std::max(0.f, h.t - back);

                    BeamShot r = s;
                    r.origin = entry + refl * 2.f;
                    r.dir = refl;
                    r.ownerId = m_playerEntityId;
                    r.byPlayer = true;
                    r.bounce = s.bounce + 1;
                    r.pierceDamage = wcfg("lancer_reflect_damage", 380.f);
                    r.killHp = wcfg("lancer_reflect_kill_hp", 500.f);

                    m_em->spawnShockRing(entry, 6.f, 80.f, 0.22f, sf::Color(250, 244, 230), 4.f, 255.f);
                    m_em->spawnShockRing(entry, 4.f, 46.f, 0.16f, parryColor(), 3.f, 240.f);
                    m_em->spawnExplosion(entry, sf::Color(250, 244, 230), 18, 2.6f);
                    m_em->spawnScreenFlash(sf::Color(250, 244, 230), 0.12f, 70.f);
                    onParrySuccess(playerIdx, entry);
                    onParryRearm(playerIdx, entry);
                    traceBeam(playerIdx, r);
                    break;
                }
                if (ph.invulTimer > 0.f && !s.byPlayer) onDodgeAte(playerIdx, at);
                if (ph.invulTimer <= 0.f && !s.byPlayer) {
                    m_em->damagePlayer(playerIdx, s.damage);
                    ph.invulTimer = s.iframes;
                    m_em->staggerPlayer(playerIdx, s.dir, s.knockback,
                        vcfg("stagger_tumble_duration", 1.1f),
                        vcfg("stagger_recover_duration", 0.55f),
                        vcfg("stagger_spin_speed", 620.f));
                    m_em->spawnExplosion(at, sf::Color(255, 90, 60), 16, 2.8f);
                }
                continue;
            }

            auto& hp = m_em->healths[j];
            if (h.type == BodyType::Enemy && j < m_em->enemies.size()) {
                auto& ej = m_em->enemies[j];
                if (hp.maxHp <= s.killHp) hp.currentHp = -1.f;           // through it
                else {
                    hp.currentHp -= s.pierceDamage;
                    if (hp.currentHp > 0.f && !ej.dormant)
                        staggerEnemy(j, s.dir, 650.f, 0.8f);
                }
                hp.hitFlash = 0.2f;
                ej.hitFlashTimer = 0.25f;
                ej.timesHit += 2;
                if (s.byPlayer) ej.provoke();
            }
            else {
                hp.currentHp -= s.objectDamage;                          // rocks, wrecks, scrap
                hp.hitFlash = 0.2f;
            }
            m_em->spawnExplosion(at, sf::Color(250, 244, 230), 10, 2.4f);
            m_em->spawnImpact(at, sf::Color(255, 90, 60), s.dir * 700.f);
        }

        // ---- The beam itself ----
        BeamFx fx;
        fx.a = s.origin;
        fx.b = s.origin + s.dir * end;
        fx.width = s.width;
        fx.reflected = s.byPlayer;
        m_em->beams.push_back(fx);
        // Sparks shed along the line.
        for (int k = 0; k < 26; ++k) {
            const float t = (rand() % 1000) / 1000.f * end;
            const sf::Vector2f side(-s.dir.y, s.dir.x);
            const float life = 0.18f + (rand() % 20) / 100.f;
            m_em->particles.push_back({ s.origin + s.dir * t,
                side * static_cast<float>((rand() % 240) - 120) + s.dir * static_cast<float>(rand() % 200),
                s.byPlayer ? sf::Color(255, 238, 120, 230) : sf::Color(250, 244, 230, 230),
                life, life, 2.f + rand() % 2 });
        }
        m_em->addTrauma(0.30f);
        m_em->requestHitstop(0.03f, 0.08f, 0.5f);
    }

    /** @brief Enemy-on-enemy damage from death-chaos melee. */
    void hurtShip(size_t t, float dmg) {
        m_em->healths[t].currentHp -= dmg;
        m_em->healths[t].hitFlash = 0.2f;
        m_em->enemies[t].hitFlashTimer = 0.2f;
        m_em->enemies[t].timesHit += 1;
    }

    /**
     * @brief Feral charge `r` meets ship `v`. Returns true if it was one.
     *
     * Any hull, not just the picked target: a charge in a crowd is chaos.
     * Damage ram_damage x death_chaos_damage, thrown sideways out of the
     * lane like the player is. Not a dormant hull, not another charge.
     */
    bool feralRamHit(size_t r, size_t v) {
        auto& er = m_em->enemies[r];
        if (er.ramState != RamState::Charge || er.feralTimer <= 0.f) return false;
        auto& ev = m_em->enemies[v];
        if (ev.dormant || isRamInvulnerable(v) || m_em->healths[v].currentHp <= 0.f) return false;
        const float mult = er.feralDamage > 0.f ? er.feralDamage : 1.f;
        hurtShip(v, acfg(r, "ram_damage", 95.f) * mult);
        const sf::Vector2f rp = m_em->transforms[r].position, vp = m_em->transforms[v].position;
        sf::Vector2f away = vp - rp;
        const float along = away.x * er.ramDir.x + away.y * er.ramDir.y;
        sf::Vector2f side = away - er.ramDir * along;
        const float sl = std::sqrt(side.x * side.x + side.y * side.y);
        side = sl > 0.5f ? side / sl : sf::Vector2f(-er.ramDir.y, er.ramDir.x);
        staggerEnemy(v, er.ramDir * 0.55f + side * 0.85f, acfg(r, "ram_knockback", 1100.f) * 0.8f, 1.f);
        m_em->spawnExplosion((rp + vp) * 0.5f, sf::Color(255, 90, 60), 18, 3.5f);
        m_em->addTrauma(0.16f);
        return true;
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

        // I-frames ate it: NOTHING changes on the attacker's side. The chain
        // keeps coming, no hit-confirm lockout -- dodging well is the
        // player's reward, never a reason for the enemy to back off (1.10c).
        auto& php = m_em->healths[playerIdx];
        if (php.invulTimer > 0.f) {
            onDodgeAte(playerIdx, m_em->transforms[enIdx].position);
            return;
        }

        // A chain that LANDS stops, with the hit-confirm lockout, so a hit
        // cannot chain into a tumble-lock. See AISystem 2.1, note 11.
        ec.ramChainLeft = 0;
        ec.bashCooldown = std::max(ec.bashCooldown, acfg(enIdx, "hit_confirm_cooldown", 0.f));

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
            m_em->particles.push_back({
                pos + d * (radius * 0.18f), d * sp,
                sf::Color(255, static_cast<uint8_t>(150 + rand() % 90), 60, 240),
                life, life, 2.f + rand() % 3 });
        }

        // Slow smoke that outlives the flash and marks the spot.
        for (int k = 0; k < 8; ++k) {
            const float r = (rand() % 360) * 3.14159f / 180.f;
            m_em->particles.push_back({ pos,
                sf::Vector2f(std::cos(r), std::sin(r)) * (25.f + rand() % 55),
                sf::Color(80, 62, 55, 165), 0.85f, 0.85f, 6.f + rand() % 6 });
        }

        for (size_t j = 0; j < m_em->physics.size(); ++j) {
            if (j == skipIdx) continue;

            // Bullets in the blast are ignored on purpose: chaining every
            // round in the air turns two rockets into an arena-wide cascade.
            BodyUserData* ud = b2Body_IsValid(m_em->physics[j].bodyId)
                ? bodyUD(m_em->physics[j].bodyId) : nullptr;
            if (ud && ud->type == BodyType::Bullet) continue;

            const sf::Vector2f o = m_em->transforms[j].position;
            const float dx = pos.x - o.x, dy = pos.y - o.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= radius) continue;

            const float falloff = 1.f - (d / radius);
            // A dodge's i-frames cover blasts too: the player was promised
            // "untouchable while dodging", and a mine is still an attack.
            if (j == playerIdx && j < m_em->players.size() && m_em->players[j].dodgeIframeTimer > 0.f) {
                onDodgeAte(playerIdx, o);
                continue;
            }
            if (j == playerIdx) m_em->damagePlayer(j, damage * falloff);
            else {
                m_em->healths[j].currentHp -= damage * falloff;
                m_em->healths[j].hitFlash = 0.16f;
            }

            if (ud && ud->type == BodyType::Enemy && j < m_em->enemies.size()) {
                m_em->enemies[j].hitFlashTimer = 0.2f;
                m_em->enemies[j].timesHit += 2;   // an AoE is unambiguous
                // Credited to the player (a parried rocket, a thrown Maniac):
                // the player's blast. A pirate's own rocket going off next to
                // a dormant hull is not.
                if (creditIdx == playerIdx) m_em->enemies[j].provoke();
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
        float speed = wcfg("parry_rocket_speed", 1150.f);

        // ---- SEEKER (playtest 1.9): it picks a random victim and gets there ----
        // Any enemy ship or rock in range, never you. From here WeaponSystem
        // flies it: a drunken weave that tightens as it closes, collisions
        // off (mask 0) so nothing on the way can stop it, detonation on
        // arrival. Nothing in range: the old wild flight, as before.
        const b2BodyId rb = m_em->physics[rocketIdx].bodyId;
        const uint32_t victim = seek::pickTarget(*m_em, rPos, wcfg("parry_rocket_seek_range", 1300.f),
            m_playerEntityId, m_em->transforms[rocketIdx].entityId,
            wcfg("parry_rocket_ship_weight", 3.f), wcfg("parry_rocket_rock_weight", 1.f));
        if (victim != 0 && b2Body_IsValid(rb)) {
            b.seekTargetId = victim;
            b.seekSpeed = wcfg("parry_rocket_seek_speed", 900.f);
            b.seekTurn = wcfg("parry_rocket_seek_turn", 600.f);
            b.seekWobbleAmp = wcfg("parry_rocket_wobble", 55.f);
            b.seekWobble = (rand() % 628) / 100.f;
            speed = b.seekSpeed;
            b2ShapeId sid;
            if (b2Body_GetShapes(rb, &sid, 1) > 0) {
                b2Filter f = b2Shape_GetFilter(sid);
                f.maskBits = 0;
                b2Shape_SetFilter(sid, f);
            }
            // Mark the pick: a yellow ring closing on the victim.
            const size_t vi = m_em->getEntityIndex(victim);
            if (vi != (size_t)-1)
                m_em->spawnShockRing(m_em->transforms[vi].position, 70.f, 8.f, 0.35f,
                    sf::Color(255, 238, 0), 3.f, 230.f);
        }
        if (b2Body_IsValid(rb)) {
            b2Body_SetLinearVelocity(rb, { dir.x * speed / SCALE, dir.y * speed / SCALE });
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

        m_em->spawnExplosion(rPos, parryColor(), 14, 2.2f);
        m_em->spawnShockRing(rPos, 5.f, 52.f, 0.20f, sf::Color(255, 240, 90), 3.f, 240.f);
        onParrySuccess(playerIdx, rPos);
        onParryRearm(playerIdx, rPos);
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
                m_em->particles.push_back({
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

            m_em->spawnExplosion(ePos, parryColor(), 26, 3.4f);
            m_em->spawnShockRing(ePos, 14.f, 150.f, 0.28f, lighten(parryColor(), 0.55f), 4.f, 240.f);
            onParrySuccess(playerIdx, (pPos + ePos) * 0.5f);
            onParryRearm(playerIdx, ePos);
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
        const enemyarch::ArchetypeDef* adef = m_registry
            ? &m_registry->resolve(m_em->enemies[i].archetype) : nullptr;

        // Already blew himself up -- the blast WAS the death. Stacking a
        // standard explosion on top of it would read as two separate events.
        // The hull still has to go somewhere, though: it is thrown apart by
        // its own charge, hardest of any death.
        if (m_em->enemies[i].detonated) {
            if (adef) spawnWreckage(i, *adef, WRECK_BLAST);
            return;
        }

        // Shot down mid-charge: it still goes off, just smaller. A Maniac who
        // simply vanished when killed during the charge would make the whole
        // ignition beat retroactively meaningless.
        if (m_em->enemies[i].frenzyState == FrenzyState::Ignite ||
            m_em->enemies[i].frenzyState == FrenzyState::Charge) {
            blowUpManiac(i, false);
            if (adef) spawnWreckage(i, *adef, WRECK_BLAST);
            return;
        }

        if (adef && adef->config["death_style"].get_or<std::string>("standard") == "visceral") {
            spawnVisceralDeath(i, deathPos, *adef);
            return;
        }

        // ---- Standard: the 1.3 blast, and now the hull comes apart in it ----
        if (adef) spawnWreckage(i, *adef, WRECK_STANDARD);
        m_em->spawnExplosion(deathPos, sf::Color::Red, 35, 5.0f);
        m_em->spawnExplosion(deathPos, sf::Color::Yellow, 15, 2.5f);
        m_em->spawnShockRing(deathPos, 15.f, 190.f, 0.40f,
            sf::Color(255, 90, 40), 5.f, 230.f);
        m_em->addTrauma(0.30f);
    }

    // ========================================================================
    // SCRAP
    // ========================================================================

    static int rollScrap(int lo, int hi) {
        if (hi <= 0) return 0;
        lo = std::max(0, lo);
        hi = std::max(lo, hi);
        return lo + rand() % (hi - lo + 1);
    }

    /**
     * @brief What a dying ship is worth, read from its archetype NOW.
     *
     * `scrap_drop = { min, max }` in enemy.lua. Read at death rather than
     * cached at spawn so an F5 retune applies to ships already in the field.
     * A single number means a fixed amount.
     */
    int enemyScrap(size_t i) const {
        if (!m_registry || i >= m_em->enemies.size()) return 0;
        const sol::table& cfg = m_registry->resolve(m_em->enemies[i].archetype).config;
        sol::object sd = cfg["scrap_drop"];
        if (sd.is<sol::table>()) {
            sol::table t = sd.as<sol::table>();
            const int lo = t[1].get_or(0);
            return rollScrap(lo, t[2].get_or(lo));
        }
        if (sd.is<double>()) return std::max(0, static_cast<int>(sd.as<double>()));
        return 0;
    }

    /// Throw `amount` scrap out of entity i, carrying its momentum.
    void dropScrap(size_t i, int amount) {
        if (amount <= 0) return;
        sf::Vector2f vel(0.f, 0.f);
        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
            vel = { v.x * SCALE, v.y * SCALE };
        }
        m_em->spawnScrap(m_em->transforms[i].position, vel, amount);
    }

    // ========================================================================
    // SHIP WRECKAGE
    // ========================================================================

    /// Throw force per death style. Multiplies the archetype's wreck_speed and
    /// spin, so per-unit tuning stays in Lua and the STYLE keeps its ordering:
    /// a blast scatters harder than a visceral break, which scatters harder
    /// than an ordinary kill.
    static constexpr float WRECK_STANDARD = 1.0f;
    static constexpr float WRECK_VISCERAL = 1.6f;
    static constexpr float WRECK_BLAST = 2.2f;

    /**
     * @brief Break the dying ship into decorative pieces of its own hull.
     *
     * The hull is cut by shatter::slice (utils/ShipShatter.hpp); the armour
     * plates come off whole on top. Every piece starts exactly where that part
     * of the ship was, at the ship's heading, and leaves with:
     *
     *   - an outward throw along its own direction from the centre, jittered
     *   - a share of the ship's momentum -- a ship killed at speed leaves a
     *     debris trail that keeps going the way it was going
     *   - the ship's own spin, as tangential speed at that radius, so a
     *     tumbling ship flings its pieces off the way a tumbling ship would
     *
     * Then each piece holds for wreck_life seconds and fades over wreck_fade.
     * Nothing collides; see DebrisSystem.
     *
     * @return false if the unit opts out (`wreckage = false`) or has no hull,
     *         so a caller can fall back to its old shards.
     */
    bool spawnWreckage(size_t i, const enemyarch::ArchetypeDef& adef, float force) {
        const sol::table& cfg = adef.config;
        if (!cfg["wreckage"].get_or(true)) return false;
        if (adef.visualTris.size() < 3 || adef.visual.size() < 3) return false;

        // A kill-all on a packed field can land many deaths in one frame. The
        // pieces are pure spectacle, so past this many in flight, new kills
        // shed fewer: plates go first, then the cut gets coarser.
        constexpr size_t SOFT_CAP = 400;
        const bool crowded = m_em->wreckShards.size() > SOFT_CAP;
        if (m_em->wreckShards.size() > SOFT_CAP * 2) return true;

        const auto& tf = m_em->transforms[i];
        sf::Vector2f shipVel(0.f, 0.f);
        float shipSpin = 0.f;   // rad/s
        if (b2Body_IsValid(m_em->physics[i].bodyId)) {
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
            shipVel = { v.x * SCALE, v.y * SCALE };
            shipSpin = b2Body_GetAngularVelocity(m_em->physics[i].bodyId);
        }
        const float rr = tf.rotation * 3.14159f / 180.f;
        const float cs = std::cos(rr), sn = std::sin(rr);

        // ---- Tuning (Lua, per archetype; see enemy_defaults) ----
        const auto range = [&](const char* key, float lo, float hi) {
            sol::optional<sol::table> t = cfg[key];
            if (t) { lo = (*t)[1].get_or(lo); hi = (*t)[2].get_or(hi); }
            return std::pair<float, float>(lo, std::max(lo, hi));
            };
        const auto roll = [](std::pair<float, float> r) {
            return r.first + (r.second - r.first) * ((rand() % 1000) / 1000.f);
            };
        const auto life = range("wreck_life", 2.0f, 3.0f);
        const auto speed = range("wreck_speed", 40.f, 160.f);
        const auto plateSpeed = range("wreck_plate_speed", 80.f, 240.f);
        const float fade = std::max(0.05f, cfg["wreck_fade"].get_or(0.8f));
        const float spin = cfg["wreck_spin"].get_or(230.f);
        const float inherit = cfg["wreck_inherit"].get_or(0.6f);
        const float charK = cfg["wreck_char"].get_or(0.45f);
        const float drag = cfg["wreck_drag"].get_or(0.45f);
        const float cool = std::max(0.05f, cfg["wreck_cool_time"].get_or(1.1f));

        int cuts = cfg["wreck_cuts"].get_or(0);
        if (cuts <= 0) cuts = (adef.radius < 26.f) ? 2 : (adef.radius < 45.f) ? 3 : 4;
        if (crowded) cuts = std::max(1, cuts - 1);

        // ---- Colours ----
        // The hull's own colour, charred. A unit that dies still powered
        // down never showed its live colours, so it breaks in its cold ones
        // (the same drain EntityFactory::coldColorFor falls back to).
        sf::Color base = adef.color;
        if (i < m_em->enemies.size() && m_em->enemies[i].dormant)
            base = sf::Color(static_cast<uint8_t>(base.r * 0.22f + 44.f),
                static_cast<uint8_t>(base.g * 0.22f + 42.f),
                static_cast<uint8_t>(base.b * 0.22f + 42.f));
        const auto scaled = [](sf::Color c, float k, uint8_t a) {
            return sf::Color(static_cast<uint8_t>(std::clamp(c.r * k, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(c.g * k, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(c.b * k, 0.f, 255.f)), a);
            };
        // The live outline is fill + (60,50,50); the wreck keeps it, dimmed,
        // so a piece still reads as THIS ship's edge.
        const sf::Color liveEdge(static_cast<uint8_t>(std::min(255, base.r + 60)),
            static_cast<uint8_t>(std::min(255, base.g + 50)),
            static_cast<uint8_t>(std::min(255, base.b + 50)));
        const sf::Color skinCol = scaled(liveEdge, 0.85f, 230);
        const float lineW = (adef.radius > 45.f) ? 1.8f : 1.4f;

        const auto launch = [&](const shatter::Piece& pc, sf::Color fill, sf::Color skin,
            std::pair<float, float> spd, float spinMul, float lifeMul, float heat)
            {
                // Where this piece is in the world, and which way is "out".
                const sf::Vector2f wc(pc.centre.x * cs - pc.centre.y * sn,
                    pc.centre.x * sn + pc.centre.y * cs);
                float a;
                const float wl = std::sqrt(wc.x * wc.x + wc.y * wc.y);
                if (wl > 0.5f) a = std::atan2(wc.y, wc.x) + ((rand() % 1000) / 1000.f - 0.5f) * 1.0f;
                else           a = (rand() % 360) * 3.14159f / 180.f;
                const sf::Vector2f out(std::cos(a), std::sin(a));

                WreckShard s;
                s.position = tf.position + wc;
                s.velocity = out * (roll(spd) * force)
                    + shipVel * inherit
                    + sf::Vector2f(-wc.y, wc.x) * shipSpin;          // omega x r
                s.rotation = tf.rotation;
                s.angularVelocity = ((rand() % 2) ? 1.f : -1.f)
                    * (0.35f + 0.65f * ((rand() % 1000) / 1000.f)) * spin * spinMul * force
                    + shipSpin * 180.f / 3.14159f;
                s.fadeTime = fade;
                s.lifetime = roll(life) * lifeMul + fade;
                s.drag = drag;
                s.heat = heat;
                s.coolRate = 1.f / cool;
                s.emberTimer = (rand() % 60) / 1000.f;
                s.radius = pc.radius;
                s.lineWidth = lineW;
                s.fill = fill;
                s.skinColor = skin;
                s.tris = pc.tris;
                s.skin = pc.skin;
                s.scar = pc.scar;
                m_em->wreckShards.push_back(std::move(s));
            };

        // ---- 1. The hull, cut ----
        const sf::Color hullFill = scaled(base, charK, 255);
        for (const auto& pc : shatter::slice(adef.visual, adef.visualTris, cuts))
            launch(pc, hullFill, skinCol, speed, 1.f, 1.f, 1.f);

        // ---- 2. The plates, torn off whole, AFTER the hull so they start on
        //         top of it exactly as they were drawn on the live ship ----
        // Smaller and lighter, so they go faster, spin harder, and are gone a
        // little sooner. Cold from the start: a plate came off at the bolts,
        // it was not cut.
        if (!crowded && cfg["wreck_plates"].get_or(true)) {
            for (const auto& pl : adef.plates) {
                const shatter::Piece pc = shatter::whole(pl.points, pl.tris);
                if (pc.tris.empty()) continue;
                const sf::Color plFill = scaled(base, std::max(0.f, pl.shade) * charK, 255);
                const sf::Color plSkin = pl.accent ? skinCol : scaled(liveEdge, 0.6f, 200);
                launch(pc, plFill, plSkin, plateSpeed, 1.5f, 0.85f, 0.f);
            }
        }
        return true;
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
        m_em->particles.push_back({ pos, shipVel * 0.3f,
            sf::Color(255, 245, 225, 255), 0.10f, 0.10f, 34.f });

        // 2. Tight, fast pressure ring. Small radius on purpose -- this is a
        //    point-blank death, not an area event like the magma rock.
        m_em->spawnShockRing(pos, 10.f, 125.f, 0.22f, sf::Color(255, 120, 60), 7.f, 255.f);

        // 3. Dense, dirty sparks thrown WITH the ship's momentum.
        for (int k = 0; k < 28; ++k) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float sp = 180.f + rand() % 340;
            const float life = 0.25f + (rand() % 30) / 100.f;
            m_em->particles.push_back({ pos,
                sf::Vector2f(std::cos(a), std::sin(a)) * sp + shipVel * 0.5f,
                sf::Color(255, static_cast<uint8_t>(110 + rand() % 120), 50, 235),
                life, life, 2.f + rand() % 4 });
        }

        // 4. The hull, in pieces. Real wreckage cut from the hull, thrown
        //    harder than a standard kill. The old triangle shards stay as the
        //    fallback for a unit that sets `wreckage = false`.
        const size_t triCount = adef.visualTris.size() / 3;
        const int maxShards = adef.config["death_shards"].get_or(7);
        if (!spawnWreckage(i, adef, WRECK_VISCERAL) && triCount > 0 && maxShards > 0) {
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
            m_em->particles.push_back({ pos,
                sf::Vector2f(std::cos(a), std::sin(a)) * (20.f + rand() % 40),
                sf::Color(70, 55, 50, 170), 0.9f, 0.9f, 7.f + rand() % 5 });
        }

        m_em->addTrauma(adef.config["death_trauma"].get_or(0.38f));
        m_em->requestHitstop(0.02f, 0.07f, 0.45f);
    }


    /**
     * @brief Cancel parry recovery and grant brief i-frames.
     *
     * Every successful BULLET or SHIP parry (and a mirrored lancer, and a
     * parried rocket) -- never asteroids. Until 1.10 this sat behind a
     * "perfect timing" test that in practice nearly always passed and that
     * no player could feel; the playtest read it as "bullet parries re-arm,
     * rock parries don't", which is exactly the rule, so now it IS the rule.
     *
     * Two separate effects, solving two separate problems:
     *   - Cancelling the animation lockout and cooldown fixes "I parried and
     *     then stood there spinning while the fight moved on."
     *   - The i-frames fix "I parried one bullet and the next one hit me
     *     anyway." Cancelling recovery alone does NOT fix that: the second
     *     shot was already in flight when the first connected, so there is no
     *     amount of reaction speed that covers it.
     *
     * Chained re-arms get progressively fewer i-frames so a parry-lock cannot
     * be held forever against sustained fire.
     */
    void onParryRearm(size_t playerIdx, sf::Vector2f at) {
        auto& ps = m_em->players[playerIdx];
        auto& hp = m_em->healths[playerIdx];

        ps.parryAnimTimer = 0.f;
        ps.parryWhiffRecovery = false;
        ps.parryWhiffTimer = 0.f;
        ps.parryCooldown = (*m_lua)["parry_rearm_cooldown"].get_or(
            (*m_lua)["parry_perfect_cooldown"].get_or(0.12f));

        const float base = (*m_lua)["parry_rearm_iframes"].get_or(
            (*m_lua)["parry_perfect_iframes"].get_or(0.22f));
        const float falloff = (*m_lua)["parry_rearm_chain_falloff"].get_or(
            (*m_lua)["parry_perfect_chain_falloff"].get_or(0.75f));
        const float grant = base * std::pow(falloff, static_cast<float>(ps.parryChain));

        hp.invulTimer = std::max(hp.invulTimer, grant);
        ps.parryChain++;
        ps.parryChainTimer = 0.30f;

        // A brighter version of the player's own parry paint, so a repaint
        // keeps the "that one re-armed" signal intact.
        m_em->spawnScreenFlash(lighten(parryColor(), 0.72f), 0.16f, 70.f);
        m_em->spawnShockRing(at, 10.f, 165.f, 0.30f,
            lighten(parryColor(), 0.55f), 4.f, 300.f);
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
        breakDive(idx);                      // ...nor mid-dive
        breakRangeAttack(idx);               // ...nor mid-lancer or mid-cone
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
