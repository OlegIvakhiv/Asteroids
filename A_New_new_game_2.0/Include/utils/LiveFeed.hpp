/**
 * @file LiveFeed.hpp
 * @brief The terminal's tactical feed: another hunter's real fight.
 *
 * A ShadowWorld running the actual game -- the real spawn director, the real
 * asteroid spawner, the real AI -- with a random hunter (random class, random
 * paint, random skill) flown by HunterPilot. What the feed shows is what the
 * game does, not an animation of it.
 *
 * THE LOOP
 *   1. A hunter is dropped into an empty patch of the current zone, with a
 *      first pack already closing on it so the feed never opens on nothing.
 *   2. The director takes it from there. If the field goes quiet for a while
 *      a new pack is summoned ahead of the hunter (the "encounter").
 *   3. The hunter dies: the camera holds on the wreck, the signal breaks up
 *      (SIGNAL LOST), and a new hunter -- new feed number, new callsign -- is
 *      dropped in. MenuSystem draws the static; this class reports `lost()`.
 *
 * The camera is the game's CameraSystem (follow, look-ahead, speed zoom,
 * shake). The feed only nudges the framing a share of the way toward the
 * hunter's target, so a duel reads as two ships, not one ship and an arrow.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "utils/ShadowWorld.hpp"
#include "utils/HunterPilot.hpp"
#include "utils/ShipShatter.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

class LiveFeed {
public:
    static constexpr float LOST_HOLD = 1.1f;    ///< camera holds on the wreck
    static constexpr float LOST_TOTAL = 3.0f;   ///< then static, then a new hunter

    void attach(sf::RenderWindow* window, sol::state* lua, const enemyarch::EnemyRegistry* reg,
        const zonearch::ZoneState* zone, const sf::Font* font) {
        m_window = window; m_lua = lua; m_reg = reg; m_zoneState = zone; m_font = font;
    }

    /// Size of the feed image in px (the camera sees exactly this at zoom 1).
    void setSize(sf::Vector2u s) {
        if (s == m_size) return;
        m_size = s;
        // A new texture needs a new world; a pixel of rounding does not.
        // Otherwise the size is simply used by the next hunter.
        if (m_world && (std::abs(static_cast<int>(s.x) - static_cast<int>(m_world->size().x)) > 8
            || std::abs(static_cast<int>(s.y) - static_cast<int>(m_world->size().y)) > 8)) m_dirty = true;
    }

    /// After F5: archetype ids may have moved. Drop the world, start a new feed.
    void invalidate() { m_dirty = true; }

    void step(float dt) {
        if (!m_window || !m_lua || !m_reg) return;
        if (!m_world || m_dirty) { newHunter(); if (!m_world) return; }
        dt = std::clamp(dt, 0.f, 0.05f);

        if (m_lostT >= 0.f) {
            m_lostT += dt;
            m_pilot.orders = pilot::Orders{};
            m_world->pad().clear();
            m_world->step(dt);
            if (m_lostT >= LOST_TOTAL) newHunter();
            return;
        }

        m_pilot.think(*m_world, dt);
        m_world->step(dt);
        frame(dt);

        // A hull at zero is a dead hunter. (Nothing in the gameplay systems
        // removes the PLAYER entity at 0 HP -- see the delivery note -- so the
        // feed ends its own hunter, with the same wreckage enemies get.)
        if (m_world->playerAlive()) {
            const std::size_t pi = m_world->playerIndex();
            if (m_world->em().healths[pi].currentHp <= 0.f) killHunter(pi);
        }
        if (!m_world->playerAlive()) { m_lostT = 0.f; ++m_lost; return; }

        // Keep it a fight: a quiet field gets a new pack ahead of the hunter.
        m_quiet = hostiles() == 0 ? m_quiet + dt : 0.f;
        if (m_quiet > 5.5f) { m_quiet = 0.f; encounter(820.f); }
    }

    void render() {
        if (!m_world) return;
        m_world->render(voidColor());
    }

    // ---- What the panel prints ----
    const sf::Texture* texture() const { return m_world ? m_world->texture() : nullptr; }
    int feedNo() const { return m_feedNo; }
    const std::string& callsign() const { return m_call; }
    const char* hullClass() const { return m_class; }
    const char* intent() const { return m_pilot.intent(); }
    bool lost() const { return m_lostT >= 0.f; }
    float lostT() const { return m_lostT; }
    int kills() const { return m_world ? m_world->kills() : 0; }   // this hunter's
    /// Everything that died on the feed since launch, hunters included.
    long long casualties() const { return m_totalKills + (m_world ? m_world->kills() : 0) + m_lost; }
    float hull01() const {
        if (!m_world || !m_world->playerAlive()) return 0.f;
        const auto& h = m_world->em().healths[m_world->playerIndex()];
        return std::clamp(h.currentHp / std::max(1.f, h.maxHp), 0.f, 1.f);
    }
    int hostiles() const {
        if (!m_world) return 0;
        int n = 0;
        const auto& em = m_world->em();
        for (std::size_t i = 0; i < em.enemies.size() && i < em.physics.size(); ++i) {
            if (!b2Body_IsValid(em.physics[i].bodyId)) continue;
            const BodyUserData* ud = bodyUD(em.physics[i].bodyId);
            if (ud && ud->type == BodyType::Enemy && em.enemies[i].powered() && em.healths[i].currentHp > 0.f) ++n;
        }
        return n;
    }
    /// Screen position (0..1 of the image) of the hunter's current target, or -1.
    sf::Vector2f targetUV(std::string* label = nullptr) const {
        if (!m_world || lost()) return { -1.f, -1.f };
        const std::size_t ti = m_world->em().getEntityIndex(m_pilot.target());
        if (ti == (std::size_t)-1) return { -1.f, -1.f };
        if (label && ti < m_world->em().enemies.size()) {
            const BodyUserData* ud = bodyUD(m_world->em().physics[ti].bodyId);
            if (ud && ud->type == BodyType::Enemy) {
                const auto* d = m_reg->byId(m_world->em().enemies[ti].archetype);
                *label = d ? d->display : std::string();
            }
            else *label = "ROCK";
        }
        return toUV(m_world->em().transforms[ti].position);
    }
    sf::Vector2f hunterUV() const {
        if (!m_world || !m_world->playerAlive()) return { -1.f, -1.f };
        return toUV(m_world->em().transforms[m_world->playerIndex()].position);
    }

private:
    sf::Vector2f toUV(sf::Vector2f p) const {
        const sf::View& v = m_world->worldView();
        const sf::Vector2f d = p - v.getCenter();
        return { 0.5f + d.x / v.getSize().x, 0.5f + d.y / v.getSize().y };
    }

    sf::Color voidColor() const {
        const zonearch::ZoneDef* z = m_zoneState ? m_zoneState->def() : nullptr;
        return z ? z->voidColor : sf::Color(3, 5, 8);
    }

    static float frand() { return std::rand() / static_cast<float>(RAND_MAX); }

    void newHunter() {
        m_dirty = false;
        if (m_world) m_totalKills += m_world->kills();
        if (!m_world) m_world = std::make_unique<ShadowWorld>();

        static const char* kCalls[] = { "VESK-11", "ORRA-04", "TALLOW-19", "KAIN-02", "MERIDIAN-33",
            "SOT-08", "HALCYON-21", "BRANDT-15", "IMMRE-06", "CASK-27", "LUME-12", "OSTRAVA-09" };
        ++m_feedNo;
        m_call = kCalls[std::rand() % (sizeof(kCalls) / sizeof(kCalls[0]))];

        ShadowWorld::Setup s;
        s.size = m_size;
        const int cls = std::rand() % 3;
        const auto hc = cls == 0 ? ship::HullClass::Light : cls == 1 ? ship::HullClass::Medium : ship::HullClass::Heavy;
        m_class = cls == 0 ? "LIGHT" : cls == 1 ? "MEDIUM" : "HEAVY";
        s.design = ship::ShipDesign::preset(hc);
        s.livery = randomLivery();
        m_design = s.design;
        m_livery = s.livery;
        s.spawn = { frand() * 4000.f - 2000.f, frand() * 4000.f - 2000.f };
        s.spawnAngle = frand() * 360.f;
        s.director = true;
        s.asteroids = true;
        s.stars = 260;
        if (!m_world->build(*m_window, *m_lua, *m_reg, m_zoneState ? m_zoneState->registry : nullptr,
            m_zoneState ? m_zoneState->current : zonearch::INVALID_ZONE, m_font, s)) {
            m_world.reset();
            return;
        }
        m_pilot.skill = pilot::Skill::random();
        m_pilot.orders = pilot::Orders{};
        m_pilot.reset(*m_lua);
        m_lostT = -1.f;
        m_quiet = 0.f;
        m_frame = { 0.f, 0.f };

        // A few rocks so the first frame is a field, then the first pack.
        for (int i = 0; i < 6; ++i) {
            const float a = frand() * 6.2831853f, d = 260.f + frand() * 520.f;
            const char* t = frand() < 0.15f ? "MAGMATIC" : frand() < 0.5f ? "LARGE" : "MEDIUM";
            m_world->rock(t, s.spawn + sf::Vector2f{ std::cos(a), std::sin(a) } * d,
                { frand() * 60.f - 30.f, frand() * 60.f - 30.f });
        }
        encounter(700.f);
    }

    /// Blow the hunter apart: its own hull, sliced, plus a blast and a ring.
    void killHunter(std::size_t pi) {
        EntityManager& em = m_world->em();
        const auto& tf = em.transforms[pi];
        const sf::Vector2f pos = tf.position;
        sf::Vector2f vel{ 0.f, 0.f };
        if (b2Body_IsValid(em.physics[pi].bodyId)) {
            const b2Vec2 v = b2Body_GetLinearVelocity(em.physics[pi].bodyId);
            vel = { v.x * SCALE, v.y * SCALE };
        }
        const float rr = tf.rotation * 3.14159265f / 180.f, cs = std::cos(rr), sn = std::sin(rr);
        const sf::Color hull = m_livery.paint.hull;
        const sf::Color fill(static_cast<uint8_t>(hull.r * 0.45f), static_cast<uint8_t>(hull.g * 0.45f),
            static_cast<uint8_t>(hull.b * 0.45f), 235);
        for (const auto& pc : shatter::slice(m_design.renderOutline(), m_design.renderTriangles(), 3)) {
            const sf::Vector2f wc(pc.centre.x * cs - pc.centre.y * sn, pc.centre.x * sn + pc.centre.y * cs);
            const float L = std::max(0.5f, std::sqrt(wc.x * wc.x + wc.y * wc.y));
            WreckShard s;
            s.position = pos + wc;
            s.velocity = wc / L * (80.f + frand() * 140.f) + vel * 0.6f;
            s.rotation = tf.rotation;
            s.angularVelocity = (frand() < 0.5f ? -1.f : 1.f) * (90.f + frand() * 200.f);
            s.lifetime = 2.6f + frand() * 0.8f;
            s.fadeTime = 0.8f;
            s.drag = 0.45f;
            s.heat = 1.f;
            s.coolRate = 0.9f;
            s.radius = pc.radius;
            s.lineWidth = 1.6f;
            s.fill = fill;
            s.skinColor = m_livery.paint.outline;
            s.tris = pc.tris;
            s.skin = pc.skin;
            s.scar = pc.scar;
            em.wreckShards.push_back(std::move(s));
        }
        em.spawnExplosion(pos, sf::Color(255, 170, 70), 34, 2.4f);
        em.spawnShockRing(pos, 12.f, 230.f, 0.55f, m_livery.paint.plasma, 4.f, 320.f);
        em.addTrauma(0.9f);
        em.destroyEntity(pi);
    }

    /// A pack from the roster, closing from one bearing.
    void encounter(float dist) {
        if (!m_world || !m_world->playerAlive()) return;
        static const std::vector<std::vector<const char*>> kPacks = {
            { "BERSERKER", "BERSERKER" }, { "RAIDER", "RAIDER", "WARDOG" }, { "BERSERKER", "RAIDER" },
            { "MANIAC", "WARDOG", "WARDOG" }, { "BARGE" }, { "BERSERKER", "BERSERKER", "RAIDER" },
            { "BLOODSEEKER" }, { "WARDOG", "WARDOG", "WARDOG" },
        };
        const auto& pack = kPacks[std::rand() % kPacks.size()];
        const auto& em = m_world->em();
        const std::size_t pi = m_world->playerIndex();
        const sf::Vector2f P = em.transforms[pi].position;
        const float a = frand() * 6.2831853f;
        const sf::Vector2f c = P + sf::Vector2f{ std::cos(a), std::sin(a) } * dist;
        int k = 0;
        for (const char* key : pack) {
            if (m_reg->idOf(key) == enemyarch::INVALID_ARCHETYPE) continue;
            const float b = a + 1.5708f;
            m_world->summon(key, c + sf::Vector2f{ std::cos(b), std::sin(b) } * (k * 90.f - 60.f));
            ++k;
        }
    }

    /// Lean the camera a share of the way toward the target.
    void frame(float dt) {
        if (!m_world->playerAlive()) return;
        sf::Vector2f want{ 0.f, 0.f };
        const auto& em = m_world->em();
        const std::size_t ti = em.getEntityIndex(m_pilot.target());
        if (ti != (std::size_t)-1) {
            const sf::Vector2f d = em.transforms[ti].position - em.transforms[m_world->playerIndex()].position;
            const float L = std::sqrt(d.x * d.x + d.y * d.y);
            if (L < 700.f) want = d * 0.32f;
        }
        m_frame += (want - m_frame) * std::min(1.f, dt * 2.5f);
        sf::View& v = m_world->view();
        v.setCenter(v.getCenter() + m_frame);
    }

    static ship::Livery randomLivery() {
        int n = 0;
        const sf::Color* pal = ship::liveryPalette(n);
        auto pick = [&](int lo, int hi) { return pal[lo + std::rand() % (hi - lo + 1)]; };
        ship::Livery lv;
        lv.paint.hull = pick(0, 11);
        lv.paint.outline = std::rand() % 3 ? sf::Color(236, 240, 245) : pick(0, 11);
        const sf::Color accent = pick(0, 11);
        lv.paint.plasma = accent;
        lv.paint.thrust = sf::Color(accent.r, accent.g, accent.b, 180);
        lv.paint.turbo = sf::Color(accent.r, accent.g, accent.b, 220);
        lv.paint.dodge = pick(0, 11);
        lv.paint.parry = pick(0, 11);
        lv.paint.homing = lv.paint.parry;
        lv.paint.cockpit = sf::Color(180, 245, 255, 235);
        return lv;
    }

    sf::RenderWindow* m_window = nullptr;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_reg = nullptr;
    const zonearch::ZoneState* m_zoneState = nullptr;
    const sf::Font* m_font = nullptr;
    sf::Vector2u m_size{ 1060, 644 };
    bool m_dirty = true;

    std::unique_ptr<ShadowWorld> m_world;
    pilot::HunterPilot m_pilot;
    ship::ShipDesign m_design = ship::ShipDesign::stock();
    ship::Livery m_livery;
    int m_feedNo = 0, m_lost = 0;
    long long m_totalKills = 0;
    std::string m_call = "----";
    const char* m_class = "MEDIUM";
    float m_lostT = -1.f, m_quiet = 0.f;
    sf::Vector2f m_frame{ 0.f, 0.f };
};
