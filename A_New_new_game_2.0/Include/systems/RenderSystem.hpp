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
 * @author Oleg Ivakhiv
 * @version 1.8 -- frenzy corona and colour override
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "core/EnemyArchetypes.hpp"        // added for archetype registry and geometry
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
        m_window = ctx.window;
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
        m_magmaPulseTime += dt;

        for (size_t i = 0; i < m_em->renders.size(); ++i) {
            auto& tf = m_em->transforms[i];
            auto& rd = m_em->renders[i];

            BodyUserData* ud = (BodyUserData*)b2Body_GetUserData(m_em->physics[i].bodyId);
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
                if (m_em->healths[i].isExplosive) {
                    drawMagmatic(i, tf, rd, detailFor(i, tf.entityId));
                }
                else {
                    rd.shape.setPosition(tf.position);
                    rd.shape.setRotation(sf::degrees(tf.rotation));
                    m_window->draw(rd.shape);

                    // Optional flat faceting. Off by default; see drawFacets().
                    if (acfg("facets", 0.f) > 0.5f) {
                        drawFacets(rd.shape, detailFor(i, tf.entityId));
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
                if (playerStats.isParrying) {
                    float pulse = (std::sin(playerStats.parryTimer * 30.f) + 1.f) / 2.f;
                    rd.shape.setOutlineThickness(2.0f + pulse * 5.0f);
                    rd.shape.setOutlineColor(sf::Color(0, 255, 255,
                        200 + static_cast<uint8_t>(55 * pulse)));
                }
                else {
                    rd.shape.setOutlineThickness(2.5f);
                    sol::table luaOutline = (*m_lua)["outline_color"];
                    rd.shape.setOutlineColor(sf::Color(
                        luaOutline["r"].get_or(255),
                        luaOutline["g"].get_or(255),
                        luaOutline["b"].get_or(255)));
                }

                // ---- Fill, in priority order ----
                const float heatT = playerStats.weaponHeat /
                    std::max(1.f, playerStats.maxWeaponHeat);

                if (playerStats.parryFlashTimer > 0.f) {
                    const float w = std::clamp(playerStats.parryFlashTimer / 0.25f, 0.f, 1.f);
                    sol::table clr = (*m_lua)["color"];
                    const float br = clr["r"].get_or(40), bg = clr["g"].get_or(100), bb = clr["b"].get_or(255);
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(br + (255.f - br) * w),
                        static_cast<uint8_t>(bg + (255.f - bg) * w),
                        static_cast<uint8_t>(bb + (255.f - bb) * w)));
                    rd.shape.setOutlineThickness(2.5f + 6.f * w);
                    rd.shape.setOutlineColor(sf::Color(255, 255, 255,
                        static_cast<uint8_t>(255 * w)));
                }
                else if (playerStats.staggerTimer > 0.f || playerStats.staggerRecoverTimer > 0.f) {
                    const float flicker = 0.6f + 0.4f * std::sin(m_magmaPulseTime * 40.f);
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(140 * flicker),
                        static_cast<uint8_t>(60 * flicker),
                        static_cast<uint8_t>(60 * flicker)));
                }
                else if (playerStats.dashCooldown > (playerStats.dashMaxCooldown - 0.15f)) {
                    sol::table flash = (*m_lua)["dash_flash_color"];
                    rd.shape.setFillColor(sf::Color(
                        flash["r"].get_or(100), flash["g"].get_or(255),
                        flash["b"].get_or(255), flash["a"].get_or(200)));
                }
                else {
                    sol::table clr = (*m_lua)["color"];
                    const float br = clr["r"].get_or(40), bg = clr["g"].get_or(100), bb = clr["b"].get_or(255);
                    const float k = heatT * heatT * 0.55f;
                    rd.shape.setFillColor(sf::Color(
                        static_cast<uint8_t>(br + (200.f - br) * k),
                        static_cast<uint8_t>(bg + (90.f - bg) * k),
                        static_cast<uint8_t>(bb + (60.f - bb) * k)));
                }

                // ---- Parry arcs ----
                if (playerStats.parryAnimTimer > 0) {
                    float parryAnimDuration = (*m_lua)["parry_anim_duration"].get_or(0.6f);
                    float t = playerStats.parryAnimTimer / parryAnimDuration;
                    uint8_t alpha = static_cast<uint8_t>(t * 200);
                    sf::Color arcCol(0, 255, 220, alpha);

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
                m_window->draw(rd.shape);

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

                    const sf::Color barrelCol(
                        255,
                        static_cast<uint8_t>(std::clamp(200.f - 120.f * heat, 0.f, 255.f)),
                        static_cast<uint8_t>(std::clamp(150.f - 130.f * heat, 0.f, 255.f)),
                        static_cast<uint8_t>(std::clamp(200.f + 55.f * heat, 0.f, 255.f)));

                    // Barrel: a bar running forward (-Y local) from the mount.
                    const float bl = ts * (2.1f + 0.25f * heat);
                    const float bw = ts * 0.28f;
                    sf::VertexArray barrel(sf::PrimitiveType::TriangleStrip, 4);
                    barrel[0] = sf::Vertex{ { -bw, 0.f },  barrelCol };
                    barrel[1] = sf::Vertex{ {  bw, 0.f },  barrelCol };
                    barrel[2] = sf::Vertex{ { -bw, -bl },  barrelCol };
                    barrel[3] = sf::Vertex{ {  bw, -bl },  barrelCol };
                    m_window->draw(barrel, tst);

                    // Housing: an octagon, so the gun reads as a separate
                    // machine bolted on rather than part of the hull plating.
                    sf::VertexArray housing(sf::PrimitiveType::TriangleFan, 10);
                    housing[0] = sf::Vertex{ { 0.f, 0.f },
                        sf::Color(std::min(255, fill.r + 40),
                                  std::min(255, fill.g + 30),
                                  std::min(255, fill.b + 30)) };
                    for (int k = 0; k <= 8; ++k) {
                        const float a = k * 3.14159f * 2.f / 8.f;
                        housing[k + 1] = sf::Vertex{
                            { std::cos(a) * ts, std::sin(a) * ts }, outlineColor };
                    }
                    m_window->draw(housing, tst);

                    // Muzzle flash.
                    if (ec.turretMuzzleFlash > 0.f) {
                        const float u = ec.turretMuzzleFlash / 0.11f;
                        sf::VertexArray fl(sf::PrimitiveType::TriangleFan, 4);
                        const sf::Color fc(255, 230, 150,
                            static_cast<uint8_t>(230 * u));
                        fl[0] = sf::Vertex{ { 0.f, -bl }, fc };
                        fl[1] = sf::Vertex{ { -ts * 0.8f * u, -bl - ts * 0.6f }, sf::Color(255,180,80,0) };
                        fl[2] = sf::Vertex{ { 0.f, -bl - ts * 2.2f * u },        sf::Color(255,200,90,0) };
                        fl[3] = sf::Vertex{ {  ts * 0.8f * u, -bl - ts * 0.6f }, sf::Color(255,180,80,0) };
                        m_window->draw(fl, tst);
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
        pruneDetailCache();
    }

    /**
     * @brief Screen-space overlay pass (full-screen flashes)
     * Called by SystemManager after switching to the default view.
     */
    void drawScreenSpace() {
        if (!m_em || !m_window || m_em->screenFlashes.empty()) return;

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
                static_cast<uint8_t>((lit ? 190.f : 90.f) * fade)));
            m_window->draw(sweep);

            // The boundary itself, held faintly all the time, so the edge is
            // readable even between sweeps.
            sf::CircleShape edge(mine.mineTrigger, 44);
            edge.setOrigin({ mine.mineTrigger, mine.mineTrigger });
            edge.setPosition(tf.position);
            edge.setFillColor(sf::Color::Transparent);
            edge.setOutlineThickness(1.f);
            edge.setOutlineColor(sf::Color(blink.r, blink.g, blink.b,
                static_cast<uint8_t>(lit ? 70 + 60 * on : 34)));
            m_window->draw(edge);
        }

        // ---- Body ----
        rd.shape.setPosition(tf.position);
        rd.shape.setRotation(sf::degrees(tf.rotation));
        rd.shape.setOutlineColor(sf::Color(blink.r, blink.g, blink.b,
            static_cast<uint8_t>(140 + 115 * on)));
        rd.shape.setFillColor(lit
            ? sf::Color(static_cast<uint8_t>(60 + 120 * on), 34, 30)
            : sf::Color(64, 58, 56));
        m_window->draw(rd.shape);

        // ---- Lamp ----
        // A hard dot at the centre. The hull outline can be lost against a
        // bright background; this cannot.
        if (on > 0.5f) {
            const float s = lit ? 5.f + 3.f * urgency : 3.f;
            sf::CircleShape lamp(s, 8);
            lamp.setOrigin({ s, s });
            lamp.setPosition(tf.position);
            lamp.setFillColor(sf::Color(blink.r, blink.g, blink.b, 245));
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
        const float coreScale = acfg("magma_core_size", 0.30f);
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
                m_em->nextEntityId++,
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
    void drawNoseHeat(const sf::ConvexShape& hull, const PlayerComponent& ps, float heatT) {
        if (heatT <= 0.02f && !ps.weaponOverheated) return;

        const sf::Vector2f nose = hull.getTransform().transformPoint({ 0.f, -30.f });

        float intensity = heatT;
        if (ps.weaponOverheated)  intensity = 0.85f + 0.15f * std::sin(m_magmaPulseTime * 28.f);
        else if (heatT > 0.75f)   intensity *= 0.88f + 0.12f * std::sin(m_magmaPulseTime * 18.f);

        const float baseR = 5.f + intensity * 16.f;
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
            const float rr = 22.f + 4.f * std::sin(m_magmaPulseTime * 22.f);
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

    /// Read a float from the `asteroid_visuals` table in Lua.
    float acfg(const char* key, float def) const {
        if (!m_lua) return def;
        sol::optional<sol::table> v = (*m_lua)["asteroid_visuals"];
        if (!v) return def;
        return (*v)[key].get_or(def);
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
    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    uint32_t m_playerEntityId = 0;
    float m_magmaPulseTime = 0.f;
    const enemyarch::EnemyRegistry* m_enemyReg = nullptr;   // added for archetype access
    std::vector<sf::Vector2f> m_outlineScratch;             // reused across enemies
};