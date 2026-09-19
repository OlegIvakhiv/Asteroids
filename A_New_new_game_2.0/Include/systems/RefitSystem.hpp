/**
 * @file RefitSystem.hpp
 * @brief The refit bay: class frame, hitbox editor, decorative model, mounts, readout.
 *
 * Screen-space only. Owns no ship data -- it edits a ship::ShipDesign that
 * lives in SystemManager and persists between runs. Polls its own mouse and
 * keyboard so game.cpp only has to route the state.
 *
 * ============================================================================
 * TWO LAYERS, TWO MODES
 * ============================================================================
 *   HULL   the hitbox: <= 8 convex points, what Box2D collides with. Guns and
 *          drives mount here. Stats come from here.
 *   MODEL  what the player sees: any shape, up to 64 points. Must cover the
 *          hull, stay inside the dim envelope, and use <= 150% of its area.
 *          A spike above a gun is that gun's muzzle.
 *
 * Colour means ROLE, everywhere on this screen:
 *   CYAN    plasma gun          VIOLET  Rift (spinal) mount
 *   AMBER   drive               RED     refused / invalid
 *
 * ============================================================================
 * INTERACTION
 * ============================================================================
 *   M / Tab         switch HULL <-> MODEL
 *   Left drag       move a point (of the layer being edited)
 *   Right click     HULL:  fit/remove the mount under the cursor
 *                   MODEL: on a point removes it, anywhere else adds one
 *   R               HULL: Rift mount / toggle SHARED-DEDICATED
 *   Insert / =      add a point at the cursor      Delete  remove hovered point
 *   Y               mirror editing                 1 2 3   LIGHT / MEDIUM / HEAVY
 *   A               HULL: auto-mount               Esc     back
 *
 * The view zooms to fit the class frame and the model envelope, and holds
 * still while a drag is in progress.
 *
 * @author Oleg Ivakhiv
 * @version 3.0
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/TerminalUI.hpp"
#include "utils/ClassTuning.hpp"
#include "utils/ShipLivery.hpp"
#include "utils/ShipFile.hpp"
#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

class RefitSystem : public ISystem {
public:
    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    void init(const SystemContext& ctx) override {
        m_window = ctx.window;
        m_lua = ctx.lua;
        m_ui.attach(ctx.window);
    }

    void setFont(sf::Font* f) { m_font = f; m_ui.setFonts(m_font, m_mono); }
    void setMonoFont(sf::Font* f) { m_mono = f; m_ui.setFonts(m_font, m_mono); }

    /// Point at the design SystemManager owns. Must be called before use.
    void setDesign(ship::ShipDesign* d) { m_design = d; }
    void setLivery(ship::Livery* l) { m_livery = l; }

    /// Clear transient interaction state. Call on every entry to the screen.
    void onEnter() {
        m_dragging = -1;
        m_hoverPoint = -1;
        m_hoverEdge = -1;
        m_exit = false;
        m_openTimer = 0.f;
        m_rejectFlash = 0.f;
        m_rejectText.clear();
        m_toast.clear();
        m_toastTimer = 0.f;

        // Prime EVERY edge-detector from the current physical state. Entering
        // this screen means Enter is still held from the row that opened it;
        // an unprimed detector reads that hold as a fresh press and exits on
        // frame one.
        m_kExit = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        m_kY = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Y);
        m_k1 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num1);
        m_k2 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num2);
        m_k3 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num3);
        m_kA = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A);
        m_kR = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::R);
        m_kM = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
        m_hoverDecor = -1;
        m_viewInit = false;
        m_selDecal = -1;
        m_clickPending = false;
        m_kDel = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Delete);
        m_kAdd = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        m_lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        m_rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);

        m_inputLock = 0.12f;   // belt and braces against fast re-entry

        m_ui.primeInput();
        m_ui.resetReveal();
    }

    bool wantsExit() const { return m_exit; }
    void clearExit() { m_exit = false; }

    void update(float dt) override {
        if (!m_window || !m_font || !m_design) return;
        dt = std::clamp(dt, 0.f, 0.1f);
        m_time += dt;
        if (m_openTimer < 3.f) m_openTimer += dt;
        m_rejectFlash = std::max(0.f, m_rejectFlash - dt * 1.6f);
        if (m_toastTimer > 0.f) m_toastTimer -= dt;

        m_ui.begin(dt);
        updateView(dt);
        if (m_mode == Mode::Paint) updateFx(dt);

        m_inputLock = std::max(0.f, m_inputLock - dt);
        if (m_inputLock <= 0.f && !m_ui.glossaryOpen()) handleInput();
        draw();
    }

private:
    // ========================================================================
    // PALETTE  (identical to MenuSystem -- do not let these drift apart)
    // ========================================================================

    static inline const sf::Color VOID_BG{ 2,   3,   5 };
    static inline const sf::Color PANEL_BG{ 6,   8,  12 };
    static inline const sf::Color CYAN{ 40, 245, 255 };
    static inline const sf::Color CYAN_MID{ 20, 150, 170 };
    static inline const sf::Color CYAN_LOW{ 12,  70,  84 };
    static inline const sf::Color AMBER{ 255, 214,   0 };
    static inline const sf::Color RED{ 255,  48,   0 };
    static inline const sf::Color GREEN{ 80, 240, 130 };
    static inline const sf::Color TEXT{ 214, 222, 232 };
    static inline const sf::Color TEXT_DIM{ 84,  92, 104 };
    static inline const sf::Color TEXT_DEAD{ 52,  58,  68 };
    /// The Rift's own colour -- WeaponSystem's bolt, charge sparks and rings.
    static inline const sf::Color VIOLET{ 175, 95, 255 };

    static constexpr float OPEN_DUR = 0.30f;
    static constexpr float HANDLE_R = 7.f;

    enum class Mode { Hull, Model, Paint };

    /// What a palette click paints. Decal = whatever detail is selected.
    enum class PaintTarget { Hull, Outline, Plasma, Thrust, Turbo, Dodge, Parry, Homing, Cockpit, Decal };

    /// What the handles are moving. Cockpit and decals share one gizmo.
    enum class SelKind { None, Decal, Cockpit };

    /// Which handle the mouse grabbed. Photoshop rules: corners scale both
    /// axes, edges scale one, the stalk above the box rotates, inside moves.
    enum class Grab { None, Move, Rotate, ScaleBoth, ScaleW, ScaleH };

    /// A decal and a cockpit expose the same four numbers, so one gizmo
    /// drives both instead of two near-identical code paths.
    struct Xform {
        sf::Vector2f* pos = nullptr;
        float* w = nullptr;
        float* h = nullptr;
        float* angle = nullptr;
        bool valid() const { return pos && w && h && angle; }
    };

    struct Rect {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
        bool contains(sf::Vector2f p) const {
            return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
        }
    };

    // ========================================================================
    // LAYOUT
    // ========================================================================

    /// Screen view matching the CURRENT framebuffer. Rebuilt every frame --
    /// getDefaultView() is frozen at creation size.
    sf::View uiView() const {
        const sf::Vector2u s = m_window->getSize();
        return sf::View(sf::FloatRect({ 0.f, 0.f },
            { static_cast<float>(s.x), static_cast<float>(s.y) }));
    }

    sf::Vector2f viewSize() const {
        const sf::Vector2u s = m_window->getSize();
        return { static_cast<float>(s.x), static_cast<float>(s.y) };
    }

    Rect frac(float x, float y, float w, float h) const {
        const sf::Vector2f s = viewSize();
        return { x * s.x, y * s.y, w * s.x, h * s.y };
    }

    Rect hullRect()   const { return frac(0.015f, 0.105f, 0.600f, 0.775f); }
    Rect statsRect()  const { return frac(0.632f, 0.112f, 0.353f, 0.420f); }
    Rect mountRect()  const { return frac(0.632f, 0.560f, 0.353f, 0.180f); }
    Rect statusRect() const { return frac(0.632f, 0.768f, 0.353f, 0.112f); }

    sf::Vector2f toLocal(sf::Vector2f screen) const {
        const Rect r = hullRect();
        return { (screen.x - r.cx()) / m_zoom + m_viewCenter.x,
                 (screen.y - r.cy()) / m_zoom + m_viewCenter.y };
    }
    sf::Vector2f toScreen(sf::Vector2f local) const {
        const Rect r = hullRect();
        return { r.cx() + (local.x - m_viewCenter.x) * m_zoom,
                 r.cy() + (local.y - m_viewCenter.y) * m_zoom };
    }

    /**
     * @brief Fit the view to the class frame plus the model envelope.
     *
     * Symmetric about x = 0 so the centreline stays in the middle of the
     * panel. Frozen during a drag -- a view that rescales under the cursor
     * makes the point you are holding run away from it.
     */
    void updateView(float dt) {
        const auto& spec = m_design->spec();
        float maxX = spec.halfWidthPx, minY = spec.foreYPx, maxY = spec.aftYPx;
        const auto grow = [&](const std::vector<sf::Vector2f>& pts) {
            for (const auto& p : pts) {
                maxX = std::max(maxX, std::fabs(p.x));
                minY = std::min(minY, p.y);
                maxY = std::max(maxY, p.y);
            }
            };
        grow(m_design->envelope());
        grow(m_design->decor());

        const Rect r = hullRect();
        const float w = maxX * 2.f * 1.12f + 8.f;
        const float h = (maxY - minY) * 1.12f + 16.f;   // room for tags above spikes
        const float zoomT = std::clamp(std::min(r.w / w, r.h / h), 2.5f, 10.f);
        const sf::Vector2f centreT{ 0.f, (minY + maxY) * 0.5f - 2.f };

        if (!m_viewInit) { m_zoom = zoomT; m_viewCenter = centreT; m_viewInit = true; return; }
        if (m_dragging >= 0) return;
        const float k = 1.f - std::exp(-7.f * dt);
        m_zoom += (zoomT - m_zoom) * k;
        m_viewCenter += (centreT - m_viewCenter) * k;
    }

    // ========================================================================
    // INPUT
    // ========================================================================

    void handleInput() {
        const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
        m_mouse = m_window->mapPixelToCoords(pix, uiView());

        const bool lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        const bool rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
        const bool inPanel = hullRect().contains(m_mouse);
        const bool model = (m_mode == Mode::Model);
        const bool paint = (m_mode == Mode::Paint);

        // Click latch: the panel widgets are drawn after input runs, so they
        // consume this on their own rects during draw.
        if (lDown && !m_lDown) { m_clickPending = true; m_clickPos = m_mouse; }

        updateHover(inPanel);

        if (paint) {
            handlePaintInput(lDown, rDown, inPanel);
            if (keyEdge(sf::Keyboard::Key::Y, m_kY)) toggleMirror();
            const bool mKeyP = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
            if (mKeyP && !m_kM) setMode(Mode::Hull);
            m_kM = mKeyP;
            const bool exitKeyP = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
                || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
            if (exitKeyP && !m_kExit) m_exit = true;
            m_kExit = exitKeyP;
            m_lDown = lDown;
            m_rDown = rDown;
            return;
        }

        // ---- Mode ----
        const bool mKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
        if (mKey && !m_kM) setMode(model ? Mode::Paint : Mode::Model);
        m_kM = mKey;

        // ---- Left drag ----
        const int hovered = model ? m_hoverDecor : m_hoverPoint;
        if (lDown && !m_lDown && inPanel && hovered >= 0) m_dragging = hovered;
        if (!lDown) m_dragging = -1;

        if (m_dragging >= 0 && lDown) {
            if (model) {
                const sf::Vector2f target = snapFine(toLocal(m_mouse));
                if (m_dragging < m_design->decorPointCount()
                    && target != m_design->decor()[m_dragging]
                    && !m_design->moveDecorPoint(m_dragging, target))
                    reject(m_design->lastRejectReason());
            }
            else {
                const sf::Vector2f target = snap(toLocal(m_mouse));
                if (target != m_design->points()[m_dragging]
                    && !m_design->movePoint(m_dragging, target))
                    reject(m_design->lastRejectReason());
            }
        }

        // ---- Right click ----
        if (rDown && !m_rDown && inPanel) {
            if (model) {
                if (m_hoverDecor >= 0) {
                    if (!m_design->removeDecorPoint(m_hoverDecor)) reject(m_design->lastRejectReason());
                    m_hoverDecor = -1;
                }
                else if (!m_design->addDecorPoint(snapFine(toLocal(m_mouse))))
                    reject(m_design->lastRejectReason());
            }
            else {
                if (m_hoverPoint >= 0)     toggleGun(m_hoverPoint);
                else if (m_hoverEdge >= 0) toggleEngine(m_hoverEdge);
            }
        }

        // ---- Keys ----
        if (keyEdge(sf::Keyboard::Key::Y, m_kY)) toggleMirror();
        if (keyEdge(sf::Keyboard::Key::Num1, m_k1)) loadClass(ship::HullClass::Light);
        if (keyEdge(sf::Keyboard::Key::Num2, m_k2)) loadClass(ship::HullClass::Medium);
        if (keyEdge(sf::Keyboard::Key::Num3, m_k3)) loadClass(ship::HullClass::Heavy);

        if (keyEdge(sf::Keyboard::Key::A, m_kA) && !model) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        if (keyEdge(sf::Keyboard::Key::R, m_kR) && !model && m_hoverPoint >= 0) assignRift(m_hoverPoint);

        if (keyEdge(sf::Keyboard::Key::Delete, m_kDel)) {
            if (model && m_hoverDecor >= 0) {
                if (!m_design->removeDecorPoint(m_hoverDecor)) reject(m_design->lastRejectReason());
                m_hoverDecor = -1;
            }
            else if (!model && m_hoverPoint >= 0) {
                if (!m_design->removePoint(m_hoverPoint)) reject(m_design->lastRejectReason());
                m_hoverPoint = -1;
            }
        }

        const bool addKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        if (addKey && !m_kAdd && inPanel) {
            const bool ok = model ? m_design->addDecorPoint(snapFine(toLocal(m_mouse)))
                : m_design->addPoint(snap(toLocal(m_mouse)));
            if (!ok) reject(m_design->lastRejectReason());
        }
        m_kAdd = addKey;

        const bool exitKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        if (exitKey && !m_kExit) m_exit = true;
        m_kExit = exitKey;

        m_lDown = lDown;
        m_rDown = rDown;
    }

    bool keyEdge(sf::Keyboard::Key k, bool& was) {
        const bool now = sf::Keyboard::isKeyPressed(k);
        const bool edge = now && !was;
        was = now;
        return edge;
    }

    /// Hull grid: 2 units, so mirrored pairs land on matching coordinates.
    static sf::Vector2f snap(sf::Vector2f v) {
        return { std::round(v.x * 0.5f) * 2.f, std::round(v.y * 0.5f) * 2.f };
    }
    /// Model grid: 1 unit. Detail work needs the finer step.
    static sf::Vector2f snapFine(sf::Vector2f v) {
        return { std::round(v.x), std::round(v.y) };
    }

    void updateHover(bool inPanel) {
        m_hoverPoint = -1;
        m_hoverEdge = -1;
        m_hoverDecor = -1;
        if (!inPanel || m_dragging >= 0) return;

        if (m_mode == Mode::Model) {
            const auto& d = m_design->decor();
            float best = 9.f;
            for (int i = 0; i < static_cast<int>(d.size()); ++i) {
                const sf::Vector2f sp = toScreen(d[i]);
                const float dist = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
                if (dist < best) { best = dist; m_hoverDecor = i; }
            }
            return;
        }

        const auto& pts = m_design->points();
        float best = HANDLE_R + 3.f;
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            const sf::Vector2f sp = toScreen(pts[i]);
            const float d = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
            if (d < best) { best = d; m_hoverPoint = i; }
        }
        if (m_hoverPoint >= 0) return;

        best = HANDLE_R + 5.f;
        for (const auto& e : m_design->engineSlots()) {
            const sf::Vector2f sp = toScreen(e.position);
            const float d = std::hypot(sp.x - m_mouse.x, sp.y - m_mouse.y);
            if (d < best) { best = d; m_hoverEdge = e.index; }
        }
    }

    // ========================================================================
    // PAINT INPUT
    // ========================================================================

    void handlePaintInput(bool lDown, bool rDown, bool inPanel) {
        if (!m_livery) return;
        const bool shift = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);

        // Colour picker drags use the rects the panel published last frame.
        if (lDown && (m_pickDrag != 0 || (!m_lDown && (m_svRect.contains(m_mouse)
            || m_hueRect.contains(m_mouse) || m_alphaRect.contains(m_mouse))))) {
            if (m_pickDrag == 0)
                m_pickDrag = m_svRect.contains(m_mouse) ? 1 : (m_hueRect.contains(m_mouse) ? 2 : 3);
            dragPicker();
            m_clickPending = false;
            m_lDown = lDown; m_rDown = rDown;
            return;
        }
        if (!lDown) m_pickDrag = 0;

        // Hover: nearest detail centre under the cursor.
        int hover = -1;
        if (inPanel && m_grab == Grab::None) {
            float best = 1e9f;
            for (int i = 0; i < static_cast<int>(m_livery->decals.size()); ++i) {
                const auto& d = m_livery->decals[i];
                const float r = std::max(10.f, ship::decalRadius(d) * m_zoom);
                for (int m = 0; m < (d.mirrored ? 2 : 1); ++m) {
                    const sf::Vector2f c = toScreen({ m ? -d.pos.x : d.pos.x, d.pos.y });
                    const float dist = std::hypot(c.x - m_mouse.x, c.y - m_mouse.y);
                    if (dist < r && dist < best) { best = dist; hover = i; }
                }
            }
        }
        m_hoverDecal = hover;

        // ---- Press: handle first, then a new selection ----
        if (lDown && !m_lDown && inPanel) {
            const Grab g = handleUnderCursor();
            if (g != Grab::None) {
                beginGrab(g);
                m_clickPending = false;
            }
            else {
                if (hover >= 0) {
                    m_selKind = SelKind::Decal; m_selDecal = hover;
                    m_paintTarget = PaintTarget::Decal;
                    syncPickerFrom(m_livery->decals[hover].color);   // picker follows the selection
                }
                else if (cockpitUnderCursor()) {
                    m_selKind = SelKind::Cockpit; m_selDecal = -1;
                    m_paintTarget = PaintTarget::Cockpit;
                    syncPickerFrom(m_livery->paint.cockpit);
                }
                else { m_selKind = SelKind::None; m_selDecal = -1; }
                if (m_selKind != SelKind::None) { beginGrab(Grab::Move); m_clickPending = false; }
            }
        }
        if (!lDown) m_grab = Grab::None;
        if (m_grab != Grab::None && lDown) dragGizmo(shift);

        if (rDown && !m_rDown && inPanel && hover >= 0) {
            m_livery->decals.erase(m_livery->decals.begin() + hover);
            m_selKind = SelKind::None;
            m_selDecal = -1;
            toast("DETAIL REMOVED");
        }

        if (keyEdge(sf::Keyboard::Key::Delete, m_kDel)) deleteSelected();
    }

    // ---- Gizmo ------------------------------------------------------------

    Xform xform() {
        Xform x;
        if (m_selKind == SelKind::Decal) {
            if (ship::Decal* d = selected()) { x.pos = &d->pos; x.w = &d->w; x.h = &d->h; x.angle = &d->angle; }
        }
        else if (m_selKind == SelKind::Cockpit && m_livery
            && m_livery->cockpit.style != ship::CockpitStyle::None) {
            auto& c = m_livery->cockpit;
            x.pos = &c.pos; x.w = &c.w; x.h = &c.h; x.angle = &c.angle;
        }
        return x;
    }

    /// Screen positions of the eight scale handles, in the object's own frame.
    /// Index 0-3 corners (TL TR BR BL), 4-7 edges (T R B L).
    bool gizmoHandles(sf::Vector2f out[8], sf::Vector2f& centre, sf::Vector2f& rotator) {
        Xform x = xform();
        if (!x.valid()) return false;
        const float a = *x.angle * 3.14159f / 180.f;
        const float ca = std::cos(a), sa = std::sin(a);
        const float hw = std::max(3.f, *x.w) * 0.5f, hh = std::max(3.f, *x.h) * 0.5f;
        const auto L = [&](float lx, float ly) {
            return toScreen({ x.pos->x + lx * ca - ly * sa, x.pos->y + lx * sa + ly * ca });
            };
        out[0] = L(-hw, -hh); out[1] = L(hw, -hh); out[2] = L(hw, hh); out[3] = L(-hw, hh);
        out[4] = L(0.f, -hh);  out[5] = L(hw, 0.f); out[6] = L(0.f, hh); out[7] = L(-hw, 0.f);
        centre = toScreen(*x.pos);
        rotator = L(0.f, -hh - 22.f / std::max(0.1f, m_zoom));
        return true;
    }

    Grab handleUnderCursor() {
        sf::Vector2f h[8], c, rot;
        if (!gizmoHandles(h, c, rot)) return Grab::None;
        const auto near = [&](sf::Vector2f p) { return std::hypot(p.x - m_mouse.x, p.y - m_mouse.y) < 9.f; };
        if (near(rot)) return Grab::Rotate;
        for (int i = 0; i < 4; ++i) if (near(h[i])) { m_grabIdx = i; return Grab::ScaleBoth; }
        for (int i = 4; i < 8; ++i) if (near(h[i])) { m_grabIdx = i; return (i == 4 || i == 6) ? Grab::ScaleH : Grab::ScaleW; }
        return Grab::None;
    }

    bool cockpitUnderCursor() {
        if (!m_livery || m_livery->cockpit.style == ship::CockpitStyle::None) return false;
        const auto& c = m_livery->cockpit;
        const sf::Vector2f p = toScreen(c.pos);
        return std::hypot(p.x - m_mouse.x, p.y - m_mouse.y) < std::max(12.f, std::max(c.w, c.h) * 0.5f * m_zoom);
    }

    void beginGrab(Grab g) {
        Xform x = xform();
        if (!x.valid()) { m_grab = Grab::None; return; }
        m_grab = g;
        m_grabStartPos = *x.pos;
        m_grabStartW = *x.w;
        m_grabStartH = *x.h;
        m_grabStartAngle = *x.angle;
        m_grabLocal = toLocal(m_mouse);
        const sf::Vector2f c = toScreen(*x.pos);
        m_grabStartMouseAngle = std::atan2(m_mouse.y - c.y, m_mouse.x - c.x) * 180.f / 3.14159f;
    }

    /**
     * @brief Free-form move / rotate / scale, continuous by default.
     *
     * No grid: a detail can sit at 45.6 degrees and 45.44 px wide. Hold SHIFT
     * to snap -- 1px for position and size, 5 degrees for rotation -- because
     * an exact stripe still needs to be possible.
     */
    void dragGizmo(bool shift) {
        Xform x = xform();
        if (!x.valid()) return;

        const ship::Decal backup = (m_selKind == SelKind::Decal && selected()) ? *selected() : ship::Decal{};
        const ship::Cockpit cpBackup = m_livery->cockpit;

        if (m_grab == Grab::Move) {
            sf::Vector2f p = m_grabStartPos + (toLocal(m_mouse) - m_grabLocal);
            if (shift) p = { std::round(p.x), std::round(p.y) };
            *x.pos = p;
        }
        else if (m_grab == Grab::Rotate) {
            const sf::Vector2f c = toScreen(*x.pos);
            const float now = std::atan2(m_mouse.y - c.y, m_mouse.x - c.x) * 180.f / 3.14159f;
            float a = m_grabStartAngle + (now - m_grabStartMouseAngle);
            if (shift) a = std::round(a / 5.f) * 5.f;
            *x.angle = a;
        }
        else {
            // Mouse into the object's own axes: scaling stays intuitive at any angle.
            const float a = *x.angle * 3.14159f / 180.f;
            const float ca = std::cos(a), sa = std::sin(a);
            const sf::Vector2f d = toLocal(m_mouse) - *x.pos;
            const float lx = d.x * ca + d.y * sa;
            const float ly = -d.x * sa + d.y * ca;
            if (m_grab != Grab::ScaleH) *x.w = std::max(2.f, std::fabs(lx) * 2.f);
            if (m_grab != Grab::ScaleW) *x.h = std::max(2.f, std::fabs(ly) * 2.f);
            if (shift) { *x.w = std::round(*x.w); *x.h = std::round(*x.h); }
        }

        if (m_selKind == SelKind::Decal) {
            ship::Decal* d = selected();
            ship::clampDecal(*d);
            if (!ship::decalInside(*d, m_design->envelope())) { *d = backup; reject("OUTSIDE THE MODEL LIMIT"); }
        }
        else {
            auto& c = m_livery->cockpit;
            c.w = std::clamp(c.w, 3.f, 60.f);
            c.h = std::clamp(c.h, 3.f, 60.f);
            ship::Decal probe;
            probe.pos = c.pos; probe.w = c.w; probe.h = c.h;
            if (!ship::decalInside(probe, m_design->envelope())) { m_livery->cockpit = cpBackup; reject("CANOPY OUTSIDE THE MODEL LIMIT"); }
        }
    }

    // ---- Colour picker ----------------------------------------------------

    void syncPickerFrom(sf::Color c) {
        ship::toHSV(c, m_pickH, m_pickS, m_pickV);
        m_pickA = c.a / 255.f;
    }

    void dragPicker() {
        if (m_pickDrag == 1) {
            m_pickS = std::clamp((m_mouse.x - m_svRect.x) / std::max(1.f, m_svRect.w), 0.f, 1.f);
            m_pickV = 1.f - std::clamp((m_mouse.y - m_svRect.y) / std::max(1.f, m_svRect.h), 0.f, 1.f);
        }
        else if (m_pickDrag == 2) {
            m_pickH = std::clamp((m_mouse.x - m_hueRect.x) / std::max(1.f, m_hueRect.w), 0.f, 1.f) * 360.f;
        }
        else if (m_pickDrag == 3) {
            m_pickA = std::clamp((m_mouse.x - m_alphaRect.x) / std::max(1.f, m_alphaRect.w), 0.f, 1.f);
        }
        applyPaletteColor(ship::fromHSV(m_pickH, m_pickS, m_pickV,
            static_cast<std::uint8_t>(m_pickA * 255.f)));
    }

    ship::Decal* selected() {
        if (!m_livery || m_selDecal < 0 || m_selDecal >= static_cast<int>(m_livery->decals.size())) return nullptr;
        return &m_livery->decals[m_selDecal];
    }

    void deleteSelected() {
        if (!selected()) return;
        m_livery->decals.erase(m_livery->decals.begin() + m_selDecal);
        m_selDecal = -1;
        m_selKind = SelKind::None;
        toast("DETAIL REMOVED");
    }

    void addDecal(ship::DecalKind kind) {
        if (!m_livery || !m_design) return;
        if (static_cast<int>(m_livery->decals.size()) >= ship::MAX_DECALS) {
            reject("DETAIL LIMIT REACHED"); return;
        }
        ship::Decal d;
        d.kind = kind;
        d.pos = { 0.f, m_design->stats().centreOfMass.y };
        d.color = m_livery->paint.outline;
        if (kind == ship::DecalKind::Line) { d.w = 22.f; d.h = 3.f; d.thickness = 3.f; }
        if (kind == ship::DecalKind::Ring) { d.w = 18.f; d.h = 18.f; d.thickness = 2.f; }
        if (kind == ship::DecalKind::Oval) { d.w = 16.f; d.h = 10.f; }
        if (kind == ship::DecalKind::Tri) { d.w = 12.f; d.h = 14.f; }
        if (!ship::decalInside(d, m_design->envelope())) { reject("NO ROOM INSIDE THE MODEL LIMIT"); return; }
        m_livery->decals.push_back(d);
        m_selDecal = static_cast<int>(m_livery->decals.size()) - 1;
        m_selKind = SelKind::Decal;
        m_paintTarget = PaintTarget::Decal;
        syncPickerFrom(d.color);
        toast(std::string(ship::decalKindName(kind)) + " ADDED - DRAG TO PLACE");
    }

    /// Nudge one property of the selected detail (or the cockpit if none).
    void tweak(int prop, float delta) {
        ship::Decal* d = selected();
        if (!d) {
            if (!m_livery || m_livery->cockpit.style == ship::CockpitStyle::None) return;
            auto& c = m_livery->cockpit;
            if (prop == 0) c.w = std::clamp(c.w + delta, 3.f, 60.f);
            else if (prop == 1) c.h = std::clamp(c.h + delta, 3.f, 60.f);
            else if (prop == 2) c.angle += delta;
            return;
        }
        const ship::Decal before = *d;
        if (prop == 0) d->w += delta;
        else if (prop == 1) d->h += delta;
        else if (prop == 2) d->angle += delta;
        else if (prop == 3) d->thickness += delta;
        ship::clampDecal(*d);
        if (!ship::decalInside(*d, m_design->envelope())) { *d = before; reject("OUTSIDE THE MODEL LIMIT"); }
    }

    void applyPaletteColor(sf::Color c) {
        if (!m_livery) return;
        auto& p = m_livery->paint;
        switch (m_paintTarget) {
        case PaintTarget::Hull:    p.hull = c; break;
        case PaintTarget::Outline: p.outline = c; break;
        case PaintTarget::Plasma:  p.plasma = c; break;
        case PaintTarget::Thrust:  p.thrust = sf::Color(c.r, c.g, c.b, 180); break;
        case PaintTarget::Turbo:   p.turbo = sf::Color(c.r, c.g, c.b, 220); break;
        case PaintTarget::Dodge:   p.dodge = sf::Color(c.r, c.g, c.b, 230); break;
        case PaintTarget::Parry:   p.parry = c; break;
        case PaintTarget::Homing:  p.homing = c; break;
        case PaintTarget::Cockpit: p.cockpit = sf::Color(c.r, c.g, c.b, 235); break;
        case PaintTarget::Decal:   if (ship::Decal* d = selected()) d->color = c; break;
        }
    }

    sf::Color targetColor(PaintTarget t) const {
        if (!m_livery) return TEXT;
        const auto& p = m_livery->paint;
        switch (t) {
        case PaintTarget::Hull:    return p.hull;
        case PaintTarget::Outline: return p.outline;
        case PaintTarget::Plasma:  return p.plasma;
        case PaintTarget::Thrust:  return p.thrust;
        case PaintTarget::Turbo:   return p.turbo;
        case PaintTarget::Dodge:   return p.dodge;
        case PaintTarget::Parry:   return p.parry;
        case PaintTarget::Homing:  return p.homing;
        case PaintTarget::Cockpit: return p.cockpit;
        default: break;
        }
        if (m_selDecal >= 0 && m_selDecal < static_cast<int>(m_livery->decals.size()))
            return m_livery->decals[m_selDecal].color;
        return TEXT_DEAD;
    }

    // ---- Ship files -------------------------------------------------------

    void exportShip() {
        if (!m_design || !m_livery) return;
        std::string err;
        char name[64];
        std::snprintf(name, sizeof(name), "%s-%d", m_design->spec().name, m_design->pointCount());
        const std::string path = ship::shipfile::save(*m_design, *m_livery, name, err);
        m_fileMsg = path.empty() ? ("EXPORT FAILED - " + err) : ("SAVED " + path);
    }

    void importShip() {
        if (!m_design || !m_livery) return;
        const auto files = ship::shipfile::list();
        if (files.empty()) { m_fileMsg = "NO FILES IN ships/"; return; }
        m_fileIdx = (m_fileIdx % static_cast<int>(files.size()) + static_cast<int>(files.size()))
            % static_cast<int>(files.size());
        std::string err, name;
        ship::ShipDesign d = *m_design;
        ship::Livery lv;
        if (ship::shipfile::load(files[m_fileIdx], d, lv, name, err)) {
            *m_design = d;
            *m_livery = lv;
            m_selDecal = -1;
            m_viewInit = false;
            m_fileMsg = "LOADED " + name + (err.empty() ? "" : "  (" + err + ")");
        }
        else {
            m_fileMsg = err;
        }
        m_fileIdx = (m_fileIdx + 1) % static_cast<int>(files.size());
    }

    /// One-shot hit test against the latched click.
    bool consumeClick(const Rect& r) {
        if (!m_clickPending || !r.contains(m_clickPos)) return false;
        m_clickPending = false;
        return true;
    }

    void setMode(Mode m) {
        m_mode = m;
        m_dragging = -1;
        m_selDecal = -1;
        m_selKind = SelKind::None;
        m_grab = Grab::None;
        toast(m == Mode::Model ? "MODEL - SHAPE WHAT THE SHIP LOOKS LIKE"
            : m == Mode::Paint ? "PAINT - COLOURS, DETAILS AND THE CANOPY"
            : "HULL - HITBOX AND MOUNTS");
    }

    // ---- Actions ----------------------------------------------------------

    void toggleGun(int vertexIndex) {
        if (m_design->isGunMounted(vertexIndex)) {
            m_design->unmountGun(vertexIndex);
            toast("WEAPON REMOVED");
        }
        else if (m_design->mountGun(vertexIndex)) {
            toast(m_design->stats().spinalVertex == vertexIndex ? "WEAPON FITTED - RIFT MOUNT"
                : "WEAPON FITTED - PLASMA");
        }
        else {
            reject(m_design->lastRejectReason());
        }
    }

    void toggleEngine(int edgeIndex) {
        if (m_design->isEngineMounted(edgeIndex)) {
            m_design->unmountEngine(edgeIndex);
            toast("DRIVE REMOVED");
        }
        else if (m_design->mountEngine(edgeIndex)) {
            toast("DRIVE FITTED");
        }
        else {
            reject(m_design->lastRejectReason());
        }
    }

    void assignRift(int vertexIndex) {
        const bool wasSpinal = (m_design->stats().spinalVertex == vertexIndex);
        if (!m_design->assignRift(vertexIndex)) { reject(m_design->lastRejectReason()); return; }
        if (!wasSpinal) toast("RIFT MOUNT MOVED");
        else toast(m_design->stats().riftShared ? "RIFT SHARED - FIRES PLASMA TOO"
            : "RIFT DEDICATED");
    }

    /**
     * @brief Load a class's preset hull.
     *
     * The tonnage bands do not overlap, so a hull can never "fit" a different
     * class -- switching always starts from that class's pattern. Mirror mode
     * carries over; a hull that snaps to a different shape than the button
     * promised would be worse than a clean preset.
     */
    void loadClass(ship::HullClass c) {
        const bool sym = m_design->symmetric();
        *m_design = ship::ShipDesign::preset(c);
        if (!sym) m_design->setSymmetric(false);
        const ship::HullClassSpec& s = ship::classSpec(c);
        toast(std::string(s.name) + " FRAME - " + s.pattern + " PATTERN");
    }

    void toggleMirror() {
        m_design->setSymmetric(!m_design->symmetric());
        toast(m_design->symmetric() ? "MIRROR ON - MOUNTS CLEARED, RE-MOUNT" : "MIRROR OFF");
        if (m_design->symmetric() && m_design->mountedGuns().empty()) m_design->autoMount();
    }

    void toast(const std::string& s) { m_toast = s; m_toastTimer = 2.2f; }

    void reject(const char* why) {
        m_rejectFlash = 1.f;
        m_rejectAt = m_mouse;
        m_rejectText = (why && *why) ? why : "REFUSED";
    }

    // ========================================================================
    // CLIPPING
    // ========================================================================

    /// Delegates to tui::UI so clipped widgets hit-test in the right space.
    void beginClip(const Rect& r) { m_ui.beginClip({ r.x, r.y, r.w, r.h }); }
    void endClip() { m_ui.endClip(); }

    float revealOf(float delay) const {
        const float t = m_openTimer - delay;
        return (t <= 0.f) ? 0.f : std::clamp(t / OPEN_DUR, 0.f, 1.f);
    }

    // ========================================================================
    // DRAW
    // ========================================================================

    void draw() {
        m_window->setView(uiView());
        const sf::Vector2f size = viewSize();

        sf::RectangleShape bg(size);
        bg.setPosition({ 0.f, 0.f });
        bg.setFillColor(VOID_BG);
        m_window->draw(bg);

        drawHeader(size);
        drawHullPanel();
        drawStatsPanel();
        drawMountPanel();
        drawStatusPanel();
        drawHints(size);
        drawControls(size);
    }

    void drawHeader(const sf::Vector2f& size) {
        sf::Text title(*m_font, "REFIT BAY", 34);
        title.setLetterSpacing(1.8f);
        title.setFillColor(CYAN);
        title.setPosition({ size.x * 0.015f, size.y * 0.028f });
        m_window->draw(title);

        // ---- Layer tabs ----
        float tx = size.x * 0.015f + title.getGlobalBounds().size.x + 36.f;
        const float ty = size.y * 0.030f, th = 34.f;
        if (m_ui.button({ tx, ty, 120.f, th }, "HULL", m_mode == Mode::Hull, false, false, 16))
            if (m_mode != Mode::Hull) setMode(Mode::Hull);
        tx += 128.f;
        if (m_ui.button({ tx, ty, 120.f, th }, "MODEL", m_mode == Mode::Model, false, false, 16))
            if (m_mode != Mode::Model) setMode(Mode::Model);
        tx += 128.f;
        if (m_ui.button({ tx, ty, 120.f, th }, "PAINT", m_mode == Mode::Paint, false, false, 16))
            if (m_mode != Mode::Paint) setMode(Mode::Paint);
        tx += 150.f;

        std::string sub = std::string(m_design->spec().name) + " FRAME";
        sf::Text cls(monoFont(), sub, 15);
        cls.setLetterSpacing(1.8f);
        cls.setFillColor(AMBER);
        cls.setPosition({ tx, size.y * 0.042f });
        m_window->draw(cls);

        sf::Text mir(monoFont(), m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF", 15);
        mir.setLetterSpacing(1.8f);
        mir.setFillColor(m_design->symmetric() ? CYAN : AMBER);
        mir.setPosition({ tx + cls.getGlobalBounds().size.x + 30.f, size.y * 0.042f });
        m_window->draw(mir);

        const ship::ValidationResult v = m_design->validate();
        sf::Text st(monoFont(), v.ok ? "HULL  AIRWORTHY" : "HULL  REJECTED", 16);
        st.setLetterSpacing(1.6f);
        st.setFillColor(v.ok ? GREEN : RED);
        st.setPosition({ size.x - st.getGlobalBounds().size.x - size.x * 0.015f,
                         size.y * 0.042f });
        m_window->draw(st);

        hline(size.x * 0.012f, size.y * 0.092f, size.x * 0.976f, CYAN_LOW);
    }

    bool panelChrome(const Rect& r, const std::string& label, float delay,
        sf::Color accent, bool heavy) {
        const float p = revealOf(delay);
        if (p <= 0.f) return false;

        const float wf = std::clamp(p / 0.34f, 0.06f, 1.f);
        const float hf = std::clamp((p - 0.22f) / 0.78f, 0.f, 1.f);
        const float dw = r.w * wf;
        const float dh = std::max(2.f, r.h * hf);
        const Rect d{ r.cx() - dw * 0.5f, r.cy() - dh * 0.5f, dw, dh };

        sf::RectangleShape fill({ d.w, d.h });
        fill.setPosition({ d.x, d.y });
        fill.setFillColor(PANEL_BG);
        m_window->draw(fill);

        if (p < 0.98f) {
            const float a = std::clamp((1.f - p) * 2.2f, 0.f, 1.f);
            hline(d.x, d.cy(), d.w, sf::Color(accent.r, accent.g, accent.b,
                static_cast<std::uint8_t>(235 * a)));
        }

        const sf::Color c(accent.r, accent.g, accent.b, heavy ? 210 : 130);
        for (int i = 0; i < (heavy ? 2 : 1); ++i) {
            hline(d.x, d.y + static_cast<float>(i), d.w, c);
            hline(d.x, d.y + d.h - static_cast<float>(i), d.w, c);
        }
        vline(d.x, d.y, d.h, c);
        vline(d.x + d.w, d.y, d.h, c);

        if (p < 1.f) return false;

        if (!label.empty()) {
            sf::Text t(monoFont(), label, 14);
            t.setLetterSpacing(1.8f);
            if (heavy) {
                const float tw = t.getGlobalBounds().size.x + 14.f;
                sf::RectangleShape tab({ tw, 20.f });
                tab.setPosition({ r.x + 8.f, r.y - 10.f });
                tab.setFillColor(accent);
                m_window->draw(tab);
                t.setFillColor(sf::Color(4, 5, 8));
                t.setPosition({ r.x + 15.f, r.y - 8.f });
            }
            else {
                const float tw = t.getGlobalBounds().size.x + 12.f;
                sf::RectangleShape gap({ tw, 4.f });
                gap.setPosition({ r.x + 10.f, r.y - 2.f });
                gap.setFillColor(VOID_BG);
                m_window->draw(gap);
                t.setFillColor(accent);
                t.setPosition({ r.x + 16.f, r.y - 9.f });
            }
            m_window->draw(t);
        }
        return true;
    }

    // ---- EDITOR panel -----------------------------------------------------
    void drawHullPanel() {
        if (m_mode == Mode::Paint) { drawPaintCanvas(); return; }
        const Rect r = hullRect();
        const bool model = (m_mode == Mode::Model);
        if (!panelChrome(r, model ? "MODEL" : "HULL GEOMETRY", 0.00f, model ? VIOLET : CYAN_MID, true)) return;

        const auto& pts = m_design->points();
        const auto& stats = m_design->stats();
        const auto& spec = m_design->spec();
        const auto& dc = m_design->decorCheck();
        const int n = static_cast<int>(pts.size());
        const float Z = m_zoom;

        beginClip(r);
        {
            const float cx = r.w * 0.5f, cy = r.h * 0.5f;
            const auto L = [&](sf::Vector2f v) {
                return sf::Vector2f{ cx + (v.x - m_viewCenter.x) * Z, cy + (v.y - m_viewCenter.y) * Z };
                };

            // ---- Grid, one line per 10 design units, only what is visible ----
            {
                sf::VertexArray grid(sf::PrimitiveType::Lines);
                const sf::Color gc(16, 27, 35);
                const sf::Vector2f lo = { m_viewCenter.x - cx / Z, m_viewCenter.y - cy / Z };
                const sf::Vector2f hi = { m_viewCenter.x + cx / Z, m_viewCenter.y + cy / Z };
                for (float g = std::floor(lo.x / 10.f) * 10.f; g <= hi.x; g += 10.f) {
                    const float x = L({ g, 0.f }).x;
                    grid.append({ { x, 0.f }, gc }); grid.append({ { x, r.h }, gc });
                }
                for (float g = std::floor(lo.y / 10.f) * 10.f; g <= hi.y; g += 10.f) {
                    const float y = L({ 0.f, g }).y;
                    grid.append({ { 0.f, y }, gc }); grid.append({ { r.w, y }, gc });
                }
                m_window->draw(grid);
            }

            // ---- Class frame (the hitbox's box) ----
            {
                const sf::Vector2f a = L({ -spec.halfWidthPx, spec.foreYPx });
                const sf::Vector2f b = L({ spec.halfWidthPx, spec.aftYPx });
                const bool breached = m_rejectFlash > 0.f && m_rejectText.find("FRAME") != std::string::npos;
                const std::uint8_t base = model ? 30 : 60;
                const sf::Color fc = breached
                    ? sf::Color(255, 48, 0, static_cast<std::uint8_t>(90 + 140 * m_rejectFlash))
                    : sf::Color(255, 214, 0, base);

                sf::RectangleShape frame({ b.x - a.x, b.y - a.y });
                frame.setPosition(a);
                frame.setFillColor(sf::Color(255, 214, 0, model ? 0 : 6));
                frame.setOutlineThickness(1.f);
                frame.setOutlineColor(fc);
                m_window->draw(frame);

                const sf::Color tc(255, 214, 0, model ? 70 : 170);
                const float t = 12.f;
                sf::VertexArray ticks(sf::PrimitiveType::Lines);
                const auto tick = [&](sf::Vector2f p, float sx, float sy) {
                    ticks.append({ p, tc }); ticks.append({ { p.x + t * sx, p.y }, tc });
                    ticks.append({ p, tc }); ticks.append({ { p.x, p.y + t * sy }, tc });
                    };
                tick(a, 1.f, 1.f);             tick({ b.x, a.y }, -1.f, 1.f);
                tick({ a.x, b.y }, 1.f, -1.f); tick(b, -1.f, -1.f);
                m_window->draw(ticks);

                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s FRAME", spec.name);
                sf::Text ft(monoFont(), buf, 11);
                ft.setLetterSpacing(1.8f);
                ft.setFillColor(tc);
                ft.setPosition({ a.x + 6.f, a.y + 4.f });
                m_window->draw(ft);
            }

            // ---- Model envelope: where the model may reach ----
            if (model) {
                const auto env = m_design->envelope();
                dashedPolygon(env, L, sf::Color(175, 95, 255, 110), 6.f, 5.f);
                sf::Text et(monoFont(), "MODEL LIMIT", 11);
                et.setLetterSpacing(1.8f);
                et.setFillColor(sf::Color(175, 95, 255, 140));
                const sf::Vector2f top = L(env[0]);
                sf::Vector2f topmost = top;
                for (const auto& p : env) { const sf::Vector2f q = L(p); if (q.y < topmost.y) topmost = q; }
                et.setPosition({ topmost.x + 10.f, topmost.y - 4.f });
                m_window->draw(et);
            }

            // ---- Centreline ----
            {
                const float ax = L({ 0.f, 0.f }).x;
                sf::VertexArray axis(sf::PrimitiveType::Lines, 2);
                const sf::Color ac = m_design->symmetric()
                    ? sf::Color(40, 245, 255, 70) : sf::Color(60, 66, 78, 70);
                axis[0] = { { ax, 0.f }, ac };
                axis[1] = { { ax, r.h }, ac };
                m_window->draw(axis);

                sf::Text fwd(monoFont(), "FORWARD", 12);
                fwd.setLetterSpacing(1.8f);
                fwd.setFillColor(TEXT_DEAD);
                fwd.setPosition({ ax + 12.f, 12.f });
                m_window->draw(fwd);

                sf::VertexArray arrow(sf::PrimitiveType::Lines, 6);
                arrow[0] = { { ax, 42.f }, TEXT_DEAD }; arrow[1] = { { ax, 14.f }, TEXT_DEAD };
                arrow[2] = { { ax, 14.f }, TEXT_DEAD }; arrow[3] = { { ax - 5.f, 23.f }, TEXT_DEAD };
                arrow[4] = { { ax, 14.f }, TEXT_DEAD }; arrow[5] = { { ax + 5.f, 23.f }, TEXT_DEAD };
                m_window->draw(arrow);
            }

            // ---- The two layers. The one being edited is drawn on top, bright. ----
            const auto& decor = m_design->decor();
            const bool showModel = m_design->decorAuthored() || model;
            const sf::Color modelLine = dc.ok ? (model ? CYAN : sf::Color(40, 245, 255, 90)) : RED;

            if (model) {
                // Model solid, hitbox as a ghost on top of it.
                fillPolygon(m_design->decor(), L, sf::Color(20, 60, 90, 150));
                strokePolygon(decor, L, modelLine, 2.f);
                if (n >= 3) dashedPolygon(pts, L, sf::Color(40, 245, 255, 150), 4.f, 4.f);
            }
            else {
                // Model as a silhouette behind the hull you are editing.
                if (showModel && decor.size() >= 3) {
                    fillPolygon(decor, L, sf::Color(20, 60, 90, 45));
                    strokePolygon(decor, L, dc.ok ? sf::Color(40, 245, 255, 55) : sf::Color(255, 48, 0, 120), 1.f);
                }
                if (n >= 3) {
                    fillPolygon(pts, L, sf::Color(20, 60, 90, 130));
                    strokePolygon(pts, L, CYAN, 2.f);
                }
            }

            // ---- Drives: the edge itself lights up. No nozzle art. ----
            for (const auto& e : m_design->engineSlots()) {
                const bool mounted = m_design->isEngineMounted(e.index);
                const bool hov = !model && (m_hoverEdge == e.index);
                if (!mounted && !hov && (model || !e.valid)) continue;

                const sf::Vector2f a = L(pts[e.index]);
                const sf::Vector2f b = L(pts[(e.index + 1) % n]);
                sf::Color c;
                float w;
                if (mounted) { c = model ? sf::Color(255, 214, 0, 90) : AMBER; w = model ? 2.f : 4.f; }
                else if (e.valid) { c = sf::Color(255, 214, 0, hov ? 150 : 45); w = hov ? 3.f : 1.5f; }
                else { c = sf::Color(255, 48, 0, 150); w = 2.f; }
                thickLine(a, b, c, w);

                if (hov) {
                    const sf::Vector2f mid = (a + b) * 0.5f;
                    sf::Text t(monoFont(), e.valid ? (mounted ? "DRIVE  RMB REMOVE" : "DRIVE  RMB FIT")
                        : e.reason, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(e.valid ? AMBER : RED);
                    t.setPosition({ mid.x + e.outward.x * 14.f + 6.f, mid.y + e.outward.y * 14.f });
                    m_window->draw(t);
                }
            }

            // ---- Guns ----
            for (const auto& g : m_design->gunSlots()) {
                const sf::Vector2f p = L(pts[g.index]);
                const bool mounted = m_design->isGunMounted(g.index);
                const bool spinal = mounted && stats.spinalVertex == g.index;
                const sf::Color role = spinal ? VIOLET : CYAN;

                if (model) {
                    // Show where each gun's bolts will leave: the model spike tip.
                    if (!mounted) continue;
                    const sf::Vector2f mz = L(m_design->muzzleFor(g.index));
                    dashedLine(p, mz, sf::Color(role.r, role.g, role.b, 150), 3.f, 3.f);
                    diamond(p, 3.f, sf::Color(role.r, role.g, role.b, 170));
                    cross(mz, 5.f, role);
                    continue;
                }

                const bool hov = (m_hoverPoint == g.index);
                const bool drag = (m_dragging == g.index);

                if (mounted) {
                    // A plain spike: reads as "a gun points this way", nothing more.
                    const float len = spinal ? 18.f : 13.f, half = spinal ? 5.f : 4.f;
                    sf::ConvexShape spike(3);
                    spike.setPoint(0, { p.x - half, p.y - 2.f });
                    spike.setPoint(1, { p.x + half, p.y - 2.f });
                    spike.setPoint(2, { p.x, p.y - len });
                    spike.setFillColor(role);
                    m_window->draw(spike);

                    if (spinal) {
                        sf::Text tag(monoFont(), stats.riftShared ? "RIFT+PLASMA" : "RIFT", 11);
                        tag.setLetterSpacing(1.6f);
                        tag.setFillColor(VIOLET);
                        tag.setPosition({ p.x - tag.getGlobalBounds().size.x * 0.5f, p.y - len - 18.f });
                        m_window->draw(tag);
                    }
                }

                const float sz = (drag || hov) ? 9.f : 7.f;
                sf::RectangleShape h({ sz, sz });
                h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                h.setFillColor(mounted ? role : (g.valid ? CYAN_MID : sf::Color(90, 100, 112)));
                m_window->draw(h);

                if (hov || drag) {
                    const float rs = sz + 8.f;
                    sf::RectangleShape ringSq({ rs, rs });
                    ringSq.setPosition({ p.x - rs * 0.5f, p.y - rs * 0.5f });
                    ringSq.setFillColor(sf::Color::Transparent);
                    ringSq.setOutlineThickness(1.f);
                    ringSq.setOutlineColor(AMBER);
                    m_window->draw(ringSq);

                    char buf[96];
                    if (!g.valid)
                        std::snprintf(buf, sizeof(buf), "%.0f deg  %s", g.interiorAngleDeg, g.reason);
                    else if (spinal)
                        std::snprintf(buf, sizeof(buf), "RIFT MOUNT  R: %s",
                            stats.gunCount > 1 ? (stats.riftShared ? "DEDICATE" : "SHARE") : "LONE GUN");
                    else if (mounted)
                        std::snprintf(buf, sizeof(buf), "PLASMA  R: MAKE RIFT MOUNT");
                    else
                        std::snprintf(buf, sizeof(buf), "%.0f deg  RMB FIT WEAPON", g.interiorAngleDeg);
                    sf::Text t(monoFont(), buf, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(g.valid ? (spinal ? VIOLET : GREEN) : RED);
                    t.setPosition({ p.x + 14.f, p.y - 6.f });
                    m_window->draw(t);
                }
            }

            // ---- Model handles ----
            if (model) {
                for (int i = 0; i < static_cast<int>(decor.size()); ++i) {
                    const sf::Vector2f p = L(decor[i]);
                    const bool hov = (m_hoverDecor == i), drag = (m_dragging == i);
                    const float sz = (hov || drag) ? 8.f : 5.f;
                    sf::RectangleShape h({ sz, sz });
                    h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                    h.setFillColor(hov || drag ? AMBER : CYAN);
                    m_window->draw(h);
                }
                if (m_hoverDecor >= 0 && m_dragging < 0) {
                    const sf::Vector2f p = L(decor[m_hoverDecor]);
                    sf::Text t(monoFont(), "DRAG MOVE   RMB REMOVE", 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(AMBER);
                    t.setPosition({ p.x + 12.f, p.y - 6.f });
                    m_window->draw(t);
                }
            }

            // ---- Centre of mass and thrust point (hull only) ----
            if (!model) {
                const sf::Vector2f com = L(stats.centreOfMass);
                cross(com, 7.f, sf::Color(255, 255, 255, 190));
                if (stats.engineCount > 0) {
                    const sf::Vector2f tp = L(stats.thrustPoint);
                    cross(tp, 5.f, AMBER);
                    if (std::fabs(stats.lateralOffsetPx) > 1.5f) thickLine(com, { tp.x, com.y }, sf::Color(255, 48, 0, 200), 1.f);
                }
            }

            // ---- Refused edit ----
            if (m_rejectFlash > 0.f) {
                const sf::Vector2f p{ m_rejectAt.x - r.x, m_rejectAt.y - r.y };
                const std::uint8_t a = static_cast<std::uint8_t>(230 * std::min(1.f, m_rejectFlash * 1.5f));
                cross(p, 10.f, sf::Color(255, 48, 0, a));
                sf::Text t(monoFont(), m_rejectText, 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(sf::Color(255, 48, 0, a));
                t.setPosition({ p.x + 14.f, p.y + 6.f });
                m_window->draw(t);
            }

            if (m_design->validate().wasConcave) {
                sf::Text t(monoFont(), "POINTS INSIDE THE HULL WERE DISCARDED", 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(AMBER);
                t.setPosition({ 12.f, r.h - 24.f });
                m_window->draw(t);
            }
        }
        endClip();
    }

    // ---- PAINT canvas -----------------------------------------------------
    void drawPaintCanvas() {
        const Rect r = hullRect();
        if (!panelChrome(r, isEffectTarget(m_paintTarget) ? "LIVE PREVIEW" : "PAINT",
            0.00f, VIOLET, true) || !m_livery) return;
        if (isEffectTarget(m_paintTarget)) { drawEffectShowcase(r); return; }

        const auto& lv = *m_livery;
        const auto& outline = m_design->renderOutline();
        const float Z = m_zoom;

        beginClip(r);
        {
            const float cx = r.w * 0.5f, cy = r.h * 0.5f;
            const auto L = [&](sf::Vector2f v) {
                return sf::Vector2f{ cx + (v.x - m_viewCenter.x) * Z, cy + (v.y - m_viewCenter.y) * Z };
                };
            const auto tris = [&](const std::vector<sf::Vector2f>& t, sf::Color c) {
                sf::VertexArray va(sf::PrimitiveType::Triangles, t.size());
                for (std::size_t i = 0; i < t.size(); ++i) va[i] = { L(t[i]), c };
                m_window->draw(va);
                };

            // Faint grid only: this is a preview, not a blueprint.
            sf::VertexArray grid(sf::PrimitiveType::Lines);
            const sf::Color gc(14, 22, 30);
            for (float g = -160.f; g <= 160.f; g += 20.f) {
                grid.append({ { L({ g, 0.f }).x, 0.f }, gc }); grid.append({ { L({ g, 0.f }).x, r.h }, gc });
                grid.append({ { 0.f, L({ 0.f, g }).y }, gc }); grid.append({ { r.w, L({ 0.f, g }).y }, gc });
            }
            m_window->draw(grid);

            // The ship exactly as the game draws it: decals under, hull, decals over, canopy.
            std::vector<sf::Vector2f> buf;
            for (const auto& d : lv.decals) {
                if (d.over) continue;
                buf.clear(); ship::decalGeometry(d, buf, false);
                if (d.mirrored) ship::decalGeometry(d, buf, true);
                tris(buf, d.color);
            }
            fillPolygon(outline, L, lv.paint.hull);
            strokePolygon(outline, L, lv.paint.outline, 2.5f);
            for (const auto& d : lv.decals) {
                if (!d.over) continue;
                buf.clear(); ship::decalGeometry(d, buf, false);
                if (d.mirrored) ship::decalGeometry(d, buf, true);
                tris(buf, d.color);
            }
            if (lv.cockpit.style != ship::CockpitStyle::None) {
                std::vector<sf::Vector2f> glass, rim;
                ship::cockpitGeometry(lv.cockpit, glass, rim);
                tris(rim, sf::Color(14, 18, 26, 235));
                tris(glass, lv.paint.cockpit);
            }

            // Hover hint on an unselected detail.
            if (m_hoverDecal >= 0 && m_hoverDecal != m_selDecal
                && m_hoverDecal < static_cast<int>(lv.decals.size())) {
                const auto& d = lv.decals[m_hoverDecal];
                const float rad = std::max(9.f, ship::decalRadius(d) * Z);
                const sf::Vector2f c = L(d.pos);
                sf::RectangleShape box({ rad * 2.f, rad * 2.f });
                box.setPosition({ c.x - rad, c.y - rad });
                box.setFillColor(sf::Color::Transparent);
                box.setOutlineThickness(1.f);
                box.setOutlineColor(sf::Color(255, 214, 0, 110));
                m_window->draw(box);
            }

            // ---- Transform gizmo: box, eight handles, rotation stalk ----
            {
                sf::Vector2f hs[8], centre, rot;
                if (gizmoHandles(hs, centre, rot)) {
                    const sf::Vector2f o{ r.x, r.y };   // gizmo comes back in screen space
                    const auto P = [&](sf::Vector2f p) { return sf::Vector2f{ p.x - o.x, p.y - o.y }; };
                    for (int i = 0; i < 4; ++i) thickLine(P(hs[i]), P(hs[(i + 1) % 4]), sf::Color(255, 214, 0, 190), 1.f);
                    dashedLine(P(hs[4]), P(rot), AMBER, 4.f, 3.f);

                    sf::CircleShape rh(5.f);
                    rh.setOrigin({ 5.f, 5.f });
                    rh.setPosition(P(rot));
                    rh.setFillColor(m_grab == Grab::Rotate ? sf::Color::White : AMBER);
                    m_window->draw(rh);

                    for (int i = 0; i < 8; ++i) {
                        const float sz = (i < 4) ? 8.f : 6.f;
                        sf::RectangleShape h({ sz, sz });
                        const sf::Vector2f p = P(hs[i]);
                        h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                        h.setFillColor(std::hypot(hs[i].x - m_mouse.x, hs[i].y - m_mouse.y) < 9.f
                            ? sf::Color::White : AMBER);
                        m_window->draw(h);
                    }
                    cross(P(centre), 4.f, sf::Color(255, 214, 0, 160));

                    // Live numbers while dragging: free values need a readout.
                    Xform x = xform();
                    if (x.valid()) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf), "%.2f x %.2f   %.2f deg", *x.w, *x.h, *x.angle);
                        sf::Text t(monoFont(), buf, 12);
                        t.setLetterSpacing(1.3f);
                        t.setFillColor(AMBER);
                        t.setPosition({ P(centre).x + 14.f, P(centre).y + 10.f });
                        m_window->draw(t);
                    }
                }
            }

            // Where the guns and the Rift sit, so paint does not hide the ship's grammar.
            for (int g : m_design->mountedGuns()) {
                const bool spinal = (m_design->stats().spinalVertex == g);
                cross(L(m_design->muzzleFor(g)), 4.f,
                    spinal ? sf::Color(175, 95, 255, 170) : sf::Color(40, 245, 255, 130));
            }

            if (m_rejectFlash > 0.f) {
                const sf::Vector2f p{ m_rejectAt.x - r.x, m_rejectAt.y - r.y };
                const std::uint8_t a = static_cast<std::uint8_t>(230 * std::min(1.f, m_rejectFlash * 1.5f));
                cross(p, 10.f, sf::Color(255, 48, 0, a));
                sf::Text t(monoFont(), m_rejectText, 12);
                t.setLetterSpacing(1.4f);
                t.setFillColor(sf::Color(255, 48, 0, a));
                t.setPosition({ p.x + 14.f, p.y + 6.f });
                m_window->draw(t);
            }

            sf::Text hint(monoFont(),
                lv.decals.empty() ? "ADD A DETAIL FROM THE PANEL ON THE RIGHT"
                : "DRAG INSIDE MOVES   CORNERS SCALE   STALK ROTATES   SHIFT SNAPS   RMB REMOVES", 12);
            hint.setLetterSpacing(1.4f);
            hint.setFillColor(TEXT_DEAD);
            hint.setPosition({ 12.f, r.h - 24.f });
            m_window->draw(hint);
        }
        endClip();
    }

    // ========================================================================
    // LIVE EFFECT PREVIEW
    // ========================================================================
    //
    // Colours for particles cannot be judged from a swatch: a thruster plume
    // is fifty translucent sprites over a dark background. So the canvas runs
    // the effect: a static sample on the left, the ship performing it on the
    // right, forever. A self-contained sim -- the real systems need a Box2D
    // world and a live player, neither of which exists in the refit bay.

    static bool isEffectTarget(PaintTarget t) {
        return t == PaintTarget::Plasma || t == PaintTarget::Thrust || t == PaintTarget::Turbo
            || t == PaintTarget::Dodge || t == PaintTarget::Parry || t == PaintTarget::Homing;
    }

    static const char* targetPreviewName(PaintTarget t) {
        switch (t) {
        case PaintTarget::Plasma: return "PLASMA - FIRING";
        case PaintTarget::Thrust: return "THRUST - HOLDING POSITION";
        case PaintTarget::Turbo:  return "TURBO - FULL BURN";
        case PaintTarget::Dodge:  return "DODGE - REPEATING";
        case PaintTarget::Parry:  return "PARRY - REPEATING";
        default:                  return "HOMING - PARRIED AND HIJACKED ROCKS";
        }
    }

    struct FxP {
        sf::Vector2f pos, vel;
        float life = 0.f, maxLife = 1.f, size = 3.f;
        sf::Color color;
    };

    void spawnFx(sf::Vector2f p, sf::Vector2f v, float life, float size, sf::Color c) {
        if (m_fx.size() > 900) return;
        m_fx.push_back({ p, v, life, life, size, c });
    }

    /// Advance the preview. Local pixel space, centred on the preview ship.
    void updateFx(float dt) {
        if (!isEffectTarget(m_paintTarget) || !m_livery) { m_fx.clear(); return; }
        m_fxTime += dt;
        m_fxBeat -= dt;

        for (auto& p : m_fx) {
            p.life -= dt;
            p.pos += p.vel * dt;
            p.vel *= (1.f - std::min(0.9f, dt * 1.6f));
        }
        m_fx.erase(std::remove_if(m_fx.begin(), m_fx.end(),
            [](const FxP& p) { return p.life <= 0.f; }), m_fx.end());

        const auto& paint = m_livery->paint;
        const auto rnd = [](float a, float b) {
            return a + (b - a) * (static_cast<float>(rand() % 1000) / 1000.f);
            };

        switch (m_paintTarget) {
        case PaintTarget::Thrust:
        case PaintTarget::Turbo: {
            // Exhaust from the ship's real drives, standing still on the spot.
            const bool turbo = (m_paintTarget == PaintTarget::Turbo);
            const sf::Color c = turbo ? paint.turbo : paint.thrust;
            for (int e : m_design->mountedEngines()) {
                for (const auto& slot : m_design->engineSlots()) {
                    if (slot.index != e) continue;
                    for (int n = 0; n < (turbo ? 3 : 1); ++n) {
                        const float spread = rnd(-0.35f, 0.35f);
                        const sf::Vector2f o = slot.outward;
                        const sf::Vector2f dir{ o.x * std::cos(spread) - o.y * std::sin(spread),
                                                o.x * std::sin(spread) + o.y * std::cos(spread) };
                        spawnFx(slot.position + o * 4.f, dir * rnd(120.f, 200.f),
                            turbo ? rnd(0.25f, 0.40f) : rnd(0.12f, 0.20f),
                            turbo ? rnd(4.f, 7.f) : rnd(2.5f, 4.5f), c);
                    }
                }
            }
            break;
        }
        case PaintTarget::Plasma: {
            if (m_fxBeat <= 0.f) {
                m_fxBeat = 0.28f;
                const auto& guns = m_design->mountedGuns();
                if (!guns.empty()) {
                    const int g = guns[m_fxShot++ % static_cast<int>(guns.size())];
                    const sf::Vector2f mz = m_design->muzzleFor(g);
                    m_fxBolts.push_back({ mz, { 0.f, -520.f }, 1.1f, 1.1f, 3.f, paint.plasma });
                    for (int n = 0; n < 6; ++n)
                        spawnFx(mz, { rnd(-90.f, 90.f), rnd(-260.f, -120.f) },
                            rnd(0.08f, 0.16f), rnd(2.f, 4.f), paint.plasma);
                }
            }
            break;
        }
        case PaintTarget::Dodge: {
            if (m_fxBeat <= 0.f) {
                m_fxBeat = 1.10f;
                for (int n = 0; n < 22; ++n) {
                    const float a = rnd(0.f, 6.2831f);
                    spawnFx({ 0.f, 0.f }, { std::cos(a) * rnd(160.f, 320.f), std::sin(a) * rnd(160.f, 320.f) },
                        0.22f, rnd(3.f, 6.f), paint.dodge);
                }
                spawnFx({ 0.f, 0.f }, { 0.f, 0.f }, 0.16f, 22.f,
                    sf::Color(std::min(255, paint.dodge.r + 90), std::min(255, paint.dodge.g + 60),
                        std::min(255, paint.dodge.b + 60), 200));
            }
            break;
        }
        case PaintTarget::Parry: {
            if (m_fxBeat <= 0.f) {
                m_fxBeat = 1.30f;
                for (int n = 0; n < 26; ++n) {
                    const float a = rnd(0.f, 6.2831f);
                    spawnFx({ std::cos(a) * 34.f, std::sin(a) * 34.f },
                        { std::cos(a) * rnd(60.f, 170.f), std::sin(a) * rnd(60.f, 170.f) },
                        0.35f, rnd(3.f, 6.f), paint.parry);
                }
            }
            break;
        }
        case PaintTarget::Homing: {
            if (m_fxBeat <= 0.f) {
                m_fxBeat = 1.6f;
                m_fxRock = { -120.f, 70.f };
            }
            // A hijacked rock curving in, trailing the player's colour.
            const sf::Vector2f to{ 0.f - m_fxRock.x, -30.f - m_fxRock.y };
            const float l = std::max(1.f, std::sqrt(to.x * to.x + to.y * to.y));
            m_fxRock += to / l * 170.f * dt;
            spawnFx(m_fxRock, { rnd(-30.f, 30.f), rnd(-30.f, 30.f) }, 0.4f, rnd(3.f, 6.f), paint.homing);
            break;
        }
        default: break;
        }

        for (auto& b : m_fxBolts) { b.life -= dt; b.pos += b.vel * dt; }
        m_fxBolts.erase(std::remove_if(m_fxBolts.begin(), m_fxBolts.end(),
            [](const FxP& p) { return p.life <= 0.f || p.pos.y < -260.f; }), m_fxBolts.end());
    }

    void drawEffectShowcase(const Rect& r) {
        const auto& lv = *m_livery;
        beginClip(r);
        {
            const float sampleX = r.w * 0.24f, shipX = r.w * 0.66f, midY = r.h * 0.54f;
            const float Z = std::min(3.4f, m_zoom * 0.75f);
            const auto S = [&](sf::Vector2f v) { return sf::Vector2f{ shipX + v.x * Z, midY + v.y * Z }; };

            vline(r.w * 0.44f, 24.f, r.h - 48.f, sf::Color(24, 32, 42));

            sf::Text lt(monoFont(), "SAMPLE", 12);
            lt.setLetterSpacing(1.8f);
            lt.setFillColor(TEXT_DEAD);
            lt.setPosition({ sampleX - 30.f, 18.f });
            m_window->draw(lt);

            sf::Text rt(monoFont(), targetPreviewName(m_paintTarget), 12);
            rt.setLetterSpacing(1.8f);
            rt.setFillColor(VIOLET);
            rt.setPosition({ r.w * 0.48f, 18.f });
            m_window->draw(rt);

            // ---- Static sample, big enough to judge the colour ----
            const sf::Color c = targetColor(m_paintTarget);
            const sf::Vector2f sp{ sampleX, midY };
            switch (m_paintTarget) {
            case PaintTarget::Plasma: {
                sf::RectangleShape bolt({ 10.f, 30.f });
                bolt.setPosition({ sp.x - 5.f, sp.y - 15.f });
                bolt.setFillColor(c);
                bolt.setOutlineThickness(2.f);
                bolt.setOutlineColor(sf::Color(std::min(255, c.r + 60), std::min(255, c.g + 60), std::min(255, c.b + 60)));
                m_window->draw(bolt);
                break;
            }
            case PaintTarget::Parry: {
                sf::CircleShape ring(42.f);
                ring.setOrigin({ 42.f, 42.f });
                ring.setPosition(sp);
                ring.setFillColor(sf::Color::Transparent);
                ring.setOutlineThickness(4.f);
                ring.setOutlineColor(c);
                m_window->draw(ring);
                break;
            }
            default: {
                // Particle cloud: alpha and size read the way they will in play.
                for (int i = 0; i < 26; ++i) {
                    const float a = static_cast<float>(i) * 0.63f;
                    const float rad = 6.f + static_cast<float>(i % 7) * 5.f;
                    sf::CircleShape q(2.f + static_cast<float>(i % 4));
                    q.setOrigin({ q.getRadius(), q.getRadius() });
                    q.setPosition({ sp.x + std::cos(a) * rad, sp.y + std::sin(a) * rad * 1.4f });
                    q.setFillColor(sf::Color(c.r, c.g, c.b, static_cast<std::uint8_t>(c.a * (1.f - rad / 50.f))));
                    m_window->draw(q);
                }
                break;
            }
            }

            // ---- The ship, doing the thing ----
            const bool parrying = (m_paintTarget == PaintTarget::Parry) && (m_fxBeat > 1.0f);
            fillPolygon(m_design->renderOutline(), S, lv.paint.hull);
            strokePolygon(m_design->renderOutline(), S,
                parrying ? lv.paint.parry : lv.paint.outline, parrying ? 4.5f : 2.5f);

            std::vector<sf::Vector2f> buf;
            for (const auto& d : lv.decals) {
                buf.clear();
                ship::decalGeometry(d, buf, false);
                if (d.mirrored) ship::decalGeometry(d, buf, true);
                sf::VertexArray va(sf::PrimitiveType::Triangles, buf.size());
                for (std::size_t i = 0; i < buf.size(); ++i) va[i] = { S(buf[i]), d.color };
                m_window->draw(va);
            }
            if (lv.cockpit.style != ship::CockpitStyle::None) {
                std::vector<sf::Vector2f> glass, rim;
                ship::cockpitGeometry(lv.cockpit, glass, rim);
                sf::VertexArray va(sf::PrimitiveType::Triangles);
                for (const auto& p : rim)   va.append({ S(p), sf::Color(14, 18, 26, 235) });
                for (const auto& p : glass) va.append({ S(p), lv.paint.cockpit });
                m_window->draw(va);
            }

            if (m_paintTarget == PaintTarget::Parry) {
                const float t = std::clamp((1.30f - m_fxBeat) / 0.5f, 0.f, 1.f);
                if (t < 1.f) {
                    sf::CircleShape ring(38.f * Z * (0.4f + t));
                    ring.setOrigin({ ring.getRadius(), ring.getRadius() });
                    ring.setPosition(S({ 0.f, 0.f }));
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineThickness(3.f);
                    ring.setOutlineColor(sf::Color(lv.paint.parry.r, lv.paint.parry.g, lv.paint.parry.b,
                        static_cast<std::uint8_t>(220 * (1.f - t))));
                    m_window->draw(ring);
                }
            }

            const auto drawP = [&](const FxP& p) {
                const float k = std::clamp(p.life / std::max(0.01f, p.maxLife), 0.f, 1.f);
                sf::CircleShape q(p.size * (0.5f + k * 0.5f));
                q.setOrigin({ q.getRadius(), q.getRadius() });
                q.setPosition(S(p.pos));
                q.setFillColor(sf::Color(p.color.r, p.color.g, p.color.b,
                    static_cast<std::uint8_t>(p.color.a * k)));
                m_window->draw(q);
                };
            for (const auto& p : m_fx) drawP(p);
            for (const auto& b : m_fxBolts) {
                sf::RectangleShape bolt({ 5.f, 16.f });
                bolt.setOrigin({ 2.5f, 8.f });
                bolt.setPosition(S(b.pos));
                bolt.setFillColor(b.color);
                m_window->draw(bolt);
            }

            sf::Text hint(monoFont(), "DRAG IN THE PICKER - THE PREVIEW UPDATES LIVE", 12);
            hint.setLetterSpacing(1.4f);
            hint.setFillColor(TEXT_DEAD);
            hint.setPosition({ 12.f, r.h - 24.f });
            m_window->draw(hint);
        }
        endClip();
    }

    // ---- PAINT panel: targets and a full colour picker --------------------
    void drawPalettePanel() {
        const Rect r = statsRect();
        if (!panelChrome(r, "PAINT", 0.10f, VIOLET, false) || !m_livery) return;

        struct Row { const char* label; PaintTarget t; };
        static const Row rows[] = {
            { "HULL",    PaintTarget::Hull },    { "OUTLINE", PaintTarget::Outline },
            { "PLASMA",  PaintTarget::Plasma },  { "THRUST",  PaintTarget::Thrust },
            { "TURBO",   PaintTarget::Turbo },   { "DODGE",   PaintTarget::Dodge },
            { "PARRY",   PaintTarget::Parry },   { "HOMING",  PaintTarget::Homing },
            { "CANOPY",  PaintTarget::Cockpit }, { "DETAIL",  PaintTarget::Decal },
        };

        float y = r.y + 12.f;
        for (const auto& row : rows) {
            const Rect line{ r.x + 10.f, y, r.w - 20.f, 19.f };
            const bool active = (m_paintTarget == row.t);
            const bool usable = (row.t != PaintTarget::Decal) || selected() != nullptr;
            if (consumeClick(line) && usable) {
                m_paintTarget = row.t;
                syncPickerFrom(targetColor(row.t));   // the picker opens on the colour in use
                if (row.t == PaintTarget::Cockpit && m_livery->cockpit.style != ship::CockpitStyle::None)
                    m_selKind = SelKind::Cockpit;
            }

            if (active) {
                sf::RectangleShape hl({ line.w, line.h });
                hl.setPosition({ line.x, line.y });
                hl.setFillColor(sf::Color(30, 40, 52));
                m_window->draw(hl);
            }
            sf::Text t(monoFont(), row.label, 12);
            t.setLetterSpacing(1.5f);
            t.setFillColor(usable ? (active ? TEXT : TEXT_DIM) : TEXT_DEAD);
            t.setPosition({ line.x + 8.f, line.y + 2.f });
            m_window->draw(t);

            if (isEffectTarget(row.t)) {
                sf::Text e(monoFont(), "LIVE", 10);
                e.setLetterSpacing(1.2f);
                e.setFillColor(active ? VIOLET : sf::Color(90, 60, 130));
                e.setPosition({ line.x + 74.f, line.y + 4.f });
                m_window->draw(e);
            }

            sf::RectangleShape sw({ 44.f, 12.f });
            sw.setPosition({ line.x + line.w - 54.f, line.y + 3.f });
            sw.setFillColor(usable ? targetColor(row.t) : sf::Color(30, 34, 40));
            sw.setOutlineThickness(1.f);
            sw.setOutlineColor(active ? AMBER : sf::Color(60, 70, 84));
            m_window->draw(sw);
            y += 21.f;
        }

        // ================= COLOUR PICKER =================
        // Saturation across, value down, over the current hue. Vertex colours
        // give the bilinear ramp for free -- no texture, one quad.
        y += 8.f;
        const float pad = 12.f;
        m_svRect = { r.x + pad, y, r.w - pad * 2.f, 96.f };
        const sf::Color pure = ship::fromHSV(m_pickH, 1.f, 1.f);
        sf::VertexArray sv(sf::PrimitiveType::Triangles, 6);
        const sf::Vector2f a{ m_svRect.x, m_svRect.y }, b{ m_svRect.x + m_svRect.w, m_svRect.y };
        const sf::Vector2f c{ m_svRect.x + m_svRect.w, m_svRect.y + m_svRect.h }, d{ m_svRect.x, m_svRect.y + m_svRect.h };
        const sf::Color black(0, 0, 0), white(255, 255, 255);
        sv[0] = { a, white }; sv[1] = { b, pure }; sv[2] = { c, black };
        sv[3] = { a, white }; sv[4] = { c, black }; sv[5] = { d, black };
        m_window->draw(sv);

        sf::CircleShape svKnob(5.f);
        svKnob.setOrigin({ 5.f, 5.f });
        svKnob.setPosition({ m_svRect.x + m_pickS * m_svRect.w, m_svRect.y + (1.f - m_pickV) * m_svRect.h });
        svKnob.setFillColor(sf::Color::Transparent);
        svKnob.setOutlineThickness(2.f);
        svKnob.setOutlineColor(m_pickV > 0.55f ? sf::Color(10, 12, 16) : sf::Color::White);
        m_window->draw(svKnob);

        // Hue strip: six interpolated segments across the spectrum.
        y += m_svRect.h + 8.f;
        m_hueRect = { r.x + pad, y, r.w - pad * 2.f, 16.f };
        sf::VertexArray hue(sf::PrimitiveType::Triangles, 6 * 6);
        for (int i = 0; i < 6; ++i) {
            const float x0 = m_hueRect.x + m_hueRect.w * (static_cast<float>(i) / 6.f);
            const float x1 = m_hueRect.x + m_hueRect.w * (static_cast<float>(i + 1) / 6.f);
            const sf::Color c0 = ship::fromHSV(static_cast<float>(i) * 60.f, 1.f, 1.f);
            const sf::Color c1 = ship::fromHSV(static_cast<float>(i + 1) * 60.f, 1.f, 1.f);
            const int o = i * 6;
            hue[o + 0] = { { x0, m_hueRect.y }, c0 };
            hue[o + 1] = { { x1, m_hueRect.y }, c1 };
            hue[o + 2] = { { x1, m_hueRect.y + m_hueRect.h }, c1 };
            hue[o + 3] = { { x0, m_hueRect.y }, c0 };
            hue[o + 4] = { { x1, m_hueRect.y + m_hueRect.h }, c1 };
            hue[o + 5] = { { x0, m_hueRect.y + m_hueRect.h }, c0 };
        }
        m_window->draw(hue);
        vline(m_hueRect.x + m_hueRect.w * (m_pickH / 360.f), m_hueRect.y - 3.f, m_hueRect.h + 6.f, sf::Color::White);

        // Alpha: only meaningful on the effect colours, shown always for consistency.
        y += m_hueRect.h + 8.f;
        m_alphaRect = { r.x + pad, y, r.w - pad * 2.f, 12.f };
        const sf::Color solid = ship::fromHSV(m_pickH, m_pickS, m_pickV);
        sf::VertexArray al(sf::PrimitiveType::Triangles, 6);
        const sf::Color a0(solid.r, solid.g, solid.b, 0), a1(solid.r, solid.g, solid.b, 255);
        const sf::Vector2f p0{ m_alphaRect.x, m_alphaRect.y }, p1{ m_alphaRect.x + m_alphaRect.w, m_alphaRect.y };
        const sf::Vector2f p2{ m_alphaRect.x + m_alphaRect.w, m_alphaRect.y + m_alphaRect.h }, p3{ m_alphaRect.x, m_alphaRect.y + m_alphaRect.h };
        al[0] = { p0, a0 }; al[1] = { p1, a1 }; al[2] = { p2, a1 };
        al[3] = { p0, a0 }; al[4] = { p2, a1 }; al[5] = { p3, a0 };
        m_window->draw(al);
        vline(m_alphaRect.x + m_alphaRect.w * m_pickA, m_alphaRect.y - 3.f, m_alphaRect.h + 6.f, sf::Color::White);

        // Readout + quick swatches.
        y += m_alphaRect.h + 8.f;
        const sf::Color cur = targetColor(m_paintTarget);
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%s   R%3d G%3d B%3d A%3d",
            ship::colorToHex(cur).c_str(), cur.r, cur.g, cur.b, cur.a);
        sf::Text t(monoFont(), hex, 12);
        t.setLetterSpacing(1.2f);
        t.setFillColor(TEXT_DIM);
        t.setPosition({ r.x + pad, y });
        m_window->draw(t);

        y += 18.f;
        int count = 0;
        const sf::Color* pal = ship::liveryPalette(count);
        const float sw2 = (r.w - pad * 2.f) / static_cast<float>(count);
        for (int i = 0; i < count; ++i) {
            const Rect cell{ r.x + pad + static_cast<float>(i) * sw2, y, sw2 - 2.f, 14.f };
            if (consumeClick(cell)) { syncPickerFrom(pal[i]); applyPaletteColor(pal[i]); }
            sf::RectangleShape q({ cell.w, cell.h });
            q.setPosition({ cell.x, cell.y });
            q.setFillColor(pal[i]);
            m_window->draw(q);
        }
    }

    // ---- DETAILS panel: add, size, depth, canopy --------------------------
    void drawDetailPanel() {
        const Rect r = mountRect();
        if (!panelChrome(r, "DETAILS", 0.18f, VIOLET, false) || !m_livery) return;

        // Add row
        static const ship::DecalKind kinds[ship::DECAL_KIND_COUNT] = {
            ship::DecalKind::Line, ship::DecalKind::Bar, ship::DecalKind::Oval,
            ship::DecalKind::Tri,  ship::DecalKind::Ring };
        const float bw = (r.w - 24.f) / static_cast<float>(ship::DECAL_KIND_COUNT + 1);
        float x = r.x + 12.f;
        for (int i = 0; i < ship::DECAL_KIND_COUNT; ++i) {
            const Rect b{ x, r.y + 10.f, bw - 4.f, 22.f };
            if (consumeClick(b)) addDecal(kinds[i]);
            drawMiniButton(b, ship::decalKindName(kinds[i]), false);
            x += bw;
        }
        const Rect del{ x, r.y + 10.f, bw - 4.f, 22.f };
        if (consumeClick(del)) deleteSelected();
        drawMiniButton(del, "DEL", false);

        // Property rows: - value +
        ship::Decal* d = selected();
        struct Prop { const char* label; int id; float step; };
        // Buttons nudge; the canvas handles do the free-form work.
        static const Prop props[4] = { { "WIDTH", 0, 1.f }, { "HEIGHT", 1, 1.f },
                                       { "ANGLE", 2, 5.f }, { "THICK", 3, 0.5f } };
        float y = r.y + 40.f;
        for (const auto& pr : props) {
            sf::Text l(monoFont(), pr.label, 12);
            l.setLetterSpacing(1.4f);
            l.setFillColor(TEXT_DIM);
            l.setPosition({ r.x + 12.f, y + 2.f });
            m_window->draw(l);

            const Rect minus{ r.x + 88.f, y, 20.f, 18.f };
            const Rect plus{ r.x + 150.f, y, 20.f, 18.f };
            if (consumeClick(minus)) tweak(pr.id, -pr.step);
            if (consumeClick(plus))  tweak(pr.id, pr.step);
            drawMiniButton(minus, "-", false);
            drawMiniButton(plus, "+", false);

            char val[24] = "--";
            if (d) {
                const float v = (pr.id == 0) ? d->w : (pr.id == 1) ? d->h
                    : (pr.id == 2) ? d->angle : d->thickness;
                std::snprintf(val, sizeof(val), "%.1f", v);
            }
            else if (m_livery->cockpit.style != ship::CockpitStyle::None && pr.id < 2) {
                std::snprintf(val, sizeof(val), "%.1f", pr.id == 0 ? m_livery->cockpit.w : m_livery->cockpit.h);
            }
            sf::Text v(monoFont(), val, 13);
            v.setLetterSpacing(1.2f);
            v.setFillColor(TEXT);
            v.setPosition({ r.x + 116.f, y + 1.f });
            m_window->draw(v);

            // Toggles live on the same rows, to the right.
            if (pr.id == 0) {
                const Rect t{ r.x + 182.f, y, 84.f, 18.f };
                if (consumeClick(t) && d) d->over = !d->over;
                drawMiniButton(t, d ? (d->over ? "OVER HULL" : "UNDER HULL") : "DEPTH", d && d->over);
            }
            else if (pr.id == 1) {
                const Rect t{ r.x + 182.f, y, 84.f, 18.f };
                if (consumeClick(t) && d) {
                    d->mirrored = !d->mirrored;
                    if (d->mirrored && !ship::decalInside(*d, m_design->envelope())) {
                        d->mirrored = false; reject("NO ROOM FOR THE MIRROR");
                    }
                }
                drawMiniButton(t, d && d->mirrored ? "MIRRORED" : "MIRROR", d && d->mirrored);
            }
            else if (pr.id == 2) {
                const Rect t{ r.x + 182.f, y, 84.f, 18.f };
                if (consumeClick(t)) {
                    auto& c = m_livery->cockpit;
                    c.style = static_cast<ship::CockpitStyle>(
                        (static_cast<int>(c.style) + 1) % ship::COCKPIT_STYLE_COUNT);
                    toast(std::string("CANOPY ") + ship::cockpitStyleName(c.style));
                }
                drawMiniButton(t, ship::cockpitStyleName(m_livery->cockpit.style),
                    m_livery->cockpit.style != ship::CockpitStyle::None);
            }
            else {
                // Put the handles on the canopy without hunting for it on the canvas.
                const Rect t{ r.x + 182.f, y, 84.f, 18.f };
                const bool canopyLive = m_livery->cockpit.style != ship::CockpitStyle::None;
                if (consumeClick(t) && canopyLive) {
                    m_selKind = SelKind::Cockpit;
                    m_selDecal = -1;
                    m_paintTarget = PaintTarget::Cockpit;
                    syncPickerFrom(m_livery->paint.cockpit);
                }
                drawMiniButton(t, canopyLive ? "EDIT CANOPY" : "NO CANOPY", m_selKind == SelKind::Cockpit);
            }
            y += 22.f;
        }

        char info[96];
        std::snprintf(info, sizeof(info), "%d / %d DETAILS%s", static_cast<int>(m_livery->decals.size()),
            ship::MAX_DECALS, d ? "   SELECTED" : "");
        sf::Text t(monoFont(), info, 12);
        t.setLetterSpacing(1.4f);
        t.setFillColor(TEXT_DIM);
        t.setPosition({ r.x + 12.f, y + 2.f });
        m_window->draw(t);
    }

    // ---- SHIP FILE panel --------------------------------------------------
    void drawFilePanel() {
        const Rect r = statusRect();
        if (!panelChrome(r, "SHIP FILE", 0.26f, CYAN_MID, false)) return;

        const Rect ex{ r.x + 12.f, r.y + 10.f, (r.w - 34.f) * 0.5f, 26.f };
        const Rect im{ ex.x + ex.w + 10.f, ex.y, ex.w, 26.f };
        if (consumeClick(ex)) exportShip();
        if (consumeClick(im)) importShip();
        drawMiniButton(ex, "EXPORT TO ships/", false);
        drawMiniButton(im, "IMPORT NEXT FILE", false);

        sf::Text t(monoFont(), m_fileMsg.empty()
            ? "SHARE THE FILE - ANY COPY OF THE GAME CAN FLY IT" : m_fileMsg, 12);
        t.setLetterSpacing(1.3f);
        t.setFillColor(m_fileMsg.rfind("SAVED", 0) == 0 || m_fileMsg.rfind("LOADED", 0) == 0
            ? GREEN : (m_fileMsg.empty() ? TEXT_DIM : AMBER));
        t.setPosition({ r.x + 12.f, r.y + 44.f });
        m_window->draw(t);
    }

    void drawMiniButton(const Rect& r, const std::string& label, bool active) {
        const bool hot = r.contains(m_mouse);
        sf::RectangleShape b({ r.w, r.h });
        b.setPosition({ r.x, r.y });
        b.setFillColor(active ? sf::Color(40, 60, 74) : (hot ? sf::Color(26, 34, 44) : sf::Color(14, 19, 26)));
        b.setOutlineThickness(1.f);
        b.setOutlineColor(active ? CYAN : (hot ? CYAN_MID : sf::Color(44, 54, 66)));
        m_window->draw(b);

        sf::Text t(monoFont(), label, 12);
        t.setLetterSpacing(1.3f);
        t.setFillColor(active ? CYAN : TEXT_DIM);
        const auto bb = t.getGlobalBounds();
        t.setPosition({ r.x + (r.w - bb.size.x) * 0.5f, r.y + (r.h - 16.f) * 0.5f });
        m_window->draw(t);
    }

    // ---- STATS panel ------------------------------------------------------
    void drawStatsPanel() {
        if (m_mode == Mode::Paint) { drawPalettePanel(); return; }
        const Rect r = statsRect();
        if (!panelChrome(r, "PERFORMANCE", 0.10f, CYAN_MID, false)) return;

        const auto& s = m_design->stats();
        const auto& ref = ship::ShipDesign::reference();
        const auto ratio = [](float a, float b) { return (b > 1e-6f) ? a / b : 0.f; };

        const ship::ClassFeel feel = m_lua ? ship::classFeel(*m_lua, m_design->hullClass())
            : ship::defaultFeel(m_design->hullClass());
        const float regen = ratio(s.regenFraction, ref.regenFraction) * feel.regen;

        beginClip(r);
        {
            float y = 14.f;
            statBar(y, r.w, "HULL", s.hpMax, ref.hpMax, "%.0f");                      y += 33.f;
            statBar(y, r.w, "ENERGY", s.energyMax, ref.energyMax, "%.0f");            y += 33.f;
            statBar(y, r.w, "REGEN", regen, 1.f, "x%.2f");                            y += 33.f;
            statBar(y, r.w, "THRUST", m_design->mobilityRatio(), 1.f, "x%.2f");       y += 33.f;
            statBar(y, r.w, "AGILITY", m_design->agilityRatio(), 1.f, "x%.2f");       y += 40.f;

            hline(14.f, y - 10.f, r.w - 28.f, CYAN_LOW);

            // Class tier. Labels and values in two columns so no line needs
            // an abbreviation to fit the panel.
            char v[7][96];
            const float mf = m_design->mobilityFactor();
            std::snprintf(v[0], 96, "x%.2f     REVERSE  x%.2f",
                ratio(s.strafeAccel, ref.accel) * mf, ratio(s.reverseAccel, ref.accel) * mf);
            std::snprintf(v[1], 96, "%.0f px,  %.2f s i-frames", feel.dashDistancePx, feel.dashIframes);
            std::snprintf(v[2], 96, "%.2f s    COST  %.0f EN", feel.dashRecovery, feel.dashEnergyCost);
            std::snprintf(v[3], 96, "capacity x%.2f   cool x%.2f", feel.heatCapacity, feel.heatCool);
            std::snprintf(v[4], 96, "vent x%.2f   parry x%.2f", feel.qteWindow, feel.parryWindow);
            if (feel.poise > 0.f)
                std::snprintf(v[5], 96, "%.0f   knockback x%.2f%s", feel.poise, feel.knockback,
                    feel.hyperarmor > 0.5f ? "   ARMORED DODGE" : "");
            else
                std::snprintf(v[5], 96, "none - every heavy hit tumbles");
            if (feel.damageReduction > 0.f || feel.shoulderBash > 0.5f || feel.ramming > 0.5f)
                std::snprintf(v[6], 96, "-%.0f%%  dodge -%.0f%%%s%s", feel.damageReduction * 100.f,
                    feel.hyperarmorReduction * 100.f,
                    feel.shoulderBash > 0.5f ? "  BASH" : "", feel.ramming > 0.5f ? "  RAM" : "");
            else
                std::snprintf(v[6], 96, "none");
            static const char* labels[7] = { "STRAFE", "DODGE", "RECOVER", "HEAT", "WINDOWS", "POISE", "ARMOUR" };

            for (int i = 0; i < 7; ++i) {
                const float ly = y + static_cast<float>(i) * 16.f;
                sf::Text l(monoFont(), labels[i], 12);
                l.setLetterSpacing(1.4f);
                l.setFillColor(TEXT_DIM);
                l.setPosition({ 14.f, ly });
                m_window->draw(l);

                sf::Text t(monoFont(), v[i], 12);
                t.setLetterSpacing(1.2f);
                t.setFillColor(TEXT);
                t.setPosition({ 104.f, ly });
                m_window->draw(t);
            }
        }
        endClip();
    }

    /**
     * @brief One stat row: label, value, bar, and a tick at the reference build.
     *
     * The tick is the whole point: "compared to standard" without a second
     * column of figures.
     */
    void statBar(float y, float panelW, const char* label,
        float value, float refValue, const char* fmt) {
        sf::Text l(monoFont(), label, 14);
        l.setLetterSpacing(1.7f);
        l.setFillColor(TEXT_DIM);
        l.setPosition({ 14.f, y });
        m_window->draw(l);

        char buf[32];
        std::snprintf(buf, sizeof(buf), fmt, value);
        const bool better = (value >= refValue * 0.995f);

        sf::Text v(monoFont(), buf, 17);
        v.setLetterSpacing(1.3f);
        v.setFillColor(better ? GREEN : AMBER);
        v.setPosition({ panelW - v.getGlobalBounds().size.x - 16.f, y - 3.f });
        m_window->draw(v);

        const float bx = 14.f, by = y + 20.f, bw = panelW - 28.f, bh = 6.f;

        sf::RectangleShape track({ bw, bh });
        track.setPosition({ bx, by });
        track.setFillColor(sf::Color(16, 22, 30));
        m_window->draw(track);

        const float full = std::max(0.0001f, refValue / 0.45f);
        sf::RectangleShape fill({ bw * std::clamp(value / full, 0.f, 1.f), bh });
        fill.setPosition({ bx, by });
        fill.setFillColor(better ? GREEN : AMBER);
        m_window->draw(fill);

        vline(bx + bw * 0.45f, by - 3.f, bh + 6.f, sf::Color(214, 222, 232, 170));
    }

    // ---- LOADOUT panel ----------------------------------------------------
    void drawMountPanel() {
        if (m_mode == Mode::Paint) { drawDetailPanel(); return; }
        const Rect r = mountRect();
        if (!panelChrome(r, "LOADOUT", 0.18f, CYAN_MID, false)) return;

        const auto& s = m_design->stats();
        const auto& spec = m_design->spec();
        const auto& tune = m_design->tuning();

        beginClip(r);
        {
            // ---- Reactor: one cell per unit. Guns cyan, drives amber, free green. ----
            const bool over = s.reactorUsed > s.reactorUnits;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "REACTOR  %d / %d", s.reactorUsed, s.reactorUnits);
            sf::Text rt(monoFont(), buf, 14);
            rt.setLetterSpacing(1.6f);
            rt.setFillColor(over ? RED : TEXT);
            rt.setPosition({ 14.f, 10.f });
            m_window->draw(rt);

            const int free = std::max(0, s.reactorUnits - s.reactorUsed);
            const char* regenNote = (free == 0) ? "NO SPARE POWER - REGEN AT FLOOR" : "SPARE UNITS FEED REGEN";
            sf::Text rn(monoFont(), regenNote, 11);
            rn.setLetterSpacing(1.3f);
            rn.setFillColor(free == 0 ? AMBER : TEXT_DIM);
            rn.setPosition({ r.w - rn.getGlobalBounds().size.x - 14.f, 13.f });
            m_window->draw(rn);

            const int cells = std::max(s.reactorUnits, s.reactorUsed);
            const float gap = 3.f;
            const float cw = std::min(28.f, (r.w - 28.f - gap * static_cast<float>(cells - 1))
                / static_cast<float>(std::max(1, cells)));
            const int gunCells = s.gunCount * tune.gunUpkeepUnits;
            for (int i = 0; i < cells; ++i) {
                sf::RectangleShape c({ cw, 12.f });
                c.setPosition({ 14.f + static_cast<float>(i) * (cw + gap), 34.f });
                if (i >= s.reactorUnits)          c.setFillColor(RED);
                else if (i < gunCells)            c.setFillColor(CYAN_MID);
                else if (i < s.reactorUsed)       c.setFillColor(sf::Color(200, 160, 0));
                else {
                    c.setFillColor(sf::Color(10, 30, 18));
                    c.setOutlineThickness(1.f);
                    c.setOutlineColor(GREEN);
                }
                m_window->draw(c);
            }

            // ---- Counts against class caps ----
            std::snprintf(buf, sizeof(buf), "WEAPONS %d/%d   DRIVES %d/%d   TONNAGE %.0f/%.0f",
                s.gunCount, spec.maxGuns, s.engineCount, spec.maxEngines, s.areaPx2, spec.maxAreaPx2);
            sf::Text ct(monoFont(), buf, 13);
            ct.setLetterSpacing(1.4f);
            ct.setFillColor(TEXT);
            ct.setPosition({ 14.f, 56.f });
            m_window->draw(ct);

            // ---- Rift mode: the one rule a player has to understand ----
            const char* rift;
            sf::Color rc;
            if (s.gunCount == 0) { rift = "RIFT     NO WEAPON TO FIRE FROM";               rc = RED; }
            else if (s.riftShared) { rift = "RIFT     SHARED - CHARGING STOPS PLASMA";       rc = AMBER; }
            else { rift = "RIFT     DEDICATED - PLASMA FIRES WHILE IT CHARGES"; rc = VIOLET; }
            sf::Text rf(monoFont(), rift, 13);
            rf.setLetterSpacing(1.4f);
            rf.setFillColor(rc);
            rf.setPosition({ 14.f, 80.f });
            m_window->draw(rf);
        }
        endClip();
    }

    // ---- STATUS panel -----------------------------------------------------
    void drawStatusPanel() {
        if (m_mode == Mode::Paint) { drawFilePanel(); return; }
        const Rect r = statusRect();
        const ship::ValidationResult v = m_design->validate();
        if (!panelChrome(r, "", 0.26f, v.ok ? CYAN_MID : RED, false)) return;

        const auto& s = m_design->stats();

        beginClip(r);
        {
            sf::Text t(monoFont(), v.message, 15);
            t.setLetterSpacing(1.5f);
            t.setFillColor(v.ok ? GREEN : RED);
            t.setPosition({ 14.f, 12.f });
            m_window->draw(t);

            const auto& dc = m_design->decorCheck();
            if (m_mode == Mode::Model || (m_design->decorAuthored() && !dc.ok)) {
                // The model's rules, as numbers the player can steer by.
                char buf[128];
                if (dc.ok)
                    std::snprintf(buf, sizeof(buf), "MODEL %d PTS   AREA %.0f%% OF %.0f%%",
                        m_design->decorPointCount(), dc.areaRatio * 100.f,
                        m_design->tuning().decorMaxAreaRatio * 100.f);
                else
                    std::snprintf(buf, sizeof(buf), "%s - GAME SHOWS THE HULL", dc.reason);
                sf::Text w(monoFont(), buf, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(dc.ok ? VIOLET : RED);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);

                if (m_toastTimer > 0.f) {
                    sf::Text tt(monoFont(), m_toast, 12);
                    tt.setLetterSpacing(1.3f);
                    tt.setFillColor(CYAN);
                    tt.setPosition({ 14.f, 54.f });
                    m_window->draw(tt);
                }
            }
            // A prediction, not a rule: thrust behind one side turns the nose
            // toward the other, and InputSystem applies exactly that.
            else if (std::fabs(s.lateralOffsetPx) > 1.5f && s.engineCount > 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "THRUST OFF-AXIS %.0f px - NOSE PULLS %s",
                    std::fabs(s.lateralOffsetPx), s.lateralOffsetPx > 0.f ? "LEFT" : "RIGHT");
                sf::Text w(monoFont(), buf, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(AMBER);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);
            }
            else if (m_toastTimer > 0.f) {
                sf::Text w(monoFont(), m_toast, 13);
                w.setLetterSpacing(1.4f);
                w.setFillColor(CYAN);
                w.setPosition({ 14.f, 34.f });
                m_window->draw(w);
            }
        }
        endClip();
    }

    /// On-screen buttons for every keyboard shortcut on this screen.
    void drawControls(const sf::Vector2f& size) {
        const float y = size.y - 92.f;
        float x = size.x * 0.015f;
        const float w = 150.f, h = 38.f, gap = 10.f;
        const bool model = (m_mode == Mode::Model);

        if (m_ui.button({ x, y, 170.f, h }, "BACK", false, false, false, 18))
            m_exit = true;
        x += 170.f + gap * 2.f;

        if (m_ui.button({ x, y, w, h },
            m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF",
            m_design->symmetric(), false, false, 16))
            toggleMirror();
        x += w + gap;

        if (m_mode == Mode::Paint) {
            if (m_ui.button({ x, y, w, h }, "CLEAR PAINT", false, false, false, 15) && m_livery) {
                *m_livery = ship::Livery{};
                m_selDecal = -1;
                toast("PAINT CLEARED");
            }
        }
        else if (model) {
            if (m_ui.button({ x, y, w, h }, "RESET MODEL", false, !m_design->decorAuthored(), false, 15)) {
                m_design->resetDecor();
                toast("MODEL RESET TO HULL");
            }
        }
        else if (m_ui.button({ x, y, w, h }, "AUTO-MOUNT", false, false, false, 16)) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }
        x += w + gap * 2.f;

        for (int i = 0; i < ship::HULL_CLASS_COUNT; ++i) {
            const auto c = static_cast<ship::HullClass>(i);
            if (m_ui.button({ x, y, 150.f, h }, ship::classSpec(c).name,
                m_design->hullClass() == c, false, false, 15))
                loadClass(c);
            x += 150.f + gap;
        }

        m_ui.glossaryTab({ size.x - 46.f, size.y * 0.100f, 32.f, 32.f });
        m_ui.drawGlossary("REFIT BAY",
            "HULL is the hitbox: what hits and gets hit, where guns and drives go.\n"
            "MODEL is what the ship looks like. It must cover the hull, stay inside\n"
            "the violet limit and use at most 150% of the hull's area.",
            { { "M / TAB",       "Switch HULL, MODEL and PAINT" },
              { "PAINT",         "Colours, details and the canopy. No hitbox change" },
              { "DETAIL",        "A figure on the hull. Must fit the model limit" },
              { "EXPORT",        "Writes ships/<name>.lua -- share that file" },
              { "IMPORT",        "Loads the next file found in ships/" },
              { "RIFT VIOLET",   "Never paintable: it reads as the heavy weapon" },
              { "LEFT DRAG",     "Move a point of the layer you are editing" },
              { "RIGHT CLICK",   "HULL: fit/remove weapon (point) or drive (edge)" },
              { "",              "MODEL: remove a point, or add one anywhere else" },
              { "R",             "HULL: fire the Rift from this gun / share-dedicate" },
              { "INSERT  =",     "Add a point at the cursor" },
              { "DELETE",        "Remove the point under the cursor" },
              { "Y",             "Mirror editing" },
              { "1 / 2 / 3",     "LIGHT / MEDIUM / HEAVY frame" },
              { "A",             "HULL: auto-mount a balanced loadout" },
              { "SPIKE = MUZZLE","A model spike above a gun is where it fires from" },
              { "CYAN / VIOLET", "Plasma gun / Rift mount" },
              { "AMBER EDGE",    "Drive - pushes the ship away from that edge" },
              { "RED LINE",      "Thrust off-centre - the nose will pull" } });
    }

    void drawHints(const sf::Vector2f& size) {
        if (m_mode == Mode::Paint) {
            sf::Text a(monoFont(),
                "CLICK A COLOUR TARGET THEN A SWATCH     LMB DRAG DETAIL     RMB REMOVE     DEL REMOVE", 13);
            a.setLetterSpacing(1.5f);
            a.setFillColor(TEXT_DIM);
            a.setPosition({ size.x * 0.015f, size.y - 46.f });
            m_window->draw(a);
            sf::Text b(monoFont(), "M HULL     Y MIRROR     1-3 FRAME     ESC BACK", 13);
            b.setLetterSpacing(1.5f);
            b.setFillColor(TEXT_DIM);
            b.setPosition({ size.x * 0.015f, size.y - 28.f });
            m_window->draw(b);
            return;
        }
        const bool model = (m_mode == Mode::Model);
        sf::Text a(monoFont(), model
            ? "LMB DRAG POINT     RMB ADD / REMOVE POINT     INS ADD     DEL REMOVE"
            : "LMB DRAG POINT     RMB FIT/REMOVE     R RIFT MOUNT     INS ADD     DEL REMOVE", 13);
        a.setLetterSpacing(1.5f);
        a.setFillColor(TEXT_DIM);
        a.setPosition({ size.x * 0.015f, size.y - 46.f });
        m_window->draw(a);

        sf::Text b(monoFont(), model
            ? "M HULL     Y MIRROR     1-3 FRAME     ESC BACK"
            : "M MODEL     Y MIRROR     1-3 FRAME     A AUTO-MOUNT     ESC BACK", 13);
        b.setLetterSpacing(1.5f);
        b.setFillColor(TEXT_DIM);
        b.setPosition({ size.x * 0.015f, size.y - 28.f });
        m_window->draw(b);
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    const sf::Font& monoFont() const { return m_mono ? *m_mono : *m_font; }

    // ---- Polygon drawing in panel space (L maps local -> panel) -------------
    // ConvexShape cannot draw a concave model, so everything polygonal on
    // this screen goes through these.

    template <class Map>
    void fillPolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c) const {
        const auto tris = ship::detail::triangulate(poly);
        sf::VertexArray va(sf::PrimitiveType::Triangles, tris.size());
        for (std::size_t i = 0; i < tris.size(); ++i) va[i] = { L(tris[i]), c };
        m_window->draw(va);
    }

    /**
     * @brief Closed outline with MITER joins, centred on the edges.
     *
     * Drawing each edge as its own thick line left a notch at every corner,
     * which blunted exactly the spikes this screen exists to make.
     */
    template <class Map>
    void strokePolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c, float w) const {
        const std::size_t n = poly.size();
        if (n < 2) return;
        std::vector<sf::Vector2f> S(n);
        for (std::size_t i = 0; i < n; ++i) S[i] = L(poly[i]);

        const auto normalOf = [&](std::size_t i) {
            const sf::Vector2f e = S[(i + 1) % n] - S[i];
            const float l = std::max(0.0001f, std::sqrt(e.x * e.x + e.y * e.y));
            return sf::Vector2f(e.y / l, -e.x / l);
            };
        std::vector<sf::Vector2f> off(n);
        for (std::size_t i = 0; i < n; ++i) {
            const sf::Vector2f n1 = normalOf((i + n - 1) % n), n2 = normalOf(i);
            sf::Vector2f m = (n1 + n2) / std::max(1.f + (n1.x * n2.x + n1.y * n2.y), 0.0001f);
            const float ml = std::sqrt(m.x * m.x + m.y * m.y);
            if (ml > 10.f) m *= 10.f / ml;
            off[i] = m * (w * 0.5f);
        }
        sf::VertexArray va(sf::PrimitiveType::Triangles, n * 6);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const sf::Vector2f a0 = S[i] + off[i], a1 = S[i] - off[i];
            const sf::Vector2f b0 = S[j] + off[j], b1 = S[j] - off[j];
            va[i * 6 + 0] = { a0, c }; va[i * 6 + 1] = { b0, c }; va[i * 6 + 2] = { b1, c };
            va[i * 6 + 3] = { a0, c }; va[i * 6 + 4] = { b1, c }; va[i * 6 + 5] = { a1, c };
        }
        m_window->draw(va);
    }

    template <class Map>
    void dashedPolygon(const std::vector<sf::Vector2f>& poly, const Map& L, sf::Color c, float dash, float gap) const {
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) dashedLine(L(poly[i]), L(poly[(i + 1) % n]), c, dash, gap);
    }

    void thickLine(sf::Vector2f a, sf::Vector2f b, sf::Color c, float w) const {
        sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l < 0.001f) return;
        const sf::Vector2f nrm{ -d.y / l * w * 0.5f, d.x / l * w * 0.5f };
        sf::VertexArray q(sf::PrimitiveType::Triangles, 6);
        q[0] = { a + nrm, c }; q[1] = { b + nrm, c }; q[2] = { b - nrm, c };
        q[3] = { a + nrm, c }; q[4] = { b - nrm, c }; q[5] = { a - nrm, c };
        m_window->draw(q);
    }

    void dashedLine(sf::Vector2f a, sf::Vector2f b, sf::Color c, float dash, float gap) const {
        const sf::Vector2f d = b - a;
        const float l = std::sqrt(d.x * d.x + d.y * d.y);
        if (l < 0.001f) return;
        const sf::Vector2f u = d / l;
        sf::VertexArray va(sf::PrimitiveType::Lines);
        for (float t = 0.f; t < l; t += dash + gap) {
            va.append({ a + u * t, c });
            va.append({ a + u * std::min(l, t + dash), c });
        }
        m_window->draw(va);
    }

    void diamond(sf::Vector2f p, float s, sf::Color c) const {
        sf::ConvexShape d(4);
        d.setPoint(0, { p.x, p.y - s }); d.setPoint(1, { p.x + s, p.y });
        d.setPoint(2, { p.x, p.y + s }); d.setPoint(3, { p.x - s, p.y });
        d.setFillColor(c);
        m_window->draw(d);
    }

    void hline(float x, float y, float w, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x, y }, c }; l[1] = { { x + w, y }, c };
        m_window->draw(l);
    }
    void vline(float x, float y, float h, sf::Color c) const {
        sf::VertexArray l(sf::PrimitiveType::Lines, 2);
        l[0] = { { x, y }, c }; l[1] = { { x, y + h }, c };
        m_window->draw(l);
    }
    void cross(sf::Vector2f p, float s, sf::Color c) const {
        sf::VertexArray v(sf::PrimitiveType::Lines, 4);
        v[0] = { { p.x - s, p.y }, c }; v[1] = { { p.x + s, p.y }, c };
        v[2] = { { p.x, p.y - s }, c }; v[3] = { { p.x, p.y + s }, c };
        m_window->draw(v);
    }

    // ========================================================================
    // STATE
    // ========================================================================

    sf::RenderWindow* m_window = nullptr;
    sf::Font* m_font = nullptr;
    sf::Font* m_mono = nullptr;
    ship::ShipDesign* m_design = nullptr;
    sol::state* m_lua = nullptr;   ///< For hull_classes overrides in the readout

    float m_time = 0.f;
    float m_openTimer = 0.f;

    tui::UI m_ui;

    sf::Vector2f m_mouse;
    int  m_dragging = -1;
    int  m_hoverPoint = -1;
    int  m_hoverEdge = -1;
    int  m_hoverDecor = -1;
    bool m_lDown = false, m_rDown = false;

    Mode         m_mode = Mode::Hull;
    ship::Livery* m_livery = nullptr;
    PaintTarget  m_paintTarget = PaintTarget::Hull;
    SelKind      m_selKind = SelKind::None;
    Grab         m_grab = Grab::None;
    int          m_grabIdx = 0;
    sf::Vector2f m_grabStartPos, m_grabLocal;
    float        m_grabStartW = 0.f, m_grabStartH = 0.f, m_grabStartAngle = 0.f, m_grabStartMouseAngle = 0.f;
    float        m_pickH = 190.f, m_pickS = 0.8f, m_pickV = 1.f, m_pickA = 1.f;
    int          m_pickDrag = 0;             ///< 0 none, 1 SV square, 2 hue, 3 alpha
    Rect         m_svRect, m_hueRect, m_alphaRect;   ///< Published by the panel for the next frame
    std::vector<FxP> m_fx, m_fxBolts;   ///< Preview-only particles
    float        m_fxTime = 0.f, m_fxBeat = 0.f;
    int          m_fxShot = 0;
    sf::Vector2f m_fxRock;
    int          m_selDecal = -1;
    int          m_hoverDecal = -1;
    sf::Vector2f m_dragGrab;
    bool         m_clickPending = false;
    sf::Vector2f m_clickPos;
    std::string  m_fileMsg;
    int          m_fileIdx = 0;
    float        m_zoom = 4.2f;
    sf::Vector2f m_viewCenter;
    bool         m_viewInit = false;

    float        m_rejectFlash = 0.f;
    sf::Vector2f m_rejectAt;
    std::string  m_rejectText;

    std::string m_toast;
    float m_toastTimer = 0.f;

    bool m_exit = false;
    bool m_kY = false, m_k1 = false, m_k2 = false, m_k3 = false;
    bool m_kA = false, m_kR = false, m_kM = false, m_kDel = false, m_kAdd = false, m_kExit = false;

    float m_inputLock = 0.f;
};