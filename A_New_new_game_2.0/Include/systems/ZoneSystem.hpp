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
#include "utils/LevelGen.hpp"          // in a level: blend across sub-zone borders
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
        m_level = ctx.level;

        m_rng.seed(std::random_device{}());
        m_junk.clear();
        m_landmarks.clear();
        m_pools.clear();
        m_levelKey = 0;
        if (m_zone) m_zone->blend.active = false;

        // A restart rebuilds every system, but the zone itself survives it --
        // so force one apply so the starfield and props match whatever zone
        // is live rather than whatever the last run left behind.
        if (m_zone) m_zone->dirty = true;
        m_applied.valid = false;   // the rebuild may have wiped the starfield: repaint it
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
        if (inLevel()) { applyLevel(); return; }
        if (m_zone) m_zone->blend.active = false;
        if (m_wasLevel) {   // left a level: everything comes back the plain way
            m_wasLevel = false;
            m_pools.clear();
            m_citadelRT.clear();
            m_applied.valid = false;
            if (m_zone) m_zone->dirty = true;
        }
        if (!m_zone || !m_zone->dirty) return;
        m_zone->dirty = false;

        const zonearch::ZoneDef* z = m_zone->def();
        if (!z) return;   // zones.lua missing -- leave the pre-zone look alone

        // SUB-ZONES. Crossing between two sub-zones of one archetype (the
        // same `sky_group`) keeps the sky and any prop layer that did not
        // change -- otherwise every cell border of a level would re-roll the
        // starfield and pop the junk around the player.
        // A reload (F5) invalidates every PropDef pointer the live props hold,
        // so after one everything is applied from scratch.
        const uint32_t gen = m_zone->registry ? m_zone->registry->generation() : 0;
        const bool was = m_applied.valid && m_applied.generation == gen;
        const bool sameSky = was && m_applied.sky == skyGroup(*z)
            && m_applied.stars == z->starCount && m_applied.tint == z->starTint;
        if (!sameSky) applySky(*z);
        const bool anchorMoved = z->landmarks.anchored
            && (m_applied.hasAnchor != m_zone->hasAnchor || m_applied.anchor != m_zone->anchor);
        if (!was || !sameLayer(m_applied.landmarks, z->landmarks) || anchorMoved) {
            m_citadelRT.clear();   // landmark slots are about to mean different props
            seedLayer(z->landmarks, m_landmarks);
        }
        if (!was || !sameLayer(m_applied.junk, z->junk)) seedLayer(z->junk, m_junk);
        m_applied.valid = true;
        m_applied.generation = gen;
        m_applied.sky = skyGroup(*z);
        m_applied.stars = z->starCount;
        m_applied.tint = z->starTint;
        m_applied.junk = z->junk;
        m_applied.landmarks = z->landmarks;
        m_applied.hasAnchor = m_zone->hasAnchor;
        m_applied.anchor = m_zone->anchor;
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

        if (inLevel()) { updateLevel(cam, dt); return; }

        // Farthest first, so a base sits BEHIND the scrap drifting past it.
        step(m_landmarks, cam, dt);
        step(m_junk, cam, dt);
        draw(m_landmarks, cam);
        draw(m_junk, cam);
    }

    /// Live prop count, for the dev stats panel.
    size_t propCount() const {
        size_t n = m_junk.size() + m_landmarks.size();
        for (const auto& p : m_pools) n += p.props.size();
        return n;
    }

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
        bool  anchored = false;     ///< stands over ZoneState::anchor, never wraps
    };

    // ========================================================================
    // APPLY
    // ========================================================================

    static std::string skyGroup(const zonearch::ZoneDef& z) {
        return z.config.valid() ? z.config["sky_group"].get_or(z.key) : z.key;
    }
    static bool sameLayer(const zonearch::PropLayer& a, const zonearch::PropLayer& b) {
        return a.count == b.count && a.props == b.props && a.depthMin == b.depthMin
            && a.depthMax == b.depthMax && a.scaleMin == b.scaleMin && a.scaleMax == b.scaleMax;
    }

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
        const sf::Vector2f cam = m_view ? m_view->getCenter() : sf::Vector2f(0.f, 0.f);
        seedLayerAt(L, out, cam, L.anchored && m_zone && m_zone->hasAnchor,
            m_zone ? m_zone->anchor : sf::Vector2f());
    }

    void seedLayerAt(const zonearch::PropLayer& L, std::vector<Prop>& out, sf::Vector2f cam,
        bool anchored, sf::Vector2f anchor) {
        out.clear();
        if (!m_zone || !m_zone->registry || !L.active()) return;

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
            // Anchored: world = P + C(1-d) puts it exactly over the anchor
            // when the camera is there, and parallaxes like any far prop.
            if (anchored) {
                p.anchored = true;
                p.layerPos = anchor * p.depth
                    + sf::Vector2f(dx(m_rng), dy(m_rng)) * 0.15f;
            }
            out.push_back(p);
        }
    }

    /// Spin and wrap one layer. Split from draw() so the farther layer can be
    /// stepped and drawn first without duplicating either.
    void step(std::vector<Prop>& layer, sf::Vector2f cam, float dt) {
        for (auto& p : layer) {
            p.rotation += p.spin * dt;
            if (p.anchored) continue;

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
    // IN A LEVEL: everything blended by POSITION, nothing reseeded at a border
    // ========================================================================
    // Outside a level a zone change re-rolls the sky and the props, which is
    // right for the menu and the feed. In a level the player crosses sub-zone
    // borders every minute, and a re-roll there is a visible cut. So in a level:
    //   - ONE starfield for the whole level, re-tinted and thinned every frame
    //     from the cells' weights at the camera (LevelGen::cellWeights);
    //   - void and dust colours blended the same way (ZoneState::blend);
    //   - one prop pool per distinct layer near the camera, kept while it is
    //     near; each prop fades by how much ITS OWN zone owns the spot it
    //     appears at, so Rakshari junk shows over the border into open space
    //     and thins out across it;
    //   - every anchored landmark (the Citadel) placed when the level starts,
    //     over its cell, and never reseeded -- visible from next door.
    static constexpr float BLEND_BAND = 3500.f;   ///< px past a border over which two sub-zones mix

    struct Pool {
        zonearch::PropLayer layer;
        bool landmark = false;
        int  anchorCell = -1;          ///< >= 0: anchored landmark over that cell, always shown
        std::vector<Prop> props;
        std::vector<float> mul;        ///< per-prop alpha weight, this frame
        std::vector<char> owns;        ///< per level cell: does its zone have this layer?
        float idle = 0.f;              ///< seconds since a nearby cell last wanted it
    };

    bool inLevel() const { return m_level && m_level->active && m_zone && m_zone->registry; }

    const zonearch::ZoneDef* zoneOfCell(int ci) const {
        const auto& L = m_level->level;
        if (ci < 0 || static_cast<size_t>(ci) >= L.cellZone.size()) return nullptr;
        return m_zone->registry->byId(m_zone->registry->idOf(L.cellZone[static_cast<size_t>(ci)]));
    }

    void applyLevel() {
        m_zone->dirty = false;
        m_wasLevel = true;
        const auto& L = m_level->level;
        const uint32_t gen = m_zone->registry->generation();
        const uint64_t key = (static_cast<uint64_t>(L.seed) << 16) ^ static_cast<uint64_t>(L.index + 1);
        const sf::Vector2f cam = m_view ? m_view->getCenter() : sf::Vector2f(0.f, 0.f);

        if (m_levelKey != key || m_levelGen != gen || m_starBase.empty()) {
            m_levelKey = key;
            m_levelGen = gen;
            m_pools.clear();
            m_citadelRT.clear();
            m_junk.clear();
            m_landmarks.clear();
            m_applied.valid = false;
            // One starfield for the whole level, at the densest sub-zone's count.
            int maxStars = 1;
            for (size_t ci = 0; ci < L.cells.size(); ++ci)
                if (const auto* z = zoneOfCell(static_cast<int>(ci))) maxStars = std::max(maxStars, z->starCount);
            if (m_em && m_window) {
                m_em->initBackground(m_window->getSize(), maxStars);
                m_starBase.clear();
                for (const auto& s : m_em->stars) m_starBase.push_back(s.color);
            }
            m_starMax = static_cast<float>(maxStars);
            m_lastStarKey = -1.f;
            // Every anchored landmark in the level, from the first frame.
            for (size_t ci = 0; ci < L.cells.size(); ++ci) {
                const auto* z = zoneOfCell(static_cast<int>(ci));
                if (!z || !z->landmarks.active() || !z->landmarks.anchored) continue;
                Pool p;
                p.layer = z->landmarks;
                p.landmark = true;
                p.anchorCell = static_cast<int>(ci);
                seedLayerAt(p.layer, p.props, cam, true, L.cellCentre(static_cast<int>(ci)));
                m_pools.push_back(std::move(p));
            }
        }

        // ---- The blend at the camera ----
        L.cellWeights(cam, BLEND_BAND, m_w);
        float v[3] = {}, d[4] = {}, t[3] = {}, ta = 0.f, sc = 0.f, wsum = 0.f;
        for (const auto& cw : m_w) {
            const auto* z = zoneOfCell(cw.first);
            if (!z) continue;
            const float w = cw.second;
            v[0] += z->voidColor.r * w; v[1] += z->voidColor.g * w; v[2] += z->voidColor.b * w;
            d[0] += z->dustColor.r * w; d[1] += z->dustColor.g * w; d[2] += z->dustColor.b * w; d[3] += z->dustAlpha * w;
            t[0] += z->starTint.r * w; t[1] += z->starTint.g * w; t[2] += z->starTint.b * w;
            ta += z->starAlphaScale * w;
            sc += z->starCount * w;
            wsum += w;
        }
        if (wsum <= 0.f) return;
        for (float& x : v) x /= wsum;
        for (float& x : d) x /= wsum;
        for (float& x : t) x /= wsum;
        ta /= wsum; sc /= wsum;
        const auto u8 = [](float x) { return static_cast<std::uint8_t>(std::clamp(x, 0.f, 255.f)); };
        auto& b = m_zone->blend;
        b.active = true;
        b.voidColor = sf::Color(u8(v[0]), u8(v[1]), u8(v[2]));
        b.dustColor = sf::Color(u8(d[0]), u8(d[1]), u8(d[2]));
        b.dustAlpha = d[3];

        // ---- Stars: tint, alpha and density, from the untinted originals ----
        const float starKey = t[0] + t[1] * 7.f + t[2] * 49.f + ta * 1000.f + sc * 0.37f;
        if (m_em && m_starBase.size() == m_em->stars.size() && std::fabs(starKey - m_lastStarKey) > 0.01f) {
            m_lastStarKey = starKey;
            const float keep = std::clamp(sc / std::max(1.f, m_starMax), 0.f, 1.f);
            for (size_t i = 0; i < m_em->stars.size(); ++i) {
                const sf::Color c = m_starBase[i];
                // A fixed threshold per star: a thinner sky loses stars one by
                // one, it does not fade them all. (A different hash from the
                // Void's thinning, so the two do not take the same stars.)
                const float u = static_cast<float>((i * 40503u + 977u) % 1000u) / 1000.f;
                const float on = std::clamp((keep - u) / 0.04f, 0.f, 1.f);
                m_em->stars[i].color = sf::Color(
                    u8(c.r * t[0] / 255.f), u8(c.g * t[1] / 255.f), u8(c.b * t[2] / 255.f),
                    u8(c.a * ta * on));
            }
        }
    }

    void touch(const zonearch::PropLayer& layer, bool landmark, sf::Vector2f cam) {
        if (!layer.active()) return;
        for (auto& p : m_pools)
            if (p.anchorCell < 0 && p.landmark == landmark && sameLayer(p.layer, layer)) { p.idle = 0.f; return; }
        Pool p;
        p.layer = layer;
        p.landmark = landmark;
        seedLayerAt(layer, p.props, cam, false, {});
        m_pools.push_back(std::move(p));
    }

    void updateLevel(sf::Vector2f cam, float dt) {
        const auto& L = m_level->level;

        // Pools for every layer within reach: seeded BEFORE their props can
        // show, so they never pop in; dropped a while after the last one left.
        L.cellWeights(cam, BLEND_BAND + 5000.f, m_wWide);
        for (auto& p : m_pools) if (p.anchorCell < 0) p.idle += dt;
        for (const auto& cw : m_wWide) {
            const auto* z = zoneOfCell(cw.first);
            if (!z) continue;
            touch(z->junk, false, cam);
            if (!z->landmarks.anchored) touch(z->landmarks, true, cam);
        }
        bool droppedLandmark = false;
        m_pools.erase(std::remove_if(m_pools.begin(), m_pools.end(), [&](const Pool& p) {
            const bool drop = p.anchorCell < 0 && p.idle > 3.f;
            if (drop && p.landmark) droppedLandmark = true;
            return drop; }), m_pools.end());
        if (droppedLandmark) m_citadelRT.clear();   // landmark slots shift

        // Each prop fades by how much its own zone owns where it appears.
        for (auto& p : m_pools) {
            step(p.props, cam, dt);
            p.mul.assign(p.props.size(), 1.f);
            if (p.anchorCell >= 0) continue;
            p.owns.assign(L.cells.size(), 0);
            for (size_t ci = 0; ci < L.cells.size(); ++ci) {
                const auto* z = zoneOfCell(static_cast<int>(ci));
                if (!z) continue;
                const zonearch::PropLayer& zl = p.landmark ? z->landmarks : z->junk;
                p.owns[ci] = zl.active() && !(p.landmark && zl.anchored) && sameLayer(zl, p.layer);
            }
            for (size_t k = 0; k < p.props.size(); ++k) {
                const Prop& pr = p.props[k];
                const sf::Vector2f world = pr.layerPos + cam * (1.f - pr.depth);
                L.cellWeights(world, BLEND_BAND, m_wp);
                float w = 0.f;
                for (const auto& cw : m_wp) if (p.owns[static_cast<size_t>(cw.first)]) w += cw.second;
                p.mul[k] = w;
            }
        }

        // Farthest first: landmarks, then the junk in front of them.
        size_t slot = 0;
        for (const auto& p : m_pools) if (p.landmark) { draw(p.props, cam, &p.mul, slot); slot += p.props.size(); }
        for (const auto& p : m_pools) if (!p.landmark) draw(p.props, cam, &p.mul, 0);
    }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw(const std::vector<Prop>& layer, sf::Vector2f cam,
        const std::vector<float>* mul = nullptr, size_t slotBase = 0) const {
        if (layer.empty()) return;

        sf::VertexArray fills(sf::PrimitiveType::Triangles);
        sf::VertexArray lines(sf::PrimitiveType::Lines);

        // One slow pulse shared by every glow in the zone. Per-prop phases
        // would read as flickering rather than as machinery breathing.
        const float pulse = 0.82f + 0.18f * std::sin(m_time * 1.7f);

        for (size_t pi = 0; pi < layer.size(); ++pi) {
            const auto& p = layer[pi];
            if (!p.def || !p.def->valid()) continue;
            const float m = (mul && pi < mul->size()) ? (*mul)[pi] : 1.f;
            if (m < 0.004f) continue;

            // A builtin model draws itself, now, in layer order.
            if (!p.def->builtin.empty()) {
                if (p.def->builtin == "RAKSHARI_CITADEL") drawCitadel(p, slotBase + pi, cam, m);
                continue;
            }

            // Far props are smaller and dimmer. Drawing a distant prop at full
            // size but slow motion is the classic broken-parallax look.
            const float sizeF = p.scale * (0.5f + 0.5f * p.depth);
            const float alphaF = p.alpha * m;

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
    void drawCitadel(const Prop& p, size_t slot, sf::Vector2f cam, float alphaMul = 1.f) const {
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

        const float alpha = std::clamp(p.alpha * alphaMul, 0.f, 1.f);

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
    levelgen::LevelState* m_level = nullptr;

    // ---- In a level (see "IN A LEVEL") ----
    std::vector<Pool> m_pools;
    std::vector<sf::Color> m_starBase;       ///< the level starfield before tint
    float    m_starMax = 1.f;
    float    m_lastStarKey = -1.f;
    uint64_t m_levelKey = 0;
    uint32_t m_levelGen = 0;
    bool     m_wasLevel = false;
    std::vector<std::pair<int, float>> m_w, m_wWide, m_wp;   ///< reused weight buffers

    /// What was last applied, by value (sub-zone diffing; see applyIfDirty).
    struct Applied {
        bool valid = false;
        uint32_t generation = 0;
        std::string sky;
        int stars = 0;
        sf::Color tint;
        zonearch::PropLayer junk, landmarks;
        bool hasAnchor = false;
        sf::Vector2f anchor;
    } m_applied;
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