/**
 * @file RenderSystem.hpp
 * @brief Renders all game entities and visual effects
 *
 * CHANGED in 1.6 — ART DIRECTION CORRECTION
 *
 *  - CRATERS REMOVED. They simulated directional lighting (lit rim, shadowed
 *    wall, dark floor), which is a technique that assumes the surface reads
 *    lighter than the background. On dark grey rock over pure black, a crater
 *    floor lands closer to the background than to the rock, so it read as a
 *    hole punched through to space rather than as a dent. More fundamentally,
 *    faked 3D lighting fights this game's flat / hard-outline / neon-on-black
 *    language. Replaced with optional FACET lines, which are flat, angular and
 *    on-style. Default OFF — the minimal look is the better default.
 *
 *  - MAGMA VEINS now run from EVERY vertex, and pulse in sequence around the
 *    rock (a travelling wave) rather than all together. Cracks reach the rim
 *    on all sides, so the rock reads as fracturing under pressure.
 *
 *  - MAGMA CORE is now a hard-edged polygon whose size is Lua-driven and can
 *    be set to 0 to remove it. The old soft multi-layer bloom was a gradient,
 *    which is exactly what this art style doesn't use.
 *
 * CONTAINMENT (unchanged, still load-bearing):
 *   Asteroid polygons are star-shaped about their local origin, so the exact
 *   boundary distance at any angle comes from intersecting the ray with each
 *   edge. clampInside() uses that to guarantee no vein escapes the silhouette.
 *   A convex-hull argument is NOT sufficient — ~14% of generated polygons are
 *   non-convex at high jaggedness.
 *
 * CHANGED in 1.7 — archetype scars, hull thruster flame, bash crescent tell.
 *
 * CHANGED in 1.14 -- turret drawn from utils/TurretModel.hpp (same model,
 * same size as the dormant / dead gun; charge = colour, not size), the
 * shotgun wedge, the turret's search-light cone, summon hatches, mine decay
 * fade. drawWedge() shared by the Bloodseeker cone and the shotgun.
 *
 * CHANGED in 1.15 -- cone telegraphs (Bloodseeker cone, Barge shotgun) are
 * drawConeTell(): marching-ant outline, a faint wash, a pulsing warning sign
 * that swells and fades as the attack fires. Lancer charge (spike-to-maw
 * lines and core) and the lancer beams.
 *
 * @author Oleg Ivakhiv
 * @version 1.15 -- cone tells, lancer charge and beams
 */

#pragma once

#include "ISystem.hpp"
#include "utils/LuaConfig.hpp"
#include "core/EntityManager.hpp"
#include "core/EnemyArchetypes.hpp"        // added for archetype registry and geometry
#include "utils/FieldGeom.hpp"              // radial gradients for the field-object overlays
#include "utils/TurretModel.hpp"            // live turret == dormant / dead turret
#include <SFML/Graphics.hpp>
#include <unordered_map>
#include <vector>
#include <array>
#include <cmath>
#include <cstdlib>
#include <algorithm>

class RenderSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.drawTarget();
        m_lua = ctx.lua;
        m_playerEntityId = ctx.playerEntityId;
        m_enemyReg = ctx.enemyRegistry;      // store registry pointer
        m_magmaPulseTime = 0.f;
    }

    void update(float dt) override {
        if (!m_em || !m_window || !m_lua) return;

        size_t playerIdx = m_em->getEntityIndex(m_playerEntityId);
        if (playerIdx == (size_t)-1) return;

        auto& playerStats = m_em->players[playerIdx];
        m_enemyAnimTime += dt;
        m_chaos = 0.f;   // rebuilt from the enemies below; read by drawScreenSpace
        m_magmaPulseTime += dt;

        for (size_t i = 0; i < m_em->renders.size(); ++i) {
            auto& tf = m_em->transforms[i];
            auto& rd = m_em->renders[i];

            BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            BodyType type = ud ? ud->type : BodyType::Asteroid;

            sf::Vector2f drawPos = tf.position;
            float drawRot = tf.rotation;

            // ================================================================
            // MINES
            // ================================================================
            if (type == BodyType::Bullet && i < m_em->bullets.size() &&
                m_em->bullets[i].isMine) {
                drawMine(tf, rd, m_em->bullets[i]);
                continue;
            }

            // ================================================================
            // ASTEROIDS
            // ================================================================
            if (type == BodyType::Asteroid) {
                // The unstable core is explosive too, but it is a baked lab
                // model, not a burning rock: it takes the detail path below.
                // MAGMATIC has no field style and still lands here, unchanged.
                if (m_em->healths[i].isExplosive &&
                    rd.fieldStyle != RenderComponent::FieldStyle::UnstableCore) {
                    drawMagmatic(i, tf, rd, detailFor(i, tf.entityId));
                }
                else {
                    rd.shape.setPosition(tf.position);
                    rd.shape.setRotation(sf::degrees(tf.rotation));

                    // Damage flash, the same lerp-to-white enemies use. Set
                    // and restored around the draw so the stored colour --
                    // which the fracture path reads back for its shards --
                    // is never left whitened.
                    const float hf = m_em->healths[i].hitFlash;
                    const sf::Color baseFill = rd.shape.getFillColor();
                    const sf::Color baseLine = rd.shape.getOutlineColor();
                    if (hf > 0.f) {
                        const float w = std::clamp(hf / 0.16f, 0.f, 1.f);
                        const auto up = [&](std::uint8_t v) {
                            return static_cast<std::uint8_t>(v + (255 - v) * w); };
                        rd.shape.setFillColor(sf::Color(up(baseFill.r), up(baseFill.g),
                            up(baseFill.b), baseFill.a));
                        rd.shape.setOutlineColor(sf::Color(up(baseLine.r), up(baseLine.g),
                            up(baseLine.b), baseLine.a));
                    }

                    // Layered detail replaces everything else: plates,
                    // struts, gaps, craters and facets, each with its own
                    // colour already baked in at spawn. Transformed here
                    // rather than through RenderStates so the hit flash can
                    // tint every vertex on the way past.
                    if (!rd.detailTris.empty() || !rd.detailLines.empty()) {
                        const float rad = tf.rotation * 3.14159f / 180.f;
                        const float ca = std::cos(rad), sa = std::sin(rad);
                        const float w = (hf > 0.f) ? std::clamp(hf / 0.16f, 0.f, 1.f) : 0.f;

                        const auto emit = [&](const std::vector<sf::Vertex>& src,
                            sf::PrimitiveType prim)
                            {
                                if (src.empty()) return;
                                m_detailScratch.resize(src.size());
                                for (size_t k = 0; k < src.size(); ++k) {
                                    const sf::Vector2f& p = src[k].position;
                                    m_detailScratch[k].position = {
                                        tf.position.x + (p.x * ca - p.y * sa),
                                        tf.position.y + (p.x * sa + p.y * ca) };
                                    sf::Color c = src[k].color;
                                    if (w > 0.f) {
                                        c.r = static_cast<uint8_t>(c.r + (255 - c.r) * w);
                                        c.g = static_cast<uint8_t>(c.g + (255 - c.g) * w);
                                        c.b = static_cast<uint8_t>(c.b + (255 - c.b) * w);
                                    }
                                    m_detailScratch[k].color = c;
                                }
                                m_window->draw(m_detailScratch.data(), m_detailScratch.size(), prim);
                            };

                        emit(rd.detailTris, sf::PrimitiveType::Triangles);
                        emit(rd.detailLines, sf::PrimitiveType::Lines);

                        // Lab models: the live layer on top of the baked one.
                        if (rd.fieldStyle != RenderComponent::FieldStyle::None)
                            drawFieldFx(i, tf, rd, dt);

                        if (rd.shape.getOutlineThickness() > 0.01f) {
                            const sf::Color keep = rd.shape.getFillColor();
                            rd.shape.setFillColor(sf::Color::Transparent);
                            m_window->draw(rd.shape);
                            rd.shape.setFillColor(keep);
                        }
                        rd.shape.setFillColor(baseFill);
                        rd.shape.setOutlineColor(baseLine);
                        continue;
                    }

                    // A hull-shaped wreck fills from its own triangles: its
                    // outline is concave, and ConvexShape would fold it.
                    // `shape` still draws, for the outline stroke only.
                    if (!rd.tris.empty()) {
                        // Fill comes from the triangles; `shape` is only
                        // drawn when it actually has a stroke to contribute.
                        sf::Transform xf;
                        xf.translate(tf.position);
                        xf.rotate(sf::degrees(tf.rotation));

                        sf::VertexArray body(sf::PrimitiveType::Triangles, rd.tris.size());
                        const sf::Color fc = rd.shape.getFillColor();
                        for (size_t k = 0; k < rd.tris.size(); ++k)
                            body[k] = sf::Vertex{ rd.tris[k], fc };

                        sf::RenderStates st;
                        st.transform = xf;
                        m_window->draw(body, st);

                        if (rd.shape.getOutlineThickness() > 0.01f) {
                            const sf::Color keep = rd.shape.getFillColor();
                            rd.shape.setFillColor(sf::Color::Transparent);
                            m_window->draw(rd.shape);
                            rd.shape.setFillColor(keep);
                        }
                        rd.shape.setFillColor(baseFill);
                        rd.shape.setOutlineColor(baseLine);
                        continue;
                    }

                    m_window->draw(rd.shape);

                    // Optional flat faceting. Off by default; see drawFacets().
                    if (acfg("facets", 0.f) > 0.5f) {
                        drawFacets(rd.shape, detailFor(i, tf.entityId));
                    }
                    if (hf > 0.f) {
                        rd.shape.setFillColor(baseFill);
                        rd.shape.setOutlineColor(baseLine);
                    }
                }
                continue;
            }

            // ================================================================
            // PLAYER
            // ================================================================
            if (type == BodyType::Player) {

                // PROCEDURAL ANIMATION (written by ShipAnimSystem).
                // SFML's transform is translate(pos)*rotate(a)*translate(-origin),
                // so setPosition places the ORIGIN. Draw at the pivot's world
                // location to keep it anchored: centre + R(baseRot) * pivotLocal.
                const float baseRad = tf.rotation * 3.14159f / 180.f;
                const float cs = std::cos(baseRad);
                const float sn = std::sin(baseRad);

                const sf::Vector2f pivotWorld(
                    tf.position.x + (tf.visualPivot.x * cs - tf.visualPivot.y * sn),
                    tf.position.y + (tf.visualPivot.x * sn + tf.visualPivot.y * cs));

                rd.shape.setOrigin(tf.visualPivot);
                rd.shape.setScale(tf.visualScale);

                drawPos = pivotWorld;
                drawRot = tf.rotation + tf.visualOffsetAngle;

                // ---- Outline ----
                // Painted colours win; the Lua defaults remain the fallback for
                // a legacy ship that has no livery.
                const ship::Paint& paint = playerStats.livery.paint;
                if (playerStats.isParrying) {
                    float pulse = (std::sin(playerStats.parryTimer * 30.f) + 1.f) / 2.f;
                    rd.shape.setOutlineThickness(2.0f + pulse * 5.0f);
                    rd.shape.setOutlineColor(sf::Color(paint.parry.r, paint.parry.g, paint.parry.b,
                        200 + static_cast<uint8_t>(55 * pulse)));
                }
                else {
                    rd.shape.setOutlineThickness(2.5f);
                    rd.shape.setOutlineColor(paint.outline);
                }

                // ---- Fill, in priority order ----
                const float heatT = playerStats.weaponHeat /
                    std::max(1.f, playerStats.maxWeaponHeat);

                const float br = paint.hull.r, bg = paint.hull.g, bb = paint.hull.b;
                // Painted alpha: a see-through hull (the refit bay floors it,
                // and the outline stays opaque, so the silhouette still reads).
                const float ba = paint.hull.a;

                if (playerStats.parryFlashTimer > 0.f) {
                    const float w = std::clamp(playerStats.parryFlashTimer / 0.25f, 0.f, 1.f);
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(br + (255.f - br) * w),
                        static_cast<uint8_t>(bg + (255.f - bg) * w),
                        static_cast<uint8_t>(bb + (255.f - bb) * w),
                        static_cast<uint8_t>(ba + (255.f - ba) * w)));   // the flash goes solid
                    rd.shape.setOutlineThickness(2.5f + 6.f * w);
                    rd.shape.setOutlineColor(sf::Color(255, 255, 255,
                        static_cast<uint8_t>(255 * w)));
                }
                else if (playerStats.staggerTimer > 0.f || playerStats.staggerRecoverTimer > 0.f) {
                    const float flicker = 0.6f + 0.4f * std::sin(m_magmaPulseTime * 40.f);
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(140 * flicker),
                        static_cast<uint8_t>(60 * flicker),
                        static_cast<uint8_t>(60 * flicker),
                        static_cast<uint8_t>(ba)));
                }
                else if (playerStats.dashCooldown > (playerStats.dashMaxCooldown - 0.15f)) {
                    rd.shape.setFillColor(sf::Color(paint.dodge.r, paint.dodge.g, paint.dodge.b, 210));
                }
                else {
                    const float k = heatT * heatT * 0.55f;
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(br + (200.f - br) * k),
                        static_cast<uint8_t>(bg + (90.f - bg) * k),
                        static_cast<uint8_t>(bb + (60.f - bb) * k),
                        static_cast<uint8_t>(ba)));
                }

                // ---- Parry arcs ----
                if (playerStats.parryAnimTimer > 0) {
                    float parryAnimDuration = (*m_lua)["parry_anim_duration"].get_or(0.6f);
                    float t = playerStats.parryAnimTimer / parryAnimDuration;
                    uint8_t alpha = static_cast<uint8_t>(t * 200);
                    sf::Color arcCol(paint.parry.r, paint.parry.g, paint.parry.b, alpha);

                    auto drawArc = [&](float centerAngleDeg) {
                        int segments = 18;
                        sf::VertexArray arc(sf::PrimitiveType::LineStrip, segments + 1);
                        for (int s = 0; s <= segments; ++s) {
                            float deg = (centerAngleDeg - 70.f) + 140.f * s / segments;
                            float rad = deg * 3.14159f / 180.f;
                            arc[s].position = { tf.position.x + std::cos(rad) * 55.f,
                                                tf.position.y + std::sin(rad) * 55.f };
                            arc[s].color = arcCol;
                        }
                        m_window->draw(arc);
                        };
                    drawArc(tf.rotation - 90.f);
                    drawArc(tf.rotation + 90.f);
                }

                rd.shape.setPosition(drawPos);
                rd.shape.setRotation(sf::degrees(drawRot));
                drawLivery(rd.shape, playerStats, false);   // decals under the hull
                if (!playerStats.modelTris.empty()) drawPlayerModel(rd.shape, playerStats);
                else                                m_window->draw(rd.shape);
                drawLivery(rd.shape, playerStats, true);    // decals over it, then the canopy

                drawNoseHeat(rd.shape, playerStats, heatT);
                continue;
            }

            // ================================================================
            // ENEMY – new archetype-based rendering
            // ================================================================
            if (type == BodyType::Enemy) {
                auto& ec = m_em->enemies[i];

                // Resolve the archetype once for this enemy
                const enemyarch::ArchetypeDef& adef = m_enemyReg->resolve(ec.archetype);

                // ====================================================================
                // 0. AMBUSH -- the powered-down disguise
                // ====================================================================
                // Dormant: drawn EXACTLY like a wreck -- the baked tier-0 hull,
                // the wreck's own hit flash, and nothing else. No flame, no
                // turret, no cone, no icon: anything drawn here that a wreck
                // does not draw would give it away.
                //
                // Rebooting: a hard flicker between the cold hull and the live
                // one. The lit share of each 14 Hz beat grows from 0 to 1 over
                // the reboot, so it reads as systems catching, not as a fade.
                bool cold = ec.dormant;
                if (!cold && ec.wakeTimer > 0.f && ec.wakeDuration > 0.f) {
                    const float lit = 1.f - ec.wakeTimer / ec.wakeDuration;
                    cold = std::fmod(m_enemyAnimTime * 14.f, 1.f) >= lit;
                }
                if (cold && !rd.detailTris.empty()) {
                    drawBakedDetail(tf, rd.detailTris, m_em->healths[i].hitFlash);
                    if (!ec.dormant && ec.alertIconTimer > 0.f && ec.alertIconDuration > 0.f)
                        drawAlertIcon(tf.position, ec);
                    continue;
                }

                // ====================================================================
                // 1. STATE COLOUR (fill)
                // ====================================================================
                sf::Color fill;
                switch (ec.visualState) {
                case EnemyState::PATROL:
                    fill = sf::Color(
                        static_cast<uint8_t>(std::clamp(adef.color.r * 0.52f + 30.f, 0.f, 255.f)),
                        static_cast<uint8_t>(std::clamp(adef.color.g * 0.52f + 34.f, 0.f, 255.f)),
                        static_cast<uint8_t>(std::clamp(adef.color.b * 0.52f + 38.f, 0.f, 255.f)));
                    break;
                case EnemyState::ALERT: {
                    const float p = 0.5f + 0.5f * std::sin(m_enemyAnimTime * 5.0f);
                    fill = sf::Color(
                        static_cast<uint8_t>(std::clamp(228.f + 27.f * p, 0.f, 255.f)),
                        static_cast<uint8_t>(std::clamp(150.f + 40.f * p, 0.f, 255.f)),
                        40);
                    break;
                }
                case EnemyState::COMBAT:
                default:
                    fill = sf::Color(
                        static_cast<uint8_t>(adef.color.r),
                        static_cast<uint8_t>(adef.color.g),
                        static_cast<uint8_t>(adef.color.b));
                    break;
                }

                // Damage flash
                if (ec.hitFlashTimer > 0.f) {
                    const float w = std::clamp(ec.hitFlashTimer / 0.16f, 0.f, 1.f);
                    fill = sf::Color(
                        static_cast<uint8_t>(fill.r + (255 - fill.r) * w),
                        static_cast<uint8_t>(fill.g + (255 - fill.g) * w),
                        static_cast<uint8_t>(fill.b + (255 - fill.b) * w));
                }

                // Stagger / stun
                const bool broken = (ec.staggerTimer > 0.f || ec.staggerRecoverTimer > 0.f ||
                    m_em->healths[i].stunTimer > 0.f);
                if (broken) {
                    const float f = 0.55f + 0.45f * std::sin(m_enemyAnimTime * 34.f);
                    fill = sf::Color(
                        static_cast<uint8_t>(fill.r * 0.45f * f + 40),
                        static_cast<uint8_t>(fill.g * 0.45f * f + 20),
                        static_cast<uint8_t>(fill.b * 0.45f * f + 20));
                }

                // ---- Ram charge overrides everything ----
                if (ec.ramState == RamState::Charge) {
                    fill = sf::Color(255, 245, 215);
                }

                // ====================================================================
                // 2. OUTLINE — carries the telegraph
                // ====================================================================
                float     outlineWidth = 1.6f;
                sf::Color outlineColor(std::min(255, fill.r + 60),
                    std::min(255, fill.g + 50),
                    std::min(255, fill.b + 50), 200);

                if (ec.telegraphActive && ec.telegraphDuration > 0.f) {
                    const float u = 1.f - (ec.telegraphTimer / ec.telegraphDuration);
                    outlineWidth = 1.6f + 5.0f * u;
                    outlineColor = sf::Color(255,
                        static_cast<uint8_t>(240 - 140 * u),
                        static_cast<uint8_t>(200 - 180 * u),
                        static_cast<uint8_t>(180 + 75 * u));
                }
                else if (ec.stormActive) {
                    const float p = 0.5f + 0.5f * std::sin(m_enemyAnimTime * 30.f);
                    outlineWidth = 2.0f + 3.0f * p;
                    outlineColor = sf::Color(255, 200, 80, 240);
                }

                // ---- Bloodseeker RANGE kit: the hull carries the beat too ----
                // Lancer: red, thickening through the charge, then bone-white
                // at the lock -- the same switch the lane makes, so either
                // one alone is enough. Cone: red, pulsing faster as the windup
                // completes, then steady while he is rooted and firing.
                if (ec.duelAttack != DuelAttack::None && ec.duelAtkDuration > 0.f) {
                    const float u = std::clamp(1.f - ec.duelAtkTimer / ec.duelAtkDuration, 0.f, 1.f);
                    switch (ec.duelAttack) {
                    case DuelAttack::LancerCharge:
                        outlineWidth = 1.8f + 3.0f * u;
                        outlineColor = sf::Color(255, 90, 60, static_cast<uint8_t>(170 + 70 * u));
                        break;
                    case DuelAttack::LancerLock:
                        outlineWidth = 5.2f;
                        outlineColor = sf::Color(245, 238, 222, 255);
                        break;
                    case DuelAttack::ConeWindup: {
                        const float p = (std::fmod(m_enemyAnimTime * (4.f + 10.f * u), 1.f) < 0.5f) ? 1.f : 0.f;
                        outlineWidth = 2.0f + 3.0f * u * p;
                        outlineColor = sf::Color(255, 90, 60, static_cast<uint8_t>(160 + 90 * p));
                        break;
                    }
                    case DuelAttack::ConeFire:
                        outlineWidth = 4.0f;
                        outlineColor = sf::Color(255, 120, 70, 245);
                        break;
                    default:
                        break;
                    }
                }

                // ---- Bash: the parry colour, on the hull itself ----
                // Evaluated last so it always wins. A bash must never be
                // mistaken for anything else, least of all the ram.
                // ====================================================================
                // FRENZY COLOUR (Maniac) — overrides the state colour entirely
                // ====================================================================
                // He is no longer in a combat state worth reading; he is a
                // fuse. Ignite BLINKS (square wave, accelerating). Charge and
                // Thrown PULSE (smooth, regular). Two beats for two meanings --
                // "something broke" versus "this is counting down" -- carried
                // on colour as well as on shape, so neither read depends on
                // the other surviving a busy frame.
                if (ec.frenzy > 0.f) {
                    // One beat, and the AI sets its rate from range: slow far
                    // away, frantic in your face. Same language as a lit mine
                    // on purpose -- both are fuses, and the player should not
                    // have to learn two vocabularies for "this is about to go
                    // off".
                    // A SQUARE WAVE, not a soft tint. The previous version
                    // faded between two reds a shade apart -- correct in
                    // principle, invisible in practice at ship size against a
                    // dark background. This flips hard between deep red and a
                    // near-white flash, exactly like a lit mine, and the rate
                    // comes from range so it winds up as he closes.
                    const float hz = std::max(1.f, ec.frenzyBlinkHz);
                    const float phase = std::fmod(m_enemyAnimTime * hz, 1.f);
                    const float on = (phase < 0.42f) ? 1.f : 0.f;
                    const float w = std::clamp(ec.frenzy, 0.f, 1.f);

                    const sf::Color dark(150, 28, 24);
                    const sf::Color flash(255, 225, 205);
                    const sf::Color hot = (on > 0.5f) ? flash : dark;

                    fill = sf::Color(
                        static_cast<uint8_t>(fill.r + (hot.r - fill.r) * w),
                        static_cast<uint8_t>(fill.g + (hot.g - fill.g) * w),
                        static_cast<uint8_t>(fill.b + (hot.b - fill.b) * w));
                    outlineWidth = std::max(outlineWidth, 2.2f + 4.0f * w * on);
                    outlineColor = (on > 0.5f)
                        ? sf::Color(255, 255, 235, 255)
                        : sf::Color(255, 70, 45, static_cast<uint8_t>(150 + 70 * w));
                }

                float bashU = -1.f;   // <0 = no bash tell this frame
                sf::Color bashCol(90, 255, 230);
                if (ec.bashState == BashState::Windup || ec.bashState == BashState::Lunge) {
                    bashU = (ec.bashState == BashState::Lunge) ? 1.f
                        : std::clamp(1.f - ec.bashTimer / std::max(0.01f, ec.bashDuration), 0.f, 1.f);

                    sol::object bc = adef.config["bash_tell_color"];
                    if (bc.valid() && bc.is<sol::table>()) {
                        sol::table t = bc.as<sol::table>();
                        bashCol = sf::Color(
                            static_cast<uint8_t>(std::clamp(t["r"].get_or(90.f), 0.f, 255.f)),
                            static_cast<uint8_t>(std::clamp(t["g"].get_or(255.f), 0.f, 255.f)),
                            static_cast<uint8_t>(std::clamp(t["b"].get_or(230.f), 0.f, 255.f)));
                    }
                    const float w = bashU * bashU;
                    outlineWidth = 1.8f + 4.5f * w;
                    outlineColor = sf::Color(
                        static_cast<uint8_t>(bashCol.r + (255 - bashCol.r) * w * 0.6f),
                        static_cast<uint8_t>(bashCol.g + (255 - bashCol.g) * w * 0.6f),
                        static_cast<uint8_t>(bashCol.b + (255 - bashCol.b) * w * 0.6f),
                        static_cast<uint8_t>(170 + 85 * bashU));
                }

                // ====================================================================
                // 3. DRAW THE SHIP with animation offsets
                //    Same pivot maths as the player.
                // ====================================================================
                const float baseRad = tf.rotation * 3.14159f / 180.f;
                const float ecs = std::cos(baseRad), esn = std::sin(baseRad);
                const sf::Vector2f pivotWorld(
                    tf.position.x + (tf.visualPivot.x * ecs - tf.visualPivot.y * esn),
                    tf.position.y + (tf.visualPivot.x * esn + tf.visualPivot.y * ecs));

                // Matches Transformable's order: T(pos) * R * S * T(-origin)
                sf::Transform xf;
                xf.translate(pivotWorld);
                xf.rotate(sf::degrees(tf.rotation + tf.visualOffsetAngle));
                xf.scale(tf.visualScale);
                xf.translate(-tf.visualPivot);

                sf::RenderStates states;
                states.transform = xf;

                // ---- Thruster flame (under the hull, so its root is hidden) ----
                if (adef.exhaust.glow > 0.f) drawThrusterFlame(i, ec, adef, states);

                // ---- Fill ----
                if (!adef.visualTris.empty()) {
                    sf::VertexArray body(sf::PrimitiveType::Triangles, adef.visualTris.size());
                    for (size_t k = 0; k < adef.visualTris.size(); ++k)
                        body[k] = sf::Vertex{ adef.visualTris[k], fill };
                    m_window->draw(body, states);
                }

                // ---- Armour plates ----
                // The whole set goes into ONE vertex array: they share the
                // ship's transform, and colour is per-vertex, so a patchwork
                // of differently shaded panels still costs a single draw.
                if (!adef.plates.empty()) {
                    size_t n = 0;
                    for (const auto& pl : adef.plates) n += pl.tris.size();
                    if (n) {
                        sf::VertexArray panels(sf::PrimitiveType::Triangles, n);
                        size_t w = 0;
                        for (const auto& pl : adef.plates) {
                            // Shade the LIVE fill, so plates flash, glow and
                            // ramp with the hull instead of sitting inert
                            // through every colour effect the unit has.
                            sf::Color pc(
                                static_cast<uint8_t>(std::clamp(fill.r * pl.shade, 0.f, 255.f)),
                                static_cast<uint8_t>(std::clamp(fill.g * pl.shade, 0.f, 255.f)),
                                static_cast<uint8_t>(std::clamp(fill.b * pl.shade, 0.f, 255.f)),
                                fill.a);
                            // Trim (elite gold / bone): mixed in AFTER the
                            // shade, so the flash and stagger still move it.
                            if (pl.tintMix > 0.f) {
                                const float m = pl.tintMix;
                                pc.r = static_cast<uint8_t>(pc.r + (pl.tint.r - pc.r) * m);
                                pc.g = static_cast<uint8_t>(pc.g + (pl.tint.g - pc.g) * m);
                                pc.b = static_cast<uint8_t>(pc.b + (pl.tint.b - pc.b) * m);
                            }
                            for (const auto& v : pl.tris) panels[w++] = sf::Vertex{ v, pc };
                        }
                        m_window->draw(panels, states);
                    }

                    // ---- Elite trim: hard edges on the accent plates ----
                    // Strips are built once at load (plate_edge_width > 0);
                    // here they only get coloured. The trim follows the hit
                    // flash like everything else on the hull.
                    if (adef.plateEdgeWidth > 0.f) {
                        sf::Color ec2 = adef.plateEdge;
                        if (ec.hitFlashTimer > 0.f) {
                            const float w = std::clamp(ec.hitFlashTimer / 0.16f, 0.f, 1.f);
                            ec2.r = static_cast<uint8_t>(ec2.r + (255 - ec2.r) * w);
                            ec2.g = static_cast<uint8_t>(ec2.g + (255 - ec2.g) * w);
                            ec2.b = static_cast<uint8_t>(ec2.b + (255 - ec2.b) * w);
                        }
                        for (const auto& pl : adef.plates) {
                            if (pl.edgeStrip.size() < 4) continue;
                            sf::VertexArray e(sf::PrimitiveType::TriangleStrip, pl.edgeStrip.size());
                            for (size_t k = 0; k < pl.edgeStrip.size(); ++k)
                                e[k] = sf::Vertex{ pl.edgeStrip[k], ec2 };
                            m_window->draw(e, states);
                        }
                    }
                }

                // ---- Scars (in the hull's frame: they bank and squash with it) ----
                if (!adef.decalSegs.empty()) {
                    const sf::Color scar(
                        static_cast<uint8_t>(fill.r * 0.42f),
                        static_cast<uint8_t>(fill.g * 0.42f),
                        static_cast<uint8_t>(fill.b * 0.42f), 235);
                    drawSegments(adef.decalSegs, adef.config["scar_width"].get_or(1.6f),
                        scar, states);
                }

                // ---- Outline ----
                m_outlineScratch = enemyarch::geom::outlineStrip(adef.visual, outlineWidth);
                if (m_outlineScratch.size() >= 4) {
                    sf::VertexArray edge(sf::PrimitiveType::TriangleStrip,
                        m_outlineScratch.size());
                    for (size_t k = 0; k < m_outlineScratch.size(); ++k)
                        edge[k] = sf::Vertex{ m_outlineScratch[k], outlineColor };
                    m_window->draw(edge, states);
                }

                // ---- Mode lamp (Bloodseeker) -- last, so nothing covers it ----
                if (!adef.lampTris.empty())
                    drawModeLamp(ec, adef, states, m_em->healths[i].stunTimer > 0.f);

                // ====================================================================
                // 3b. TURRET — drawn in its OWN frame, not the hull's
                // ====================================================================
                for (size_t mi = 0; mi < adef.turrets.size(); ++mi) {
                    const sf::Vector2f local = adef.turrets[mi];
                    const float hr = tf.rotation * 3.14159f / 180.f;
                    const sf::Vector2f mountPos(
                        tf.position.x + (local.x * std::cos(hr) - local.y * std::sin(hr)),
                        tf.position.y + (local.x * std::sin(hr) + local.y * std::cos(hr)));

                    sf::Transform txf;
                    txf.translate(mountPos);
                    txf.rotate(sf::degrees(ec.turretAngle));

                    sf::RenderStates tst;
                    tst.transform = txf;

                    const float ts = adef.config["turret_size"].get_or(10.f);

                    // Wind-up heat on the barrel: the shot tell.
                    float heat = 0.f;
                    if (ec.turretTelegraphActive && ec.turretTelegraphDuration > 0.f)
                        heat = 1.f - (ec.turretTelegraphTimer / ec.turretTelegraphDuration);
                    if (ec.turretMuzzleFlash > 0.f) heat = 1.f;

                    // Shotgun windup heats the gun the same way, on its own clock.
                    if (ec.shotgunState != 0 && ec.shotgunDuration > 0.f)
                        heat = std::max(heat, 1.f - ec.shotgunTimer / ec.shotgunDuration);
                    if (ec.shotgunFlash > 0.f) heat = 1.f;

                    // ---- The model: utils/TurretModel.hpp ----
                    // The same polygons a dormant or dead hull bakes cold, at
                    // the same size, so nothing jumps when it wakes or dies.
                    // Geometry never changes with the charge; colour carries
                    // it: barrels heat, the lane down the housing fills from
                    // the breech forward, a glow sits on each muzzle.
                    const auto shade = [&](float k) {
                        return sf::Color(
                            static_cast<uint8_t>(std::clamp(fill.r * k, 0.f, 255.f)),
                            static_cast<uint8_t>(std::clamp(fill.g * k, 0.f, 255.f)),
                            static_cast<uint8_t>(std::clamp(fill.b * k, 0.f, 255.f)));
                        };
                    const auto mix = [](sf::Color c0, sf::Color c1, float t) {
                        t = std::clamp(t, 0.f, 1.f);
                        return sf::Color(
                            static_cast<uint8_t>(c0.r + (c1.r - c0.r) * t),
                            static_cast<uint8_t>(c0.g + (c1.g - c0.g) * t),
                            static_cast<uint8_t>(c0.b + (c1.b - c0.b) * t));
                        };
                    const sf::Color hot(255, static_cast<uint8_t>(150 - 70 * heat), 60);
                    const sf::Color edgeCol = outlineColor;
                    const auto poly = [&](const std::vector<sf::Vector2f>& p, sf::Color c, bool edge) {
                        sf::VertexArray va(sf::PrimitiveType::TriangleFan, p.size());
                        for (size_t k = 0; k < p.size(); ++k) va[k] = sf::Vertex{ p[k], c };
                        m_window->draw(va, tst);
                        if (!edge) return;
                        sf::VertexArray ln(sf::PrimitiveType::LineStrip, p.size() + 1);
                        for (size_t k = 0; k <= p.size(); ++k) ln[k] = sf::Vertex{ p[k % p.size()], edgeCol };
                        m_window->draw(ln, tst);
                        };
                    using turretmodel::Part;
                    for (const auto& part : turretmodel::parts(ts)) {
                        switch (part.part) {
                        case Part::Rear:    poly(part.pts, shade(0.55f), true); break;
                        case Part::Barrel:  poly(part.pts, mix(shade(0.75f), hot, heat * 0.85f), true); break;
                        case Part::Brake:   poly(part.pts, mix(shade(0.62f), hot, heat), true); break;
                        case Part::Mantlet: poly(part.pts, shade(0.68f), true); break;
                        case Part::Housing: poly(part.pts, shade(1.18f), true); break;
                        case Part::Lane:
                            poly(part.pts, sf::Color(26, 14, 12), false);
                            if (heat > 0.01f) poly(turretmodel::laneFill(ts, heat), hot, false);
                            break;
                        }
                    }

                    // Muzzle glow: grows with the charge, flares on the shot.
                    if (heat > 0.05f) {
                        const float g = ts * (0.10f + 0.22f * heat);
                        for (const auto& mz : turretmodel::muzzles(ts)) {
                            sf::VertexArray d(sf::PrimitiveType::TriangleFan, 4);
                            const sf::Color gc(255, static_cast<uint8_t>(210 - 90 * heat), 120,
                                static_cast<uint8_t>(120 + 135 * heat));
                            d[0] = sf::Vertex{ { mz.x, mz.y - g }, gc };
                            d[1] = sf::Vertex{ { mz.x + g, mz.y }, gc };
                            d[2] = sf::Vertex{ { mz.x, mz.y + g }, gc };
                            d[3] = sf::Vertex{ { mz.x - g, mz.y }, gc };
                            m_window->draw(d, tst);
                        }
                    }

                    // Muzzle flash, both barrels.
                    if (ec.turretMuzzleFlash > 0.f) {
                        const float u = std::clamp(ec.turretMuzzleFlash / 0.11f, 0.f, 1.f);
                        const sf::Color fc(255, 230, 150, static_cast<uint8_t>(230 * u));
                        for (const auto& mz : turretmodel::muzzles(ts)) {
                            sf::VertexArray fl(sf::PrimitiveType::Triangles, 3);
                            fl[0] = sf::Vertex{ { mz.x - ts * 0.22f, mz.y }, fc };
                            fl[1] = sf::Vertex{ { mz.x + ts * 0.22f, mz.y }, fc };
                            fl[2] = sf::Vertex{ { mz.x, mz.y - ts * 1.6f * u }, sf::Color(255, 200, 90, 0) };
                            m_window->draw(fl, tst);
                        }
                    }

                    // ---- SHOTGUN wedge: exactly the cone that will land ----
                    // Tracking: thin red edges, fill creeping in. Locked: hard
                    // edges, fuller fill -- the last beat to get out. Fired:
                    // a flash of the whole wedge.
                    if (ec.shotgunState != 0 || ec.shotgunFlash > 0.f) {
                        const float R = adef.config["shotgun_range"].get_or(320.f);
                        const float half = adef.config["shotgun_half_angle"].get_or(30.f) * 3.14159f / 180.f;
                        const float ar = ec.turretAngle * 3.14159f / 180.f;
                        const sf::Vector2f dir(std::sin(ar), -std::cos(ar));
                        const sf::Vector2f apex = mountPos + dir * (turretmodel::kMuzzle * ts);
                        const float base = std::atan2(dir.y, dir.x);
                        if (ec.shotgunFlash > 0.f) {
                            drawConeTell(apex, base, half, R, 1.f, true,
                                std::clamp(ec.shotgunFlash / 0.30f, 0.f, 1.f) * 0.999f);
                        }
                        else {
                            const float u = 1.f - ec.shotgunTimer / std::max(0.01f, ec.shotgunDuration);
                            drawConeTell(apex, base, half, R, u, ec.shotgunState == 2, 1.f);
                        }
                    }

                    // ---- Turret eye: a faint searchlight while it hunts ----
                    if (ec.visualState != EnemyState::COMBAT) {
                        const float vr = adef.config["turret_vision_range"].get_or(0.f);
                        if (vr > 0.f) {
                            float vh = adef.config["turret_vision_fov"].get_or(60.f) * 0.5f;
                            if (ec.visualState == EnemyState::ALERT)
                                vh *= adef.config["vision_fov_alert_mult"].get_or(1.45f);
                            const float ar = (ec.turretAngle - 90.f) * 3.14159f / 180.f;
                            const float h = std::min(vh, 175.f) * 3.14159f / 180.f;
                            const uint8_t a0 = ec.visualState == EnemyState::ALERT ? 70 : 38;
                            const sf::Color c0(255, 190, 60, a0), c1(255, 190, 60, 0);
                            sf::VertexArray cone(sf::PrimitiveType::Lines, 4);
                            cone[0] = sf::Vertex{ mountPos, c0 };
                            cone[1] = sf::Vertex{ mountPos + sf::Vector2f(std::cos(ar - h), std::sin(ar - h)) * (vr * 0.55f), c1 };
                            cone[2] = sf::Vertex{ mountPos, c0 };
                            cone[3] = sf::Vertex{ mountPos + sf::Vector2f(std::cos(ar + h), std::sin(ar + h)) * (vr * 0.55f), c1 };
                            m_window->draw(cone);
                        }
                    }
                }

                // ====================================================================
                // 3b-2. SUMMON -- the sponson hatches glow while the hull charges
                // ====================================================================
                if (ec.summonState == 1 && ec.summonDuration > 0.f) {
                    const float u = 1.f - ec.summonTimer / ec.summonDuration;
                    const float sc = adef.config["scale"].get_or(1.f);
                    float hx = 30.f, hy = 4.f;
                    if (sol::optional<sol::table> h = adef.config["summon_hatch"]) {
                        hx = (*h)["x"].get_or(hx);
                        hy = (*h)["y"].get_or(hy);
                    }
                    const float flick = 0.8f + 0.2f * std::sin(m_enemyAnimTime * (20.f + 30.f * u));
                    const sf::Color hc(255, static_cast<uint8_t>(110 + 100 * u), 60,
                        static_cast<uint8_t>((90 + 165 * u) * flick));
                    for (float side : { -1.f, 1.f }) {
                        // A slot along the sponson, in the hull frame: a seam
                        // that cracks open wider as the charge fills.
                        const float x = side * hx * sc, y = hy * sc;
                        const float w = (1.2f + 2.6f * u) * sc, l = 11.f * sc;
                        sf::VertexArray q(sf::PrimitiveType::TriangleFan, 4);
                        q[0] = sf::Vertex{ { x - w, y - l }, hc };
                        q[1] = sf::Vertex{ { x + w, y - l }, hc };
                        q[2] = sf::Vertex{ { x + w, y + l }, hc };
                        q[3] = sf::Vertex{ { x - w, y + l }, hc };
                        m_window->draw(q, states);
                    }
                }

                // ====================================================================
                // 3c. RAM WAKE (fades out after charge)
                // ====================================================================
                if (ec.ramTrailFade > 0.f && ec.ramTrailCount >= 2) {
                    const int n = ec.ramTrailCount;
                    sf::VertexArray wake(sf::PrimitiveType::TriangleStrip, n * 2);
                    const float headW = adef.radius * 0.5f;

                    for (int k = 0; k < n; ++k) {
                        // Segment direction, from the neighbouring samples.
                        const sf::Vector2f a = ec.ramTrail[std::max(0, k - 1)];
                        const sf::Vector2f b = ec.ramTrail[std::min(n - 1, k + 1)];
                        sf::Vector2f d = b - a;
                        const float l = std::sqrt(d.x * d.x + d.y * d.y);
                        if (l > 0.01f) { d.x /= l; d.y /= l; }
                        else { d = { 0.f, -1.f }; }
                        const sf::Vector2f perp(-d.y, d.x);

                        const float t = 1.f - static_cast<float>(k) / (n - 1);  // 1 -> 0
                        const float w = headW * t;
                        const sf::Color c(255,
                            static_cast<uint8_t>(150 + 70 * t),
                            static_cast<uint8_t>(70 + 60 * t),
                            static_cast<uint8_t>(210 * t * t * ec.ramTrailFade));

                        wake[k * 2] = sf::Vertex{ ec.ramTrail[k] - perp * w, c };
                        wake[k * 2 + 1] = sf::Vertex{ ec.ramTrail[k] + perp * w, c };
                    }
                    m_window->draw(wake);
                }

                // ====================================================================
                // 3d. RAM WIND-UP GLOW (only during windup)
                // ====================================================================
                if (ec.ramState == RamState::Windup && ec.ramGlow > 0.f) {
                    const float g = ec.ramGlow;
                    const float r = tf.rotation * 3.14159f / 180.f;
                    const sf::Vector2f fwd(std::sin(r), -std::cos(r));
                    const sf::Vector2f rgt(std::cos(r), std::sin(r));
                    const sf::Vector2f nose = tf.position + fwd * (adef.radius * 0.95f);

                    const float w = 18.f + 30.f * g;
                    sf::VertexArray bar(sf::PrimitiveType::TriangleStrip, 4);
                    const sf::Color hot(255, static_cast<uint8_t>(220 - 120 * g), 80,
                        static_cast<uint8_t>(120 + 135 * g));
                    bar[0] = sf::Vertex{ nose - rgt * w,                    hot };
                    bar[1] = sf::Vertex{ nose + rgt * w,                    hot };
                    bar[2] = sf::Vertex{ nose - rgt * w * 0.5f + fwd * 26.f * g,
                                         sf::Color(255, 240, 180, 0) };
                    bar[3] = sf::Vertex{ nose + rgt * w * 0.5f + fwd * 26.f * g,
                                         sf::Color(255, 240, 180, 0) };
                    m_window->draw(bar);

                    // Aim line: shows the committed lane, not just that
                    // something is coming.
                    sf::VertexArray lane(sf::PrimitiveType::Lines, 2);
                    lane[0] = sf::Vertex{ nose, sf::Color(255, 140, 60,
                                          static_cast<uint8_t>(40 + 120 * g)) };
                    lane[1] = sf::Vertex{ nose + fwd * (300.f + 500.f * g),
                                          sf::Color(255, 140, 60, 0) };
                    m_window->draw(lane);
                }

                // ====================================================================
                // 3d-bis. FRENZY CORONA
                // ====================================================================
                if (ec.frenzy > 0.f) drawFrenzyCorona(tf, ec, adef);

                // ====================================================================
                // 3e. BASH CRESCENT (windup + lunge)
                // ====================================================================
                // Short and CURVED where the ram's tell is long and STRAIGHT;
                // cyan where the ram's is amber; hugging the prow where the
                // ram's reaches across the arena. Three independent channels
                // saying the same thing, so one lost in a busy frame still
                // leaves two.
                if (bashU >= 0.f) drawBashCrescent(tf, adef, bashU, bashCol,
                    ec.bashState == BashState::Lunge);

                // ---- Bloodseeker execution: the same crescent, in GOLD ----
                // Gold, not cyan: it is aimed at one of his own, not at you.
                // Nothing to parry -- the read is "he has turned his back".
                if (ec.duelShift == DuelShift::ExecStrike && ec.duelShiftDuration > 0.f) {
                    const float u = std::clamp(1.f - ec.duelShiftTimer / ec.duelShiftDuration, 0.f, 1.f);
                    drawBashCrescent(tf, adef, u, sf::Color(214, 172, 92), false);
                }

                // ====================================================================
                // 4. TELEGRAPH AIM LINE
                // ====================================================================
                if (ec.telegraphActive && ec.telegraphDuration > 0.f) {
                    const float u = 1.f - (ec.telegraphTimer / ec.telegraphDuration);
                    const float len = 60.f + 190.f * u;
                    const uint8_t a = static_cast<uint8_t>(60 + 120 * u);

                    sf::VertexArray beam(sf::PrimitiveType::Lines, 2);
                    beam[0] = sf::Vertex{ tf.position + ec.telegraphDir * 26.f,
                                          sf::Color(255, 90, 60, a) };
                    beam[1] = sf::Vertex{ tf.position + ec.telegraphDir * len,
                                          sf::Color(255, 90, 60, 0) };
                    m_window->draw(beam);
                }

                // ---- 4b. Bloodseeker lancer lane / cone wedge ----
                if (ec.duelAttack != DuelAttack::None) drawDuelAttack(tf, ec, adef);


                // ====================================================================
                // 5. VISION CONE (ALERT only) — uses adef.config
                // ====================================================================
                if (ec.visualState == EnemyState::ALERT) {
                    sol::table cfg = adef.config;
                    const float range = cfg["vision_range"].get_or(620.f) * 0.55f;
                    const float half = cfg["vision_fov"].get_or(110.f) * 0.5f *
                        cfg["vision_fov_alert_mult"].get_or(1.45f);

                    const float facing = (tf.rotation - 90.f) * 3.14159f / 180.f;
                    const float h = std::min(half, 175.f) * 3.14159f / 180.f;

                    sf::VertexArray cone(sf::PrimitiveType::Lines, 4);
                    const sf::Color c0(255, 190, 60, 55), c1(255, 190, 60, 0);

                    cone[0] = sf::Vertex{ tf.position, c0 };
                    cone[1] = sf::Vertex{ tf.position + sf::Vector2f(std::cos(facing - h),
                                                                      std::sin(facing - h)) * range, c1 };
                    cone[2] = sf::Vertex{ tf.position, c0 };
                    cone[3] = sf::Vertex{ tf.position + sf::Vector2f(std::cos(facing + h),
                                                                      std::sin(facing + h)) * range, c1 };
                    m_window->draw(cone);
                }

                // ====================================================================
                // 6. ALERT ICON
                // ====================================================================
                if (ec.alertIconTimer > 0.f && ec.alertIconDuration > 0.f) {
                    drawAlertIcon(tf.position, ec);
                }

                // ====================================================================
                // 7. PACK MARK -- aura / execution buff / feral
                // ====================================================================
                if (ec.packTier > 0) drawPackMark(tf.position, ec, adef.radius);
                m_chaos = std::max(m_chaos, ec.feralTimer);

                continue;   // enemy drawn, skip generic render
            }

            // ================================================================
            // DEFAULT (fallback for any other type)
            // ================================================================
            rd.shape.setPosition(drawPos);
            rd.shape.setRotation(sf::degrees(drawRot));
            m_window->draw(rd.shape);
        }

        drawShockRings();
        drawBeams();
        pruneDetailCache();
    }

    /**
     * @brief Screen-space overlay pass (full-screen flashes)
     * Called by SystemManager after switching to the default view.
     */
    void drawScreenSpace() {
        if (!m_em || !m_window) return;
        if (m_chaos > 0.f) drawChaosFrame();
        if (m_em->screenFlashes.empty()) return;

        const sf::View& v = m_window->getView();
        sf::RectangleShape quad(v.getSize());
        quad.setPosition(v.getCenter() - v.getSize() * 0.5f);

        for (const auto& f : m_em->screenFlashes) {
            const float u = std::clamp(f.timer / std::max(0.0001f, f.maxTimer), 0.f, 1.f);
            sf::Color c = f.color;
            c.a = static_cast<uint8_t>(std::clamp(f.peakAlpha * u * u, 0.f, 255.f));
            quad.setFillColor(c);
            m_window->draw(quad);
        }
    }

private:
    // ========================================================================
    // ENEMY DETAIL: scars, flame, bash tell
    // ========================================================================

    /// Flat segment pairs as thin quads. sf::Lines is always 1px and vanishes
    /// under camera zoom-out; these scale with the hull.
    void drawSegments(const std::vector<sf::Vector2f>& segs, float width,
        sf::Color col, const sf::RenderStates& states) {
        const size_t n = segs.size() / 2;
        if (n == 0) return;
        sf::VertexArray va(sf::PrimitiveType::Triangles, n * 6);
        const float h = width * 0.5f;
        for (size_t k = 0; k < n; ++k) {
            const sf::Vector2f a = segs[k * 2], b = segs[k * 2 + 1];
            sf::Vector2f d = b - a;
            const float l = std::sqrt(d.x * d.x + d.y * d.y);

            // A degenerate segment must still WRITE its six vertices. Skipping
            // leaves them default-constructed at (0,0), which draws a sliver
            // from the world origin to this hull -- a map-long streak from one
            // bad data point.
            const sf::Vector2f p = (l < 1e-4f)
                ? sf::Vector2f(0.f, 0.f)
                : sf::Vector2f(-d.y / l * h, d.x / l * h);
            va[k * 6 + 0] = sf::Vertex{ a - p, col };
            va[k * 6 + 1] = sf::Vertex{ a + p, col };
            va[k * 6 + 2] = sf::Vertex{ b + p, col };
            va[k * 6 + 3] = sf::Vertex{ a - p, col };
            va[k * 6 + 4] = sf::Vertex{ b + p, col };
            va[k * 6 + 5] = sf::Vertex{ b - p, col };
        }
        m_window->draw(va, states);
    }

    /**
     * @brief Hull-mounted exhaust flame, length driven by speed and intent.
     *
     * The roster wants the Berserker's threat telegraphed through MOTION more
     * than gunfire. This is the channel: the flame roars through an attack
     * run, goes white through a charge or lunge, and gutters to almost
     * nothing while a bash coils -- a ship that suddenly goes quiet at close
     * range is about to swing.
     */
    void drawThrusterFlame(size_t i, const EnemyComponent& ec,
        const enemyarch::ArchetypeDef& adef, const sf::RenderStates& states) {
        const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[i].bodyId);
        const float speed = std::sqrt(v.x * v.x + v.y * v.y) * SCALE;
        const float s = std::clamp(speed / 520.f, 0.f, 1.f);

        float intent = 1.f;
        bool whiteHot = false;
        if (ec.frenzyState == FrenzyState::Charge ||
            ec.frenzyState == FrenzyState::Thrown) {
            // Wide open, pulsing with the heartbeat.
            intent = 2.4f + 0.7f * std::sin(m_enemyAnimTime * 11.f);
            whiteHot = true;
        }
        else if (ec.frenzyState == FrenzyState::Ignite) {
            intent = 1.f + 1.8f * ec.frenzy; whiteHot = true;
        }
        else if (ec.ramState == RamState::Charge || ec.bashState == BashState::Lunge) {
            intent = 1.7f; whiteHot = true;
        }
        else if (ec.bashState == BashState::Windup || ec.ramState == RamState::Windup) {
            intent = 0.25f;
        }
        // ---- Bloodseeker mode tell ----
        // RANGE mode guts the aft flame: he is holding back on retros (the
        // prow plumes come from AISystem), so a long hot tail always means
        // MELEE -- he is coming. The dive opens it white, like a charge.
        else if (ec.duelShift == DuelShift::Dive) {
            intent = 1.9f; whiteHot = true;
        }
        else if (ec.duelShift == DuelShift::DiveWindup) {
            intent = 0.2f;
        }
        // Feint: the flame cuts out on the hesitation -- a coil that loses
        // its fire instead of snapping forward -- and stays low while he
        // backs off spraying.
        else if (ec.duelShift == DuelShift::FeintBreak) {
            intent = 0.08f;
        }
        else if (ec.duelShift == DuelShift::FeintFallback) {
            intent = 0.3f;
        }
        else if (ec.duelMode == DuelMode::Range) {
            intent = 0.35f;
        }

        // Pack buff: the flame is the second channel (the pip is the first).
        intent *= 1.f + 2.5f * (ec.packMult - 1.f);

        const float g = adef.exhaust.glow;
        const sf::Color outer = whiteHot ? sf::Color(255, 235, 190, 210) : sf::Color(255, 150, 60, 175);
        const sf::Color inner = whiteHot ? sf::Color(255, 255, 255, 240) : sf::Color(255, 230, 160, 220);
        const sf::Color clear(255, 120, 40, 0);

        sf::VertexArray va(sf::PrimitiveType::Triangles, adef.thrusters.size() * 6);
        size_t k = 0;
        for (size_t n = 0; n < adef.thrusters.size(); ++n) {
            const sf::Vector2f o = adef.thrusters[n];
            const float flick = 0.82f + 0.18f * std::sin(m_enemyAnimTime * 43.f
                + static_cast<float>(n) * 2.1f + static_cast<float>(i));
            const float len = g * (5.f + 17.f * s) * intent * flick;
            const float w = g * 3.0f * (0.75f + 0.45f * s);

            va[k++] = sf::Vertex{ { o.x - w, o.y }, outer };
            va[k++] = sf::Vertex{ { o.x + w, o.y }, outer };
            va[k++] = sf::Vertex{ { o.x, o.y + len }, clear };

            va[k++] = sf::Vertex{ { o.x - w * 0.45f, o.y }, inner };
            va[k++] = sf::Vertex{ { o.x + w * 0.45f, o.y }, inner };
            va[k++] = sf::Vertex{ { o.x, o.y + len * 0.55f }, clear };
        }
        m_window->draw(va, states);
    }

    /**
     * @brief A floating mine, with its trigger zone made visible.
     *
     * Two things had to be legible at a glance, and neither was before:
     *
     *  1. WHERE it reaches. A hazard whose radius the player has to learn by
     *     dying to it is not a hazard, it is a trap. The zone is drawn as a
     *     sonar sweep -- a ring that expands out to the exact trigger radius
     *     and fades -- so the boundary is stated rather than implied.
     *  2. WHETHER it is counting. Idle is slow, dim and grey-amber. Triggered
     *     is RED and accelerating, and the ring sweeps faster too. The player
     *     should be able to tell "I can still cross that" from "I cannot" from
     *     across the arena without reading anything.
     */
    void drawMine(const TransformComponent& tf, RenderComponent& rd,
        const BulletComponent& mine) {
        const bool lit = mine.mineFuse > 0.f;
        const float urgency = lit
            ? 1.f - std::clamp(mine.mineFuse / std::max(0.1f, mine.mineFuseTime), 0.f, 1.f)
            : 0.f;

        // ---- Blink ----
        // Idle ticks slowly; a lit fuse ramps from brisk to frantic.
        const float hz = lit ? (3.f + 12.f * urgency) : 0.7f;
        const float phase = std::fmod(m_enemyAnimTime * hz, 1.f);
        const float duty = lit ? 0.55f : 0.16f;   // idle: brief tick, long dark
        const float on = (phase < duty) ? 1.f : 0.f;

        const sf::Color idleCol(170, 165, 150);
        const sf::Color hotCol(255, static_cast<uint8_t>(70 - 40 * urgency), 45);
        const sf::Color blink = lit ? hotCol : idleCol;

        // ---- Decay: the last 2.5s of an unlit mine's life ----
        // Everything dims toward nothing, and the lamp sputters instead of
        // ticking: it is running out, and it will fizzle, not blow.
        const float fadeK = (!lit && mine.lifetime < 2.5f)
            ? std::clamp(mine.lifetime / 2.5f, 0.15f, 1.f) : 1.f;
        const auto fa = [fadeK](float a) { return static_cast<uint8_t>(std::clamp(a * fadeK, 0.f, 255.f)); };

        // ---- Trigger zone: sonar sweep ----
        // Only while armed. An unarmed mine has no zone yet, and drawing one
        // would promise a threat that is not live.
        if (mine.mineArmed && mine.mineTrigger > 1.f) {
            const float sweepHz = lit ? (1.1f + 1.6f * urgency) : 0.45f;
            const float t = std::fmod(m_enemyAnimTime * sweepHz, 1.f);
            const float r = mine.mineTrigger * (0.12f + 0.88f * t);
            const float fade = (1.f - t) * (1.f - t);

            sf::CircleShape sweep(r, 40);
            sweep.setOrigin({ r, r });
            sweep.setPosition(tf.position);
            sweep.setFillColor(sf::Color::Transparent);
            sweep.setOutlineThickness(lit ? 2.2f : 1.4f);
            sweep.setOutlineColor(sf::Color(blink.r, blink.g, blink.b,
                fa((lit ? 190.f : 90.f) * fade)));
            m_window->draw(sweep);

            // The boundary itself, held faintly all the time, so the edge is
            // readable even between sweeps.
            sf::CircleShape edge(mine.mineTrigger, 44);
            edge.setOrigin({ mine.mineTrigger, mine.mineTrigger });
            edge.setPosition(tf.position);
            edge.setFillColor(sf::Color::Transparent);
            edge.setOutlineThickness(1.f);
            edge.setOutlineColor(sf::Color(blink.r, blink.g, blink.b,
                fa(lit ? 70.f + 60.f * on : 34.f)));
            m_window->draw(edge);
        }

        // ---- Body ----
        rd.shape.setPosition(tf.position);
        rd.shape.setRotation(sf::degrees(tf.rotation));
        rd.shape.setOutlineColor(sf::Color(blink.r, blink.g, blink.b,
            fa(140.f + 115.f * on)));
        rd.shape.setFillColor(lit
            ? sf::Color(static_cast<uint8_t>(60 + 120 * on), 34, 30)
            : sf::Color(64, 58, 56, fa(255.f)));
        m_window->draw(rd.shape);

        // ---- Lamp ----
        // A hard dot at the centre. The hull outline can be lost against a
        // bright background; this cannot.
        const bool sputter = fadeK < 1.f && std::fmod(m_enemyAnimTime * 9.f, 1.f) < 0.3f;
        if (on > 0.5f || sputter) {
            const float s = lit ? 5.f + 3.f * urgency : 3.f * (0.5f + 0.5f * fadeK);
            sf::CircleShape lamp(s, 8);
            lamp.setOrigin({ s, s });
            lamp.setPosition(tf.position);
            lamp.setFillColor(sf::Color(blink.r, blink.g, blink.b, fa(245.f)));
            m_window->draw(lamp);
        }
    }

    /**
     * @brief Ragged corona around an ignited Maniac.
     *
     * Deliberately NOT a clean ring like the shock rings or the bash crescent:
     * those mean "a system fired". This one is irregular and jittery per
     * spoke, so it reads as the ship coming apart rather than as an attack
     * being announced.
     */
    void drawFrenzyCorona(const TransformComponent& tf, const EnemyComponent& ec,
        const enemyarch::ArchetypeDef& adef) {
        // The corona breathes on the same square wave as the hull, so the
        // whole ship reads as ONE object flashing rather than as a body and a
        // separate effect that happen to be near each other.
        const float hz = std::max(1.f, ec.frenzyBlinkHz);
        const float on = (std::fmod(m_enemyAnimTime * hz, 1.f) < 0.42f) ? 1.f : 0.45f;
        const float w = std::clamp(ec.frenzy, 0.f, 1.f) * on;
        constexpr int SPOKES = 14;

        sf::VertexArray va(sf::PrimitiveType::Triangles, SPOKES * 3);
        for (int k = 0; k < SPOKES; ++k) {
            const float base = (k / static_cast<float>(SPOKES)) * 6.28318f;
            const float wob = std::sin(m_enemyAnimTime * (23.f + k * 3.1f) + k);
            const float r0 = adef.radius * 0.92f;
            const float r1 = r0 + (6.f + 15.f * w) * (0.55f + 0.45f * wob);
            const float half = 0.11f + 0.05f * w;

            const sf::Vector2f a(tf.position.x + std::cos(base - half) * r0,
                tf.position.y + std::sin(base - half) * r0);
            const sf::Vector2f b(tf.position.x + std::cos(base + half) * r0,
                tf.position.y + std::sin(base + half) * r0);
            const sf::Vector2f tip(tf.position.x + std::cos(base) * r1,
                tf.position.y + std::sin(base) * r1);

            const sf::Color hot(255, static_cast<uint8_t>(60 + 40 * wob), 45,
                static_cast<uint8_t>(200 * w));
            const sf::Color out(255, 50, 30, 0);
            va[k * 3 + 0] = sf::Vertex{ a, hot };
            va[k * 3 + 1] = sf::Vertex{ b, hot };
            va[k * 3 + 2] = sf::Vertex{ tip, out };
        }
        m_window->draw(va);
    }

    /**
     * @brief The "parry this" read.
     *
     * A thick arc hugging the prow, spanning the strike arc (bash_arc_cos),
     * creeping outward and brightening as the coil completes, then snapping
     * to full white-cyan for the lunge itself.
     */
    /// A world-space segment as a quad, colour fading from `ca` to `cb`.
    /// sf::Lines is 1px and vanishes under zoom-out.
    /**
     * @brief Cone telegraph: marching ants + a warning sign (playtest 1.15).
     *
     * The filled red wedge read as too simple and too bright. Now:
     *   - the wedge itself is barely there (a faint wash),
     *   - its OUTLINE is marching ants: dashes crawling outward along the
     *     edges and round the arc -- the boundary is what matters, so the
     *     boundary is what moves,
     *   - a warning triangle with a "!" sits inside the cone, its glow
     *     pulsing faster as the charge fills.
     * `charge` 0..1 = windup progress; `locked` = last beat (ants faster,
     * thicker, solid sign); `fade` 1 -> 0 after it fires: the sign swells
     * and fades out with the ants, so the attack starting reads as the
     * warning being spent.
     */
    void drawConeTell(sf::Vector2f apex, float base, float half, float R,
        float charge, bool locked, float fade)
    {
        fade = std::clamp(fade, 0.f, 1.f);
        charge = std::clamp(charge, 0.f, 1.f);
        if (fade <= 0.01f) return;
        const sf::Color red(255, 80, 55);
        const auto A = [&](float a) { return static_cast<uint8_t>(std::clamp(a * fade, 0.f, 255.f)); };

        // ---- Faint wash ----
        {
            constexpr int SEG = 18;
            sf::VertexArray fan(sf::PrimitiveType::TriangleFan, SEG + 2);
            const sf::Color f(red.r, red.g, red.b, A(locked ? 22.f : 6.f + 10.f * charge));
            fan[0] = sf::Vertex{ apex, f };
            for (int k = 0; k <= SEG; ++k) {
                const float a = base - half + 2.f * half * (static_cast<float>(k) / SEG);
                fan[k + 1] = sf::Vertex{ apex + sf::Vector2f(std::cos(a), std::sin(a)) * R, f };
            }
            m_window->draw(fan);
        }

        // ---- Marching ants round the boundary ----
        // One closed path apex -> edge -> arc -> edge -> apex, walked with a
        // dash pattern whose offset advances with time.
        std::vector<sf::Vector2f> path;
        path.push_back(apex);
        constexpr int ARC = 24;
        for (int k = 0; k <= ARC; ++k) {
            const float a = base - half + 2.f * half * (static_cast<float>(k) / ARC);
            path.push_back(apex + sf::Vector2f(std::cos(a), std::sin(a)) * R);
        }
        path.push_back(apex);
        const float dash = 12.f, gap = 9.f, period = dash + gap;
        const float speed = locked ? 150.f : 45.f + 55.f * charge;
        const float off = std::fmod(m_enemyAnimTime * speed, period);
        const float w = locked ? 2.4f : 1.4f + 0.6f * charge;
        const sf::Color ant(red.r, red.g, red.b, A(locked ? 240.f : 90.f + 120.f * charge));
        float s0 = -off;   // path distance where the current dash pattern starts
        float walked = 0.f;
        for (size_t k = 0; k + 1 < path.size(); ++k) {
            const sf::Vector2f p0 = path[k], p1 = path[k + 1];
            const sf::Vector2f d = p1 - p0;
            const float L = std::sqrt(d.x * d.x + d.y * d.y);
            if (L < 1e-3f) continue;
            const sf::Vector2f n = d / L;
            // every dash [s0 + m*period, s0 + m*period + dash] overlapping [walked, walked+L]
            float m0 = std::floor((walked - s0 - dash) / period);
            for (float m = m0; ; m += 1.f) {
                const float a0 = s0 + m * period, a1 = a0 + dash;
                if (a0 > walked + L) break;
                const float c0 = std::max(a0, walked) - walked, c1 = std::min(a1, walked + L) - walked;
                if (c1 > c0) drawBand(p0 + n * c0, p0 + n * c1, w, ant, ant);
            }
            walked += L;
        }

        // ---- Warning sign ----
        const sf::Vector2f dir(std::cos(base), std::sin(base));
        const sf::Vector2f c = apex + dir * (R * 0.42f);
        const float swell = 1.f + 0.6f * (1.f - fade) * (fade < 1.f ? 1.f : 0.f);
        const float sz = 12.f * swell;
        const float pulse = 0.5f + 0.5f * std::sin(m_enemyAnimTime * (5.f + 12.f * charge));
        const auto tri = [&](float s) {
            return std::array<sf::Vector2f, 3>{ c + sf::Vector2f(0.f, -s),
                c + sf::Vector2f(s * 0.95f, s * 0.70f), c + sf::Vector2f(-s * 0.95f, s * 0.70f) };
            };
        // glow ring: an outline a step out, breathing
        {
            const auto g = tri(sz * 1.45f);
            const sf::Color gc(red.r, 120, 80, A((locked ? 150.f : 50.f + 90.f * charge) * (0.4f + 0.6f * pulse)));
            for (int k = 0; k < 3; ++k) drawBand(g[k], g[(k + 1) % 3], 1.6f, gc, gc);
        }
        {
            const auto t = tri(sz);
            sf::VertexArray body(sf::PrimitiveType::Triangles, 3);
            const sf::Color bc(28, 8, 8, A(210.f));
            for (int k = 0; k < 3; ++k) body[k] = sf::Vertex{ t[k], bc };
            m_window->draw(body);
            const sf::Color ec(255, static_cast<uint8_t>(110 + 60 * pulse), 70, A(locked ? 255.f : 150.f + 100.f * charge));
            for (int k = 0; k < 3; ++k) drawBand(t[k], t[(k + 1) % 3], 2.0f, ec, ec);
            // the "!"
            drawBand(c + sf::Vector2f(0.f, -sz * 0.52f), c + sf::Vector2f(0.f, sz * 0.18f), 2.4f * swell, ec, ec);
            drawBand(c + sf::Vector2f(0.f, sz * 0.32f), c + sf::Vector2f(0.f, sz * 0.48f), 2.4f * swell, ec, ec);
        }
    }

    /// A filled wedge with hard edges. Apex, centre bearing (radians),
    /// half-angle, range. (Cone telegraphs use drawConeTell now.)
    void drawWedge(sf::Vector2f apex, float base, float half, float R,
        sf::Color fill, sf::Color edge, float w) {
        constexpr int SEG = 18;
        sf::VertexArray fan(sf::PrimitiveType::TriangleFan, SEG + 2);
        fan[0] = sf::Vertex{ apex, fill };
        for (int k = 0; k <= SEG; ++k) {
            const float a = base - half + 2.f * half * (static_cast<float>(k) / SEG);
            fan[k + 1] = sf::Vertex{ apex + sf::Vector2f(std::cos(a), std::sin(a)) * R, fill };
        }
        m_window->draw(fan);
        const sf::Vector2f e0 = apex + sf::Vector2f(std::cos(base - half), std::sin(base - half)) * R;
        const sf::Vector2f e1 = apex + sf::Vector2f(std::cos(base + half), std::sin(base + half)) * R;
        drawBand(apex, e0, w, edge, edge);
        drawBand(apex, e1, w, edge, edge);
        for (int k = 0; k < SEG; ++k) {
            const float a0 = base - half + 2.f * half * (static_cast<float>(k) / SEG);
            const float a1 = base - half + 2.f * half * (static_cast<float>(k + 1) / SEG);
            drawBand(apex + sf::Vector2f(std::cos(a0), std::sin(a0)) * R,
                apex + sf::Vector2f(std::cos(a1), std::sin(a1)) * R, w, edge, edge);
        }
    }

    void drawBand(sf::Vector2f a, sf::Vector2f b, float width, sf::Color ca, sf::Color cb) {
        sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l < 1e-3f) return;
        const sf::Vector2f n(-d.y / l * width * 0.5f, d.x / l * width * 0.5f);
        sf::VertexArray q(sf::PrimitiveType::TriangleStrip, 4);
        q[0] = sf::Vertex{ a - n, ca }; q[1] = sf::Vertex{ a + n, ca };
        q[2] = sf::Vertex{ b - n, cb }; q[3] = sf::Vertex{ b + n, cb };
        m_window->draw(q);
    }

    /**
     * @brief The Bloodseeker's RANGE tells, in world space.
     *
     * LANCER  a lane from the prow along the solved intercept. Charge: thin,
     *         red, lengthening -- "a shot is being lined up on where you are
     *         going". Lock: full length, thick, bone-white -- "it has stopped
     *         tracking; move off this line now".
     *
     * CONE    the exact wedge the rounds will fill, out to their real reach.
     *         Flat fill (no gradient), hard edges and a rim arc. The fill
     *         thickens through the windup so "how long until it starts" is
     *         readable without a timer; while firing it holds, and it is gone
     *         the moment he goes into recovery.
     */
    void drawDuelAttack(const TransformComponent& tf, const EnemyComponent& ec,
        const enemyarch::ArchetypeDef& adef) {
        const float u = (ec.duelAtkDuration > 0.f)
            ? std::clamp(1.f - ec.duelAtkTimer / ec.duelAtkDuration, 0.f, 1.f) : 1.f;
        const sf::Vector2f dir = ec.duelAtkDir;
        const sf::Vector2f nose = tf.position + dir * (adef.radius * 0.95f);

        switch (ec.duelAttack) {
        case DuelAttack::LancerCharge: {
            const float len = 260.f + 640.f * u;
            drawBand(nose, nose + dir * len, 1.4f + 1.6f * u,
                sf::Color(255, 90, 60, static_cast<uint8_t>(60 + 130 * u)),
                sf::Color(255, 90, 60, 0));
            drawLancerCharge(tf, adef, u, false);
            break;
        }
        case DuelAttack::LancerLock: {
            const float R = adef.config["lancer_range"].get_or(1300.f);
            drawBand(nose, nose + dir * R, 4.0f,
                sf::Color(245, 238, 222, 245), sf::Color(245, 238, 222, 0));
            drawLancerCharge(tf, adef, 1.f, true);
            break;
        }

        case DuelAttack::ConeWindup:
        case DuelAttack::ConeFire: {
            const bool firing = (ec.duelAttack == DuelAttack::ConeFire);
            const float half = ec.duelConeHalf * 3.14159f / 180.f;
            const float R = ec.duelConeRange;
            const float base = std::atan2(dir.y, dir.x);
            // Rounds leave from 0.9r and fly R: the wedge is drawn from there
            // to exactly where they die.
            const sf::Vector2f apex = tf.position + dir * (adef.radius * 0.9f);

            // Windup: ants + sign charging. Firing: the warning is spent --
            // sign swells and fades with the ants over the volley.
            drawConeTell(apex, base, half, R, firing ? 1.f : u, false, firing ? 1.f - u : 1.f);
            break;
        }
        default:
            break;
        }
    }

    /**
     * @brief Under-the-ship mark for the Bloodseeker's pack.
     *
     *   AURA       one small gold chevron under the hull, still.
     *   EXECUTION  two gold chevrons, pulsing: +20% and back in the fight.
     *   FERAL      three red slashes over the hull, flickering: this one is
     *              fighting whoever is nearest, you or its own.
     *
     * Screen-aligned, like the alert icons, so it reads the same whatever
     * way the ship is facing. Below the hull so it never sits on an icon.
     */
    void drawPackMark(sf::Vector2f pos, const EnemyComponent& ec, float radius) {
        if (ec.packTier == 3) {
            const bool on = std::fmod(m_enemyAnimTime * 9.f, 1.f) < 0.6f;
            const sf::Color red(255, 50, 35, on ? 245 : 120);
            const float y = pos.y - radius - 14.f;
            for (int k = -1; k <= 1; ++k) {
                const float x = pos.x + k * 7.f;
                drawBand({ x + 4.f, y - 6.f }, { x - 4.f, y + 6.f }, on ? 3.0f : 2.4f, red, red);
            }
            return;
        }
        const bool exec = (ec.packTier == 2);
        float a = 190.f;
        if (exec) a = 180.f + 75.f * (0.5f + 0.5f * std::sin(m_enemyAnimTime * 9.f));
        const sf::Color gold(214, 172, 92, static_cast<uint8_t>(a));
        const int n = exec ? 2 : 1;
        for (int k = 0; k < n; ++k) {
            const float y = pos.y + radius + 10.f + k * 6.f;
            drawBand({ pos.x - 7.f, y + 4.f }, { pos.x, y }, 2.4f, gold, gold);
            drawBand({ pos.x + 7.f, y + 4.f }, { pos.x, y }, 2.4f, gold, gold);
        }
    }

    /**
     * @brief Death-chaos screen frame: red corner brackets, screen space.
     *
     * Up for as long as any Rakshari is feral, fading over the last second.
     * Flat, hard-edged, on the edge of the screen: it changes how EVERY enemy
     * should be read, so it lives where the whole screen is, not on a ship.
     */
    void drawChaosFrame() {
        const sf::View& v = m_window->getView();
        const sf::Vector2f c = v.getCenter(), h = v.getSize() * 0.5f;
        const float fade = std::clamp(m_chaos, 0.f, 1.f);
        const float pulse = 0.65f + 0.35f * std::sin(m_enemyAnimTime * 7.f);
        const sf::Color red(200, 25, 20, static_cast<uint8_t>(220.f * fade * pulse));
        const float inset = std::min(h.x, h.y) * 0.04f;
        const float arm = std::min(h.x, h.y) * 0.22f;
        const float w = std::max(4.f, std::min(h.x, h.y) * 0.012f);
        for (int k = 0; k < 4; ++k) {
            const float sx = (k & 1) ? 1.f : -1.f, sy = (k & 2) ? 1.f : -1.f;
            const sf::Vector2f p(c.x + sx * (h.x - inset), c.y + sy * (h.y - inset));
            drawBand(p, { p.x - sx * arm, p.y }, w, red, red);
            drawBand(p, { p.x, p.y - sy * arm }, w, red, red);
        }
    }

    /**
     * @brief The Bloodseeker's mode light: one small piece of the hull.
     *
     *   ORANGE      MELEE. Quick pulse (he is coming).
     *   BLUE-CYAN   RANGE. Slow pulse (he is sighting you). Bluer than the
     *               bash crescent's mint on purpose, so a RANGE Bloodseeker
     *               winding up a bash still shows two different cyans.
     *
     * The colour is FIXED -- it does not follow the hull's state tint, hit
     * flash or stagger -- because it is the one thing on him that has to
     * read the same in every frame. Each pulse also sends a thin ring of the
     * lamp's own shape out over the plating: flat, hard-edged, gone in a beat.
     *
     * Hard fast blink = perfect-dodging right now (disengage or post-stun
     * window). Dim and still = stunned or tumbling, i.e. open.
     * During a switch it already shows the mode he is going INTO.
     *
     * Drawn in the hull's frame (`states`), so it banks and squashes with it.
     */
    void drawModeLamp(const EnemyComponent& ec, const enemyarch::ArchetypeDef& adef,
        const sf::RenderStates& states, bool stunned)
    {
        const bool range = (ec.duelShift == DuelShift::Disengage) ? true
            : (ec.duelShift == DuelShift::DiveWindup || ec.duelShift == DuelShift::Dive)
                ? false : (ec.duelMode == DuelMode::Range);
        const sf::Color base = range ? adef.lampRange : adef.lampMelee;
        const bool broken = stunned || ec.staggerTimer > 0.f;
        const bool evading = ec.duelEvading() && !broken;

        // ---- Brightness ----
        float b, phase = 0.f;
        if (broken) b = 0.30f;
        else if (evading) b = (std::fmod(m_enemyAnimTime * 11.f, 1.f) < 0.5f) ? 1.f : 0.22f;
        else {
            const float hz = range ? 1.5f : 3.0f;
            phase = std::fmod(m_enemyAnimTime * hz, 1.f);
            // Snap up, ease down: a heartbeat rather than a sine wave.
            b = 0.55f + 0.45f * (1.f - phase) * (1.f - phase);
        }
        const auto scale = [](sf::Color c, float k) {
            return sf::Color(static_cast<uint8_t>(std::clamp(c.r * k, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(c.g * k, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(c.b * k, 0.f, 255.f)));
        };
        const sf::Color fillC = scale(base, b);
        // Rim: toward white at the top of the beat, so the lamp has a hard
        // bright edge against the plate around it.
        const float wr = std::clamp((b - 0.55f) / 0.45f, 0.f, 1.f) * 0.6f;
        const sf::Color rimC(
            static_cast<uint8_t>(fillC.r + (255 - fillC.r) * wr),
            static_cast<uint8_t>(fillC.g + (255 - fillC.g) * wr),
            static_cast<uint8_t>(fillC.b + (255 - fillC.b) * wr), 255);

        // ---- Pulse ring: the lamp's own outline, swelling and fading ----
        if (!broken && !evading) {
            const float k = 1.f + 0.9f * phase;
            m_lampScratch.clear();
            for (const auto& v : adef.lamp)
                m_lampScratch.push_back(adef.lampCentre + (v - adef.lampCentre) * k);
            const auto ring = enemyarch::geom::outlineStrip(m_lampScratch, 1.2f);
            if (ring.size() >= 4) {
                const uint8_t a = static_cast<uint8_t>(190.f * (1.f - phase) * (1.f - phase));
                sf::VertexArray r(sf::PrimitiveType::TriangleStrip, ring.size());
                for (size_t n = 0; n < ring.size(); ++n)
                    r[n] = sf::Vertex{ ring[n], sf::Color(base.r, base.g, base.b, a) };
                m_window->draw(r, states);
            }
        }

        // ---- The lamp ----
        sf::VertexArray body(sf::PrimitiveType::Triangles, adef.lampTris.size());
        for (size_t n = 0; n < adef.lampTris.size(); ++n)
            body[n] = sf::Vertex{ adef.lampTris[n], fillC };
        m_window->draw(body, states);

        const auto rim = enemyarch::geom::outlineStrip(adef.lamp, 1.1f);
        if (rim.size() >= 4) {
            sf::VertexArray r(sf::PrimitiveType::TriangleStrip, rim.size());
            for (size_t n = 0; n < rim.size(); ++n) r[n] = sf::Vertex{ rim[n], rimC };
            m_window->draw(r, states);
        }
    }

    void drawBashCrescent(const TransformComponent& tf, const enemyarch::ArchetypeDef& adef,
        float u, sf::Color col, bool lunging) {
        const float arcCos = std::clamp(adef.config["bash_arc_cos"].get_or(0.30f), -0.9f, 0.95f);
        const float half = std::min(std::acos(arcCos), 1.25f);   // cap ~72 deg
        const float facing = (tf.rotation - 90.f) * 3.14159f / 180.f;

        const float e = u * u * (3.f - 2.f * u);
        const float r0 = adef.radius * 0.95f + 6.f + 16.f * e;
        const float thick = 2.5f + 7.f * e + (lunging ? 3.f : 0.f);

        const float wmix = lunging ? 0.75f : 0.35f * e;
        const sf::Color c(
            static_cast<uint8_t>(col.r + (255 - col.r) * wmix),
            static_cast<uint8_t>(col.g + (255 - col.g) * wmix),
            static_cast<uint8_t>(col.b + (255 - col.b) * wmix),
            static_cast<uint8_t>(lunging ? 250 : 70 + 170 * e));
        const sf::Color edge(c.r, c.g, c.b, 0);

        constexpr int SEG = 16;
        sf::VertexArray arc(sf::PrimitiveType::TriangleStrip, (SEG + 1) * 2);
        for (int k = 0; k <= SEG; ++k) {
            const float t = static_cast<float>(k) / SEG;
            const float a = facing - half + 2.f * half * t;
            // Taper at the tips so it reads as a blade, not a band.
            const float taper = std::sin(t * 3.14159f);
            const sf::Vector2f dir(std::cos(a), std::sin(a));
            arc[k * 2] = sf::Vertex{ tf.position + dir * r0, c };
            arc[k * 2 + 1] = sf::Vertex{ tf.position + dir * (r0 + thick * taper),
                                         (taper > 0.2f) ? c : edge };
        }
        m_window->draw(arc);
    }

    // ========================================================================
    // SURFACE DETAIL CACHE
    // ========================================================================
    struct Vein {
        std::array<sf::Vector2f, 4> pts;  ///< Local space, RIM -> inward
        float waveOrder = 0.f;            ///< 0..1 position in the pulse sequence
    };
    struct Facet {
        sf::Vector2f a, b;                ///< Local-space line across the face
    };
    struct AsteroidDetail {
        std::vector<Vein>  veins;
        std::vector<Facet> facets;
        float seed = 0.f;
        float minRadius = 8.f;   ///< Nearest boundary point; caps the core
    };

    std::unordered_map<uint32_t, AsteroidDetail> m_detail;

    float m_enemyAnimTime = 0.f;
    /**
     * @brief Exact boundary distance of a star-shaped polygon at a given angle
     *
     * Intersects the ray from the local origin with every edge, returning the
     * nearest positive hit. This is what makes containment exact rather than
     * approximate.
     *
     * @return distance, or -1 if the ray misses (degenerate polygon)
     */
    static float boundaryRadiusAt(float theta, const std::vector<sf::Vector2f>& P) {
        const float dx = std::cos(theta), dy = std::sin(theta);
        float best = -1.f;
        const int n = static_cast<int>(P.size());

        for (int i = 0; i < n; ++i) {
            const sf::Vector2f& A = P[i];
            const sf::Vector2f& B = P[(i + 1) % n];
            const float ex = B.x - A.x, ey = B.y - A.y;

            const float den = dx * ey - dy * ex;
            if (std::abs(den) < 1e-9f) continue;

            const float s = (A.x * ey - A.y * ex) / den;
            if (s <= 0.f) continue;

            float u;
            if (std::abs(ex) > std::abs(ey)) u = (s * dx - A.x) / ex;
            else                             u = (s * dy - A.y) / ey;

            if (u < -1e-6f || u > 1.f + 1e-6f) continue;
            if (best < 0.f || s < best) best = s;
        }
        return best;
    }

    /// Pull a local-space point inside the outline, preserving its angle.
    static sf::Vector2f clampInside(sf::Vector2f p, const std::vector<sf::Vector2f>& P,
        float margin = 0.90f) {
        const float r = std::sqrt(p.x * p.x + p.y * p.y);
        if (r < 0.001f) return p;

        const float theta = std::atan2(p.y, p.x);
        const float rMax = boundaryRadiusAt(theta, P);
        if (rMax <= 0.f) return p;

        const float lim = rMax * margin;
        if (r <= lim) return p;
        return { p.x / r * lim, p.y / r * lim };
    }

    const AsteroidDetail& detailFor(size_t idx, uint32_t entityId) {
        auto it = m_detail.find(entityId);
        if (it != m_detail.end()) return it->second;

        AsteroidDetail det;
        det.seed = (rand() % 1000) / 1000.f * 6.28318f;

        const std::vector<sf::Vector2f>& P = m_em->physicsShapes[idx].vertices;
        const int n = static_cast<int>(P.size());
        if (n < 3) return m_detail.emplace(entityId, std::move(det)).first->second;

        det.minRadius = 1e9f;
        float maxR = 0.f;
        for (const auto& v : P) {
            const float r = std::sqrt(v.x * v.x + v.y * v.y);
            det.minRadius = std::min(det.minRadius, r);
            maxR = std::max(maxR, r);
        }
        if (det.minRadius > 1e8f) det.minRadius = 8.f;

        if (m_em->healths[idx].isExplosive) {
            // ================================================================
            // VEINS — count and depth driven by the detail budget
            // ================================================================
            const int detailLevel = static_cast<int>(acfg("magma_detail", 1.f));
            if (detailLevel > 0) {

                // Level 1: a few short cracks. Level 2: one per vertex, deep.
                const int veinCount = (detailLevel >= 2) ? n
                    : std::max(2, n / 3);
                const float depth = (detailLevel >= 2)
                    ? acfg("magma_vein_depth", 0.72f)
                    : acfg("magma_vein_depth_min", 0.34f);

                const int step = std::max(1, n / veinCount);
                int vi = rand() % n;

                for (int v = 0; v < veinCount; ++v, vi = (vi + step) % n) {
                    Vein vn;
                    vn.waveOrder = vi / static_cast<float>(n);

                    const sf::Vector2f start = P[vi];
                    const float tMax = depth * (0.85f + (rand() % 30) / 100.f);

                    for (int k = 0; k < 4; ++k) {
                        const float t = (k / 3.f) * tMax;
                        sf::Vector2f p(start.x * (1.f - t), start.y * (1.f - t));

                        const float drift = ((rand() % 100) / 100.f - 0.5f) * maxR * 0.10f * t;
                        const float pl = std::sqrt(p.x * p.x + p.y * p.y);
                        if (pl > 0.01f) {
                            const float px = p.x, py = p.y;
                            p.x += (-py / pl) * drift;
                            p.y += (px / pl) * drift;
                        }
                        vn.pts[k] = clampInside(p, P, 0.90f);
                    }
                    det.veins.push_back(vn);
                }
            }
        }
        else {
            // ================================================================
            // FACETS (optional, default off)
            // ================================================================
            const int count = std::clamp(1 + static_cast<int>(maxR / 18.f), 1, 3);
            for (int f = 0; f < count && n >= 5; ++f) {
                const int a = rand() % n;
                const int b = (a + 2 + (rand() % std::max(1, n - 3))) % n;
                if (a == b) continue;

                Facet fc;
                fc.a = { P[a].x * 0.82f, P[a].y * 0.82f };
                fc.b = { P[b].x * 0.72f, P[b].y * 0.72f };
                det.facets.push_back(fc);
            }
        }

        return m_detail.emplace(entityId, std::move(det)).first->second;
    }

    void pruneDetailCache() {
        if (m_detail.size() < 96) return;
        for (auto it = m_detail.begin(); it != m_detail.end(); ) {
            it = (m_em->getEntityIndex(it->first) == (size_t)-1)
                ? m_detail.erase(it) : std::next(it);
        }
    }

    /// Flat interior lines. No lighting model, no curves — deliberately plain.
    void drawFacets(const sf::ConvexShape& shape, const AsteroidDetail& det) const {
        if (det.facets.empty()) return;

        const sf::Color base = shape.getFillColor();
        const sf::Transform& xf = shape.getTransform();

        const sf::Color line(
            static_cast<uint8_t>(std::min(255, base.r + 26)),
            static_cast<uint8_t>(std::min(255, base.g + 26)),
            static_cast<uint8_t>(std::min(255, base.b + 28)), 190);

        for (const auto& f : det.facets) {
            sf::VertexArray va(sf::PrimitiveType::Lines, 2);
            va[0] = sf::Vertex{ xf.transformPoint(f.a), line };
            va[1] = sf::Vertex{ xf.transformPoint(f.b), line };
            m_window->draw(va);
        }
    }

    // ========================================================================
    // FIELD OBJECT OVERLAYS -- the animated half of the lab models
    // ========================================================================
    /**
     * Everything static about a rock, scrap pile or unstable core is baked in
     * detailTris. What is left moves, and is rebuilt here each frame from a
     * handful of points, straight from the lab's draw code:
     *
     *   Rock          ore flecks, alpha 0.22 flickering at 1.2 rad/s
     *   Scrap         rivets pulsing at 0.8 rad/s; every 2.5-7 s one corner
     *                 catches a short additive glint
     *   UnstableCore  an orange radial breath over the whole vessel. The lab
     *                 pulses at 3.2 rad/s; here it quickens as the core takes
     *                 damage, which is the "about to go" tell the magmatic
     *                 rock gets from its embers.
     *
     * `asteroid_visuals.field_glow = 0` switches off the two soft effects
     * (glint and breath) and leaves the flat ones.
     */
    void drawFieldFx(size_t i, const TransformComponent& tf, RenderComponent& rd, float dt) {
        using Style = RenderComponent::FieldStyle;
        const float t = m_magmaPulseTime;
        const float rad = tf.rotation * 3.14159265f / 180.f;
        const float ca = std::cos(rad), sa = std::sin(rad);
        const auto W = [&](sf::Vector2f p) {
            return sf::Vector2f(tf.position.x + p.x * ca - p.y * sa, tf.position.y + p.x * sa + p.y * ca);
            };
        const bool soft = acfg("field_glow", 1.f) > 0.5f;

        fieldgeom::Mesh m;

        if (rd.fieldStyle == Style::Rock) {
            for (const auto& o : rd.fieldFx) {
                const float flicker = 0.6f + 0.4f * std::sin(t * 1.2f + o.phase);
                fieldgeom::disc(m, W(o.p), o.r, fieldgeom::rgba(190, 190, 195, 0.22f * flicker), 8);
            }
            if (!m.empty()) m_window->draw(m.v.data(), m.v.size(), sf::PrimitiveType::Triangles);
            return;
        }

        if (rd.fieldStyle == Style::Scrap) {
            for (const auto& r : rd.fieldFx) {
                const float pulse = 0.5f + 0.5f * std::sin(t * 0.8f + r.phase);
                const sf::Color c = fieldgeom::rgba(170.f + pulse * 30.f, 110.f + pulse * 20.f,
                    70.f + pulse * 15.f, 0.08f + pulse * 0.12f);
                // The lab's fillRect(x-0.6, y-0.6, 1.6, 1.6), in the pile's frame.
                const float lo = -0.375f * r.r, hi = 0.625f * r.r;
                m.quad(W(r.p + sf::Vector2f(lo, lo)), W(r.p + sf::Vector2f(hi, lo)),
                    W(r.p + sf::Vector2f(hi, hi)), W(r.p + sf::Vector2f(lo, hi)), c);
            }
            if (!m.empty()) m_window->draw(m.v.data(), m.v.size(), sf::PrimitiveType::Triangles);

            // ---- Glint: one corner at a time, additive ----
            if (rd.glintLife > 0.f) {
                rd.glintLife -= dt;
            }
            else if (!rd.glintSpots.empty()) {
                rd.glintTimer -= dt;
                if (rd.glintTimer <= 0.f) {
                    rd.glintPos = rd.glintSpots[static_cast<size_t>(rand()) % rd.glintSpots.size()];
                    rd.glintLife = rd.glintMax = 0.6f + (rand() % 400) / 1000.f;
                    rd.glintTimer = 2.5f + (rand() % 4500) / 1000.f;
                }
            }
            if (soft && rd.glintLife > 0.f && rd.fieldGlowR > 0.f) {
                const float p = rd.glintLife / std::max(0.01f, rd.glintMax);
                const float a = std::clamp(p < 0.5f ? p * 2.f : (1.f - p) * 2.f, 0.f, 1.f) * 0.55f;
                fieldgeom::Mesh g;
                fieldgeom::radialGradient(g, W(rd.glintPos), 0.f, rd.fieldGlowR, rd.fieldGlowR, {
                    { 0.0f, fieldgeom::rgba(230, 200, 160, a) },
                    { 0.5f, fieldgeom::rgba(180, 140, 100, a * 0.4f) },
                    { 1.0f, fieldgeom::rgba(0, 0, 0, 0.f) } }, 12);
                m_window->draw(g.v.data(), g.v.size(), sf::PrimitiveType::Triangles,
                    sf::RenderStates(sf::BlendAdd));
            }
            return;
        }

        if (rd.fieldStyle == Style::UnstableCore && soft && rd.fieldGlowR > 0.f) {
            const auto& h = m_em->healths[i];
            const float wounded = 1.f - std::clamp(h.currentHp / std::max(1.f, h.maxHp), 0.f, 1.f);
            // Integrated phase, so the rate can rise without the pulse jumping.
            rd.fieldPhase += dt * 3.2f * (1.f + 1.6f * wounded);
            const float pulse = 0.55f + 0.45f * std::sin(rd.fieldPhase);
            fieldgeom::radialGradient(m, tf.position, 0.f, rd.fieldGlowR, rd.fieldGlowR, {
                { 0.0f, fieldgeom::rgba(255, 140, 60, 0.32f * pulse) },
                { 0.5f, fieldgeom::rgba(200, 60, 20, 0.14f * pulse) },
                { 1.0f, fieldgeom::rgba(0, 0, 0, 0.f) } }, 24);
            m_window->draw(m.v.data(), m.v.size(), sf::PrimitiveType::Triangles);
        }
    }

    // ========================================================================
    // MAGMATIC
    // ========================================================================
    void drawMagmatic(size_t i, const TransformComponent& tf,
        RenderComponent& rd, const AsteroidDetail& det)
    {
        const float slow = 0.5f + 0.5f * std::sin(m_magmaPulseTime * 3.5f + det.seed);
        const float heatN = 0.15f + 0.95f * slow;   // 0.45 .. 1.0

        const float hpFrac = std::clamp(
            m_em->healths[i].currentHp / std::max(1.f, m_em->healths[i].maxHp), 0.f, 1.f);
        const float wounded = 1.f - hpFrac;

        // ---- Crust + outline. At detail 0 these carry the whole read, so the
         //      outline pulse has to do the work the cracks were doing. ----
        rd.shape.setFillColor(sf::Color(
            static_cast<uint8_t>(40 + 26 * heatN + 24 * wounded),
            static_cast<uint8_t>(22 + 10 * heatN + 8 * wounded),
            static_cast<uint8_t>(18 + 6 * heatN)));

        rd.shape.setOutlineThickness(2.0f + heatN * 0.8f);
        rd.shape.setOutlineColor(sf::Color(255,
            static_cast<uint8_t>(std::clamp(45.f + 150.f * heatN + 55.f * wounded, 0.f, 255.f)),
            static_cast<uint8_t>(std::clamp(15.f + 70.f * heatN, 0.f, 255.f)),
            static_cast<uint8_t>(std::clamp(170.f + 85.f * heatN, 0.f, 255.f))));

        rd.shape.setPosition(tf.position);
        rd.shape.setRotation(sf::degrees(tf.rotation));
        m_window->draw(rd.shape);

        const sf::Transform& xf = rd.shape.getTransform();

        // ====================================================================
        // VEINS — sequential pulse wave around the rock
        // ====================================================================
        const float waveSpeed = acfg("magma_wave_speed", 2.6f);
        const float waveSharp = acfg("magma_wave_sharpness", 1.8f);

        for (const auto& vn : det.veins) {
            const float ph = m_magmaPulseTime * waveSpeed - vn.waveOrder * 6.28318f;
            float w = 0.5f + 0.5f * std::sin(ph);
            w = std::pow(w, waveSharp);
            const float b = 0.40f + 0.60f * w;

            sf::VertexArray line(sf::PrimitiveType::LineStrip, 4);
            const uint8_t a = static_cast<uint8_t>(std::clamp(235.f * b, 0.f, 255.f));

            for (int p = 0; p < 4; ++p) {
                const float depth = p / 3.f;
                line[p].position = xf.transformPoint(vn.pts[p]);
                line[p].color = sf::Color(255,
                    static_cast<uint8_t>(std::clamp(60.f + 160.f * depth * b + 50.f * wounded, 0.f, 255.f)),
                    static_cast<uint8_t>(std::clamp(15.f + 55.f * depth, 0.f, 255.f)),
                    a);
            }
            m_window->draw(line);
        }

        // ====================================================================
        // CORE — flat polygon, hard edge, Lua-sized
        // ====================================================================
        // Per ENTITY, not per global: a reactor burns, an ordinary magmatic
        // rock does not, and they share this renderer.
        const float coreScale = m_em->healths[i].magmaCore;
        if (coreScale > 0.01f) {
            const float r = det.minRadius * coreScale * (0.88f + 0.12f * heatN + 0.15f * wounded);

            sf::CircleShape core(r, 6);
            core.setOrigin({ r, r });
            core.setPosition(xf.transformPoint({ 0.f, 0.f }));
            core.setRotation(sf::degrees(m_magmaPulseTime * 22.f));
            core.setFillColor(sf::Color(255,
                static_cast<uint8_t>(std::clamp(150.f + 80.f * heatN, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(60.f + 60.f * heatN, 0.f, 255.f)),
                static_cast<uint8_t>(std::clamp(215.f + 40.f * heatN, 0.f, 255.f))));
            m_window->draw(core);
        }

        // ---- Embers, more frequent as it takes damage: the "about to blow" tell ----
        if ((rand() % 100) < static_cast<int>(5 + 450 * wounded)) {
            const float a = (rand() % 360) * 3.14159f / 180.f;
            const float r = det.minRadius * 0.8f;
            m_em->particles.push_back({
                tf.position + sf::Vector2f(std::cos(a), std::sin(a)) * r,
                sf::Vector2f(std::cos(a), std::sin(a)) * (16.f + rand() % 28),
                sf::Color(255, static_cast<uint8_t>(130 + rand() % 80), 40, 205),
                0.45f + (rand() % 40) / 100.f,
                0.85f,
                1.5f + rand() % 3
                });
        }
    }

    // ========================================================================
    // SHIP NOSE HEAT
    // ========================================================================
    // ========================================================================
    // PLAYER MODEL
    // ========================================================================

    /**
     * @brief Draw the player's decorative model in place of the hitbox shape.
     *
     * Everything that styles the ship -- fill priority, parry pulse, stagger
     * flicker, dash flash, visual pivot/scale/offset -- has already been
     * written onto the hitbox ConvexShape by the time this runs. The model
     * borrows that transform and those colours, so no effect needs to know
     * the model exists. SFML's ConvexShape cannot draw a concave outline,
     * hence the pre-triangulated mesh and hand-built outline.
     */
     /**
      * @brief Plates, figures and canopy, through the hull's transform.
      *
      * @param over false = the pass under the hull, true = the pass above it
      *             plus the cockpit. The mesh comes from ship::liveryPass(),
      *             the same call the refit bay makes, so the editor cannot lie.
      *
      * TONE-inked parts take the hull's fill AS IT IS THIS FRAME -- parry
      * flash, heat tint, stagger flicker -- which is why this reads the
      * shape's current colours rather than the paint.
      */
    void drawLivery(const sf::ConvexShape& hull, const PlayerComponent& ps, bool over) {
        const auto& lv = ps.livery;
        if (lv.decals.empty() && lv.plates.empty()
            && (!over || lv.cockpit.style == ship::CockpitStyle::None)) return;

        m_liveryScratch.clear();
        ship::liveryPass(lv, { hull.getFillColor(), hull.getOutlineColor() }, over, m_liveryScratch);
        if (m_liveryScratch.empty()) return;

        const sf::Transform& tr = hull.getTransform();
        for (auto& v : m_liveryScratch) v.position = tr.transformPoint(v.position);
        m_window->draw(m_liveryScratch.data(), m_liveryScratch.size(), sf::PrimitiveType::Triangles);
    }

    /// Reused every frame by drawLivery: no allocation once warmed up.
    std::vector<sf::Vertex> m_liveryScratch;

    /// Outline miter cap, in multiples of thickness. ~11 degree spikes stay sharp.
    static constexpr float MITER_LIMIT = 10.f;

    void drawPlayerModel(const sf::ConvexShape& hull, const PlayerComponent& ps) {
        const sf::Transform& tr = hull.getTransform();
        const sf::Color fill = hull.getFillColor();

        sf::VertexArray mesh(sf::PrimitiveType::Triangles, ps.modelTris.size());
        for (std::size_t i = 0; i < ps.modelTris.size(); ++i)
            mesh[i] = { tr.transformPoint(ps.modelTris[i]), fill };
        m_window->draw(mesh);

        const float t = hull.getOutlineThickness();
        if (t <= 0.f) return;
        const sf::Color oc = hull.getOutlineColor();
        const auto& P = ps.modelOutline;
        const std::size_t n = P.size();
        if (n < 3) return;

        // Outline grows OUTWARD, like SFML's. Which side is out depends on winding.
        float area = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& a = P[i]; const auto& b = P[(i + 1) % n];
            area += a.x * b.y - b.x * a.y;
        }
        // Shoelace > 0 here means (e.y, -e.x) already points outward.
        const float side = (area > 0.f) ? 1.f : -1.f;

        const auto normalOf = [&](std::size_t i) {
            const sf::Vector2f e = P[(i + 1) % n] - P[i];
            const float l = std::max(0.0001f, std::sqrt(e.x * e.x + e.y * e.y));
            return sf::Vector2f(e.y / l, -e.x / l) * side;
            };

        // MITER joins -- the same construction SFML uses for ConvexShape
        // outlines. The earlier bevel join cut every spike tip flat, which on
        // a 2.5-7px outline read as a smoothed, blunt model. The miter is
        // capped so a needle-thin spike cannot grow a lance of outline.
        std::vector<sf::Vector2f> outer(n);
        for (std::size_t i = 0; i < n; ++i) {
            const sf::Vector2f n1 = normalOf((i + n - 1) % n);
            const sf::Vector2f n2 = normalOf(i);
            const float k = 1.f + (n1.x * n2.x + n1.y * n2.y);
            sf::Vector2f m = (n1 + n2) / std::max(k, 0.0001f);
            const float ml = std::sqrt(m.x * m.x + m.y * m.y);
            if (ml > MITER_LIMIT) m *= MITER_LIMIT / ml;
            outer[i] = P[i] + m * t;
        }

        sf::VertexArray line(sf::PrimitiveType::Triangles, n * 6);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const sf::Vector2f A = tr.transformPoint(P[i]), B = tr.transformPoint(P[j]);
            const sf::Vector2f C = tr.transformPoint(outer[j]), D = tr.transformPoint(outer[i]);
            line[i * 6 + 0] = { A, oc }; line[i * 6 + 1] = { B, oc }; line[i * 6 + 2] = { C, oc };
            line[i * 6 + 3] = { A, oc }; line[i * 6 + 4] = { C, oc }; line[i * 6 + 5] = { D, oc };
        }
        m_window->draw(line);
    }

    void drawNoseHeat(const sf::ConvexShape& hull, const PlayerComponent& ps, float heatT) {
        if (heatT <= 0.02f && !ps.weaponOverheated) return;

        // Heat shows where plasma actually leaves: every plasma muzzle on a
        // refit ship, the old nose point on a legacy one.
        const ship::KitProfile& kit = ps.kit;
        if (kit.valid && kit.primaryCount > 0) {
            for (int n = 0; n < kit.primaryCount; ++n) {
                const sf::Vector2f g = kit.gunMuzzlePx[kit.primarySlots[n]];
                drawHeatGlow(hull.getTransform().transformPoint({ g.x, g.y - 1.f }), ps, heatT,
                    kit.primaryCount > 1 ? 0.7f : 1.f);
            }
            return;
        }
        drawHeatGlow(hull.getTransform().transformPoint({ 0.f, -30.f }), ps, heatT, 1.f);
    }

    void drawHeatGlow(sf::Vector2f nose, const PlayerComponent& ps, float heatT, float sizeK) {

        float intensity = heatT;
        if (ps.weaponOverheated)  intensity = 0.85f + 0.15f * std::sin(m_magmaPulseTime * 28.f);
        else if (heatT > 0.75f)   intensity *= 0.88f + 0.12f * std::sin(m_magmaPulseTime * 18.f);

        const float baseR = (5.f + intensity * 16.f) * sizeK;
        struct Layer { float scale; float alpha; };
        const Layer layers[3] = { {1.00f, 0.28f}, {0.62f, 0.55f}, {0.30f, 0.95f} };

        for (const auto& L : layers) {
            const float r = baseR * L.scale;
            sf::CircleShape glow(r, 20);
            glow.setOrigin({ r, r });
            glow.setPosition(nose);
            sf::Color c = heatRamp(intensity);
            c.a = static_cast<uint8_t>(std::clamp(255.f * L.alpha * intensity, 0.f, 255.f));
            glow.setFillColor(c);
            m_window->draw(glow);
        }

        if (ps.weaponOverheated) {
            const float rr = (22.f + 4.f * std::sin(m_magmaPulseTime * 22.f)) * sizeK;
            sf::CircleShape ring(rr, 24);
            ring.setOrigin({ rr, rr });
            ring.setPosition(nose);
            ring.setFillColor(sf::Color::Transparent);
            ring.setOutlineThickness(2.5f);
            ring.setOutlineColor(sf::Color(255, 120, 40, 200));
            m_window->draw(ring);
        }
    }

    static sf::Color heatRamp(float t) {
        t = std::clamp(t, 0.f, 1.f);
        float r, g, b;
        if (t < 0.5f) {
            const float u = t / 0.5f;
            r = 255.f * u; g = 220.f + (150.f - 220.f) * u; b = 200.f + (40.f - 200.f) * u;
        }
        else {
            const float u = (t - 0.5f) / 0.5f;
            r = 255.f; g = 150.f + 105.f * u; b = 40.f + 195.f * u;
        }
        return sf::Color(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    }

    /**
     * @brief The lancer charging: both side spikes pour into the central one.
     * Lines from each side spike tip to the maw tip, flickering brighter and
     * thicker as it fills, and a bone-white core swelling at the maw. Locked:
     * full, with a red rim on the core -- the next frame is light.
     */
    void drawLancerCharge(const TransformComponent& tf, const enemyarch::ArchetypeDef& adef,
        float u, bool locked)
    {
        const sol::table& c = adef.config;
        const float sc = c["scale"].get_or(1.f);
        float mx = 0.f, my = -50.f, sx = 22.f, sy = -48.f;
        if (sol::optional<sol::table> m = c["lancer_maw"]) { mx = (*m)["x"].get_or(mx); my = (*m)["y"].get_or(my); }
        if (sol::optional<sol::table> s = c["lancer_spike"]) { sx = (*s)["x"].get_or(sx); sy = (*s)["y"].get_or(sy); }
        const float r = tf.rotation * 3.14159265f / 180.f;
        const float cr = std::cos(r), sr = std::sin(r);
        const auto w = [&](float x, float y) {
            x *= sc; y *= sc;
            return tf.position + sf::Vector2f(x * cr - y * sr, x * sr + y * cr);
            };
        const sf::Vector2f maw = w(mx, my), L = w(-sx, sy), Rt = w(sx, sy);
        const float flick = locked ? 1.f : 0.75f + 0.25f * std::sin(m_enemyAnimTime * 50.f);
        const sf::Color line(250, 244, 230, static_cast<uint8_t>((60.f + 180.f * u) * flick));
        const float lw = 1.f + 1.8f * u;
        drawBand(L, maw, lw, line, line);
        drawBand(Rt, maw, lw, line, line);
        // spike tips glow too, draining into the core
        const float tipS = 1.5f + 2.5f * (locked ? 0.3f : 1.f - u * 0.6f);
        const float coreS = locked ? 8.f + 1.5f * std::sin(m_enemyAnimTime * 40.f) : 2.f + 6.f * u;
        const auto diamond = [&](sf::Vector2f p, float s, sf::Color col) {
            sf::VertexArray d(sf::PrimitiveType::TriangleFan, 4);
            d[0] = sf::Vertex{ p + sf::Vector2f(0.f, -s), col };
            d[1] = sf::Vertex{ p + sf::Vector2f(s, 0.f), col };
            d[2] = sf::Vertex{ p + sf::Vector2f(0.f, s), col };
            d[3] = sf::Vertex{ p + sf::Vector2f(-s, 0.f), col };
            m_window->draw(d);
            };
        diamond(L, tipS, line);
        diamond(Rt, tipS, line);
        if (locked) diamond(maw, coreS + 3.f, sf::Color(255, 70, 50, 230));
        diamond(maw, coreS, sf::Color(250, 244, 230, 250));
    }

    /// Lancer beams: a red rim and a bone-white core that thins as it fades.
    /// A reflected beam (the player's) burns yellow, the parry colour.
    void drawBeams() {
        for (const auto& b : m_em->beams) {
            const float f = std::clamp(b.timer / std::max(0.001f, b.maxTimer), 0.f, 1.f);
            const sf::Color rim = b.reflected ? sf::Color(255, 220, 60, static_cast<uint8_t>(190 * f))
                                              : sf::Color(255, 70, 50, static_cast<uint8_t>(190 * f));
            drawBand(b.a, b.b, b.width * (1.1f + 0.9f * (1.f - f)), rim, rim);
            const sf::Color core(250, 244, 230, static_cast<uint8_t>(255 * f));
            drawBand(b.a, b.b, b.width * 0.45f * f + 1.f, core, core);
        }
    }

    void drawShockRings() const {
        for (const auto& r : m_em->shockRings) {
            const float u = std::clamp(1.f - (r.timer / std::max(0.0001f, r.maxTimer)), 0.f, 1.f);
            const float ease = 1.f - std::pow(1.f - u, 3.f);
            const float radius = r.startRadius + (r.maxRadius - r.startRadius) * ease;

            sf::CircleShape ring(radius, 48);
            ring.setOrigin({ radius, radius });
            ring.setPosition(r.position);
            ring.setFillColor(sf::Color::Transparent);

            sf::Color c = r.color;
            c.a = static_cast<uint8_t>(std::clamp(r.peakAlpha * (1.f - u) * (1.f - u), 0.f, 255.f));
            ring.setOutlineColor(c);
            ring.setOutlineThickness(r.thickness * (1.f - u * 0.7f));
            m_window->draw(ring);
        }
    }

    /// Baked detail (colours already in the vertices) at an entity's world
    /// transform, lerped toward white by `flash` -- the same maths the
    /// asteroid detail path uses, for anything else that carries a bake.
    void drawBakedDetail(const TransformComponent& tf, const std::vector<sf::Vertex>& src, float flash) {
        if (src.empty()) return;
        const float rad = tf.rotation * 3.14159f / 180.f;
        const float ca = std::cos(rad), sa = std::sin(rad);
        const float w = (flash > 0.f) ? std::clamp(flash / 0.16f, 0.f, 1.f) : 0.f;
        m_detailScratch.resize(src.size());
        for (size_t k = 0; k < src.size(); ++k) {
            const sf::Vector2f& p = src[k].position;
            m_detailScratch[k].position = { tf.position.x + (p.x * ca - p.y * sa),
                                            tf.position.y + (p.x * sa + p.y * ca) };
            sf::Color c = src[k].color;
            if (w > 0.f) {
                c.r = static_cast<uint8_t>(c.r + (255 - c.r) * w);
                c.g = static_cast<uint8_t>(c.g + (255 - c.g) * w);
                c.b = static_cast<uint8_t>(c.b + (255 - c.b) * w);
            }
            m_detailScratch[k].color = c;
        }
        m_window->draw(m_detailScratch.data(), m_detailScratch.size(), sf::PrimitiveType::Triangles);
    }

    /// Read a float from the `asteroid_visuals` table in Lua.
    /// Lua `asteroid_visuals` table, cached per config epoch (see LuaConfig.hpp).
    luacfg::Table m_cfgAsteroidVisuals{ "asteroid_visuals" };
    float acfg(const char* key, float def) const {
        return m_cfgAsteroidVisuals.get(m_lua, key, def);
    }

    /**
 * @brief Pop-and-fade state indicator above an enemy
 *
 * Overshoot-then-settle scale, then fade. The overshoot is what makes it
 * register in peripheral vision — a linear fade-in at this size is very easy
 * to miss in a busy fight.
 *
 * Icons are built from flat triangles and quads rather than text, so they
 * stay in the game's vocabulary and need no font.
 */
    void drawAlertIcon(sf::Vector2f pos, const EnemyComponent& ec) {
        const float t = std::clamp(ec.alertIconTimer / ec.alertIconDuration, 0.f, 1.f);
        const float age = 1.f - t;   // 0 at spawn -> 1 at end

        // Scale: overshoot to 1.35 in the first 18%, settle back to 1.0.
        float scale;
        if (age < 0.18f) {
            const float u = age / 0.18f;
            scale = 1.35f * (u * u * (3.f - 2.f * u));
        }
        else {
            const float u = (age - 0.18f) / 0.82f;
            scale = 1.35f - 0.35f * std::min(1.f, u * 3.f);
        }

        // Rise slightly as it fades.
        const float rise = 34.f + age * 12.f;
        const sf::Vector2f p(pos.x, pos.y - rise);

        const uint8_t alpha = static_cast<uint8_t>(
            std::clamp(255.f * std::min(1.f, t * 2.6f), 0.f, 255.f));

        sf::Color col;
        switch (ec.alertIcon) {
        case AlertIcon::Suspicion: col = sf::Color(255, 205, 55, alpha); break;
        case AlertIcon::Spotted:   col = sf::Color(255, 60, 45, alpha); break;
        case AlertIcon::Lost:      col = sf::Color(165, 170, 180, alpha); break;
        default: return;
        }

        const float s = 9.f * scale;

        if (ec.alertIcon == AlertIcon::Spotted) {
            // "!" — tapered bar plus a dot.
            sf::ConvexShape bar(4);
            bar.setPoint(0, { p.x - s * 0.30f, p.y - s * 1.25f });
            bar.setPoint(1, { p.x + s * 0.30f, p.y - s * 1.25f });
            bar.setPoint(2, { p.x + s * 0.17f, p.y + s * 0.25f });
            bar.setPoint(3, { p.x - s * 0.17f, p.y + s * 0.25f });
            bar.setFillColor(col);
            m_window->draw(bar);

            sf::CircleShape dot(s * 0.26f, 4);
            dot.setOrigin({ s * 0.26f, s * 0.26f });
            dot.setPosition({ p.x, p.y + s * 0.78f });
            dot.setRotation(sf::degrees(45.f));
            dot.setFillColor(col);
            m_window->draw(dot);
        }
        else if (ec.alertIcon == AlertIcon::Suspicion) {
            // "?" — an arc of chunky segments plus a dot. Drawn as discrete quads
            // so it stays angular rather than reading as a smooth curve.
            const int seg = 5;
            for (int k = 0; k < seg; ++k) {
                const float a = -2.5f + (k / float(seg - 1)) * 3.6f;
                const float r = s * 0.62f;
                const sf::Vector2f q(p.x + std::cos(a) * r,
                    p.y - s * 0.55f + std::sin(a) * r);
                sf::RectangleShape blk({ s * 0.30f, s * 0.30f });
                blk.setOrigin({ s * 0.15f, s * 0.15f });
                blk.setPosition(q);
                blk.setFillColor(col);
                m_window->draw(blk);
            }
            sf::RectangleShape stem({ s * 0.28f, s * 0.42f });
            stem.setOrigin({ s * 0.14f, 0.f });
            stem.setPosition({ p.x + s * 0.10f, p.y + s * 0.05f });
            stem.setFillColor(col);
            m_window->draw(stem);

            sf::RectangleShape dot({ s * 0.30f, s * 0.30f });
            dot.setOrigin({ s * 0.15f, s * 0.15f });
            dot.setPosition({ p.x + s * 0.10f, p.y + s * 0.80f });
            dot.setFillColor(col);
            m_window->draw(dot);
        }
        else {
            // Lost — three fading dots, like a trailing "...".
            for (int k = 0; k < 3; ++k) {
                sf::Color c = col;
                c.a = static_cast<uint8_t>(col.a * (1.f - k * 0.28f));
                sf::RectangleShape d({ s * 0.26f, s * 0.26f });
                d.setOrigin({ s * 0.13f, s * 0.13f });
                d.setPosition({ p.x + (k - 1) * s * 0.5f, p.y });
                d.setFillColor(c);
                m_window->draw(d);
            }
        }
    }

    EntityManager* m_em = nullptr;
    sf::RenderTarget* m_window = nullptr;   ///< ctx.drawTarget(): the window, or a hidden world's texture
    sol::state* m_lua = nullptr;
    uint32_t m_playerEntityId = 0;
    /// Scratch for transforming baked detail vertices into world space.
    /// A member so the per-frame cost is a resize, not an allocation.
    std::vector<sf::Vertex> m_detailScratch;

    float m_magmaPulseTime = 0.f;
    const enemyarch::EnemyRegistry* m_enemyReg = nullptr;   // added for archetype access
    std::vector<sf::Vector2f> m_outlineScratch;             // reused across enemies
    std::vector<sf::Vector2f> m_lampScratch;                // mode lamp pulse ring
    float m_chaos = 0.f;                                    // longest feral timer this frame
};