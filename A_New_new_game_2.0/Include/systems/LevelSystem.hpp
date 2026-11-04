/**
 * @file LevelSystem.hpp
 * @brief The generated level in play: sub-zone switching, the Void border,
 *        the rift, fog of war, the minimap and the full map.
 *
 * ============================================================================
 * FOUR ENTRY POINTS, FOUR MOMENTS OF THE FRAME
 * ============================================================================
 *   update(dt)     logic pass (scaled time): which sub-zone the ship is in,
 *                  how deep in the Void it is, the Void's damage, the rift's
 *                  pull and the jump
 *   drawWorld()    world view, after ships: the Void swallowing the edge of
 *                  the screen, the border's line, the rift
 *   drawHud(dt)    screen view, after the HUD: minimap, full map (M), the
 *                  border warnings, sector banners, the jump fade
 *
 * Nothing here creates or destroys an entity. The jump only RAISES
 * LevelState::wantNextLevel; SystemManager builds the next level between
 * frames, through the same rebuild a restart uses.
 *
 * ============================================================================
 * FOG OF WAR
 * ============================================================================
 * The map knows the level's OUTLINE from the start -- the shape of the
 * sector, nothing inside it. Sub-zones stay hidden until seen. Inside the
 * auspex radius everything is live; everywhere else the map shows what was
 * there when it was last seen (LevelState::marks), so a pack you flew past
 * stays drawn where you left it until you go back and look.
 *
 * Dormant ambushers are drawn as hulks on the map, live or remembered. The
 * map must not be a way to tell a sleeper from a wreck.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ISystem.hpp"
#include "core/EntityManager.hpp"
#include "utils/LevelGen.hpp"
#include "utils/ZoneArchetypes.hpp"
#include "utils/UiPalette.hpp"
#include "utils/InputRegistry.hpp"
#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

class LevelSystem : public ISystem {
public:
    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_window = ctx.drawTarget();
        m_view = ctx.gameView;
        m_zone = ctx.zone;
        m_level = ctx.level;
        m_playerEntityId = ctx.playerEntityId;

        m_zoneCell = -1;
        m_pendingCell = -1;
        m_pendingT = 0.f;
        m_dmgT = 0.f;
        m_scanT = 0.f;
        m_jumpT = -1.f;
        m_fadeIn = (m_level && m_level->active) ? 1.f : 0.f;
        m_bannerT = (m_level && m_level->active) ? 0.f : -1.f;
        m_entryT = -1.f;
        m_entryText.clear();
        m_mapCacheVersion = 0xFFFFFFFFu;
        m_kMap = true;     // a key held through the jump must not toggle the map
        if (m_zone) m_zone->voidFade = 0.f;
    }

    /// The HUD font (owned by game.cpp, handed over by SystemManager).
    void setFont(const sf::Font* f) { m_font = f; }

    bool jumping() const { return m_jumpT >= 0.f; }

    // ========================================================================
    // LOGIC
    // ========================================================================
    void update(float dt) override {
        if (!active() || dt <= 0.f) return;
        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        if (pi == (size_t)-1) return;
        const levelgen::Config& cfg = *m_level->cfg;
        const levelgen::Level& L = m_level->level;
        const sf::Vector2f p = m_em->transforms[pi].position;

        // ---- Where am I ----
        const float edge = L.edgeDist(p);
        m_level->playerEdge = edge;
        m_level->voidFade = std::clamp((cfg.warnDistance - edge) / cfg.warnDistance, 0.f, 1.f);
        if (m_zone) m_zone->voidFade = m_level->voidFade;

        int cell = L.cellAt(p);
        if (cell < 0) cell = L.nearestCell(p);
        m_level->playerCell = cell;
        updateSubzone(cell, dt);

        // ---- The Void ----
        if (edge < 0.f) {
            m_em->addTrauma(dt * 0.35f);    // the screen will not sit still in there
            if (cfg.damageOn && m_jumpT < 0.f) {
                m_dmgT += dt;
                while (m_dmgT >= cfg.damageInterval) {
                    m_dmgT -= cfg.damageInterval;
                    const float k = std::clamp(-edge / cfg.damageDeepAt, 0.f, 1.f);
                    const float dmg = cfg.damage * (1.f + (cfg.damageDeep - 1.f) * k);
                    if (m_em->damagePlayer(pi, dmg) > 0.f) {
                        m_em->spawnScreenFlash(sf::Color(70, 20, 120), 0.18f, 70.f);
                        m_em->sfx("player.hit", p, 0.45f, 0.7f);
                    }
                }
            }
        }
        else {
            m_dmgT = 0.f;
            if (m_level->voidFade > 0.25f) m_em->addTrauma(dt * 0.06f * m_level->voidFade);
        }

        // ---- The rift: pull, then the jump ----
        const sf::Vector2f toRift = L.rift - p;
        const float rd = std::hypot(toRift.x, toRift.y);
        if (m_jumpT >= 0.f) {
            m_jumpT += dt;
            // Drawn into the tear: velocity straight at the centre, slowing
            // as it arrives, so the ship visibly goes IN rather than past.
            const b2BodyId body = m_em->physics[pi].bodyId;
            if (b2Body_IsValid(body) && rd > 1.f) {
                const float sp = std::min(rd * 4.f, 900.f) / 30.f;
                b2Body_SetLinearVelocity(body, { toRift.x / rd * sp, toRift.y / rd * sp });
            }
            if (m_jumpT >= JUMP_TIME && !m_level->wantNextLevel) m_level->wantNextLevel = true;
        }
        else if (rd < cfg.riftRadius) {
            m_jumpT = 0.f;
            m_em->spawnScreenFlash(sf::Color(175, 95, 255), 0.35f, 120.f);
            m_em->addTrauma(0.35f);
            m_em->sfx("player.dodge.perfect", p, 1.f, 0.6f);
        }
        else if (rd < cfg.riftPull) {
            const b2BodyId body = m_em->physics[pi].bodyId;
            if (b2Body_IsValid(body)) {
                const float k = 1.f - rd / cfg.riftPull;
                const b2Vec2 v = b2Body_GetLinearVelocity(body);
                const float pull = 26.f * std::sqrt(k) * dt;   // m/s gained per second, strongest at the lip
                b2Body_SetLinearVelocity(body, { v.x + toRift.x / rd * pull, v.y + toRift.y / rd * pull });
            }
        }

        // ---- Auspex: fog and last-seen marks, ten times a second ----
        m_scanT -= dt;
        if (m_scanT <= 0.f) {
            m_scanT = 0.1f;
            scan(p, cfg.scanRadius);
        }
    }

    // ========================================================================
    // WORLD
    // ========================================================================
    void drawWorld() {
        if (!active() || !m_window || !m_view) return;
        const levelgen::Level& L = m_level->level;
        const levelgen::Config& cfg = *m_level->cfg;
        const sf::Vector2f c = m_view->getCenter(), s = m_view->getSize();

        drawRift(L.rift, cfg.riftRadius);

        // ---- The Void swallows the edge: a darkness field over the world ----
        // Sampled on a coarse grid and blended per vertex -- cheap, and soft
        // enough that the border reads as a place, not as a wall.
        const int GX = 32, GY = 18;
        const sf::Vector2f o = c - s * 0.6f, step = { s.x * 1.2f / GX, s.y * 1.2f / GY };
        std::vector<float> a(static_cast<size_t>((GX + 1) * (GY + 1)));
        float maxA = 0.f;
        const float inner = cfg.warnDistance * 0.45f, outer = 1600.f;
        for (int y = 0; y <= GY; ++y)
            for (int x = 0; x <= GX; ++x) {
                const float d = L.edgeDist(o + sf::Vector2f(x * step.x, y * step.y));
                float t = std::clamp((inner - d) / (inner + outer), 0.f, 1.f);
                t = std::pow(t, 1.3f);
                a[static_cast<size_t>(y * (GX + 1) + x)] = t;
                maxA = std::max(maxA, t);
            }
        if (maxA > 0.f) {
            sf::VertexArray va(sf::PrimitiveType::Triangles);
            const auto col = [&](int x, int y) {
                return sf::Color(3, 2, 7, static_cast<std::uint8_t>(240.f * a[static_cast<size_t>(y * (GX + 1) + x)]));
                };
            for (int y = 0; y < GY; ++y)
                for (int x = 0; x < GX; ++x) {
                    const sf::Vector2f p0 = o + sf::Vector2f(x * step.x, y * step.y);
                    const sf::Vector2f p1 = p0 + sf::Vector2f(step.x, 0.f), p2 = p0 + step, p3 = p0 + sf::Vector2f(0.f, step.y);
                    va.append({ p0, col(x, y) }); va.append({ p1, col(x + 1, y) }); va.append({ p2, col(x + 1, y + 1) });
                    va.append({ p0, col(x, y) }); va.append({ p2, col(x + 1, y + 1) }); va.append({ p3, col(x, y + 1) });
                }
            m_window->draw(va);
        }

        // ---- The edge itself: a faint, unsteady violet seam ----
        sf::VertexArray seam(sf::PrimitiveType::Lines);
        const float flick = 0.55f + 0.45f * std::sin(m_time * 7.3f) * std::sin(m_time * 2.1f);
        const sf::Color sc(120, 70, 190, static_cast<std::uint8_t>(60 + 50 * flick));
        const sf::Vector2f lo = c - s * 0.7f, hi = c + s * 0.7f;
        for (const auto& e : L.outline) {
            if (std::max(e.first.x, e.second.x) < lo.x || std::min(e.first.x, e.second.x) > hi.x
                || std::max(e.first.y, e.second.y) < lo.y || std::min(e.first.y, e.second.y) > hi.y) continue;
            seam.append({ e.first, sc }); seam.append({ e.second, sc });
        }
        if (seam.getVertexCount()) m_window->draw(seam);
    }

    // ========================================================================
    // SCREEN
    // ========================================================================
    void drawHud(float realDt) {
        m_time += realDt;
        if (!active() || !m_window) return;
        const sf::Vector2f ws(m_window->getSize());

        // ---- M: the full map ----
        const std::string mapKey = m_lua ? (*m_lua)["key_bindings"]["map"].get_or<std::string>("M") : "M";
        const bool kDown = InputRegistry::isPressed(mapKey);
        if (kDown && !m_kMap && m_jumpT < 0.f) m_level->mapOpen = !m_level->mapOpen;
        m_kMap = kDown;

        drawVoidScreen(ws);
        drawMinimap(ws);
        drawBanners(ws, realDt);
        if (m_level->mapOpen) drawFullMap(ws);

        // ---- The jump: violet, then black. And back out the other side. ----
        if (m_jumpT >= 0.f) {
            const float t = std::clamp(m_jumpT / JUMP_TIME, 0.f, 1.f);
            fillRect({ 0.f, 0.f }, ws, sf::Color(4, 2, 8, static_cast<std::uint8_t>(255.f * std::pow(t, 1.4f))));
        }
        if (m_fadeIn > 0.f) {
            fillRect({ 0.f, 0.f }, ws, sf::Color(4, 2, 8, static_cast<std::uint8_t>(255.f * m_fadeIn)));
            m_fadeIn = std::max(0.f, m_fadeIn - realDt * 1.4f);
        }
    }

private:
    static constexpr float JUMP_TIME = 0.8f;
    static inline const sf::Color UNCHARTED{ 18, 24, 31 };   ///< inside the sector, never seen

    bool active() const { return m_em && m_level && m_level->active && m_level->cfg; }

    // ---- Sub-zone switching, with a little hysteresis ----
    void updateSubzone(int cell, float dt) {
        if (cell < 0) return;
        const levelgen::Level& L = m_level->level;
        if (m_zoneCell < 0) { applySubzone(cell, false); return; }
        if (cell == m_zoneCell) { m_pendingCell = -1; return; }
        if (cell != m_pendingCell) { m_pendingCell = cell; m_pendingT = 0.f; }
        m_pendingT += dt;
        if (m_pendingT < 0.35f) return;
        // Same sub-zone in the next cell: nothing to change but the cell.
        if (L.cellZone[static_cast<size_t>(cell)] == L.cellZone[static_cast<size_t>(m_zoneCell)]) {
            m_zoneCell = cell;
            markVisited(cell);
            return;
        }
        applySubzone(cell, true);
    }

    void applySubzone(int cell, bool announce) {
        const levelgen::Level& L = m_level->level;
        const std::string& key = L.cellZone[static_cast<size_t>(cell)];
        const std::string before = m_zone && m_zone->def() ? skyGroup(*m_zone->def()) : std::string();
        if (m_zone) m_zone->setByKey(key);
        const std::string after = m_zone && m_zone->def() ? skyGroup(*m_zone->def()) : std::string();
        // A new sky is a hard cut in ZoneSystem: hide it under a short fade.
        if (announce && before != after) m_em->spawnScreenFlash(sf::Color(3, 2, 6), 0.55f, 235.f);
        m_zoneCell = cell;
        m_pendingCell = -1;
        const bool first = !m_level->cellVisited[static_cast<size_t>(cell)];
        markVisited(cell);
        if (announce) {
            m_entryText = std::string(first ? "UNCHARTED  //  " : "ENTERING  //  ") + upper(displayOf(key));
            m_entryT = 0.f;
        }
    }

    void markVisited(int cell) {
        if (cell >= 0 && static_cast<size_t>(cell) < m_level->cellVisited.size()) {
            if (!m_level->cellVisited[static_cast<size_t>(cell)]) ++m_level->fogVersion;
            m_level->cellVisited[static_cast<size_t>(cell)] = 1;
        }
    }

    static std::string skyGroup(const zonearch::ZoneDef& z) {
        return z.config.valid() ? z.config["sky_group"].get_or(z.key) : z.key;
    }

    std::string displayOf(const std::string& zoneKey) const {
        if (m_zone && m_zone->registry) {
            const uint8_t id = m_zone->registry->idOf(zoneKey);
            if (const zonearch::ZoneDef* z = m_zone->registry->byId(id)) return z->display;
        }
        return zoneKey;
    }

    static std::string upper(std::string s) {
        for (auto& ch : s) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        return s;
    }

    // ---- Fog of war and marks ----
    void scan(sf::Vector2f p, float r) {
        m_level->reveal(p, r);
        std::unordered_set<uint32_t> here;
        const float r2 = r * r;
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId) || i >= m_em->healths.size()) continue;
            const BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud) continue;
            const sf::Vector2f q = m_em->transforms[i].position;
            if ((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) > r2) continue;
            levelgen::Mark mk;
            mk.pos = q;
            if (ud->type == BodyType::Enemy && i < m_em->enemies.size()) {
                mk.kind = m_em->enemies[i].dormant ? levelgen::MarkKind::Hulk : levelgen::MarkKind::Hostile;
                mk.size = m_em->enemies[i].dormant ? 1.4f : 1.f;
            }
            else if (ud->type == BodyType::Asteroid && m_em->healths[i].visualRadius >= 45.f) {
                mk.kind = levelgen::MarkKind::Hulk;
                mk.size = std::clamp(m_em->healths[i].visualRadius / 45.f, 1.f, 2.2f);
            }
            else continue;
            const uint32_t id = m_em->transforms[i].entityId;
            m_level->marks[id] = mk;
            here.insert(id);
        }
        // Looked, and it is not there any more: forget it.
        for (auto it = m_level->marks.begin(); it != m_level->marks.end();) {
            const sf::Vector2f q = it->second.pos;
            const bool inRange = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) <= r2 * 0.9f;
            if (inRange && !here.count(it->first)) it = m_level->marks.erase(it);
            else ++it;
        }
        if (m_level->marks.size() > 900) {
            // Far, old memories go first.
            std::vector<std::pair<float, uint32_t>> far;
            for (const auto& kv : m_level->marks)
                far.push_back({ -std::hypot(kv.second.pos.x - p.x, kv.second.pos.y - p.y), kv.first });
            std::sort(far.begin(), far.end());
            for (size_t k = 0; k < far.size() && m_level->marks.size() > 700; ++k) m_level->marks.erase(far[k].second);
        }
    }

    // ========================================================================
    // DRAWING HELPERS
    // ========================================================================

    void fillRect(sf::Vector2f pos, sf::Vector2f size, sf::Color c) {
        sf::RectangleShape r(size);
        r.setPosition(pos);
        r.setFillColor(c);
        m_window->draw(r);
    }
    void frameRect(sf::Vector2f pos, sf::Vector2f size, sf::Color c, float t = 1.f) {
        sf::RectangleShape r(size - sf::Vector2f(2 * t, 2 * t));
        r.setPosition(pos + sf::Vector2f(t, t));
        r.setFillColor(sf::Color::Transparent);
        r.setOutlineColor(c);
        r.setOutlineThickness(t);
        m_window->draw(r);
    }
    void text(const std::string& s, sf::Vector2f pos, unsigned size, sf::Color c, float spacing = 1.6f, bool right = false, bool centre = false) {
        if (!m_font || s.empty()) return;
        sf::Text t(*m_font, s, size);
        t.setLetterSpacing(spacing);
        t.setFillColor(c);
        const float w = t.getLocalBounds().position.x + t.getLocalBounds().size.x;
        if (right) pos.x -= w;
        if (centre) pos.x -= w * 0.5f;
        t.setPosition({ std::round(pos.x), std::round(pos.y) });
        m_window->draw(t);
    }
    float textW(const std::string& s, unsigned size, float spacing = 1.6f) const {
        if (!m_font || s.empty()) return 0.f;
        sf::Text t(*m_font, s, size);
        t.setLetterSpacing(spacing);
        return t.getLocalBounds().position.x + t.getLocalBounds().size.x;
    }
    static sf::Color withA(sf::Color c, float a) { c.a = static_cast<std::uint8_t>(std::clamp(a, 0.f, 1.f) * c.a); return c; }
    static sf::Color dim(sf::Color c, float k) {
        return sf::Color(static_cast<std::uint8_t>(c.r * k), static_cast<std::uint8_t>(c.g * k), static_cast<std::uint8_t>(c.b * k), c.a);
    }

    /// Terminal chrome: hairline frame with corner brackets and a label tab.
    void chrome(sf::Vector2f pos, sf::Vector2f size, const std::string& label, const std::string& code) {
        fillRect(pos, size, sf::Color(4, 6, 9, 225));
        frameRect(pos, size, withA(ui::CYAN_MID, 0.55f));
        const float L = 12.f;
        const sf::Color bc = ui::CYAN_MID;
        fillRect(pos, { L, 2.f }, bc); fillRect(pos, { 2.f, L }, bc);
        fillRect({ pos.x + size.x - L, pos.y }, { L, 2.f }, bc); fillRect({ pos.x + size.x - 2.f, pos.y }, { 2.f, L }, bc);
        fillRect({ pos.x, pos.y + size.y - 2.f }, { L, 2.f }, bc); fillRect({ pos.x, pos.y + size.y - L }, { 2.f, L }, bc);
        fillRect({ pos.x + size.x - L, pos.y + size.y - 2.f }, { L, 2.f }, bc);
        fillRect({ pos.x + size.x - 2.f, pos.y + size.y - L }, { 2.f, L }, bc);
        if (!label.empty()) {
            const float w = textW(label, 13) + 14.f;
            fillRect({ pos.x + 8.f, pos.y - 9.f }, { w, 18.f }, ui::CYAN_MID);
            text(label, { pos.x + 15.f, pos.y - 9.f }, 13, ui::INK);
        }
        if (!code.empty()) {
            const float w = textW(code, 12) + 12.f;
            fillRect({ pos.x + size.x - 14.f - w, pos.y - 2.f }, { w, 4.f }, ui::VOID_BG);
            text(code, { pos.x + size.x - 20.f, pos.y - 9.f }, 12, ui::TEXT_DIM, 1.6f, true);
        }
    }

    // ---- The rift, in the world ----
    void drawRift(sf::Vector2f at, float radius) {
        const sf::Vector2f c = m_view->getCenter(), s = m_view->getSize();
        if (std::fabs(at.x - c.x) > s.x || std::fabs(at.y - c.y) > s.y) return;
        const float H = radius * 1.35f, W = radius * 0.5f;
        const float t = m_time;

        // Expanding rings, slow: the tear breathing.
        for (int k = 0; k < 3; ++k) {
            const float ph = std::fmod(t * 0.45f + k / 3.f, 1.f);
            sf::CircleShape ring(radius * (0.6f + 2.4f * ph));
            ring.setOrigin({ ring.getRadius(), ring.getRadius() });
            ring.setPosition(at);
            ring.setScale({ 0.55f, 1.f });
            ring.setFillColor(sf::Color::Transparent);
            ring.setOutlineThickness(2.f);
            ring.setOutlineColor(withA(ui::VIOLET, 0.55f * (1.f - ph)));
            m_window->draw(ring);
        }

        // The tear: a jagged vertical slit whose edge never holds still.
        const int N = 22;
        std::vector<sf::Vector2f> pts;
        for (int i = 0; i < N; ++i) {
            const float a = i * 6.2831853f / N;
            const float j = 1.f + 0.22f * std::sin(a * 5.f + t * 3.1f) + 0.14f * std::sin(a * 9.f - t * 4.7f);
            const float pinch = std::pow(std::fabs(std::cos(a)), 0.6f);   // narrow at the tips
            pts.push_back(at + sf::Vector2f(std::sin(a) * W * j * (0.35f + 0.65f * pinch), -std::cos(a) * H * (0.9f + 0.1f * j)));
        }
        sf::VertexArray fill(sf::PrimitiveType::TriangleFan);
        fill.append({ at, sf::Color(2, 0, 6) });
        for (int i = 0; i <= N; ++i) fill.append({ pts[static_cast<size_t>(i % N)], sf::Color(10, 2, 22) });
        m_window->draw(fill);
        sf::VertexArray edge(sf::PrimitiveType::LineStrip);
        for (int i = 0; i <= N; ++i) edge.append({ pts[static_cast<size_t>(i % N)], ui::VIOLET });
        m_window->draw(edge);
        sf::VertexArray edge2(sf::PrimitiveType::LineStrip);
        for (int i = 0; i <= N; ++i) {
            const sf::Vector2f q = at + (pts[static_cast<size_t>(i % N)] - at) * 1.12f;
            edge2.append({ q, withA(ui::VIOLET, 0.35f) });
        }
        m_window->draw(edge2);

        // Shards orbiting, being pulled in.
        sf::VertexArray shards(sf::PrimitiveType::Triangles);
        for (int k = 0; k < 14; ++k) {
            const float ph = std::fmod(t * 0.25f + k * 0.0714f, 1.f);
            const float ang = k * 2.39996f + t * (0.6f + 0.05f * k);
            const float r = radius * (2.6f - 2.1f * ph);
            const sf::Vector2f q = at + sf::Vector2f(std::cos(ang) * r * 0.7f, std::sin(ang) * r);
            const float sz = 5.f + 4.f * (1.f - ph);
            const sf::Color sc = withA(ui::VIOLET, 0.25f + 0.6f * ph);
            shards.append({ q + sf::Vector2f(0.f, -sz), sc });
            shards.append({ q + sf::Vector2f(sz * 0.6f, sz * 0.5f), sc });
            shards.append({ q + sf::Vector2f(-sz * 0.6f, sz * 0.5f), sc });
        }
        m_window->draw(shards);
    }

    // ---- Screen-space Void: vignette, glitches, the warning line ----
    void drawVoidScreen(sf::Vector2f ws) {
        const float f = m_level->voidFade;
        if (f <= 0.01f) return;
        const float edge = m_level->playerEdge;

        // Vignette closing in from the screen edges.
        sf::VertexArray v(sf::PrimitiveType::Triangles);
        const float in = std::min(ws.x, ws.y) * (0.18f + 0.30f * f);
        const sf::Color outC(3, 2, 7, static_cast<std::uint8_t>(230.f * f)), inC(3, 2, 7, 0);
        const auto band = [&](sf::Vector2f a, sf::Vector2f b, sf::Vector2f c2, sf::Vector2f d) {
            // a-b outer edge, c2-d inner edge
            v.append({ a, outC }); v.append({ b, outC }); v.append({ c2, inC });
            v.append({ a, outC }); v.append({ c2, inC }); v.append({ d, inC });
            };
        band({ 0, 0 }, { ws.x, 0 }, { ws.x - in, in }, { in, in });
        band({ ws.x, 0 }, { ws.x, ws.y }, { ws.x - in, ws.y - in }, { ws.x - in, in });
        band({ ws.x, ws.y }, { 0, ws.y }, { in, ws.y - in }, { ws.x - in, ws.y - in });
        band({ 0, ws.y }, { 0, 0 }, { in, in }, { in, ws.y - in });
        m_window->draw(v);

        // Glitch bars: the picture losing its lines.
        if (f > 0.35f) {
            const int bars = static_cast<int>(2 + 10 * f);
            const float seedT = std::floor(m_time * 14.f);
            for (int k = 0; k < bars; ++k) {
                const float h1 = levelgen::detail::hash2(k, static_cast<int>(seedT), 77u);
                const float h2 = levelgen::detail::hash2(k + 31, static_cast<int>(seedT), 91u);
                if (h2 > 0.55f + 0.4f * (1.f - f)) continue;
                const float y = h1 * ws.y, hh = 1.f + 5.f * h2;
                fillRect({ 0.f, y }, { ws.x, hh }, sf::Color(120, 70, 190, static_cast<std::uint8_t>(18 + 30 * f)));
            }
        }

        // The warning, top centre.
        const bool inVoid = edge < 0.f;
        if (f > 0.15f) {
            const float pulse = 0.55f + 0.45f * std::fabs(std::sin(m_time * (inVoid ? 7.f : 3.2f)));
            std::string msg = inVoid
                ? (m_level->cfg->damageOn ? "IN THE VOID  //  HULL FAILING" : "IN THE VOID")
                : "VOID PROXIMITY  //  TURN BACK";
            const sf::Color c = inVoid ? ui::RED : ui::AMBER;
            text(msg, { ws.x * 0.5f, ws.y * 0.16f }, 22, withA(c, pulse), 2.2f, false, true);
        }
    }

    // ---- Shared map painter: draws in WORLD coordinates through `view` ----
    struct MapFrame { sf::Vector2f pos, size; sf::View view; float pxPerWorld; };

    void paintMap(const MapFrame& F, bool full) {
        const levelgen::Level& L = m_level->level;
        const levelgen::Config& cfg = *m_level->cfg;
        m_window->setView(F.view);
        const sf::Vector2f vc = F.view.getCenter(), vs = F.view.getSize();
        const sf::Vector2f lo = vc - vs * 0.5f, hi = vc + vs * 0.5f;
        const float k = 1.f / F.pxPerWorld;     // world units per screen px

        // Void ground.
        { sf::RectangleShape r(vs); r.setPosition(lo); r.setFillColor(sf::Color(6, 4, 11)); m_window->draw(r); }

        // ---- Terrain: unexplored = the bare shape, explored = its sub-zone ----
        const float fc = m_level->fogCell;
        const int x0 = std::max(0, static_cast<int>((lo.x - m_level->fogOrigin.x) / fc));
        const int x1 = std::min(m_level->fogW - 1, static_cast<int>((hi.x - m_level->fogOrigin.x) / fc) + 1);
        const int y0 = std::max(0, static_cast<int>((lo.y - m_level->fogOrigin.y) / fc));
        const int y1 = std::min(m_level->fogH - 1, static_cast<int>((hi.y - m_level->fogOrigin.y) / fc) + 1);
        if (full && m_mapCacheVersion == m_level->fogVersion && m_mapCache.getVertexCount() > 0) {
            m_window->draw(m_mapCache);
        }
        else {
            sf::VertexArray va(sf::PrimitiveType::Triangles);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const sf::Vector2f c = m_level->fogOrigin + sf::Vector2f((x + 0.5f) * fc, (y + 0.5f) * fc);
                    if (L.edgeDist(c) <= 0.f) continue;
                    sf::Color col = UNCHARTED;
                    if (m_level->seen(x, y)) {
                        int cell = L.cellAt(c);
                        if (cell < 0) cell = L.nearestCell(c);
                        const levelgen::Subzone* sz = cell >= 0 ? cfg.subzone(L.cellZone[static_cast<size_t>(cell)]) : nullptr;
                        col = sz ? sz->mapColor : sf::Color(40, 50, 60);
                    }
                    const sf::Vector2f p0 = m_level->fogOrigin + sf::Vector2f(x * fc, y * fc);
                    const sf::Vector2f p1 = p0 + sf::Vector2f(fc, 0.f), p2 = p0 + sf::Vector2f(fc, fc), p3 = p0 + sf::Vector2f(0.f, fc);
                    va.append({ p0, col }); va.append({ p1, col }); va.append({ p2, col });
                    va.append({ p0, col }); va.append({ p2, col }); va.append({ p3, col });
                }
            m_window->draw(va);
            if (full) { m_mapCache = va; m_mapCacheVersion = m_level->fogVersion; }
        }

        // ---- The outline: known from the start ----
        sf::VertexArray ol(sf::PrimitiveType::Lines);
        const sf::Color oc = ui::CYAN_MID;
        for (const auto& e : L.outline) {
            if (std::max(e.first.x, e.second.x) < lo.x || std::min(e.first.x, e.second.x) > hi.x
                || std::max(e.first.y, e.second.y) < lo.y || std::min(e.first.y, e.second.y) > hi.y) continue;
            ol.append({ e.first, oc }); ol.append({ e.second, oc });
        }
        m_window->draw(ol);

        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        const sf::Vector2f pp = pi != (size_t)-1 ? m_em->transforms[pi].position : L.start;
        const float scanR = cfg.scanRadius;

        // Auspex ring.
        {
            sf::CircleShape ring(scanR);
            ring.setOrigin({ scanR, scanR });
            ring.setPosition(pp);
            ring.setFillColor(sf::Color(40, 245, 255, 10));
            ring.setOutlineThickness(k * 1.f);
            ring.setOutlineColor(withA(ui::CYAN_LOW, 0.9f));
            ring.setPointCount(64);
            m_window->draw(ring);
        }

        // ---- Blips ----
        sf::VertexArray blips(sf::PrimitiveType::Triangles);
        const auto square = [&](sf::Vector2f p, float px, sf::Color c) {
            const float h = px * k * 0.5f;
            blips.append({ p + sf::Vector2f(-h, -h), c }); blips.append({ p + sf::Vector2f(h, -h), c }); blips.append({ p + sf::Vector2f(h, h), c });
            blips.append({ p + sf::Vector2f(-h, -h), c }); blips.append({ p + sf::Vector2f(h, h), c }); blips.append({ p + sf::Vector2f(-h, h), c });
            };
        const float r2 = scanR * scanR;
        // Remembered (outside the auspex): what was there, dimmed.
        for (const auto& kv : m_level->marks) {
            const auto& m = kv.second;
            if ((m.pos.x - pp.x) * (m.pos.x - pp.x) + (m.pos.y - pp.y) * (m.pos.y - pp.y) <= r2) continue;
            if (m.pos.x < lo.x || m.pos.x > hi.x || m.pos.y < lo.y || m.pos.y > hi.y) continue;
            if (m.kind == levelgen::MarkKind::Hostile) square(m.pos, full ? 4.f : 3.f, sf::Color(150, 40, 30, 200));
            else square(m.pos, (full ? 3.f : 2.5f) * m.size, sf::Color(70, 78, 90, 190));
        }
        // Live (inside the auspex).
        for (size_t i = 0; i < m_em->physics.size(); ++i) {
            if (i == pi || !b2Body_IsValid(m_em->physics[i].bodyId)) continue;
            const BodyUserData* ud = bodyUD(m_em->physics[i].bodyId);
            if (!ud) continue;
            const sf::Vector2f q = m_em->transforms[i].position;
            if ((q.x - pp.x) * (q.x - pp.x) + (q.y - pp.y) * (q.y - pp.y) > r2) continue;
            if (ud->type == BodyType::Enemy && i < m_em->enemies.size()) {
                if (m_em->enemies[i].dormant) square(q, (full ? 4.f : 3.5f), sf::Color(120, 128, 140));
                else square(q, full ? 5.f : 4.f, ui::RED);
            }
            else if (ud->type == BodyType::Asteroid && i < m_em->healths.size()) {
                const float vr = m_em->healths[i].visualRadius;
                square(q, std::clamp(vr / 25.f, 1.5f, 4.f), sf::Color(110, 118, 130));
            }
        }
        m_window->draw(blips);

        // ---- The rift: always marked ----
        {
            const float pulse = 0.6f + 0.4f * std::sin(m_time * 4.f);
            const float h = (full ? 9.f : 7.f) * k * (0.9f + 0.2f * pulse);
            sf::ConvexShape d(4);
            d.setPoint(0, L.rift + sf::Vector2f(0.f, -h * 1.3f)); d.setPoint(1, L.rift + sf::Vector2f(h * 0.7f, 0.f));
            d.setPoint(2, L.rift + sf::Vector2f(0.f, h * 1.3f));   d.setPoint(3, L.rift + sf::Vector2f(-h * 0.7f, 0.f));
            d.setFillColor(sf::Color(10, 2, 22));
            d.setOutlineThickness(k * 1.5f);
            d.setOutlineColor(withA(ui::VIOLET, 0.6f + 0.4f * pulse));
            m_window->draw(d);
        }

        // ---- The ship ----
        if (pi != (size_t)-1) {
            const float rot = (m_em->transforms[pi].rotation) * 3.14159265f / 180.f;
            const float s = (full ? 9.f : 8.f) * k;
            const sf::Vector2f f(std::sin(rot), -std::cos(rot)), r(std::cos(rot), std::sin(rot));
            sf::ConvexShape tri(3);
            tri.setPoint(0, pp + f * s);
            tri.setPoint(1, pp - f * (s * 0.6f) + r * (s * 0.6f));
            tri.setPoint(2, pp - f * (s * 0.6f) - r * (s * 0.6f));
            tri.setFillColor(ui::CYAN);
            m_window->draw(tri);
        }
    }

    sf::View screenView(sf::Vector2f ws) const { return sf::View(sf::FloatRect({ 0.f, 0.f }, ws)); }

    void drawMinimap(sf::Vector2f ws) {
        const levelgen::Level& L = m_level->level;
        const levelgen::Config& cfg = *m_level->cfg;
        const float S = std::clamp(std::round(ws.y * 0.24f), 170.f, 300.f);
        const sf::Vector2f pos{ ws.x - S - 26.f, 34.f }, size{ S, S };
        char code[24];
        std::snprintf(code, sizeof code, "S-%02d", L.index + 1);
        chrome(pos - sf::Vector2f(6.f, 6.f), size + sf::Vector2f(12.f, 12.f), "AUSPEX", code);

        const size_t pi = m_em->getEntityIndex(m_playerEntityId);
        const sf::Vector2f pp = pi != (size_t)-1 ? m_em->transforms[pi].position : L.start;
        const float range = cfg.minimapRange;
        MapFrame F;
        F.pos = pos; F.size = size;
        F.view = sf::View(pp, { range * 2.f, range * 2.f });
        F.view.setViewport(sf::FloatRect({ pos.x / ws.x, pos.y / ws.y }, { size.x / ws.x, size.y / ws.y }));
        F.pxPerWorld = S / (range * 2.f);
        paintMap(F, false);
        m_window->setView(screenView(ws));

        // Rift off the edge: a violet arrow on the frame pointing at it.
        const sf::Vector2f d = L.rift - pp;
        if (std::fabs(d.x) > range * 0.92f || std::fabs(d.y) > range * 0.92f) {
            const float len = std::max(1.f, std::hypot(d.x, d.y));
            const sf::Vector2f u = d / len;
            const float t = std::min((S * 0.5f - 8.f) / std::max(0.001f, std::fabs(u.x)), (S * 0.5f - 8.f) / std::max(0.001f, std::fabs(u.y)));
            const sf::Vector2f c = pos + size * 0.5f + u * t;
            sf::ConvexShape ar(3);
            const sf::Vector2f n(-u.y, u.x);
            ar.setPoint(0, c + u * 7.f); ar.setPoint(1, c - u * 5.f + n * 6.f); ar.setPoint(2, c - u * 5.f - n * 6.f);
            ar.setFillColor(withA(ui::VIOLET, 0.7f + 0.3f * std::sin(m_time * 5.f)));
            m_window->draw(ar);
            char dist[32];
            std::snprintf(dist, sizeof dist, "RIFT %.1f KM", len / 1000.f);
            text(dist, { pos.x + S, pos.y + S + 10.f }, 12, withA(ui::VIOLET, 0.85f), 1.4f, true);
        }

        // Where am I, in words -- only once the place has been seen.
        std::string where = "UNCHARTED";
        const int cell = m_level->playerCell;
        if (cell >= 0 && m_level->cellVisited[static_cast<size_t>(cell)]) where = upper(displayOf(L.cellZone[static_cast<size_t>(cell)]));
        if (m_level->playerEdge < 0.f) where = "THE VOID";
        text(where, { pos.x, pos.y + S + 10.f }, 12, m_level->playerEdge < 0.f ? ui::RED : ui::TEXT_DIM, 1.6f);
        text("M  MAP", { pos.x, pos.y + S + 28.f }, 11, ui::TEXT_DEAD, 1.6f);
    }

    void drawFullMap(sf::Vector2f ws) {
        const levelgen::Level& L = m_level->level;
        fillRect({ 0.f, 0.f }, ws, sf::Color(2, 3, 5, 170));
        const sf::Vector2f bmin = L.boundsMin(), bmax = L.boundsMax();
        const sf::Vector2f bs = bmax - bmin;
        // Fit the level into ~76% x 74% of the screen, keeping its aspect.
        const sf::Vector2f area{ ws.x * 0.76f, ws.y * 0.70f };
        const float scale = std::min(area.x / bs.x, area.y / bs.y);
        const sf::Vector2f size{ bs.x * scale, bs.y * scale };
        const sf::Vector2f pos{ (ws.x - size.x) * 0.5f, (ws.y - size.y) * 0.5f + ws.y * 0.02f };

        char code[64];
        std::snprintf(code, sizeof code, "SECTOR %02d  //  %s", L.index + 1, upper(L.kindDisplay).c_str());
        chrome(pos - sf::Vector2f(10.f, 10.f), size + sf::Vector2f(20.f, 20.f), "AUSPEX // FULL SECTOR", code);

        MapFrame F;
        F.pos = pos; F.size = size;
        F.view = sf::View((bmin + bmax) * 0.5f, bs);
        F.view.setViewport(sf::FloatRect({ pos.x / ws.x, pos.y / ws.y }, { size.x / ws.x, size.y / ws.y }));
        F.pxPerWorld = scale;
        paintMap(F, true);
        m_window->setView(screenView(ws));

        // Names of the places you have been.
        const auto toScreen = [&](sf::Vector2f w) { return pos + (w - bmin) * scale; };
        for (size_t i = 0; i < L.cells.size(); ++i) {
            if (!m_level->cellVisited[i]) continue;
            const sf::Vector2f c = toScreen(L.cellCentre(static_cast<int>(i)));
            text(upper(displayOf(L.cellZone[i])), c, 12, withA(ui::TEXT, 0.75f), 1.6f, false, true);
        }
        const sf::Vector2f rs = toScreen(L.rift);
        text("VOID RIFT", { rs.x, rs.y + 14.f }, 12, ui::VIOLET, 1.6f, false, true);

        // Legend, centred under the panel; the key on its own line.
        const float ly = pos.y + size.y + 22.f;
        const char* names[6] = { "YOU", "VOID RIFT", "HOSTILE", "LAST SEEN", "HULK / ROCK", "UNCHARTED" };
        float total = 0.f;
        for (const char* nm : names) total += 34.f + textW(nm, 12);
        float lx = ws.x * 0.5f - (total - 18.f) * 0.5f;
        const auto legend = [&](sf::Color c, const char* s) {
            fillRect({ lx, ly + 4.f }, { 9.f, 9.f }, c);
            text(s, { lx + 16.f, ly }, 12, ui::TEXT_DIM);
            lx += 34.f + textW(s, 12);
            };
        legend(ui::CYAN, "YOU");
        legend(ui::VIOLET, "VOID RIFT");
        legend(ui::RED, "HOSTILE");
        legend(sf::Color(150, 40, 30), "LAST SEEN");
        legend(sf::Color(110, 118, 130), "HULK / ROCK");
        legend(UNCHARTED, "UNCHARTED");
        text("M  CLOSE", { ws.x * 0.5f, ly + 24.f }, 12, ui::TEXT_DIM, 1.6f, false, true);
    }

    void drawBanners(sf::Vector2f ws, float dt) {
        const levelgen::Level& L = m_level->level;
        // Sector banner on arrival.
        if (m_bannerT >= 0.f && m_fadeIn < 0.6f) {
            m_bannerT += dt;
            const float T = m_bannerT;
            const float a = T < 3.6f ? 1.f : std::max(0.f, 1.f - (T - 3.6f) / 0.8f);
            if (a <= 0.f) m_bannerT = -1.f;
            else {
                char head[32];
                std::snprintf(head, sizeof head, "SECTOR %02d", L.index + 1);
                const std::string sub = upper(L.kindDisplay) + "  //  DERANT RIM";
                const size_t nh = static_cast<size_t>(std::clamp(T * 30.f, 0.f, 99.f));
                const size_t ns = static_cast<size_t>(std::clamp((T - 0.35f) * 45.f, 0.f, 99.f));
                const float y = ws.y * 0.22f;
                text(std::string(head).substr(0, nh), { ws.x * 0.5f, y }, 40, withA(ui::CYAN, a), 3.f, false, true);
                text(sub.substr(0, std::min(ns, sub.size())), { ws.x * 0.5f, y + 54.f }, 15, withA(ui::TEXT_DIM, a), 2.2f, false, true);
                const float w = 260.f * std::clamp(T * 2.f, 0.f, 1.f);
                fillRect({ ws.x * 0.5f - w, y + 48.f }, { w * 2.f, 1.f }, withA(ui::CYAN_LOW, a));
                if (T > 1.0f) text("THE RIFT IS MARKED ON YOUR AUSPEX", { ws.x * 0.5f, y + 80.f }, 12,
                    withA(ui::VIOLET, a * std::clamp((T - 1.f) * 3.f, 0.f, 1.f)), 1.8f, false, true);
            }
        }
        // Sub-zone entry line, under the minimap.
        if (m_entryT >= 0.f) {
            m_entryT += dt;
            const float a = m_entryT < 2.6f ? 1.f : std::max(0.f, 1.f - (m_entryT - 2.6f) / 0.6f);
            if (a <= 0.f) m_entryT = -1.f;
            else {
                const size_t n = static_cast<size_t>(std::clamp(m_entryT * 50.f, 0.f, 99.f));
                const float S = std::clamp(std::round(ws.y * 0.24f), 170.f, 300.f);
                text(m_entryText.substr(0, std::min(n, m_entryText.size())),
                    { ws.x - 26.f, 34.f + S + 50.f }, 13, withA(ui::AMBER, a), 1.8f, true);
            }
        }
    }

    // ---- State ----
    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    sf::RenderTarget* m_window = nullptr;
    sf::View* m_view = nullptr;
    zonearch::ZoneState* m_zone = nullptr;
    levelgen::LevelState* m_level = nullptr;
    const sf::Font* m_font = nullptr;
    uint32_t m_playerEntityId = 0;

    int   m_zoneCell = -1, m_pendingCell = -1;
    float m_pendingT = 0.f;
    float m_dmgT = 0.f, m_scanT = 0.f;
    float m_jumpT = -1.f, m_fadeIn = 0.f;
    float m_bannerT = -1.f, m_entryT = -1.f;
    std::string m_entryText;
    float m_time = 0.f;
    bool  m_kMap = false;

    sf::VertexArray m_mapCache;
    uint32_t m_mapCacheVersion = 0xFFFFFFFFu;
};
