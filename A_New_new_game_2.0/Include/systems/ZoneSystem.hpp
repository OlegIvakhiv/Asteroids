/**
 * @file ZoneSystem.hpp
 * @brief Applies the active zone, and owns the background prop layer.
 *
 * ============================================================================
 * WHAT IT DOES
 * ============================================================================
 * Two jobs, at two different points of the frame:
 *
 *   applyIfDirty()  early    the zone changed -- regenerate the starfield at
 *                            the new density and tint, and reseed the props
 *   update(dt)      world    wrap the prop pool around the camera and draw it
 *
 * Everything else a zone controls is READ where it is used rather than pushed
 * from here: EnemySystem reads the rock and faction tables, SpaceDustSystem
 * reads the dust tint, game.cpp reads the void colour. A zone is data, and
 * data is better pulled than pushed -- one push path per consumer would be
 * five things to keep in sync.
 *
 * ============================================================================
 * THE PARALLAX, WRITTEN OUT
 * ============================================================================
 * A prop at depth d belongs to a layer whose camera moves at d times the real
 * camera's speed. So for a prop at layer position P and a camera at C:
 *
 *      screen position = P - C*d
 *
 * The props are drawn with the WORLD view, which already subtracts C, so the
 * world coordinate to hand the renderer is:
 *
 *      world = P + C*(1 - d)
 *
 * and P is wrapped toroidally around C*d -- the layer's own centre -- so a
 * fixed pool covers infinite space. Same trick as SpaceDustSystem, one term
 * heavier because dust sits at d = 1.
 *
 * depth is clamped away from 1.0 in zones.lua on purpose. A prop at d = 1 is
 * welded to the world and reads as cover the player then discovers they can
 * fly straight through, which is worse than no prop at all.
 *
 * ============================================================================
 * COST
 * ============================================================================
 * Props are NOT entities. They have no body, no health and no component rows,
 * so they never touch EntityManager's 8192 reserve and never appear in the
 * dev menu's entity budget. The whole layer is two draw calls -- one vertex
 * array of triangles for the fills, one of lines for the outlines -- which is
 * why they are capped by fill rate (zonearch::MAX_PROPS) rather than by
 * anything structural.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/ZoneArchetypes.hpp"
#include "utils/CitadelModel.hpp"      // builtin landmark: the Rakshari citadel
#include <SFML/Graphics.hpp>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <memory>

class ZoneSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_window = ctx.drawTarget();
        m_view = ctx.gameView;
        m_zone = ctx.zone;

        m_rng.seed(std::random_device{}());
        m_junk.clear();
        m_landmarks.clear();

        // A restart rebuilds every system, but the zone itself survives it --
        // so force one apply so the starfield and props match whatever zone
        // is live rather than whatever the last run left behind.
        if (m_zone) m_zone->dirty = true;
    }

    /**
     * @brief Re-apply the zone's visuals if it changed. Call EARLY in a frame.
     *
     * Early matters: the starfield is drawn before the world-space pass, so
     * regenerating it from update() would leave one frame of the old sky under
     * the new zone. It is a single frame, but it is the frame the player is
     * looking at when they switch.
     */
    void applyIfDirty() {
        if (!m_zone || !m_zone->dirty) return;
        m_zone->dirty = false;

        const zonearch::ZoneDef* z = m_zone->def();
        if (!z) return;   // zones.lua missing -- leave the pre-zone look alone

        applySky(*z);
        m_citadelRT.clear();   // landmark slots are about to mean different props
        seedLayer(z->junk, m_junk);
        seedLayer(z->landmarks, m_landmarks);
    }

    /**
     * @brief Wrap and draw the prop layer. Call with the WORLD view installed.
     *
     * Drawn from this system rather than RenderSystem because props are not
     * entities and have no component rows for RenderSystem to walk.
     */
    void update(float dt) override {
        if (!m_em || !m_window) return;
        m_time += dt;
        const sf::Vector2f cam = m_view ? m_view->getCenter() : sf::Vector2f(0.f, 0.f);

        // Farthest first, so a base sits BEHIND the scrap drifting past it.
        step(m_landmarks, cam, dt);
        step(m_junk, cam, dt);
        draw(m_landmarks, cam);
        draw(m_junk, cam);
    }

    /// Live prop count, for the dev stats panel.
    size_t propCount() const { return m_junk.size() + m_landmarks.size(); }

private:
    struct Prop {
        const zonearch::PropDef* def = nullptr;
        sf::Vector2f layerPos;      ///< Position in its parallax layer, NOT world
        float depth = 0.4f;
        float scale = 1.f;          ///< layer roll x the prop's own size class
        float rotation = 0.f;       ///< degrees
        float spin = 0.f;           ///< deg/sec, signed
        float pad = 1.6f;           ///< its layer's wrap padding
        float alpha = 0.5f;         ///< from its layer, by depth
    };

    // ========================================================================
    // APPLY
    // ========================================================================

    /**
     * @brief Rebuild the starfield at the zone's density and tint.
     *
     * The tint is applied AFTER generation rather than by changing
     * initBackground(), so EntityManager keeps one way of making stars and the
     * zone layer stays additive. Multiplying rather than replacing preserves
     * the per-star alpha and size variation that sells the depth.
     *
     * initBackground() also rewrites starFieldSize, which CameraSystem's
     * makeStarView() reads -- so the 1.5x coverage margin is re-asserted here
     * on every zone change and the star view can never drift out of sync.
     */
    void applySky(const zonearch::ZoneDef& z) {
        if (!m_em || !m_window) return;

        m_em->initBackground(m_window->getSize(), z.starCount);

        const float tr = z.starTint.r / 255.f;
        const float tg = z.starTint.g / 255.f;
        const float tb = z.starTint.b / 255.f;

        for (auto& s : m_em->stars) {
            s.color = sf::Color(
                static_cast<std::uint8_t>(s.color.r * tr),
                static_cast<std::uint8_t>(s.color.g * tg),
                static_cast<std::uint8_t>(s.color.b * tb),
                static_cast<std::uint8_t>(std::clamp(s.color.a * z.starAlphaScale, 0.f, 255.f)));
        }
    }

    void seedLayer(const zonearch::PropLayer& L, std::vector<Prop>& out) {
        out.clear();
        if (!m_zone || !m_zone->registry || !L.active()) return;

        const sf::Vector2f cam = m_view ? m_view->getCenter() : sf::Vector2f(0.f, 0.f);
        const sf::Vector2f half = halfExtent(L.pad);

        std::uniform_real_distribution<float> dDepth(L.depthMin, L.depthMax);
        std::uniform_real_distribution<float> dScale(L.scaleMin, L.scaleMax);
        std::uniform_real_distribution<float> dRot(0.f, 360.f);
        std::uniform_real_distribution<float> dSpin(-L.spinMax, L.spinMax);
        std::uniform_real_distribution<float> dx(-half.x, half.x);
        std::uniform_real_distribution<float> dy(-half.y, half.y);
        std::uniform_int_distribution<int>    dPick(0, static_cast<int>(L.props.size()) - 1);

        out.reserve(static_cast<size_t>(L.count));
        for (int i = 0; i < L.count; ++i) {
            Prop p;
            p.def = m_zone->registry->propById(L.props[static_cast<size_t>(dPick(m_rng))]);
            if (!p.def) continue;

            p.depth = std::clamp(dDepth(m_rng), 0.02f, 1.f);
            // Alpha is the LAYER's, lerped across its own depth range, not one
            // global curve. A landmark is meant to be looked at; junk is not.
            {
                const float span = std::max(0.0001f, L.depthMax - L.depthMin);
                const float t = std::clamp((p.depth - L.depthMin) / span, 0.f, 1.f);
                p.alpha = L.alphaFar + (L.alphaNear - L.alphaFar) * t;
            }
            // The layer rolls a size, the PROP scales it by its own class. A
            // chunk stays a chunk and a hulk stays a hulk no matter what the
            // roll gives, which is what keeps the silhouettes distinguishable.
            p.scale = dScale(m_rng) * p.def->scale;
            p.rotation = dRot(m_rng);
            p.spin = dSpin(m_rng);
            p.pad = L.pad;
            // Seeded in LAYER space, around this prop's own layer centre.
            p.layerPos = cam * p.depth + sf::Vector2f(dx(m_rng), dy(m_rng));
            out.push_back(p);
        }
    }

    /// Spin and wrap one layer. Split from draw() so the farther layer can be
    /// stepped and drawn first without duplicating either.
    void step(std::vector<Prop>& layer, sf::Vector2f cam, float dt) {
        for (auto& p : layer) {
            p.rotation += p.spin * dt;

            const sf::Vector2f half = halfExtent(p.pad);
            std::uniform_real_distribution<float> jx(-half.x, half.x);
            std::uniform_real_distribution<float> jy(-half.y, half.y);

            // ---- Toroidal wrap around this prop's OWN layer centre ----
            const sf::Vector2f layerCenter = cam * p.depth;
            sf::Vector2f d = p.layerPos - layerCenter;

            if (d.x > half.x) { p.layerPos.x -= half.x * 2.f; p.layerPos.y = layerCenter.y + jy(m_rng); }
            else if (d.x < -half.x) { p.layerPos.x += half.x * 2.f; p.layerPos.y = layerCenter.y + jy(m_rng); }

            d = p.layerPos - layerCenter;
            if (d.y > half.y) { p.layerPos.y -= half.y * 2.f; p.layerPos.x = layerCenter.x + jx(m_rng); }
            else if (d.y < -half.y) { p.layerPos.y += half.y * 2.f; p.layerPos.x = layerCenter.x + jx(m_rng); }
        }
    }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw(const std::vector<Prop>& layer, sf::Vector2f cam) const {
        if (layer.empty()) return;

        sf::VertexArray fills(sf::PrimitiveType::Triangles);
        sf::VertexArray lines(sf::PrimitiveType::Lines);

        // One slow pulse shared by every glow in the zone. Per-prop phases
        // would read as flickering rather than as machinery breathing.
        const float pulse = 0.82f + 0.18f * std::sin(m_time * 1.7f);

        for (size_t pi = 0; pi < layer.size(); ++pi) {
            const auto& p = layer[pi];
            if (!p.def || !p.def->valid()) continue;

            // A builtin model draws itself, now, in layer order.
            if (!p.def->builtin.empty()) {
                if (p.def->builtin == "RAKSHARI_CITADEL") drawCitadel(p, pi, cam);
                continue;
            }

            // Far props are smaller and dimmer. Drawing a distant prop at full
            // size but slow motion is the classic broken-parallax look.
            const float sizeF = p.scale * (0.5f + 0.5f * p.depth);
            const float alphaF = p.alpha;

            const float rad = p.rotation * 3.14159265f / 180.f;
            const float ca = std::cos(rad), sa = std::sin(rad);
            const sf::Vector2f origin = p.layerPos + cam * (1.f - p.depth);

            auto place = [&](sf::Vector2f local, float grow = 1.f) {
                const sf::Vector2f s = local * (sizeF * grow);
                return origin + sf::Vector2f(s.x * ca - s.y * sa, s.x * sa + s.y * ca);
                };

            // Parts draw in authored order: rock, then the plates bolted over
            // it, then what is welded on top. Painter's algorithm, because
            // there is no depth buffer and the author already knows the order.
            for (const auto& part : p.def->parts) {
                const size_t n = part.points.size();

                // ---- Halo, behind its own part ----
                // Three scaled copies at falling alpha. Crude next to a real
                // bloom and completely invisible as a fake at this distance,
                // which is the entire budget it deserves.
                if (part.glow > 0.f && !part.tris.empty()) {
                    for (int ring = 3; ring >= 1; --ring) {
                        const float grow = 1.f + 0.16f * ring;
                        const float a = alphaF * part.glow * pulse * (0.18f / ring);
                        const sf::Color gc = fade(part.line, std::min(1.f, a));
                        for (size_t i = 0; i + 2 < part.tris.size(); i += 3) {
                            fills.append(sf::Vertex{ place(part.tris[i], grow),     gc });
                            fills.append(sf::Vertex{ place(part.tris[i + 1], grow), gc });
                            fills.append(sf::Vertex{ place(part.tris[i + 2], grow), gc });
                        }
                    }
                }

                if (part.filled && !part.tris.empty()) {
                    const float fa = (part.glow > 0.f) ? std::min(1.f, alphaF * pulse) : alphaF;
                    const sf::Color fc = fade(part.fill, fa);
                    for (size_t i = 0; i + 2 < part.tris.size(); i += 3) {
                        fills.append(sf::Vertex{ place(part.tris[i]),     fc });
                        fills.append(sf::Vertex{ place(part.tris[i + 1]), fc });
                        fills.append(sf::Vertex{ place(part.tris[i + 2]), fc });
                    }
                }

                if (part.outlined) {
                    const sf::Color lc = fade(part.line, alphaF);
                    for (size_t i = 0; i < n; ++i) {
                        lines.append(sf::Vertex{ place(part.points[i]), lc });
                        lines.append(sf::Vertex{ place(part.points[(i + 1) % n]), lc });
                    }
                }
            }
        }

        // Two calls, not one: a vertex array carries a single primitive type,
        // so fills and outlines cannot share one. Still O(1) in prop count.
        if (fills.getVertexCount()) m_window->draw(fills);
        if (lines.getVertexCount()) m_window->draw(lines);
    }

    // ========================================================================
    // BUILTIN: RAKSHARI SCRAP CITADEL
    // ========================================================================
    /**
     * @brief Draw one citadel prop through its own render texture.
     *
     * WHY A RENDER TEXTURE. The landmark layer fades a prop to 34-50% alpha.
     * Fading every triangle instead would let each layer show through the
     * one above it -- the rock through the armour, the chains through the
     * plates -- and the fortress would read as a pile of glass. So the model
     * is drawn opaque, then the finished image is faded as one sheet.
     *
     * The texture holds PREMULTIPLIED colour (anything drawn with normal
     * alpha blending onto a transparent clear ends up that way), so it goes
     * to the window with One / OneMinusSrcAlpha and a (A,A,A,A) tint; plain
     * alpha blending would darken every soft edge twice.
     *
     * Rendered at 2x and filtered down, which is the anti-aliasing: the page
     * is full of 0.6px detail. Re-rendered at 30 Hz -- it is a distant,
     * slow thing, and the sprite itself still moves every frame.
     */
    void drawCitadel(const Prop& p, size_t slot, sf::Vector2f cam) const {
        const float sizeF = p.scale * (0.5f + 0.5f * p.depth);
        const sf::Vector2f origin = p.layerPos + cam * (1.f - p.depth);

        if (!m_citadel) m_citadel = std::make_unique<citadel::Model>();
        const float radiusPx = m_citadel->extent() * sizeF;

        if (m_citadelRT.size() <= slot) m_citadelRT.resize(slot + 1);
        auto& cr = m_citadelRT[slot];

        // Off screen: skip it, and give the texture back.
        const sf::Vector2f vs = m_view ? m_view->getSize() : sf::Vector2f(m_window->getSize());
        const sf::Vector2f d = origin - cam;
        if (std::sqrt(d.x * d.x + d.y * d.y) > 0.5f * std::sqrt(vs.x * vs.x + vs.y * vs.y) + radiusPx) {
            cr.reset();
            return;
        }

        const float ss = 2.f;
        const unsigned need = std::min(4096u, static_cast<unsigned>(std::ceil(radiusPx * 2.f * ss)) + 4u);
        if (!cr) cr = std::make_unique<CitadelRT>();
        if (!cr->failed && cr->rt.getSize().x < need) {
            const unsigned sz = ((need + 255u) / 256u) * 256u;
            if (cr->rt.resize({ sz, sz })) {
                cr->rt.setSmooth(true);
                cr->lastT = -1.f;
            }
            else {
                cr->failed = true;   // no FBO: fall back to direct drawing below
            }
        }

        const float alpha = std::clamp(p.alpha, 0.f, 1.f);

        if (cr->failed) {
            // Correct shapes, slightly glassy layering -- still better than nothing.
            m_citadelMesh.v.clear();
            m_citadel->frame(m_time, p.def->builtinGlow, p.def->builtinTrophies, m_citadelMesh);
            for (auto& v : m_citadelMesh.v) v.color.a = static_cast<std::uint8_t>(v.color.a * alpha);
            sf::Transform xf;
            xf.translate(origin).rotate(sf::degrees(p.rotation)).scale({ sizeF, sizeF });
            m_window->draw(m_citadelMesh.v.data(), m_citadelMesh.v.size(),
                sf::PrimitiveType::Triangles, sf::RenderStates(xf));
            return;
        }

        const sf::Vector2u rs = cr->rt.getSize();
        if (cr->lastT < 0.f || m_time - cr->lastT >= 1.f / 30.f || std::fabs(cr->lastScale - sizeF) > 1e-4f) {
            cr->lastT = m_time;
            cr->lastScale = sizeF;
            m_citadelMesh.v.clear();
            m_citadel->frame(m_time, p.def->builtinGlow, p.def->builtinTrophies, m_citadelMesh);

            cr->rt.clear(sf::Color::Transparent);
            const float k = sizeF * ss;
            cr->rt.setView(sf::View({ 0.f, 0.f }, { rs.x / k, rs.y / k }));
            cr->rt.draw(m_citadelMesh.v.data(), m_citadelMesh.v.size(), sf::PrimitiveType::Triangles);
            cr->rt.display();
        }

        sf::Sprite spr(cr->rt.getTexture());
        spr.setOrigin({ rs.x * 0.5f, rs.y * 0.5f });
        spr.setPosition(origin);
        spr.setRotation(sf::degrees(p.rotation));
        spr.setScale({ 1.f / ss, 1.f / ss });
        const std::uint8_t a8 = static_cast<std::uint8_t>(alpha * 255.f);
        spr.setColor(sf::Color(a8, a8, a8, a8));
        m_window->draw(spr, sf::RenderStates(
            sf::BlendMode(sf::BlendMode::Factor::One, sf::BlendMode::Factor::OneMinusSrcAlpha)));
    }

    struct CitadelRT {
        sf::RenderTexture rt;
        float lastT = -1.f;
        float lastScale = 0.f;
        bool  failed = false;
    };

    static sf::Color fade(sf::Color c, float f) {
        return sf::Color(c.r, c.g, c.b,
            static_cast<std::uint8_t>(std::clamp(c.a * f, 0.f, 255.f)));
    }

    /// Half-size of the region a layer's props are kept inside. The padding
    /// covers zoom-out and the dev menu's view scale, which can show 2x the
    /// normal world; a landmark layer passes a larger one so a station never
    /// recycles anywhere the player could see it happen.
    sf::Vector2f halfExtent(float pad = 1.6f) const {
        sf::Vector2f size = m_view ? m_view->getSize()
            : sf::Vector2f(m_window->getSize());
        return { size.x * 0.5f * pad, size.y * 0.5f * pad };
    }

    EntityManager* m_em = nullptr;
    sf::RenderTarget* m_window = nullptr;   ///< ctx.drawTarget(): the window, or a hidden world's texture
    sf::View* m_view = nullptr;
    zonearch::ZoneState* m_zone = nullptr;

    std::vector<Prop> m_junk;        ///< Near layer: drifting scrap
    std::vector<Prop> m_landmarks;   ///< Far layer: stations and bases

    // Citadel: one shared model, one texture per on-screen landmark slot.
    // mutable: draw() is const, and these are caches, not state.
    mutable std::unique_ptr<citadel::Model>           m_citadel;
    mutable std::vector<std::unique_ptr<CitadelRT>>   m_citadelRT;
    mutable fieldgeom::Mesh                           m_citadelMesh;   ///< reused, keeps capacity
    float m_time = 0.f;              ///< Drives the shared glow pulse
    std::mt19937 m_rng;
};