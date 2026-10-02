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
 *   Ctrl+Z          undo   Ctrl+Y / Ctrl+Shift+Z redo   (every mode)
 *   Wheel           zoom at the cursor   MMB drag  pan   F  fit the view again
 *
 * The view zooms to fit the class frame and the model envelope, and holds
 * still while a drag is in progress. Zooming or panning by hand stops the
 * auto-fit until F, a class change or an import.
 *
 * PAINT has four tools (W E R T, or the buttons in DETAILS):
 *   MOVE   drag a part                Shift: one axis
 *   TURN   drag inside the ring       Shift: 15 degree steps
 *   SIZE   handles pin the opposite side; drag inside to scale evenly
 *                                     Shift: keep ratio   Alt: from centre
 *   SHAPE  drag a plate's points; RMB on an edge adds one, on a point removes
 *          it. Plates on the centreline are edited mirrored while MIRROR is on.
 * Arrow keys nudge in the current tool, Ctrl+D duplicates. At the model limit
 * a part slides along it instead of freezing.
 *
 * Parts are FIGURES (line, bar, oval, tri, ring) and PLATES (any polygon, up
 * to 16 points). Either takes TONE ink (live hull colour x shade: flashes and
 * heats with the hull, like enemy plates) or a fixed COLOUR. The hull itself
 * may be see-through; parts set UNDER the hull then show through it.
 *
 * @author Oleg Ivakhiv
 * @version 3.2 -- Phase 1: plates, PAINT tools, point editing, TONE ink,
 *                 see-through hull
 *          3.1 -- Phase 0: undo/redo, zoom/pan, anchored gizmo with
 *                 clamp-to-limit, exact fit tests, picker fix, new Details panel
 */

#pragma once

#include "ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/ShipDesign.hpp"
#include "utils/TerminalUI.hpp"
#include "utils/UiPalette.hpp"
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
#include <optional>

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

    /// Mouse wheel, routed from game.cpp's event loop (SFML 3 cannot poll it).
    /// Accumulated here, applied on the next update so it hit-tests against
    /// the same mouse position as everything else that frame.
    void onMouseWheel(float delta) { m_wheel += delta; }

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
        m_kZ = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Z);
        m_kF = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::F);
        m_k1 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num1);
        m_k2 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num2);
        m_k3 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num3);
        m_kA = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A);
        m_kR = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::R);
        m_kM = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::M)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Tab);
        m_hoverDecor = -1;
        m_viewInit = false;
        m_viewManual = false;
        m_panning = false;
        m_wheel = 0.f;
        m_selDecal = m_selPlate = -1;
        m_selKind = SelKind::None;
        m_grab = Grab::None;
        m_hover = {};
        m_vertDrag = m_hoverVert = m_insEdge = m_lastVert = -1;
        m_nudgeHeld = false;
        m_kW = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W);
        m_kE = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::E);
        m_kT = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::T);
        m_kD = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D);
        m_kH = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::H);
        m_hangarOpen = false;
        m_confirm = Confirm::None;
        m_clickPending = false;
        m_kDel = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Delete);
        m_kAdd = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Insert)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Equal);
        m_lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        m_rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
        m_mDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Middle);

        // A run may have happened since the last visit; history from before
        // it is not worth the confusion of undoing across it.
        m_undo.clear();
        m_redo.clear();
        m_gestureOpen = false;
        m_skipCommit = false;

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

        // ---- Undo bookkeeping, part 1: remember the state before input ----
        const bool lDownNow = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        openGesture(lDownNow);

        m_inputLock = std::max(0.f, m_inputLock - dt);
        if (m_inputLock <= 0.f && !m_ui.glossaryOpen()) handleInput();
        m_wheel = 0.f;
        draw();

        // ---- Part 2: after draw, because panel buttons act during draw ----
        commitGesture(lDownNow);
    }

private:
    // ========================================================================
    // PALETTE -- aliases to the shared ui:: palette (UiPalette.hpp)
    // ========================================================================
    // Same pattern as MenuSystem: short names for this file, values from the
    // one source, so the refit bay and the menu cannot drift apart.
    static inline const sf::Color VOID_BG = ui::VOID_BG;
    static inline const sf::Color PANEL_BG = ui::PANEL_BG;
    static inline const sf::Color CYAN = ui::CYAN;
    static inline const sf::Color CYAN_MID = ui::CYAN_MID;
    static inline const sf::Color CYAN_LOW = ui::CYAN_LOW;
    static inline const sf::Color AMBER = ui::AMBER;
    static inline const sf::Color RED = ui::RED;
    static inline const sf::Color GREEN = ui::GREEN;
    static inline const sf::Color TEXT = ui::TEXT;
    static inline const sf::Color TEXT_DIM = ui::TEXT_DIM;
    static inline const sf::Color TEXT_DEAD = ui::TEXT_DEAD;
    /// The Rift's own colour -- WeaponSystem's bolt, charge sparks and rings.
    static inline const sf::Color VIOLET = ui::VIOLET;

    static constexpr float OPEN_DUR = 0.30f;
    static constexpr float HANDLE_R = 7.f;

    enum class Mode { Hull, Model, Paint };

    /// What a palette click paints. Part = the selected figure or plate.
    enum class PaintTarget { Hull, Outline, Plasma, Thrust, Turbo, Dodge, Parry, Homing, Cockpit, Part };

    /// What the gizmo is on. Figures, plates and the canopy share one gizmo.
    enum class SelKind { None, Decal, Plate, Cockpit };

    /// The PAINT tool: one job per tool, picked with W E R T or in DETAILS.
    enum class Tool { Move, Turn, Size, Shape };

    /// What the current drag is doing. Scale = a handle (m_grabIdx says which,
    /// so the OPPOSITE one stays pinned); Uniform = inside the box, even scale;
    /// Vertex = a plate point in SHAPE.
    enum class Grab { None, Move, Rotate, Scale, Uniform, Vertex };

    using Plate = ship::Plate;

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
    // UNDO
    // ========================================================================
    //
    // ShipDesign and Livery are plain value types, so a snapshot is a copy.
    // No command objects, no per-action bookkeeping: the screen compares the
    // state before and after each GESTURE and keeps the "before" if they
    // differ.
    //
    //   gesture  = left press .. left release. One drag, one colour drag, one
    //              button click = one undo step, however many frames it took.
    //   no press = a single frame. Catches keyboard edits (Delete, 1-3, A...).
    //
    // Commit happens AFTER draw(), because the panel buttons act during draw.

    static constexpr std::size_t UNDO_DEPTH = 64;

    struct Snapshot {
        ship::ShipDesign design;
        ship::Livery     livery;
        std::string      name;      ///< Which file it was: undoing a LOAD restores this too
    };

    Snapshot snapshot() const {
        Snapshot s;
        s.design = *m_design;
        if (m_livery) s.livery = *m_livery;
        s.name = m_shipName;
        return s;
    }

    static bool sameDesign(const ship::ShipDesign& a, const ship::ShipDesign& b) {
        return a.hullClass() == b.hullClass()
            && a.symmetric() == b.symmetric()
            && a.points() == b.points()
            && a.decorAuthored() == b.decorAuthored()
            && a.decor() == b.decor()
            && a.mountedGuns() == b.mountedGuns()
            && a.mountedEngines() == b.mountedEngines()
            && a.stats().spinalVertex == b.stats().spinalVertex
            && a.stats().riftShared == b.stats().riftShared;
    }

    static bool sameLivery(const ship::Livery& a, const ship::Livery& b) {
        const auto& p = a.paint;
        const auto& q = b.paint;
        if (!(p.hull == q.hull && p.outline == q.outline && p.plasma == q.plasma
            && p.thrust == q.thrust && p.turbo == q.turbo && p.dodge == q.dodge
            && p.parry == q.parry && p.cockpit == q.cockpit && p.homing == q.homing)) return false;
        if (a.decals.size() != b.decals.size()) return false;
        for (std::size_t i = 0; i < a.decals.size(); ++i) {
            const auto& d = a.decals[i];
            const auto& e = b.decals[i];
            if (!(d.kind == e.kind && d.pos == e.pos && d.w == e.w && d.h == e.h
                && d.angle == e.angle && d.thickness == e.thickness && d.color == e.color
                && d.over == e.over && d.mirrored == e.mirrored
                && d.tonal == e.tonal && d.shade == e.shade)) return false;
        }
        if (a.plates.size() != b.plates.size()) return false;
        for (std::size_t i = 0; i < a.plates.size(); ++i) {
            const auto& d = a.plates[i];
            const auto& e = b.plates[i];
            if (!(d.shape == e.shape && d.pos == e.pos && d.w == e.w && d.h == e.h && d.angle == e.angle
                && d.tonal == e.tonal && d.shade == e.shade && d.color == e.color
                && d.accent == e.accent && d.over == e.over && d.mirrored == e.mirrored)) return false;
        }
        const auto& c = a.cockpit;
        const auto& k = b.cockpit;
        return c.style == k.style && c.pos == k.pos && c.w == k.w && c.h == k.h && c.angle == k.angle;
    }

    bool sameAsNow(const Snapshot& s) const {
        if (!sameDesign(s.design, *m_design)) return false;
        return !m_livery || sameLivery(s.livery, *m_livery);
    }

    void pushUndo(Snapshot s) {
        m_undo.push_back(std::move(s));
        if (m_undo.size() > UNDO_DEPTH) m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    void openGesture(bool lDown) {
        if (m_gestureOpen) return;
        m_frameBefore = snapshot();
        if (lDown) { m_gestureOpen = true; m_gestureBefore = m_frameBefore; }
    }

    void commitGesture(bool lDown) {
        if (m_skipCommit) { m_skipCommit = false; m_gestureOpen = false; return; }
        if (m_gestureOpen) {
            if (lDown) return;                      // still dragging
            m_gestureOpen = false;
            if (!sameAsNow(m_gestureBefore)) pushUndo(std::move(m_gestureBefore));
        }
        else if (!sameAsNow(m_frameBefore)) {
            pushUndo(std::move(m_frameBefore));
        }
    }

    /// Put a snapshot back and drop every transient handle into it: indices
    /// held by a drag or a selection may not exist in the restored state.
    void restore(const Snapshot& s) {
        *m_design = s.design;
        if (m_livery) *m_livery = s.livery;
        m_shipName = s.name;
        m_dragging = -1;
        m_hoverPoint = m_hoverEdge = m_hoverDecor = -1;
        m_hover = {};
        m_hoverVert = m_insEdge = m_vertDrag = -1;
        deselect();
        m_pickDrag = 0;
        syncPickerFrom(targetColor(m_paintTarget));
        m_skipCommit = true;                        // the restore is not itself an edit
    }

    void undo() {
        if (m_undo.empty()) { toast("NOTHING TO UNDO"); return; }
        m_redo.push_back(snapshot());
        const Snapshot s = std::move(m_undo.back());
        m_undo.pop_back();
        restore(s);
        toast("UNDO");
    }

    void redo() {
        if (m_redo.empty()) { toast("NOTHING TO REDO"); return; }
        m_undo.push_back(snapshot());
        const Snapshot s = std::move(m_redo.back());
        m_redo.pop_back();
        restore(s);
        toast("REDO");
    }

    // ========================================================================
    // STAT PREVIEW
    // ========================================================================
    //
    // The player must see WHY a stat changes, before committing to it. Every
    // hover that would change the ship is run on a COPY of the design -- it
    // is a plain value, so "what if" costs one copy -- and the readouts show
    // from -> to:
    //   HULL canvas   a weapon vertex or a drive edge: fit / remove it
    //   buttons       MIRROR, AUTO-MOUNT, a class frame, UNDO, REDO
    //   a hull drag   the change since the point was grabbed
    // A change the design would refuse (reactor full, class limit, no
    // barrel clearance) shows the refusal instead of numbers.

    /// Everything the readouts show about one design.
    struct StatRow {
        float hp = 0.f, energy = 0.f, regen = 0.f, thrust = 0.f, agility = 0.f;
        float thrustMin = 0.f;               ///< Class minimum, in THRUST bar units
        int   reactorUsed = 0, reactorUnits = 0;
        int   guns = 0, engines = 0, maxGuns = 0, maxEngines = 0;
        float tonnage = 0.f, maxTonnage = 0.f;
        bool  ok = true;
        std::string message;
        std::string feel[7];                 ///< STRAFE .. ARMOUR lines
    };

    StatRow rowOf(const ship::ShipDesign& d) const {
        StatRow r;
        const auto& s = d.stats();
        const auto& ref = ship::ShipDesign::reference();
        const auto& spec = d.spec();
        const auto ratio = [](float a, float b) { return (b > 1e-6f) ? a / b : 0.f; };
        const ship::ClassFeel feel = m_lua ? ship::classFeel(*m_lua, d.hullClass())
            : ship::defaultFeel(d.hullClass());

        r.hp = s.hpMax;
        r.energy = s.energyMax;
        r.regen = ratio(s.regenFraction, ref.regenFraction) * feel.regen;
        r.thrust = d.mobilityRatio();
        r.agility = d.agilityRatio();
        // validate() calls a hull UNDERPOWERED below minAccelRatio of RAW
        // acceleration; the bar shows the compressed ratio, so compress the line too.
        r.thrustMin = std::pow(spec.minAccelRatio, d.tuning().mobilityExponent);
        r.reactorUsed = s.reactorUsed;
        r.reactorUnits = s.reactorUnits;
        r.guns = s.gunCount;          r.maxGuns = spec.maxGuns;
        r.engines = s.engineCount;    r.maxEngines = spec.maxEngines;
        r.tonnage = s.areaPx2;        r.maxTonnage = spec.maxAreaPx2;
        const ship::ValidationResult v = d.validate();
        r.ok = v.ok;
        r.message = v.message;

        char b[96];
        const float mf = d.mobilityFactor();
        std::snprintf(b, 96, "x%.2f     REVERSE  x%.2f",
            ratio(s.strafeAccel, ref.accel) * mf, ratio(s.reverseAccel, ref.accel) * mf);           r.feel[0] = b;
        std::snprintf(b, 96, "%.0f px,  %.2f s i-frames", feel.dashDistancePx, feel.dashIframes);    r.feel[1] = b;
        std::snprintf(b, 96, "%.2f s    COST  %.0f EN", feel.dashRecovery, feel.dashEnergyCost);    r.feel[2] = b;
        std::snprintf(b, 96, "capacity x%.2f   cool x%.2f", feel.heatCapacity, feel.heatCool);     r.feel[3] = b;
        std::snprintf(b, 96, "vent x%.2f   parry x%.2f", feel.qteWindow, feel.parryWindow);        r.feel[4] = b;
        if (feel.poise > 0.f)
            std::snprintf(b, 96, "%.0f   knockback x%.2f%s", feel.poise, feel.knockback,
                feel.hyperarmor > 0.5f ? "   ARMORED DODGE" : "");
        else
            std::snprintf(b, 96, "none - every heavy hit tumbles");
        r.feel[5] = b;
        if (feel.damageReduction > 0.f || feel.shoulderBash > 0.5f || feel.ramming > 0.5f)
            std::snprintf(b, 96, "-%.0f%%  dodge -%.0f%%%s%s", feel.damageReduction * 100.f,
                feel.hyperarmorReduction * 100.f,
                feel.shoulderBash > 0.5f ? "  BASH" : "", feel.ramming > 0.5f ? "  RAM" : "");
        else
            std::snprintf(b, 96, "none");
        r.feel[6] = b;
        return r;
    }

    /// The bottom row of buttons. One function places them for drawing AND
    /// for the preview's hit tests, so the two cannot drift apart.
    struct ControlRects {
        Rect back, mirror, third, cls[ship::HULL_CLASS_COUNT], undo, redo, hangar;
    };

    ControlRects controlRects() const {
        const sf::Vector2f size = viewSize();
        const float y = size.y - 92.f, w = 150.f, h = 38.f, gap = 10.f;
        float x = size.x * 0.015f;
        ControlRects c;
        c.back = { x, y, 170.f, h };   x += 170.f + gap * 2.f;
        c.mirror = { x, y, w, h };     x += w + gap;
        c.third = { x, y, w, h };      x += w + gap * 2.f;
        for (int i = 0; i < ship::HULL_CLASS_COUNT; ++i) { c.cls[i] = { x, y, 150.f, h }; x += 150.f + gap; }
        x += gap;
        c.undo = { x, y, 110.f, h };   x += 110.f + gap;
        c.redo = { x, y, 110.f, h };   x += 110.f + gap * 2.f;
        c.hangar = { x, y, 140.f, h };
        return c;
    }

    /// Work out this frame's preview. Runs after input, before anything draws.
    void updatePreview() {
        m_pvActive = false;
        m_pvDrag = false;
        m_pvLabel.clear();
        m_pvRefused.clear();
        if (m_mode == Mode::Paint || m_hangarOpen || m_ui.glossaryOpen() || m_inputLock > 0.f) return;

        std::optional<ship::ShipDesign> cand;

        // A hull point being dragged: the change since it was grabbed.
        if (m_mode == Mode::Hull && m_dragging >= 0 && m_gestureOpen) {
            m_pvFrom = rowOf(m_gestureBefore.design);
            m_pvTo = rowOf(*m_design);
            m_pvActive = m_pvDrag = true;
            m_pvLabel = "SINCE YOU GRABBED IT";
            return;
        }

        if (m_mode == Mode::Hull && m_dragging < 0 && hullRect().contains(m_mouse)) {
            ship::ShipDesign c = *m_design;
            if (m_hoverPoint >= 0) {
                if (c.isGunMounted(m_hoverPoint)) { c.unmountGun(m_hoverPoint); m_pvLabel = "REMOVE WEAPON"; cand = c; }
                else {
                    m_pvLabel = "FIT WEAPON";
                    if (c.mountGun(m_hoverPoint)) cand = c; else m_pvRefused = c.lastRejectReason();
                }
            }
            else if (m_hoverEdge >= 0) {
                if (c.isEngineMounted(m_hoverEdge)) { c.unmountEngine(m_hoverEdge); m_pvLabel = "REMOVE DRIVE"; cand = c; }
                else {
                    m_pvLabel = "FIT DRIVE";
                    if (c.mountEngine(m_hoverEdge)) cand = c; else m_pvRefused = c.lastRejectReason();
                }
            }
        }
        else {
            const ControlRects cr = controlRects();
            if (cr.mirror.contains(m_mouse)) {
                ship::ShipDesign c = *m_design;
                c.setSymmetric(!c.symmetric());
                if (c.symmetric() && c.mountedGuns().empty()) c.autoMount();   // what toggleMirror does
                m_pvLabel = c.symmetric() ? "MIRROR ON" : "MIRROR OFF";
                cand = c;
            }
            else if (m_mode == Mode::Hull && cr.third.contains(m_mouse)) {
                ship::ShipDesign c = *m_design;
                c.autoMount();
                m_pvLabel = "AUTO-MOUNT";
                cand = c;
            }
            else if (cr.undo.contains(m_mouse) && !m_undo.empty()) { cand = m_undo.back().design; m_pvLabel = "UNDO"; }
            else if (cr.redo.contains(m_mouse) && !m_redo.empty()) { cand = m_redo.back().design; m_pvLabel = "REDO"; }
            else {
                for (int i = 0; i < ship::HULL_CLASS_COUNT; ++i) {
                    const auto cls = static_cast<ship::HullClass>(i);
                    if (!cr.cls[i].contains(m_mouse) || cls == m_design->hullClass()) continue;
                    ship::ShipDesign c = ship::ShipDesign::preset(cls);      // what loadClass does
                    if (!m_design->symmetric()) c.setSymmetric(false);
                    m_pvLabel = std::string(ship::classSpec(cls).name) + " FRAME";
                    cand = c;
                }
            }
        }

        if (cand) {
            m_pvFrom = rowOf(*m_design);
            m_pvTo = rowOf(*cand);
            m_pvActive = true;
        }
    }

    // ========================================================================
    // HANGAR -- every saved ship, by name
    // ========================================================================
    //
    // Replaces "IMPORT NEXT FILE". Open with H or the HANGAR button, from any
    // tab. Lists ships/ with a silhouette of each, loads on double-click or
    // LOAD (undoable like any other change), saves under a typed name, and
    // overwrites or deletes with a second click to confirm.

    struct HangarEntry {
        std::string      path;
        std::string      name;
        ship::ShipDesign design;
        ship::Livery     livery;
        bool             ok = false;
        std::string      err;
    };

    enum class Confirm { None, Replace, Overwrite, Delete };

    static constexpr int SHIP_NAME_LEN = 24;

    void openHangar() {
        m_hangarOpen = true;
        m_hangarMsg.clear();
        m_confirm = Confirm::None;
        m_hoverPoint = m_hoverEdge = m_hoverDecor = -1;
        m_hover = {};
        m_dragging = -1;
        m_grab = Grab::None;
        if (m_nameBuf.empty()) m_nameBuf = m_shipName.empty() ? std::string(m_design->spec().name) : m_shipName;
        refreshHangar();
        // Open on the ship being edited, if it came from a file.
        m_hangarSel = -1;
        for (int i = 0; i < static_cast<int>(m_hangar.size()); ++i)
            if (!m_shipName.empty() && m_hangar[i].name == m_shipName) m_hangarSel = i;
        m_hangarScroll = std::max(0, m_hangarSel - 2);
        // Esc / Enter are probably still down from whatever opened this.
        m_kExit = true;
        m_kEnter = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
        m_kUp = m_kDown = true;
    }

    void closeHangar() {
        m_hangarOpen = false;
        m_confirm = Confirm::None;
        m_kExit = true;   // the Esc that closed it must not also leave the bay
        m_clickPending = false;
    }

    void refreshHangar() {
        m_hangar.clear();
        for (const auto& path : ship::shipfile::list()) {
            HangarEntry e;
            e.path = path;
            e.design = *m_design;
            std::string name, err;
            e.ok = ship::shipfile::load(path, e.design, e.livery, name, err);
            e.name = e.ok ? name : std::filesystem::path(path).stem().string();
            e.err = err;
            m_hangar.push_back(std::move(e));
        }
        if (m_hangarSel >= static_cast<int>(m_hangar.size())) m_hangarSel = static_cast<int>(m_hangar.size()) - 1;
    }

    /// Two-step confirm: the first press arms, a second within 3 s acts.
    bool armed(Confirm c, int idx) {
        if (m_confirm == c && m_confirmIdx == idx && m_time < m_confirmUntil) { m_confirm = Confirm::None; return true; }
        m_confirm = c;
        m_confirmIdx = idx;
        m_confirmUntil = m_time + 3.f;
        return false;
    }

    void hangarLoad(int i) {
        if (i < 0 || i >= static_cast<int>(m_hangar.size())) return;
        const HangarEntry& e = m_hangar[i];
        if (!e.ok) { m_hangarMsg = "CANNOT LOAD - " + e.err; m_hangarMsgBad = true; return; }
        *m_design = e.design;
        if (m_livery) *m_livery = e.livery;
        deselect();
        m_viewInit = false;
        m_viewManual = false;
        m_shipName = e.name;
        m_nameBuf = e.name;
        closeHangar();
        toast("LOADED " + e.name + (e.err.empty() ? "" : "  (" + e.err + ")") + " - CTRL+Z TO GO BACK");
    }

    void hangarSaveNew() {
        if (!m_livery) return;
        const std::string name = ship::shipfile::slug(m_nameBuf);
        if (m_nameBuf.empty()) { m_hangarMsg = "TYPE A NAME FIRST"; m_hangarMsgBad = true; return; }
        std::error_code ec;
        const bool exists = std::filesystem::exists(ship::shipfile::pathFor(name), ec);
        if (exists && !armed(Confirm::Replace, -1)) {
            m_hangarMsg = name + " ALREADY EXISTS - SAVE AGAIN TO REPLACE IT";
            m_hangarMsgBad = true;
            return;
        }
        std::string err;
        const std::string path = ship::shipfile::save(*m_design, *m_livery, name, err, exists);
        if (path.empty()) { m_hangarMsg = "SAVE FAILED - " + err; m_hangarMsgBad = true; return; }
        m_shipName = name;
        m_hangarMsg = "SAVED " + path;
        m_hangarMsgBad = false;
        refreshHangar();
        for (int i = 0; i < static_cast<int>(m_hangar.size()); ++i)
            if (m_hangar[i].path == path) m_hangarSel = i;
    }

    void hangarOverwrite(int i) {
        if (!m_livery || i < 0 || i >= static_cast<int>(m_hangar.size())) return;
        const HangarEntry& e = m_hangar[i];
        if (!armed(Confirm::Overwrite, i)) {
            m_hangarMsg = "OVERWRITE " + e.name + " WITH THIS SHIP? CLICK AGAIN";
            m_hangarMsgBad = true;
            return;
        }
        std::string err;
        if (!ship::shipfile::writeFile(e.path, *m_design, *m_livery, e.name, err)) {
            m_hangarMsg = "SAVE FAILED - " + err; m_hangarMsgBad = true; return;
        }
        m_shipName = e.name;
        m_hangarMsg = "OVERWROTE " + e.path;
        m_hangarMsgBad = false;
        refreshHangar();
    }

    void hangarDelete(int i) {
        if (i < 0 || i >= static_cast<int>(m_hangar.size())) return;
        const std::string name = m_hangar[i].name, path = m_hangar[i].path;
        if (!armed(Confirm::Delete, i)) {
            m_hangarMsg = "DELETE " + name + " FROM DISK? CLICK AGAIN";
            m_hangarMsgBad = true;
            return;
        }
        std::string err;
        if (!ship::shipfile::remove(path, err)) { m_hangarMsg = "DELETE FAILED - " + err; m_hangarMsgBad = true; return; }
        m_hangarMsg = "DELETED " + path;
        m_hangarMsgBad = false;
        refreshHangar();
    }

    Rect hangarRect() const { return hullRect(); }

    /// While the hangar is open it owns the keyboard: typing goes to the name.
    void handleHangarInput() {
        const bool lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        const bool rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
        const Rect R = hangarRect();
        if (lDown && !m_lDown && R.contains(m_mouse)) { m_clickPending = true; m_clickPos = m_mouse; }

        const bool esc = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape);
        if (esc && !m_kExit) closeHangar();
        else m_kExit = esc || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);

        if (keyEdge(sf::Keyboard::Key::Enter, m_kEnter) && m_hangarOpen) hangarSaveNew();

        const int n = static_cast<int>(m_hangar.size());
        if (keyEdge(sf::Keyboard::Key::Up, m_kUp) && n > 0) m_hangarSel = std::max(0, m_hangarSel - 1);
        if (keyEdge(sf::Keyboard::Key::Down, m_kDown) && n > 0) m_hangarSel = std::min(n - 1, m_hangarSel + 1);

        if (m_wheel != 0.f) m_hangarScroll -= static_cast<int>(std::round(m_wheel));
        // Keep the selection on screen when the keys move it.
        const int vis = std::max(1, m_hangarVisible);
        if (m_hangarSel >= 0) {
            if (m_hangarSel < m_hangarScroll) m_hangarScroll = m_hangarSel;
            if (m_hangarSel >= m_hangarScroll + vis) m_hangarScroll = m_hangarSel - vis + 1;
        }
        m_hangarScroll = std::clamp(m_hangarScroll, 0, std::max(0, n - vis));

        if (m_confirm != Confirm::None && m_time >= m_confirmUntil) m_confirm = Confirm::None;
        m_lDown = lDown;
        m_rDown = rDown;
    }

public:
    /// Typed characters, routed from game.cpp's event loop. Only the hangar's
    /// name field listens; everywhere else the bay uses plain key polling.
    void onTextEntered(char32_t c) {
        if (!m_hangarOpen) return;
        if (c == 8) { if (!m_nameBuf.empty()) m_nameBuf.pop_back(); return; }   // backspace
        if (static_cast<int>(m_nameBuf.size()) >= SHIP_NAME_LEN) return;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
        if (ok) m_nameBuf.push_back(c == ' ' ? '-' : static_cast<char>(c));
        m_confirm = Confirm::None;   // a new name is a new question
    }

private:
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
     *
     * Once the player zooms or pans, auto-fit stops until F (or a class
     * change / import) hands the view back.
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
        if (m_viewManual || m_dragging >= 0 || m_grab != Grab::None) return;
        const float k = 1.f - std::exp(-7.f * dt);
        m_zoom += (zoomT - m_zoom) * k;
        m_viewCenter += (centreT - m_viewCenter) * k;
    }

    static constexpr float ZOOM_MIN = 1.5f;
    static constexpr float ZOOM_MAX = 28.f;

    /// Wheel zoom about the cursor, middle-drag pan, F to hand the view back.
    void handleViewInput(bool inPanel) {
        if (m_wheel != 0.f && inPanel) {
            const sf::Vector2f before = toLocal(m_mouse);
            m_zoom = std::clamp(m_zoom * std::pow(1.15f, m_wheel), ZOOM_MIN, ZOOM_MAX);
            m_viewCenter += before - toLocal(m_mouse);   // the point under the cursor stays put
            m_viewManual = true;
        }

        const bool mDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Middle);
        if (mDown && !m_mDown && inPanel) {
            m_panning = true;
            m_panMouse = m_mouse;
            m_panCenter = m_viewCenter;
        }
        if (!mDown) m_panning = false;
        if (m_panning) {
            m_viewCenter = m_panCenter - (m_mouse - m_panMouse) / m_zoom;
            m_viewManual = true;
        }
        m_mDown = mDown;

        if (keyEdge(sf::Keyboard::Key::F, m_kF) && m_viewManual) {
            m_viewManual = false;
            toast("VIEW FIT");
        }
    }

    // ========================================================================
    // INPUT
    // ========================================================================

    void handleInput() {
        const sf::Vector2i pix = sf::Mouse::getPosition(*m_window);
        m_mouse = m_window->mapPixelToCoords(pix, uiView());

        if (m_hangarOpen) { handleHangarInput(); return; }

        const bool lDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        const bool rDown = sf::Mouse::isButtonPressed(sf::Mouse::Button::Right);
        const bool inPanel = hullRect().contains(m_mouse);
        const bool model = (m_mode == Mode::Model);
        const bool paint = (m_mode == Mode::Paint);

        // ---- Shared by every mode: view, undo/redo, mirror ----
        handleViewInput(inPanel);

        const bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl);
        const bool shift = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);
        if (keyEdge(sf::Keyboard::Key::Z, m_kZ) && ctrl) { shift ? redo() : undo(); m_lDown = lDown; m_rDown = rDown; return; }
        // Ctrl+Y is redo; plain Y stays the mirror toggle.
        if (keyEdge(sf::Keyboard::Key::Y, m_kY)) {
            if (ctrl) { redo(); m_lDown = lDown; m_rDown = rDown; return; }
            toggleMirror();
        }
        if (keyEdge(sf::Keyboard::Key::H, m_kH) && !ctrl) { openHangar(); m_lDown = lDown; m_rDown = rDown; return; }

        // Click latch: the panel widgets are drawn after input runs, so they
        // consume this on their own rects during draw.
        if (lDown && !m_lDown) { m_clickPending = true; m_clickPos = m_mouse; }

        updateHover(inPanel);

        if (paint) {
            handlePaintInput(lDown, rDown, inPanel);
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
    // PAINT INPUT -- tools
    // ========================================================================
    //
    // Four tools, one job each (W E R T, or the buttons in DETAILS):
    //   MOVE   drag a part.                         Shift: lock to an axis
    //   TURN   drag anywhere inside the ring.       Shift: 15 degree steps
    //   SIZE   drag a handle (opposite one pinned), or drag inside the box
    //          to scale evenly.                     Shift: keep ratio. Alt: from centre
    //   SHAPE  drag a plate's points; RMB on an edge adds one, RMB on a
    //          point removes it. A figure is converted to a plate first.
    // Clicking another part selects it in every tool; clicking empty space
    // (outside the TURN ring) drops the selection. Arrow keys nudge in the
    // current tool, Ctrl+D duplicates, Delete / RMB removes.

    /// Hit-test result: which part is under the cursor, and which copy of it.
    struct HoverItem {
        SelKind kind = SelKind::None;
        int     idx = -1;
        bool    mirror = false;
    };

    static bool pointInTris(sf::Vector2f p, const std::vector<sf::Vector2f>& t) {
        for (std::size_t i = 0; i + 2 < t.size(); i += 3) {
            const auto s = [&](sf::Vector2f a, sf::Vector2f b) {
                return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                };
            const float d1 = s(t[i], t[i + 1]), d2 = s(t[i + 1], t[i + 2]), d3 = s(t[i + 2], t[i]);
            const bool neg = d1 < 0.f || d2 < 0.f || d3 < 0.f, pos = d1 > 0.f || d2 > 0.f || d3 > 0.f;
            if (!(neg && pos)) return true;
        }
        return false;
    }

    /**
     * @brief The topmost part under a ship-space point.
     *
     * Same order the eye sees: canopy, figures over the hull, plates over,
     * figures under, plates under. Hits use the drawn geometry, so a long
     * thin stripe is grabbed where it is, not by an invisible disc.
     */
    HoverItem itemAt(sf::Vector2f p) const {
        HoverItem h;
        if (!m_livery) return h;
        const auto& lv = *m_livery;
        const float tol = 6.f / std::max(0.1f, m_zoom);   // thin figures: grab near the centre too

        if (lv.cockpit.style != ship::CockpitStyle::None) {
            std::vector<sf::Vector2f> glass, rim;
            ship::cockpitGeometry(lv.cockpit, glass, rim);
            if (pointInTris(p, rim) || pointInTris(p, glass)) { h.kind = SelKind::Cockpit; return h; }
        }
        const auto decalHit = [&](bool over) {
            for (int i = static_cast<int>(lv.decals.size()) - 1; i >= 0; --i) {
                const auto& d = lv.decals[i];
                if (d.over != over) continue;
                for (int m = 0; m < (d.mirrored ? 2 : 1); ++m) {
                    std::vector<sf::Vector2f> t;
                    ship::decalGeometry(d, t, m == 1);
                    const sf::Vector2f c{ m ? -d.pos.x : d.pos.x, d.pos.y };
                    if (pointInTris(p, t) || std::hypot(p.x - c.x, p.y - c.y) < tol) {
                        h.kind = SelKind::Decal; h.idx = i; h.mirror = (m == 1); return true;
                    }
                }
            }
            return false;
            };
        const auto plateHit = [&](bool over) {
            for (int i = static_cast<int>(lv.plates.size()) - 1; i >= 0; --i) {
                const auto& pl = lv.plates[i];
                if (pl.over != over) continue;
                for (int m = 0; m < (pl.mirrored ? 2 : 1); ++m) {
                    if (ship::detail::pointInPolygon(p, ship::plateWorld(pl, m == 1), 0.5f)) {
                        h.kind = SelKind::Plate; h.idx = i; h.mirror = (m == 1); return true;
                    }
                }
            }
            return false;
            };
        if (decalHit(true) || plateHit(true) || decalHit(false) || plateHit(false)) return h;
        return h;
    }

    int selIndex() const {
        return m_selKind == SelKind::Decal ? m_selDecal : (m_selKind == SelKind::Plate ? m_selPlate : -1);
    }

    void select(const HoverItem& h) {
        m_selKind = h.kind;
        m_selDecal = (h.kind == SelKind::Decal) ? h.idx : -1;
        m_selPlate = (h.kind == SelKind::Plate) ? h.idx : -1;
        m_lastVert = -1;
        if (h.kind == SelKind::Cockpit) {
            m_paintTarget = PaintTarget::Cockpit;
            syncPickerFrom(m_livery->paint.cockpit);
        }
        else if (h.kind != SelKind::None) {
            m_paintTarget = PaintTarget::Part;
            syncPickerFrom(targetColor(PaintTarget::Part));   // picker follows the selection
        }
    }

    void deselect() {
        m_selKind = SelKind::None;
        m_selDecal = m_selPlate = -1;
        m_grab = Grab::None;
        m_lastVert = -1;
        if (m_paintTarget == PaintTarget::Part) m_paintTarget = PaintTarget::Hull;
    }

    void setTool(Tool t) {
        if (m_tool == t) return;
        m_tool = t;
        m_grab = Grab::None;
        toast(t == Tool::Move ? "MOVE - DRAG A PART"
            : t == Tool::Turn ? "TURN - DRAG INSIDE THE RING"
            : t == Tool::Size ? "SIZE - HANDLES, OR DRAG INSIDE TO SCALE EVENLY"
            : "SHAPE - DRAG POINTS, RMB EDGE ADDS, RMB POINT REMOVES");
    }

    /// Screen radius of the TURN ring around the selection.
    float ringRadius() {
        Xform x = xform();
        if (!x.valid()) return 0.f;
        const float hd = 0.5f * std::sqrt(*x.w * *x.w + *x.h * *x.h);
        return std::max(34.f, hd * m_zoom + 26.f);
    }

    /// Is a screen point inside the selection's box (its own frame, a few px slack)?
    bool insideSelBox(sf::Vector2f screen) {
        Xform x = xform();
        if (!x.valid()) return false;
        const float a = *x.angle * 3.14159f / 180.f, ca = std::cos(a), sa = std::sin(a);
        const sf::Vector2f d = toLocal(screen) - *x.pos;
        const float lx = d.x * ca + d.y * sa, ly = -d.x * sa + d.y * ca;
        const float slack = 4.f / std::max(0.1f, m_zoom);
        return std::fabs(lx) <= *x.w * 0.5f + slack && std::fabs(ly) <= *x.h * 0.5f + slack;
    }

    void handlePaintInput(bool lDown, bool rDown, bool inPanel) {
        if (!m_livery) return;
        const bool shift = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);
        const bool alt = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LAlt)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RAlt);
        const bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl)
            || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl);

        // Slider drags (picker square, hue, alpha, shade) use the rects the
        // panels published last frame. A hidden slider publishes an off-screen rect.
        if (lDown && (m_pickDrag != 0 || (!m_lDown && (m_svRect.contains(m_mouse)
            || m_hueRect.contains(m_mouse) || m_alphaRect.contains(m_mouse) || m_shadeRect.contains(m_mouse))))) {
            if (m_pickDrag == 0)
                m_pickDrag = m_svRect.contains(m_mouse) ? 1 : m_hueRect.contains(m_mouse) ? 2
                : m_alphaRect.contains(m_mouse) ? 3 : 4;
            if (m_pickDrag == 4) dragShade(); else dragPicker();
            m_clickPending = false;
            m_lDown = lDown; m_rDown = rDown;
            return;
        }
        if (!lDown) m_pickDrag = 0;

        // ---- Tools ----
        if (!ctrl) {
            if (keyEdge(sf::Keyboard::Key::W, m_kW)) setTool(Tool::Move);
            if (keyEdge(sf::Keyboard::Key::E, m_kE)) setTool(Tool::Turn);
            if (keyEdge(sf::Keyboard::Key::R, m_kR)) setTool(Tool::Size);
            if (keyEdge(sf::Keyboard::Key::T, m_kT)) setTool(Tool::Shape);
        }

        // ---- Hover ----
        const sf::Vector2f ml = toLocal(m_mouse);
        m_hover = (inPanel && m_grab == Grab::None && !m_panning) ? itemAt(ml) : HoverItem{};
        updateVertexHover(inPanel);

        // ---- Press ----
        if (lDown && !m_lDown && inPanel) {
            m_clickPending = false;
            const HoverItem hit = m_hover;
            const bool hasSel = xform().valid();
            const bool onSel = hasSel && hit.kind == m_selKind
                && (hit.kind == SelKind::Cockpit || hit.idx == selIndex());

            switch (m_tool) {
            case Tool::Move:
                if (hit.kind != SelKind::None) { select(hit); m_grabMirror = hit.mirror; beginGrab(Grab::Move); }
                else deselect();
                break;
            case Tool::Turn: {
                const sf::Vector2f c = hasSel ? toScreen(*xform().pos) : sf::Vector2f{};
                const bool inRing = hasSel && std::hypot(m_mouse.x - c.x, m_mouse.y - c.y) <= ringRadius();
                if (hit.kind != SelKind::None && !onSel) select(hit);
                else if (hasSel && (onSel || inRing)) beginGrab(Grab::Rotate);
                else deselect();
                break;
            }
            case Tool::Size: {
                const Grab g = hasSel ? handleUnderCursor() : Grab::None;
                if (g != Grab::None) beginGrab(g);
                else if (hit.kind != SelKind::None && !onSel) select(hit);
                else if (hasSel && (onSel || insideSelBox(m_mouse))) beginGrab(Grab::Uniform);
                else deselect();
                break;
            }
            case Tool::Shape:
                if (m_selKind == SelKind::Plate && m_hoverVert >= 0) beginVertexDrag(m_hoverVert);
                else if (hit.kind != SelKind::None) select(hit);
                else deselect();
                break;
            }
        }
        if (!lDown) {
            if (m_grab == Grab::Vertex) m_vertDrag = -1;
            m_grab = Grab::None;
            m_atLimit = false;
        }
        if (lDown && m_grab == Grab::Vertex) dragVertex(shift);
        else if (lDown && m_grab != Grab::None) dragGizmo(shift, alt);

        // ---- Right button: reshape in SHAPE, otherwise remove ----
        if (rDown && !m_rDown && inPanel) {
            if (m_tool == Tool::Shape && m_selKind == SelKind::Plate && (m_hoverVert >= 0 || m_insEdge >= 0)) {
                if (m_hoverVert >= 0) removeVertex(m_hoverVert);
                else insertVertex(m_insEdge, m_insPos);
            }
            else if (m_hover.kind == SelKind::Decal || m_hover.kind == SelKind::Plate) {
                select(m_hover);
                deleteSelected();
            }
        }

        // ---- Keys ----
        if (keyEdge(sf::Keyboard::Key::Delete, m_kDel)) deleteSelected();
        if (keyEdge(sf::Keyboard::Key::D, m_kD) && ctrl) duplicateSelected();
        handleNudge(shift);
    }

    // ---- Arrow-key nudges: precise steps in whichever tool is active -------

    void handleNudge(bool shift) {
        const bool l = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Left);
        const bool r = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Right);
        const bool u = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Up);
        const bool d = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Down);
        const bool any = l || r || u || d;
        if (!any) { m_nudgeHeld = false; return; }
        if (m_grab != Grab::None || !xform().valid()) return;
        // First press acts at once; holding repeats after a short pause.
        if (m_nudgeHeld && m_time < m_nudgeNext) return;
        m_nudgeNext = m_time + (m_nudgeHeld ? 0.05f : 0.35f);
        m_nudgeHeld = true;
        nudge(static_cast<float>(r) - static_cast<float>(l), static_cast<float>(d) - static_cast<float>(u), shift);
    }

    void nudge(float dx, float dy, bool shift) {
        const std::vector<sf::Vector2f> env = m_design->envelope();
        if (m_tool == Tool::Shape) {
            Plate* p = selectedPlate();
            if (!p || m_lastVert < 0 || m_lastVert >= static_cast<int>(p->shape.size())) {
                toast("DRAG A POINT FIRST - ARROWS THEN MOVE THAT POINT");
                return;
            }
            const float step = shift ? 2.f : 0.5f;
            const sf::Vector2f w = ship::plateWorld(*p)[m_lastVert];
            if (auto q = movedVertex(*p, m_lastVert, { w.x + dx * step, w.y + dy * step }, env)) *p = *q;
            else reject("NO ROOM THAT WAY");
            return;
        }
        XState s = readX();
        if (m_tool == Tool::Move) {
            const float step = shift ? 5.f : 1.f;
            s.pos += sf::Vector2f{ dx * step, dy * step };
        }
        else if (m_tool == Tool::Turn) {
            s.angle += (dx + dy) * (shift ? 15.f : 1.f);
        }
        else {
            const float step = shift ? 5.f : 1.f;
            s.w += dx * step;
            s.h -= dy * step;   // up = taller
        }
        if (fitsX(s, env)) writeX(s);
        else reject("NO ROOM THAT WAY");
    }

    // ---- Gizmo ------------------------------------------------------------

    Xform xform() {
        Xform x;
        if (m_selKind == SelKind::Decal) {
            if (ship::Decal* d = selected()) { x.pos = &d->pos; x.w = &d->w; x.h = &d->h; x.angle = &d->angle; }
        }
        else if (m_selKind == SelKind::Plate) {
            if (Plate* p = selectedPlate()) { x.pos = &p->pos; x.w = &p->w; x.h = &p->h; x.angle = &p->angle; }
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
        const float hw = std::max(1.5f, *x.w * 0.5f), hh = std::max(1.5f, *x.h * 0.5f);
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
        const auto near = [&](sf::Vector2f p) { return std::hypot(p.x - m_mouse.x, p.y - m_mouse.y) < 10.f; };
        // Corners win over edges: on a small part they sit close together.
        for (int i = 0; i < 8; ++i) if (near(h[i])) { m_grabIdx = i; return Grab::Scale; }
        return Grab::None;
    }

    void beginGrab(Grab g) {
        Xform x = xform();
        if (!x.valid()) { m_grab = Grab::None; return; }
        m_grab = g;
        m_atLimit = false;
        m_grabStartPos = *x.pos;
        m_grabStartW = *x.w;
        m_grabStartH = *x.h;
        m_grabStartAngle = *x.angle;
        m_grabLocal = toLocal(m_mouse);
        m_grabStartMouse = m_mouse;
        const sf::Vector2f c = toScreen(*x.pos);
        m_grabStartMouseAngle = std::atan2(m_mouse.y - c.y, m_mouse.x - c.x) * 180.f / 3.14159f;
    }

    /// The four numbers the gizmo edits, as a value.
    struct XState {
        sf::Vector2f pos;
        float w = 0.f, h = 0.f, angle = 0.f;
    };

    XState readX() {
        Xform x = xform();
        return { *x.pos, *x.w, *x.h, *x.angle };
    }

    void writeX(const XState& s) {
        Xform x = xform();
        if (!x.valid()) return;
        *x.pos = s.pos; *x.w = s.w; *x.h = s.h; *x.angle = s.angle;
    }

    /// Size limits per kind of part: figures, plates, canopy.
    void sizeLimits(float& lo, float& hi) const {
        if (m_selKind == SelKind::Plate) { lo = 2.f; hi = 140.f; }
        else if (m_selKind == SelKind::Cockpit) { lo = 3.f; hi = 60.f; }
        else { lo = 2.f; hi = 90.f; }
    }

    /// Clamp `s` the way a commit would, then test it against the envelope.
    bool fitsX(XState& s, const std::vector<sf::Vector2f>& env) {
        if (m_selKind == SelKind::Decal) {
            ship::Decal d = *selected();
            d.pos = s.pos; d.w = s.w; d.h = s.h; d.angle = s.angle;
            ship::clampDecal(d);
            s = { d.pos, d.w, d.h, d.angle };
            return ship::decalFits(d, env);
        }
        if (m_selKind == SelKind::Plate) {
            Plate p = *selectedPlate();
            p.pos = s.pos; p.w = s.w; p.h = s.h; p.angle = s.angle;
            ship::clampPlate(p);
            s = { p.pos, p.w, p.h, p.angle };
            return ship::plateFits(p, env);
        }
        ship::Cockpit c = m_livery->cockpit;
        c.pos = s.pos;
        c.w = std::clamp(s.w, 3.f, 60.f);
        c.h = std::clamp(s.h, 3.f, 60.f);
        c.angle = s.angle;
        s = { c.pos, c.w, c.h, c.angle };
        return ship::cockpitFits(c, env);
    }

    static XState lerpX(const XState& a, const XState& b, float t) {
        // Angles take the short way round: 359 -> 1 is +2, not -358.
        const float da = std::remainder(b.angle - a.angle, 360.f);
        return { a.pos + (b.pos - a.pos) * t, a.w + (b.w - a.w) * t,
                 a.h + (b.h - a.h) * t, a.angle + da * t };
    }

    /**
     * @brief Move / turn / size the selection, continuous by default.
     *
     *   MOVE     drag.                     SHIFT locks to the dominant axis.
     *   ROTATE   around the centre.        SHIFT snaps to 15 degrees.
     *   SCALE    a handle; the OPPOSITE one stays pinned.
     *                                      SHIFT (corners) keeps proportions.
     *                                      ALT scales about the centre.
     *   UNIFORM  drag inside the box: even scale about the centre.
     *
     * At the model limit the selection SLIDES along it instead of freezing:
     * a short binary search finds the furthest legal point between where it
     * was and where the mouse wants it. A selection already outside (a hull
     * edit shrank the envelope) may move freely, so it can be dragged home.
     */
    void dragGizmo(bool shift, bool alt) {
        Xform x = xform();
        if (!x.valid()) return;

        const std::vector<sf::Vector2f> env = m_design->envelope();
        const XState cur = readX();
        XState tgt = cur;
        const sf::Vector2f mouseL = toLocal(m_mouse);
        float lo = 2.f, hi = 90.f;
        sizeLimits(lo, hi);

        if (m_grab == Grab::Move) {
            sf::Vector2f dl = mouseL - m_grabLocal;
            if (m_grabMirror) dl.x = -dl.x;          // dragging the mirror copy
            if (shift) {
                if (std::fabs(dl.x) > std::fabs(dl.y)) dl.y = 0.f; else dl.x = 0.f;
            }
            tgt.pos = m_grabStartPos + dl;
        }
        else if (m_grab == Grab::Rotate) {
            const sf::Vector2f c = toScreen(cur.pos);
            const float now = std::atan2(m_mouse.y - c.y, m_mouse.x - c.x) * 180.f / 3.14159f;
            float a = m_grabStartAngle + (now - m_grabStartMouseAngle);
            if (shift) a = std::round(a / 15.f) * 15.f;
            tgt.angle = a;
        }
        else if (m_grab == Grab::Uniform) {
            const sf::Vector2f c = toScreen(m_grabStartPos);
            const float d0 = std::max(4.f, std::hypot(m_grabStartMouse.x - c.x, m_grabStartMouse.y - c.y));
            float k = std::hypot(m_mouse.x - c.x, m_mouse.y - c.y) / d0;
            const float w0 = std::max(lo, m_grabStartW), h0 = std::max(lo, m_grabStartH);
            k = std::clamp(k, std::max(lo / w0, lo / h0), std::min(hi / w0, hi / h0));
            tgt.w = w0 * k; tgt.h = h0 * k; tgt.pos = m_grabStartPos;
        }
        else if (m_grab == Grab::Scale) {
            // Mouse into the object's own axes (at the angle it had when grabbed).
            const float a0 = m_grabStartAngle * 3.14159f / 180.f;
            const float ca = std::cos(a0), sa = std::sin(a0);
            const sf::Vector2f d = mouseL - m_grabStartPos;
            const float mx = d.x * ca + d.y * sa;
            const float my = -d.x * sa + d.y * ca;

            // Which way this handle faces: corners both axes, edges one.
            const int i = m_grabIdx;
            const float sx = (i == 1 || i == 2 || i == 5) ? 1.f : (i == 0 || i == 3 || i == 7) ? -1.f : 0.f;
            const float sy = (i == 2 || i == 3 || i == 6) ? 1.f : (i == 0 || i == 1 || i == 4) ? -1.f : 0.f;

            const float w0 = std::max(lo, m_grabStartW), h0 = std::max(lo, m_grabStartH);
            float w = w0, h = h0;
            if (alt) {
                if (sx != 0.f) w = 2.f * sx * mx;
                if (sy != 0.f) h = 2.f * sy * my;
            }
            else {
                // Distance from the pinned opposite side.
                if (sx != 0.f) w = sx * mx + w0 * 0.5f;
                if (sy != 0.f) h = sy * my + h0 * 0.5f;
            }
            if (shift && sx != 0.f && sy != 0.f) {
                const float k = std::max(w / w0, h / h0);
                w = w0 * k; h = h0 * k;
            }
            // Clamp BEFORE placing the centre, or a clamped size drifts off its anchor.
            w = std::clamp(w, lo, hi);
            h = std::clamp(h, lo, hi);

            sf::Vector2f c = m_grabStartPos;
            if (!alt) {
                const float ox = sx * (w - w0) * 0.5f, oy = sy * (h - h0) * 0.5f;
                c += { ox* ca - oy * sa, ox* sa + oy * ca };
            }
            tgt.pos = c; tgt.w = w; tgt.h = h; tgt.angle = m_grabStartAngle;
        }

        XState fitted = tgt;
        if (fitsX(fitted, env)) { writeX(fitted); m_atLimit = false; return; }

        XState here = cur;
        if (!fitsX(here, env)) { writeX(fitted); return; }   // already outside: let it come home

        float a = 0.f, b = 1.f;
        XState best = here;
        for (int it = 0; it < 7; ++it) {
            const float mid = (a + b) * 0.5f;
            XState s = lerpX(cur, tgt, mid);
            if (fitsX(s, env)) { a = mid; best = s; }
            else b = mid;
        }
        writeX(best);
        if (!m_atLimit) reject(m_selKind == SelKind::Cockpit ? "CANOPY AT THE MODEL LIMIT" : "AT THE MODEL LIMIT");
        m_atLimit = true;
    }

    // ---- SHAPE: editing a plate's points ------------------------------------



    Plate* selectedPlate() {
        if (!m_livery || m_selKind != SelKind::Plate || m_selPlate < 0
            || m_selPlate >= static_cast<int>(m_livery->plates.size())) return nullptr;
        return &m_livery->plates[m_selPlate];
    }

    /// A plate sitting on the centreline, unturned and unmirrored, is edited
    /// symmetrically while MIRROR is on -- the way the hull and model are.
    bool plateSymmetric(const Plate& p) const {
        const float a = std::fmod(std::fmod(p.angle, 360.f) + 360.f, 360.f);
        return m_design->symmetric() && !p.mirrored && std::fabs(p.pos.x) < 0.75f
            && (a < 0.5f || a > 359.5f);
    }

    /// The point mirroring `i` across the centreline, or -1.
    static int vertexPartner(const std::vector<sf::Vector2f>& w, int i) {
        if (std::fabs(w[i].x) < 0.75f) return -1;
        int best = -1; float bestD = 1.5f;
        for (int j = 0; j < static_cast<int>(w.size()); ++j) {
            if (j == i) continue;
            const float d = std::hypot(w[j].x + w[i].x, w[j].y - w[i].y);
            if (d < bestD) { bestD = d; best = j; }
        }
        return best;
    }

    void updateVertexHover(bool inPanel) {
        m_hoverVert = -1;
        m_insEdge = -1;
        Plate* p = selectedPlate();
        if (!p || m_tool != Tool::Shape || !inPanel || m_grab != Grab::None) return;
        const std::vector<sf::Vector2f> w = ship::plateWorld(*p);
        float best = 9.f;
        for (int i = 0; i < static_cast<int>(w.size()); ++i) {
            const sf::Vector2f s = toScreen(w[i]);
            const float d = std::hypot(s.x - m_mouse.x, s.y - m_mouse.y);
            if (d < best) { best = d; m_hoverVert = i; }
        }
        if (m_hoverVert >= 0) return;
        best = 8.f;
        const std::size_t n = w.size();
        for (std::size_t i = 0; i < n; ++i) {
            const sf::Vector2f a = toScreen(w[i]), b = toScreen(w[(i + 1) % n]);
            const float d = ship::detail::segDist(m_mouse, a, b);
            if (d < best) { best = d; m_insEdge = static_cast<int>(i); }
        }
        if (m_insEdge >= 0) {
            // Project onto the edge in ship space, so the new point lands ON it.
            const sf::Vector2f a = w[m_insEdge], b = w[(m_insEdge + 1) % n], m = toLocal(m_mouse);
            const sf::Vector2f ab = b - a;
            const float l2 = std::max(1e-6f, ab.x * ab.x + ab.y * ab.y);
            const float t = std::clamp(((m.x - a.x) * ab.x + (m.y - a.y) * ab.y) / l2, 0.f, 1.f);
            m_insPos = a + ab * t;
        }
    }

    void beginVertexDrag(int i) {
        Plate* p = selectedPlate();
        if (!p) return;
        m_grab = Grab::Vertex;
        m_vertDrag = i;
        m_lastVert = i;
        m_atLimit = false;
        const std::vector<sf::Vector2f> w = ship::plateWorld(*p);
        m_vertPartner = plateSymmetric(*p) ? vertexPartner(w, i) : -1;
        m_vertOnAxis = plateSymmetric(*p) && std::fabs(w[i].x) < 0.75f;
    }

    /// The plate with point `i` at ship-space `w` (partner mirrored), or nothing if illegal.
    std::optional<Plate> movedVertex(const Plate& p, int i, sf::Vector2f w,
        const std::vector<sf::Vector2f>& env) const {
        Plate q = p;
        const std::vector<sf::Vector2f> world = ship::plateWorld(q);
        const int partner = plateSymmetric(q) ? vertexPartner(world, i) : -1;
        if (plateSymmetric(q) && std::fabs(world[i].x) < 0.75f) w.x = 0.f;   // on-axis stays on-axis
        q.shape[i] = ship::plateToShape(q, w);
        if (partner >= 0) q.shape[partner] = ship::plateToShape(q, { -w.x, w.y });
        ship::normalizePlate(q);
        if (q.w > 140.f || q.h > 140.f) return std::nullopt;
        if (!ship::plateValid(q) || !ship::plateFits(q, env)) return std::nullopt;
        return q;
    }

    void dragVertex(bool shift) {
        Plate* p = selectedPlate();
        if (!p || m_vertDrag < 0 || m_vertDrag >= static_cast<int>(p->shape.size())) return;
        const std::vector<sf::Vector2f> env = m_design->envelope();
        sf::Vector2f target = toLocal(m_mouse);
        if (shift) target = { std::round(target.x), std::round(target.y) };   // 1 px grid
        const sf::Vector2f from = ship::plateWorld(*p)[m_vertDrag];
        if (std::hypot(target.x - from.x, target.y - from.y) < 1e-3f) return;

        if (auto q = movedVertex(*p, m_vertDrag, target, env)) { *p = *q; m_atLimit = false; return; }

        // Slide: the furthest legal point between where it is and the mouse.
        float a = 0.f, b = 1.f;
        std::optional<Plate> best;
        for (int it = 0; it < 7; ++it) {
            const float mid = (a + b) * 0.5f;
            if (auto q = movedVertex(*p, m_vertDrag, from + (target - from) * mid, env)) { a = mid; best = q; }
            else b = mid;
        }
        if (best) *p = *best;
        if (!m_atLimit) {
            Plate probe = *p;
            probe.shape[m_vertDrag] = ship::plateToShape(probe, target);
            ship::normalizePlate(probe);
            reject(ship::plateValid(probe) ? "AT THE MODEL LIMIT" : "SHAPE WOULD CROSS ITSELF");
        }
        m_atLimit = true;
    }

    void insertVertex(int edge, sf::Vector2f w) {
        Plate* p = selectedPlate();
        if (!p) return;
        const bool sym = plateSymmetric(*p) && std::fabs(w.x) >= 0.75f;
        const std::size_t need = sym ? 2u : 1u;
        if (p->shape.size() + need > static_cast<std::size_t>(ship::MAX_PLATE_POINTS)) {
            reject("PLATE FULL - 16 POINT LIMIT"); return;
        }
        Plate q = *p;
        std::vector<sf::Vector2f> world = ship::plateWorld(q);
        world.insert(world.begin() + edge + 1, w);
        if (sym) {
            // The mirrored point goes on whichever edge is nearest its mirror.
            const sf::Vector2f mw{ -w.x, w.y };
            int best = 0; float bestD = 1e9f;
            for (std::size_t i = 0; i < world.size(); ++i) {
                const float d = ship::detail::segDist(mw, world[i], world[(i + 1) % world.size()]);
                if (d < bestD) { bestD = d; best = static_cast<int>(i); }
            }
            world.insert(world.begin() + best + 1, mw);
        }
        q.shape.clear();
        for (const auto& v : world) q.shape.push_back(ship::plateToShape(q, v));
        ship::normalizePlate(q);
        if (!ship::plateValid(q)) { reject("SHAPE WOULD CROSS ITSELF"); return; }
        *p = q;
        m_lastVert = edge + 1;
        toast(sym ? "POINTS ADDED - MIRRORED" : "POINT ADDED");
    }

    void removeVertex(int i) {
        Plate* p = selectedPlate();
        if (!p) return;
        const std::vector<sf::Vector2f> world = ship::plateWorld(*p);
        const int partner = plateSymmetric(*p) ? vertexPartner(world, i) : -1;
        const std::size_t drop = partner >= 0 ? 2u : 1u;
        if (p->shape.size() < 3 + drop) { reject("A PLATE NEEDS 3 POINTS"); return; }
        Plate q = *p;
        if (partner >= 0) {
            q.shape.erase(q.shape.begin() + std::max(i, partner));
            q.shape.erase(q.shape.begin() + std::min(i, partner));
        }
        else q.shape.erase(q.shape.begin() + i);
        ship::normalizePlate(q);
        if (!ship::plateValid(q)) { reject("SHAPE WOULD CROSS ITSELF"); return; }
        *p = q;
        m_hoverVert = -1;
        m_lastVert = -1;
        toast("POINT REMOVED");
    }

    // ---- Colour picker and shade -------------------------------------------

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
            static_cast<std::uint8_t>(m_pickA * 255.f)), m_pickDrag == 3);
    }

    /// The SHADE slider in DETAILS: tone of the selected part.
    void dragShade() {
        const float t = std::clamp((m_mouse.x - m_shadeRect.x) / std::max(1.f, m_shadeRect.w), 0.f, 1.f);
        const float v = ship::SHADE_MIN + (ship::SHADE_MAX - ship::SHADE_MIN) * t;
        if (ship::Decal* d = (m_selKind == SelKind::Decal) ? selected() : nullptr) { d->shade = v; d->tonal = true; }
        else if (Plate* p = selectedPlate()) { p->shade = v; p->tonal = true; }
        syncPickerFrom(targetColor(m_paintTarget));
    }

    ship::Decal* selected() {
        if (!m_livery || m_selKind != SelKind::Decal || m_selDecal < 0
            || m_selDecal >= static_cast<int>(m_livery->decals.size())) return nullptr;
        return &m_livery->decals[m_selDecal];
    }

    // ---- Parts: add, convert, duplicate, delete ------------------------------

    /// Delete whatever is selected: a figure, a plate, or the canopy.
    void deleteSelected() {
        if (!m_livery) return;
        if (m_selKind == SelKind::Cockpit && m_livery->cockpit.style != ship::CockpitStyle::None) {
            m_livery->cockpit.style = ship::CockpitStyle::None;
            deselect();
            toast("CANOPY REMOVED");
            return;
        }
        if (selected()) {
            m_livery->decals.erase(m_livery->decals.begin() + m_selDecal);
            deselect();
            toast("FIGURE REMOVED");
        }
        else if (selectedPlate()) {
            m_livery->plates.erase(m_livery->plates.begin() + m_selPlate);
            deselect();
            toast("PLATE REMOVED");
        }
    }

    void addDecal(ship::DecalKind kind) {
        if (!m_livery || !m_design) return;
        if (static_cast<int>(m_livery->decals.size()) >= ship::MAX_DECALS) {
            reject("FIGURE LIMIT REACHED"); return;
        }
        ship::Decal d;
        d.kind = kind;
        d.pos = { 0.f, m_design->stats().centreOfMass.y };
        d.color = m_livery->paint.outline;
        if (kind == ship::DecalKind::Line) { d.w = 22.f; d.h = 3.f; d.thickness = 3.f; }
        if (kind == ship::DecalKind::Ring) { d.w = 18.f; d.h = 18.f; d.thickness = 2.f; }
        if (kind == ship::DecalKind::Oval) { d.w = 16.f; d.h = 10.f; }
        if (kind == ship::DecalKind::Tri) { d.w = 12.f; d.h = 14.f; }
        if (!ship::decalFits(d, m_design->envelope())) { reject("NO ROOM INSIDE THE MODEL LIMIT"); return; }
        m_livery->decals.push_back(d);
        select({ SelKind::Decal, static_cast<int>(m_livery->decals.size()) - 1, false });
        toast(std::string(ship::decalKindName(kind)) + " ADDED");
    }

    void addPlate(ship::PlateStamp kind) {
        if (!m_livery || !m_design) return;
        if (static_cast<int>(m_livery->plates.size()) >= ship::MAX_PLATES) {
            reject("PLATE LIMIT REACHED"); return;
        }
        const std::vector<sf::Vector2f> env = m_design->envelope();
        Plate p = ship::plateStamp(kind, { 0.f, m_design->stats().centreOfMass.y });
        p.color = m_livery->paint.outline;
        // Small hulls: shrink the stamp until it fits rather than refusing.
        for (int tries = 0; tries < 4 && !ship::plateFits(p, env); ++tries) { p.w *= 0.75f; p.h *= 0.75f; }
        if (!ship::plateFits(p, env)) { reject("NO ROOM INSIDE THE MODEL LIMIT"); return; }
        m_livery->plates.push_back(p);
        select({ SelKind::Plate, static_cast<int>(m_livery->plates.size()) - 1, false });
        toast(std::string(ship::plateStampName(kind)) + " PLATE ADDED - T TO RESHAPE");
    }

    /// A figure becomes a plate with the same look, so its outline can be edited.
    void convertToPlate() {
        ship::Decal* d = selected();
        if (!d) return;
        if (static_cast<int>(m_livery->plates.size()) >= ship::MAX_PLATES) { reject("PLATE LIMIT REACHED"); return; }
        Plate p = ship::plateFromDecal(*d);
        if (!ship::plateFits(p, m_design->envelope())) { reject("NO ROOM INSIDE THE MODEL LIMIT"); return; }
        const bool outlined = p.accent;
        m_livery->decals.erase(m_livery->decals.begin() + m_selDecal);
        m_livery->plates.push_back(p);
        select({ SelKind::Plate, static_cast<int>(m_livery->plates.size()) - 1, false });
        m_tool = Tool::Shape;
        toast(outlined ? "NOW A PLATE - OUTLINE BECAME SOLID + ACCENT" : "NOW A PLATE - DRAG ITS POINTS");
    }

    /// Ctrl+D: a copy beside the original, selected.
    void duplicateSelected() {
        if (!m_livery) return;
        const std::vector<sf::Vector2f> env = m_design->envelope();
        static const sf::Vector2f offs[] = { { 0.f, 6.f }, { 0.f, -6.f }, { 6.f, 0.f }, { -6.f, 0.f }, { 0.f, 0.f } };
        if (ship::Decal* d = selected()) {
            if (static_cast<int>(m_livery->decals.size()) >= ship::MAX_DECALS) { reject("FIGURE LIMIT REACHED"); return; }
            for (const auto& o : offs) {
                ship::Decal c = *d;
                c.pos += o;
                if (!ship::decalFits(c, env)) continue;
                m_livery->decals.push_back(c);
                select({ SelKind::Decal, static_cast<int>(m_livery->decals.size()) - 1, false });
                toast("FIGURE DUPLICATED");
                return;
            }
        }
        else if (Plate* p = selectedPlate()) {
            if (static_cast<int>(m_livery->plates.size()) >= ship::MAX_PLATES) { reject("PLATE LIMIT REACHED"); return; }
            for (const auto& o : offs) {
                Plate c = *p;
                c.pos += o;
                if (!ship::plateFits(c, env)) continue;
                m_livery->plates.push_back(c);
                select({ SelKind::Plate, static_cast<int>(m_livery->plates.size()) - 1, false });
                toast("PLATE DUPLICATED");
                return;
            }
        }
        else return;
        reject("NO ROOM FOR A COPY");
    }

    /// Pick a canopy style. A new canopy arrives selected, handles on it.
    void setCanopy(ship::CockpitStyle style) {
        if (!m_livery) return;
        auto& c = m_livery->cockpit;
        if (style == ship::CockpitStyle::None) {
            if (c.style != ship::CockpitStyle::None) {
                c.style = style;
                if (m_selKind == SelKind::Cockpit) deselect();
                toast("CANOPY REMOVED");
            }
            return;
        }
        const ship::CockpitStyle before = c.style;
        c.style = style;
        if (!ship::cockpitFits(c, m_design->envelope())) {
            c.style = before;
            reject("NO ROOM FOR THAT CANOPY - SHRINK OR MOVE IT");
            return;
        }
        select({ SelKind::Cockpit, -1, false });
        toast(std::string("CANOPY ") + ship::cockpitStyleName(style));
    }

    /// Ink of the selected part: TONE (follows the hull) or a fixed COLOUR.
    void setInk(bool tonal) {
        if (ship::Decal* d = selected()) d->tonal = tonal;
        else if (Plate* p = selectedPlate()) p->tonal = tonal;
        else return;
        syncPickerFrom(targetColor(PaintTarget::Part));
        toast(tonal ? "TONE - FOLLOWS THE HULL: FLASH, HEAT, STAGGER" : "COLOUR - FIXED, PICK IT ABOVE");
    }

    /**
     * @brief Put a picked colour on the current target.
     *
     * @param alphaOnly the alpha strip is being dragged: a TONE part keeps its
     *        tone and only becomes more or less see-through. Any other pick on
     *        a TONE part switches it to COLOUR -- the player chose a colour.
     */
    void applyPaletteColor(sf::Color c, bool alphaOnly = false) {
        if (!m_livery) return;
        auto& p = m_livery->paint;
        switch (m_paintTarget) {
        case PaintTarget::Hull:
            p.hull = sf::Color(c.r, c.g, c.b, std::max(c.a, ship::HULL_MIN_ALPHA));
            break;
        case PaintTarget::Outline: p.outline = sf::Color(c.r, c.g, c.b, 255); break;   // the silhouette always reads
        case PaintTarget::Plasma:  p.plasma = c; break;
        case PaintTarget::Thrust:  p.thrust = sf::Color(c.r, c.g, c.b, 180); break;
        case PaintTarget::Turbo:   p.turbo = sf::Color(c.r, c.g, c.b, 220); break;
        case PaintTarget::Dodge:   p.dodge = sf::Color(c.r, c.g, c.b, 230); break;
        case PaintTarget::Parry:   p.parry = c; break;
        case PaintTarget::Homing:  p.homing = c; break;
        case PaintTarget::Cockpit: p.cockpit = sf::Color(c.r, c.g, c.b, 235); break;
        case PaintTarget::Part: {
            ship::Decal* d = selected();
            Plate* pl = selectedPlate();
            sf::Color* col = d ? &d->color : (pl ? &pl->color : nullptr);
            bool* tonal = d ? &d->tonal : (pl ? &pl->tonal : nullptr);
            if (!col) break;
            if (alphaOnly) { col->a = c.a; break; }
            if (*tonal) { *tonal = false; toast("COLOUR INK - THIS PART NO LONGER FOLLOWS THE HULL"); }
            *col = c;
            break;
        }
        }
    }

    sf::Color targetColor(PaintTarget t) {
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
        case PaintTarget::Part: break;
        }
        // A TONE part shows what it looks like on the painted hull right now.
        if (const ship::Decal* d = selected()) return ship::inkOf(*d, p.hull);
        if (const Plate* pl = selectedPlate()) return ship::inkOf(*pl, p.hull);
        return TEXT_DEAD;
    }

    // ---- Ship files -------------------------------------------------------

    /// QUICK EXPORT: save under the current name, or CLASS-POINTS if it has none.
    /// Never overwrites -- a clash gets -2, -3 ... The HANGAR is for choosing.
    void exportShip() {
        if (!m_design || !m_livery) return;
        std::string err;
        char name[64];
        std::snprintf(name, sizeof(name), "%s-%d", m_design->spec().name, m_design->pointCount());
        const std::string base = m_shipName.empty() ? std::string(name) : m_shipName;
        const std::string path = ship::shipfile::save(*m_design, *m_livery, base, err);
        if (!path.empty()) m_shipName = std::filesystem::path(path).stem().string();
        m_fileMsg = path.empty() ? ("EXPORT FAILED - " + err) : ("SAVED " + path);
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
        deselect();
        m_hover = {};
        toast(m == Mode::Model ? "MODEL - SHAPE WHAT THE SHIP LOOKS LIKE"
            : m == Mode::Paint ? "PAINT - PARTS, COLOURS, CANOPY   W E R T TOOLS"
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
        m_viewManual = false;
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

        updatePreview();   // after input, before anything reads it
        drawHeader(size);
        drawHullPanel();
        drawStatsPanel();
        drawMountPanel();
        drawStatusPanel();
        drawHints(size);
        drawHangar();      // over the canvas; the controls below stay visible but locked
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

        // Which file this ship is, so SAVE / OVERWRITE in the hangar are not a guess.
        sf::Text nm(monoFont(), m_shipName.empty() ? "UNSAVED" : m_shipName, 15);
        nm.setLetterSpacing(1.8f);
        nm.setFillColor(m_shipName.empty() ? TEXT_DEAD : TEXT_DIM);
        nm.setPosition({ mir.getPosition().x + mir.getGlobalBounds().size.x + 30.f, size.y * 0.042f });
        m_window->draw(nm);

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
                    // A slot that is valid can still be refused (reactor full, class
                    // limit): say so on the edge, before the click, not after.
                    const bool refused = e.valid && !mounted && !m_pvRefused.empty();
                    const std::string tag = !e.valid ? std::string(e.reason)
                        : refused ? "DRIVE  " + m_pvRefused
                        : (mounted ? "DRIVE  RMB REMOVE" : "DRIVE  RMB FIT");
                    sf::Text t(monoFont(), tag, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(e.valid && !refused ? AMBER : RED);
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
                    else if (!m_pvRefused.empty())
                        std::snprintf(buf, sizeof(buf), "%.0f deg  %s", g.interiorAngleDeg, m_pvRefused.c_str());
                    else
                        std::snprintf(buf, sizeof(buf), "%.0f deg  RMB FIT WEAPON", g.interiorAngleDeg);
                    sf::Text t(monoFont(), buf, 12);
                    t.setLetterSpacing(1.3f);
                    const bool refused = g.valid && !mounted && !m_pvRefused.empty();
                    t.setFillColor(g.valid && !refused ? (spinal ? VIOLET : GREEN) : RED);
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
            const sf::Vector2f o{ r.x, r.y };   // gizmo helpers return screen space
            const auto P = [&](sf::Vector2f p) { return sf::Vector2f{ p.x - o.x, p.y - o.y }; };

            // Faint grid only: this is a preview, not a blueprint.
            sf::VertexArray grid(sf::PrimitiveType::Lines);
            const sf::Color gc(14, 22, 30);
            for (float g = -160.f; g <= 160.f; g += 20.f) {
                grid.append({ { L({ g, 0.f }).x, 0.f }, gc }); grid.append({ { L({ g, 0.f }).x, r.h }, gc });
                grid.append({ { 0.f, L({ 0.f, g }).y }, gc }); grid.append({ { r.w, L({ 0.f, g }).y }, gc });
            }
            m_window->draw(grid);

            // The ship exactly as the game draws it: the same liveryPass() mesh,
            // with the painted hull standing in for the live fill.
            const ship::LiveInk ink{ lv.paint.hull, lv.paint.outline };
            const auto pass = [&](bool over) {
                std::vector<sf::Vertex> v;
                ship::liveryPass(lv, ink, over, v);
                for (auto& q : v) q.position = L(q.position);
                if (!v.empty()) m_window->draw(v.data(), v.size(), sf::PrimitiveType::Triangles);
                };
            pass(false);
            fillPolygon(outline, L, lv.paint.hull);
            strokePolygon(outline, L, lv.paint.outline, 2.5f);
            pass(true);

            // ---- Hover: the part the next click would pick, outlined ----
            const bool hoverIsSel = m_hover.kind == m_selKind
                && (m_hover.kind == SelKind::Cockpit || m_hover.idx == selIndex());
            if (m_hover.kind != SelKind::None && !hoverIsSel) {
                const sf::Color hc(255, 214, 0, 120);
                if (m_hover.kind == SelKind::Plate && m_hover.idx < static_cast<int>(lv.plates.size())) {
                    dashedPolygon(ship::plateWorld(lv.plates[m_hover.idx], m_hover.mirror), L, hc, 4.f, 3.f);
                }
                else if (m_hover.kind == SelKind::Decal && m_hover.idx < static_cast<int>(lv.decals.size())) {
                    const auto& d = lv.decals[m_hover.idx];
                    dashedPolygon(boxCorners({ m_hover.mirror ? -d.pos.x : d.pos.x, d.pos.y },
                        d.w, d.h, m_hover.mirror ? -d.angle : d.angle), L, hc, 4.f, 3.f);
                }
                else if (m_hover.kind == SelKind::Cockpit) {
                    const auto& c = lv.cockpit;
                    dashedPolygon(boxCorners(c.pos, c.w, c.h, c.angle), L, hc, 4.f, 3.f);
                }
            }

            // ---- The selection, drawn for the active tool ----
            sf::Vector2f hs[8], centre, rot;
            if (gizmoHandles(hs, centre, rot)) {
                const sf::Color boxC = m_atLimit ? sf::Color(255, 48, 0, 220) : sf::Color(255, 214, 0, 190);
                const Plate* pl = selectedPlate();

                if (m_tool == Tool::Shape && pl) {
                    // Points and edges, no box: in SHAPE the outline IS the handle.
                    const std::vector<sf::Vector2f> w = ship::plateWorld(*pl);
                    strokePolygon(w, L, boxC, 1.f);
                    const int partner = (m_hoverVert >= 0 && plateSymmetric(*pl)) ? vertexPartner(w, m_hoverVert)
                        : (m_vertDrag >= 0 ? m_vertPartner : -1);
                    for (int i = 0; i < static_cast<int>(w.size()); ++i) {
                        const sf::Vector2f p = L(w[i]);
                        const bool hot = (i == m_hoverVert || i == m_vertDrag);
                        const float sz = hot ? 9.f : 6.f;
                        sf::RectangleShape h({ sz, sz });
                        h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                        h.setFillColor(hot ? sf::Color::White : (i == partner ? CYAN : AMBER));
                        m_window->draw(h);
                    }
                    if (m_insEdge >= 0 && m_hoverVert < 0) {
                        const sf::Vector2f p = L(m_insPos);
                        cross(p, 5.f, CYAN);
                        sf::Text t(monoFont(), "RMB ADD POINT", 11);
                        t.setLetterSpacing(1.3f);
                        t.setFillColor(CYAN);
                        t.setPosition({ p.x + 9.f, p.y + 4.f });
                        m_window->draw(t);
                    }
                    else if (m_hoverVert >= 0 && m_vertDrag < 0) {
                        const sf::Vector2f p = L(w[m_hoverVert]);
                        sf::Text t(monoFont(), partner >= 0 ? "DRAG - MIRRORED   RMB REMOVE" : "DRAG   RMB REMOVE", 11);
                        t.setLetterSpacing(1.3f);
                        t.setFillColor(AMBER);
                        t.setPosition({ p.x + 9.f, p.y + 4.f });
                        m_window->draw(t);
                    }
                }
                else {
                    for (int i = 0; i < 4; ++i) thickLine(P(hs[i]), P(hs[(i + 1) % 4]), boxC, 1.f);
                    cross(P(centre), 4.f, sf::Color(255, 214, 0, 160));

                    if (m_tool == Tool::Turn) {
                        // The ring: press anywhere inside it to turn.
                        const float rr = ringRadius();
                        sf::CircleShape ring(rr, 48);
                        ring.setOrigin({ rr, rr });
                        ring.setPosition(P(centre));
                        ring.setFillColor(sf::Color(255, 214, 0, m_grab == Grab::Rotate ? 18 : 8));
                        ring.setOutlineThickness(1.f);
                        ring.setOutlineColor(sf::Color(255, 214, 0, m_grab == Grab::Rotate ? 200 : 110));
                        m_window->draw(ring);
                        const sf::Vector2f dir = P(rot) - P(centre);
                        const float dl = std::max(0.001f, std::hypot(dir.x, dir.y));
                        thickLine(P(centre), P(centre) + dir / dl * rr, AMBER, 1.5f);
                        sf::CircleShape knob(4.f);
                        knob.setOrigin({ 4.f, 4.f });
                        knob.setPosition(P(centre) + dir / dl * rr);
                        knob.setFillColor(AMBER);
                        m_window->draw(knob);
                    }
                    else if (m_tool == Tool::Size) {
                        for (int i = 0; i < 8; ++i) {
                            const float sz = (i < 4) ? 8.f : 6.f;
                            sf::RectangleShape h({ sz, sz });
                            const sf::Vector2f p = P(hs[i]);
                            h.setPosition({ p.x - sz * 0.5f, p.y - sz * 0.5f });
                            const bool grabbed = (m_grab == Grab::Scale && m_grabIdx == i);
                            h.setFillColor(grabbed || std::hypot(hs[i].x - m_mouse.x, hs[i].y - m_mouse.y) < 10.f
                                ? sf::Color::White : AMBER);
                            m_window->draw(h);
                        }
                    }
                    else if (m_tool == Tool::Shape) {
                        sf::Text t(monoFont(), m_selKind == SelKind::Decal
                            ? "A FIGURE - USE  TO PLATE  IN DETAILS TO EDIT ITS SHAPE"
                            : "THE CANOPY HAS A FIXED SHAPE", 11);
                        t.setLetterSpacing(1.3f);
                        t.setFillColor(AMBER);
                        t.setPosition({ P(centre).x + 14.f, P(centre).y - 22.f });
                        m_window->draw(t);
                    }
                }

                // Live numbers: free values need a readout.
                Xform x = xform();
                if (x.valid()) {
                    char buf[128];
                    const float shown = std::fmod(std::fmod(*x.angle, 360.f) + 360.f, 360.f);
                    if (m_tool == Tool::Shape && pl)
                        std::snprintf(buf, sizeof(buf), "%d / %d POINTS%s", static_cast<int>(pl->shape.size()),
                            ship::MAX_PLATE_POINTS, m_atLimit ? "   AT LIMIT" : "");
                    else
                        std::snprintf(buf, sizeof(buf), "%.1f x %.1f   %.0f DEG%s", *x.w, *x.h, shown,
                            m_atLimit ? "   AT LIMIT" : "");
                    sf::Text t(monoFont(), buf, 12);
                    t.setLetterSpacing(1.3f);
                    t.setFillColor(m_atLimit ? sf::Color(255, 48, 0) : AMBER);
                    t.setPosition({ P(centre).x + 14.f, P(centre).y + 10.f });
                    m_window->draw(t);
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

            const char* hintText =
                (lv.decals.empty() && lv.plates.empty()) ? "ADD A FIGURE OR A PLATE FROM DETAILS ON THE RIGHT"
                : m_tool == Tool::Move ? "MOVE   DRAG A PART   SHIFT ONE AXIS   ARROWS NUDGE   CTRL+D COPY   RMB / DEL REMOVE"
                : m_tool == Tool::Turn ? "TURN   DRAG INSIDE THE RING   SHIFT 15 DEG   ARROWS 1 DEG"
                : m_tool == Tool::Size ? "SIZE   HANDLES PIN THE OPPOSITE SIDE   DRAG INSIDE = EVEN   SHIFT RATIO   ALT FROM CENTRE"
                : "SHAPE   DRAG POINTS   RMB ON AN EDGE ADDS   RMB ON A POINT REMOVES   ARROWS MOVE THE LAST POINT";
            sf::Text hint(monoFont(), hintText, 12);
            hint.setLetterSpacing(1.4f);
            hint.setFillColor(TEXT_DEAD);
            hint.setPosition({ 12.f, r.h - 24.f });
            m_window->draw(hint);
        }
        endClip();
    }

    /// The four corners of a turned box, ship space (for hover outlines).
    static std::vector<sf::Vector2f> boxCorners(sf::Vector2f c, float w, float h, float angleDeg) {
        const float a = angleDeg * 3.14159f / 180.f, ca = std::cos(a), sa = std::sin(a);
        const float hw = std::max(1.5f, w * 0.5f), hh = std::max(1.5f, h * 0.5f);
        std::vector<sf::Vector2f> out;
        for (const sf::Vector2f q : { sf::Vector2f{ -hw, -hh }, sf::Vector2f{ hw, -hh },
            sf::Vector2f{ hw, hh }, sf::Vector2f{ -hw, hh } })
            out.push_back({ c.x + q.x * ca - q.y * sa, c.y + q.x * sa + q.y * ca });
        return out;
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
            // On a parry the hull flashes toward white as in the game, and
            // TONE-inked parts flash with it -- the point of TONE, visible.
            const bool parrying = (m_paintTarget == PaintTarget::Parry) && (m_fxBeat > 1.0f);
            const float flash = parrying ? std::clamp((m_fxBeat - 1.0f) / 0.3f, 0.f, 1.f) : 0.f;
            const sf::Color hullC = lv.paint.hull;
            const sf::Color liveFill(
                static_cast<std::uint8_t>(hullC.r + (255.f - hullC.r) * flash),
                static_cast<std::uint8_t>(hullC.g + (255.f - hullC.g) * flash),
                static_cast<std::uint8_t>(hullC.b + (255.f - hullC.b) * flash),
                static_cast<std::uint8_t>(hullC.a + (255.f - hullC.a) * flash));
            const sf::Color liveOutline = parrying ? lv.paint.parry : lv.paint.outline;
            const auto pass = [&](bool over) {
                std::vector<sf::Vertex> v;
                ship::liveryPass(lv, { liveFill, liveOutline }, over, v);
                for (auto& q : v) q.position = S(q.position);
                if (!v.empty()) m_window->draw(v.data(), v.size(), sf::PrimitiveType::Triangles);
                };
            pass(false);
            fillPolygon(m_design->renderOutline(), S, liveFill);
            strokePolygon(m_design->renderOutline(), S, liveOutline, parrying ? 4.5f : 2.5f);
            pass(true);

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

    // ---- PAINT-mode layout ---------------------------------------------------
    // PAINT needs more room for DETAILS than the hull readout does, so the
    // right column is re-split here instead of reusing the HULL/MODEL rects.
    Rect paintListRect() const { return frac(0.632f, 0.112f, 0.353f, 0.330f); }
    Rect detailRect()    const { return frac(0.632f, 0.470f, 0.353f, 0.312f); }
    Rect fileRect()      const { return frac(0.632f, 0.810f, 0.353f, 0.070f); }

    /// Targets whose alpha the game actually uses.
    bool alphaLive() const {
        return m_paintTarget == PaintTarget::Hull || m_paintTarget == PaintTarget::Part
            || m_paintTarget == PaintTarget::Plasma || m_paintTarget == PaintTarget::Parry
            || m_paintTarget == PaintTarget::Homing;
    }

    bool partSelected() { return selected() != nullptr || selectedPlate() != nullptr; }

    // ---- PAINT panel: targets and a full colour picker --------------------
    void drawPalettePanel() {
        const Rect r = paintListRect();
        if (!panelChrome(r, "PAINT", 0.10f, VIOLET, false) || !m_livery) return;

        // Two columns: what the SHIP is painted, and what its EFFECTS look like.
        struct Row { const char* label; PaintTarget t; };
        static const Row shipRows[] = {
            { "HULL", PaintTarget::Hull }, { "OUTLINE", PaintTarget::Outline },
            { "CANOPY", PaintTarget::Cockpit }, { "PART", PaintTarget::Part } };
        static const Row fxRows[] = {
            { "PLASMA", PaintTarget::Plasma }, { "THRUST", PaintTarget::Thrust },
            { "TURBO", PaintTarget::Turbo },   { "DODGE", PaintTarget::Dodge },
            { "PARRY", PaintTarget::Parry },   { "HOMING", PaintTarget::Homing } };

        const float pad = 12.f, colGap = 14.f;
        const float colW = (r.w - pad * 2.f - colGap) * 0.5f;
        const float colX[2] = { r.x + pad, r.x + pad + colW + colGap };
        float y = r.y + 10.f;

        const char* heads[2] = { "SHIP", "EFFECTS - LIVE PREVIEW" };
        for (int c = 0; c < 2; ++c) {
            sf::Text ht(monoFont(), heads[c], 10);
            ht.setLetterSpacing(1.6f);
            ht.setFillColor(sf::Color(130, 95, 190));
            ht.setPosition({ colX[c] + 6.f, y + 2.f });
            m_window->draw(ht);
            hline(colX[c] + 12.f + ht.getGlobalBounds().size.x, y + 9.f,
                colW - 18.f - ht.getGlobalBounds().size.x, sf::Color(60, 44, 90));
        }
        y += 17.f;

        const auto drawRow = [&](const Row& row, float x, float ry) {
            const Rect line{ x, ry, colW, 19.f };
            const bool active = (m_paintTarget == row.t);
            const bool usable = (row.t != PaintTarget::Part) || partSelected();
            if (consumeClick(line) && usable) {
                m_paintTarget = row.t;
                syncPickerFrom(targetColor(row.t));   // the picker opens on the colour in use
                if (row.t == PaintTarget::Cockpit && m_livery->cockpit.style != ship::CockpitStyle::None)
                    select({ SelKind::Cockpit, -1, false });
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
            t.setPosition({ line.x + 6.f, line.y + 2.f });
            m_window->draw(t);

            if (row.t == PaintTarget::Part) {
                // Say what PART is right now, so the row never looks broken.
                const char* note = !usable ? "SELECT ONE"
                    : (selected() ? (selected()->tonal ? "FIGURE  TONE" : "FIGURE")
                        : (selectedPlate()->tonal ? "PLATE  TONE" : "PLATE"));
                sf::Text e(monoFont(), note, 10);
                e.setLetterSpacing(1.2f);
                e.setFillColor(TEXT_DEAD);
                e.setPosition({ line.x + 56.f, line.y + 4.f });
                m_window->draw(e);
            }

            sf::RectangleShape sw({ 34.f, 12.f });
            sw.setPosition({ line.x + line.w - 40.f, line.y + 3.f });
            sw.setFillColor(usable ? targetColor(row.t) : sf::Color(30, 34, 40));
            sw.setOutlineThickness(1.f);
            sw.setOutlineColor(active ? AMBER : sf::Color(60, 70, 84));
            m_window->draw(sw);
            };
        for (int i = 0; i < 4; ++i) drawRow(shipRows[i], colX[0], y + static_cast<float>(i) * 21.f);
        for (int i = 0; i < 6; ++i) drawRow(fxRows[i], colX[1], y + static_cast<float>(i) * 21.f);
        y += 6.f * 21.f + 8.f;

        // ================= COLOUR PICKER =================
        // Saturation across, value down, over the current hue.
        //
        // Two layers, each linear in ONE axis, so the triangle split cannot
        // show: white -> hue across, then clear -> black down. Alpha blending
        // gives v * lerp(white, hue, s), which is exactly HSV. A single
        // 4-colour quad is NOT bilinear -- each triangle blends only its own
        // three corners, which is where the old grey diagonal smear came from.
        //
        // The square takes whatever height is left, so the panel fits small windows.
        const bool showAlpha = alphaLive();
        const float below = 8.f + 14.f + 8.f + (showAlpha ? 20.f : 0.f) + 18.f + 14.f + 10.f;
        const float svH = std::clamp(r.y + r.h - y - below, 40.f, 110.f);
        m_svRect = { r.x + pad, y, r.w - pad * 2.f, svH };
        {
            const sf::Color pure = ship::fromHSV(m_pickH, 1.f, 1.f);
            const sf::Vector2f a{ m_svRect.x, m_svRect.y }, b{ m_svRect.x + m_svRect.w, m_svRect.y };
            const sf::Vector2f c{ m_svRect.x + m_svRect.w, m_svRect.y + m_svRect.h }, d{ m_svRect.x, m_svRect.y + m_svRect.h };
            const sf::Color white(255, 255, 255), clear(0, 0, 0, 0), black(0, 0, 0, 255);

            sf::VertexArray sat(sf::PrimitiveType::Triangles, 6);
            sat[0] = { a, white }; sat[1] = { b, pure }; sat[2] = { c, pure };
            sat[3] = { a, white }; sat[4] = { c, pure }; sat[5] = { d, white };
            m_window->draw(sat);

            sf::VertexArray val(sf::PrimitiveType::Triangles, 6);
            val[0] = { a, clear }; val[1] = { b, clear }; val[2] = { c, black };
            val[3] = { a, clear }; val[4] = { c, black }; val[5] = { d, black };
            m_window->draw(val);
        }

        sf::CircleShape svKnob(5.f);
        svKnob.setOrigin({ 5.f, 5.f });
        svKnob.setPosition({ m_svRect.x + m_pickS * m_svRect.w, m_svRect.y + (1.f - m_pickV) * m_svRect.h });
        svKnob.setFillColor(sf::Color::Transparent);
        svKnob.setOutlineThickness(2.f);
        svKnob.setOutlineColor(m_pickV > 0.55f ? sf::Color(10, 12, 16) : sf::Color::White);
        m_window->draw(svKnob);

        // Hue strip: six interpolated segments across the spectrum.
        y += m_svRect.h + 8.f;
        m_hueRect = { r.x + pad, y, r.w - pad * 2.f, 14.f };
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
        y += m_hueRect.h + 8.f;

        // Alpha: only where the game honours it. Thrust, turbo, dodge and the
        // canopy pin their own alpha, and the outline stays solid so the
        // silhouette always reads. The hull may go see-through, never below
        // HULL_MIN_ALPHA. A hidden strip parks its rect off-screen.
        if (showAlpha) {
            m_alphaRect = { r.x + pad, y, r.w - pad * 2.f, 12.f };
            const sf::Color solid = ship::fromHSV(m_pickH, m_pickS, m_pickV);
            sf::VertexArray al(sf::PrimitiveType::Triangles, 6);
            const sf::Color a0(solid.r, solid.g, solid.b, 0), a1(solid.r, solid.g, solid.b, 255);
            const sf::Vector2f p0{ m_alphaRect.x, m_alphaRect.y }, p1{ m_alphaRect.x + m_alphaRect.w, m_alphaRect.y };
            const sf::Vector2f p2{ m_alphaRect.x + m_alphaRect.w, m_alphaRect.y + m_alphaRect.h }, p3{ m_alphaRect.x, m_alphaRect.y + m_alphaRect.h };
            al[0] = { p0, a0 }; al[1] = { p1, a1 }; al[2] = { p2, a1 };
            al[3] = { p0, a0 }; al[4] = { p2, a1 }; al[5] = { p3, a0 };
            m_window->draw(al);
            if (m_paintTarget == PaintTarget::Hull) {
                // Grey out the part of the strip the hull may not use.
                const float fx = m_alphaRect.w * (static_cast<float>(ship::HULL_MIN_ALPHA) / 255.f);
                sf::RectangleShape no({ fx, m_alphaRect.h });
                no.setPosition({ m_alphaRect.x, m_alphaRect.y });
                no.setFillColor(sf::Color(8, 10, 14, 200));
                m_window->draw(no);
            }
            vline(m_alphaRect.x + m_alphaRect.w * m_pickA, m_alphaRect.y - 3.f, m_alphaRect.h + 6.f, sf::Color::White);
            y += m_alphaRect.h + 8.f;
        }
        else {
            m_alphaRect = { -1e6f, -1e6f, 0.f, 0.f };
        }

        // Readout + quick swatches.
        const sf::Color cur = targetColor(m_paintTarget);
        char hex[96];
        std::snprintf(hex, sizeof(hex), "%s   R%3d G%3d B%3d A%3d%s",
            ship::colorToHex(cur).c_str(), cur.r, cur.g, cur.b, cur.a,
            (m_paintTarget == PaintTarget::Hull && cur.a < 255) ? "   SEE-THROUGH" : "");
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
            if (consumeClick(cell)) {
                // Keep the current alpha: a swatch picks a colour, not opacity.
                sf::Color c = pal[i];
                c.a = cur.a;
                applyPaletteColor(c);
                syncPickerFrom(targetColor(m_paintTarget));
            }
            sf::RectangleShape q({ cell.w, cell.h });
            q.setPosition({ cell.x, cell.y });
            q.setFillColor(pal[i]);
            m_window->draw(q);
        }
    }

    // ---- DETAILS panel: tools, parts, ink, canopy -------------------------
    //
    // Everything the canvas gizmo cannot do: which tool is active, which part
    // to add, which side of the hull it sits on, its mirror, its ink (TONE or
    // COLOUR) and tone, and the canopy style. Each control is a picture of
    // what it makes or names its state plainly.
    void drawDetailPanel() {
        const Rect r = detailRect();
        if (!panelChrome(r, "DETAILS", 0.18f, VIOLET, false) || !m_livery) return;

        const float x0 = r.x + 12.f, innerW = r.w - 24.f, gap = 6.f;
        const auto drawTris = [&](const std::vector<sf::Vector2f>& t, sf::Vector2f c, float k, sf::Color col) {
            sf::VertexArray va(sf::PrimitiveType::Triangles, t.size());
            for (std::size_t i = 0; i < t.size(); ++i) va[i] = { { c.x + t[i].x * k, c.y + t[i].y * k }, col };
            m_window->draw(va);
            };
        const auto tinyLabel = [&](const char* s, float cx, float y, sf::Color col) {
            sf::Text t(monoFont(), s, 10);
            t.setLetterSpacing(1.3f);
            t.setFillColor(col);
            t.setPosition({ cx - t.getGlobalBounds().size.x * 0.5f, y });
            m_window->draw(t);
            };
        const auto rowRects = [&](int n, float y, float h, Rect* out) {
            const float bw = (innerW - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
            for (int i = 0; i < n; ++i) out[i] = { x0 + static_cast<float>(i) * (bw + gap), y, bw, h };
            };
        std::string hoverInfo;   // what the button under the mouse would do
        float y = r.y + 12.f;

        // ---- TOOLS ----
        {
            Rect b[4];
            rowRects(4, y, 24.f, b);
            static const char* names[4] = { "MOVE  W", "TURN  E", "SIZE  R", "SHAPE  T" };
            static const char* info[4] = { "MOVE - DRAG A PART", "TURN - DRAG INSIDE THE RING",
                "SIZE - HANDLES, OR DRAG INSIDE TO SCALE EVENLY", "SHAPE - EDIT A PLATE'S POINTS" };
            for (int i = 0; i < 4; ++i) {
                const Tool t = static_cast<Tool>(i);
                if (consumeClick(b[i])) setTool(t);
                drawMiniButton(b[i], names[i], m_tool == t);
                if (b[i].contains(m_mouse)) hoverInfo = info[i];
            }
            y += 24.f + 8.f;
        }

        // ---- FIGURES: one button per shape, drawn as that shape ----
        {
            static const ship::DecalKind kinds[ship::DECAL_KIND_COUNT] = {
                ship::DecalKind::Line, ship::DecalKind::Bar, ship::DecalKind::Oval,
                ship::DecalKind::Tri,  ship::DecalKind::Ring };
            Rect b[ship::DECAL_KIND_COUNT];
            rowRects(ship::DECAL_KIND_COUNT, y, 30.f, b);
            for (int i = 0; i < ship::DECAL_KIND_COUNT; ++i) {
                if (consumeClick(b[i])) addDecal(kinds[i]);
                drawMiniButton(b[i], "", false);
                ship::Decal icon;
                icon.kind = kinds[i];
                icon.pos = { 0.f, 0.f };
                switch (kinds[i]) {
                case ship::DecalKind::Line: icon.w = 18.f; icon.h = 3.f;  icon.thickness = 3.f; break;
                case ship::DecalKind::Bar:  icon.w = 16.f; icon.h = 7.f;  break;
                case ship::DecalKind::Oval: icon.w = 16.f; icon.h = 9.f;  break;
                case ship::DecalKind::Tri:  icon.w = 11.f; icon.h = 11.f; break;
                default:                    icon.w = 12.f; icon.h = 12.f; icon.thickness = 2.f; break;
                }
                std::vector<sf::Vector2f> t;
                ship::decalGeometry(icon, t, false);
                const bool hot = b[i].contains(m_mouse);
                drawTris(t, { b[i].cx(), b[i].y + 11.f }, 1.f, hot ? CYAN : TEXT_DIM);
                tinyLabel(ship::decalKindName(kinds[i]), b[i].cx(), b[i].y + b[i].h - 12.f, hot ? CYAN : TEXT_DEAD);
                if (hot) hoverInfo = std::string("ADD FIGURE: ") + ship::decalKindName(kinds[i]);
            }
            y += 30.f + 6.f;
        }

        // ---- PLATES: stamps, drawn solid in the hull's tone ----
        {
            Rect b[ship::PLATE_STAMP_COUNT];
            rowRects(ship::PLATE_STAMP_COUNT, y, 32.f, b);
            for (int i = 0; i < ship::PLATE_STAMP_COUNT; ++i) {
                const auto kind = static_cast<ship::PlateStamp>(i);
                if (consumeClick(b[i])) addPlate(kind);
                drawMiniButton(b[i], "", false);
                const Plate p = ship::plateStamp(kind, { 0.f, 0.f });
                const float k = std::min((b[i].w - 12.f) / p.w, 18.f / p.h);
                const bool hot = b[i].contains(m_mouse);
                const sf::Color fill = hot ? ship::tone(m_livery->paint.hull, 1.25f, 255)
                    : ship::tone(m_livery->paint.hull, 0.8f, 255);
                const std::vector<sf::Vector2f> poly = ship::plateWorld(p);
                drawTris(ship::detail::triangulate(poly), { b[i].cx(), b[i].y + 12.f }, k, fill);
                tinyLabel(ship::plateStampName(kind), b[i].cx(), b[i].y + b[i].h - 12.f, hot ? CYAN : TEXT_DEAD);
                if (hot) hoverInfo = std::string("ADD PLATE: ") + ship::plateStampName(kind) + " - RESHAPE IT WITH T";
            }
            y += 32.f + 8.f;
        }

        // ---- INFO: what is selected, or what the hovered button does ----
        ship::Decal* d = selected();
        Plate* pl = selectedPlate();
        const bool canopySel = (m_selKind == SelKind::Cockpit)
            && m_livery->cockpit.style != ship::CockpitStyle::None;
        const float infoY = y;
        y += 18.f;

        Rect row1[4], row2[4];
        rowRects(4, y, 22.f, row1);
        rowRects(4, y + 28.f, 22.f, row2);

        if (d || pl) {
            bool& over = d ? d->over : pl->over;
            bool& mir = d ? d->mirrored : pl->mirrored;
            const bool tonal = d ? d->tonal : pl->tonal;

            if (consumeClick(row1[0])) over = true;
            if (consumeClick(row1[1])) over = false;
            if (consumeClick(row1[2])) {
                mir = !mir;
                const bool fits = d ? ship::decalFits(*d, m_design->envelope()) : ship::plateFits(*pl, m_design->envelope());
                if (mir && !fits) { mir = false; reject("NO ROOM FOR THE MIRROR COPY"); }
            }
            drawMiniButton(row1[0], "OVER HULL", over);
            drawMiniButton(row1[1], "UNDER HULL", !over);
            drawMiniButton(row1[2], "MIRROR", mir);
            drawMiniButton(row1[3], "DELETE", false);
            if (row1[1].contains(m_mouse)) hoverInfo = "UNDER HULL - SHOWS THROUGH A SEE-THROUGH HULL";

            drawMiniButton(row2[0], "TONE", tonal);
            drawMiniButton(row2[1], "COLOUR", !tonal);
            if (row2[0].contains(m_mouse)) hoverInfo = "TONE - HULL COLOUR x SHADE: FLASHES AND HEATS WITH IT";
            if (row2[1].contains(m_mouse)) hoverInfo = "COLOUR - A FIXED COLOUR FROM THE PICKER";
            if (d) {
                drawMiniButton(row2[2], "DUPLICATE", false);
                drawMiniButton(row2[3], "TO PLATE", false);
                if (row2[2].contains(m_mouse)) hoverInfo = "DUPLICATE - ALSO CTRL+D";
                if (row2[3].contains(m_mouse)) hoverInfo = "TO PLATE - TURN THIS FIGURE INTO AN EDITABLE SHAPE";
            }
            else {
                drawMiniButton(row2[2], "ACCENT", pl->accent);
                drawMiniButton(row2[3], "DUPLICATE", false);
                if (row2[2].contains(m_mouse)) hoverInfo = "ACCENT - A THIN EDGE IN THE OUTLINE COLOUR";
                if (row2[3].contains(m_mouse)) hoverInfo = "DUPLICATE - ALSO CTRL+D";
            }

            // Clicks that change WHICH part is selected go last: they invalidate d / pl.
            if (consumeClick(row2[0]) && !tonal) setInk(true);
            else if (consumeClick(row2[1]) && tonal) setInk(false);
            else if (pl && consumeClick(row2[2])) pl->accent = !pl->accent;
            else if (consumeClick(d ? row2[2] : row2[3])) duplicateSelected();
            else if (d && consumeClick(row2[3])) convertToPlate();
            else if (consumeClick(row1[3])) deleteSelected();
        }
        else if (canopySel) {
            const Rect del{ row1[0].x, row1[0].y, row1[1].x + row1[1].w - row1[0].x, row1[0].h };
            drawMiniButton(del, "REMOVE CANOPY", false);
            if (consumeClick(del)) setCanopy(ship::CockpitStyle::None);
        }
        y += 28.f + 22.f + 8.f;

        // ---- SHADE: the tone of the selected part ----
        m_shadeRect = { -1e6f, -1e6f, 0.f, 0.f };
        d = selected();
        pl = selectedPlate();
        if (d || pl) {
            const bool tonal = d ? d->tonal : pl->tonal;
            const float shade = d ? d->shade : pl->shade;
            char lab[32];
            std::snprintf(lab, sizeof(lab), tonal ? "SHADE x%.2f" : "COLOUR INK", shade);
            sf::Text t(monoFont(), lab, 11);
            t.setLetterSpacing(1.3f);
            t.setFillColor(tonal ? TEXT_DIM : TEXT_DEAD);
            t.setPosition({ x0, y });
            m_window->draw(t);
            if (tonal) {
                m_shadeRect = { x0 + 104.f, y + 1.f, innerW - 104.f, 12.f };
                // The strip shows the actual tones on this hull, dark to bright.
                const int seg = 12;
                sf::VertexArray va(sf::PrimitiveType::Triangles, seg * 6);
                for (int i = 0; i < seg; ++i) {
                    const float t0 = static_cast<float>(i) / seg, t1 = static_cast<float>(i + 1) / seg;
                    const auto col = [&](float tt) {
                        return ship::tone(m_livery->paint.hull,
                            ship::SHADE_MIN + (ship::SHADE_MAX - ship::SHADE_MIN) * tt, 255);
                        };
                    const float xa = m_shadeRect.x + m_shadeRect.w * t0, xb = m_shadeRect.x + m_shadeRect.w * t1;
                    const float ya = m_shadeRect.y, yb = m_shadeRect.y + m_shadeRect.h;
                    va[i * 6 + 0] = { { xa, ya }, col(t0) }; va[i * 6 + 1] = { { xb, ya }, col(t1) };
                    va[i * 6 + 2] = { { xb, yb }, col(t1) }; va[i * 6 + 3] = { { xa, ya }, col(t0) };
                    va[i * 6 + 4] = { { xb, yb }, col(t1) }; va[i * 6 + 5] = { { xa, yb }, col(t0) };
                }
                m_window->draw(va);
                const float k = (shade - ship::SHADE_MIN) / (ship::SHADE_MAX - ship::SHADE_MIN);
                vline(m_shadeRect.x + m_shadeRect.w * k, m_shadeRect.y - 3.f, m_shadeRect.h + 6.f, sf::Color::White);
                if (m_shadeRect.contains(m_mouse)) hoverInfo = "SHADE - DARKER OR BRIGHTER THAN THE HULL";
            }
        }
        y += 22.f;

        // ---- CANOPY: every style drawn, the fitted one lit ----
        {
            Rect b[ship::COCKPIT_STYLE_COUNT];
            rowRects(ship::COCKPIT_STYLE_COUNT, y, 30.f, b);
            for (int i = 0; i < ship::COCKPIT_STYLE_COUNT; ++i) {
                const auto style = static_cast<ship::CockpitStyle>(i);
                const bool active = (m_livery->cockpit.style == style);
                if (consumeClick(b[i])) setCanopy(style);
                drawMiniButton(b[i], "", active);
                const bool hot = b[i].contains(m_mouse);
                if (hot) hoverInfo = style == ship::CockpitStyle::None ? "NO CANOPY"
                    : std::string("CANOPY: ") + ship::cockpitStyleName(style);
                if (style == ship::CockpitStyle::None) {
                    tinyLabel("NO CANOPY", b[i].cx(), b[i].cy() - 7.f, active ? CYAN : (hot ? TEXT : TEXT_DIM));
                    continue;
                }
                ship::Cockpit cp;
                cp.style = style;
                cp.pos = { 0.f, 0.f };
                cp.w = 13.f; cp.h = 17.f; cp.angle = 0.f;
                std::vector<sf::Vector2f> glass, rim;
                ship::cockpitGeometry(cp, glass, rim);
                const sf::Vector2f c{ b[i].cx(), b[i].cy() };
                drawTris(rim, c, 1.f, hot || active ? sf::Color(90, 104, 120) : sf::Color(60, 70, 84));
                const sf::Color g = m_livery->paint.cockpit;
                drawTris(glass, c, 1.f, active ? sf::Color(g.r, g.g, g.b) : sf::Color(g.r, g.g, g.b, hot ? 200 : 120));
            }
        }

        // ---- INFO line, drawn last so every hover above could feed it ----
        {
            d = selected();
            pl = selectedPlate();
            char info[128];
            sf::Color infoC = TEXT;
            if (!hoverInfo.empty()) {
                std::snprintf(info, sizeof(info), "%s", hoverInfo.c_str());
                infoC = CYAN;
            }
            else if (d) {
                const float a = std::fmod(std::fmod(d->angle, 360.f) + 360.f, 360.f);
                std::snprintf(info, sizeof(info), "%s FIGURE   %.1f x %.1f   %.0f DEG",
                    ship::decalKindName(d->kind), d->w, d->h, a);
            }
            else if (pl) {
                const float a = std::fmod(std::fmod(pl->angle, 360.f) + 360.f, 360.f);
                std::snprintf(info, sizeof(info), "PLATE   %d PTS   %.1f x %.1f   %.0f DEG",
                    static_cast<int>(pl->shape.size()), pl->w, pl->h, a);
            }
            else if (canopySel) {
                const auto& c = m_livery->cockpit;
                std::snprintf(info, sizeof(info), "CANOPY %s   %.1f x %.1f", ship::cockpitStyleName(c.style), c.w, c.h);
            }
            else {
                std::snprintf(info, sizeof(info), "CLICK A PART ON THE SHIP, OR ADD ONE ABOVE");
                infoC = TEXT_DEAD;
            }
            sf::Text t(monoFont(), info, 12);
            t.setLetterSpacing(1.3f);
            t.setFillColor(infoC);
            t.setPosition({ x0, infoY });
            m_window->draw(t);

            char count[48];
            std::snprintf(count, sizeof(count), "FIG %d/%d  PLT %d/%d",
                static_cast<int>(m_livery->decals.size()), ship::MAX_DECALS,
                static_cast<int>(m_livery->plates.size()), ship::MAX_PLATES);
            sf::Text ct(monoFont(), count, 10);
            ct.setLetterSpacing(1.2f);
            ct.setFillColor(TEXT_DEAD);
            ct.setPosition({ x0 + innerW - ct.getGlobalBounds().size.x, infoY + 2.f });
            if (hoverInfo.empty() || t.getGlobalBounds().size.x < innerW - ct.getGlobalBounds().size.x - 12.f)
                m_window->draw(ct);
        }
    }

    // ---- SHIP FILE panel --------------------------------------------------
    void drawFilePanel() {
        const Rect r = fileRect();
        if (!panelChrome(r, "SHIP FILE", 0.26f, CYAN_MID, false)) return;

        const Rect ex{ r.x + 12.f, r.y + 10.f, (r.w - 34.f) * 0.5f, 26.f };
        const Rect im{ ex.x + ex.w + 10.f, ex.y, ex.w, 26.f };
        if (consumeClick(ex)) exportShip();
        if (consumeClick(im)) openHangar();
        drawMiniButton(ex, "QUICK EXPORT", false);
        drawMiniButton(im, "HANGAR  H", false);

        sf::Text t(monoFont(), m_fileMsg.empty()
            ? "SHIPS LIVE IN ships/ - SHARE THE FILE, ANY COPY OF THE GAME CAN FLY IT" : m_fileMsg, 12);
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
    //
    // With a preview active every row reads FROM -> TO, the bar shows the gain
    // (bright green) or the loss (red) as a segment, and class-tier lines that
    // would change light up. Without one it is the plain readout it was.
    void drawStatsPanel() {
        if (m_mode == Mode::Paint) { drawPalettePanel(); return; }
        const Rect r = statsRect();

        std::string title = "PERFORMANCE";
        if (m_pvActive) title += "  -  " + m_pvLabel;
        else if (!m_pvRefused.empty()) title += "  -  " + m_pvLabel + " REFUSED";
        if (!panelChrome(r, title, 0.10f, m_pvActive ? CYAN : CYAN_MID, false)) return;

        const auto& ref = ship::ShipDesign::reference();
        const StatRow now = rowOf(*m_design);
        const StatRow& from = m_pvActive ? m_pvFrom : now;
        const StatRow& to = m_pvActive ? m_pvTo : now;

        beginClip(r);
        {
            float y = 14.f;
            statBar(y, r.w, "HULL", from.hp, to.hp, ref.hpMax, "%.0f");                      y += 33.f;
            statBar(y, r.w, "ENERGY", from.energy, to.energy, ref.energyMax, "%.0f");        y += 33.f;
            statBar(y, r.w, "REGEN", from.regen, to.regen, 1.f, "x%.2f");                    y += 33.f;
            statBar(y, r.w, "THRUST", from.thrust, to.thrust, 1.f, "x%.2f", to.thrustMin);   y += 33.f;
            statBar(y, r.w, "AGILITY", from.agility, to.agility, 1.f, "x%.2f");              y += 40.f;

            hline(14.f, y - 10.f, r.w - 28.f, CYAN_LOW);

            // Class tier. Labels and values in two columns so no line needs
            // an abbreviation to fit the panel. A line the preview changes is lit.
            static const char* labels[7] = { "STRAFE", "DODGE", "RECOVER", "HEAT", "WINDOWS", "POISE", "ARMOUR" };
            for (int i = 0; i < 7; ++i) {
                const float ly = y + static_cast<float>(i) * 16.f;
                const bool changed = m_pvActive && from.feel[i] != to.feel[i];
                sf::Text l(monoFont(), labels[i], 12);
                l.setLetterSpacing(1.4f);
                l.setFillColor(changed ? CYAN : TEXT_DIM);
                l.setPosition({ 14.f, ly });
                m_window->draw(l);

                sf::Text t(monoFont(), to.feel[i], 12);
                t.setLetterSpacing(1.2f);
                t.setFillColor(changed ? CYAN : TEXT);
                t.setPosition({ 104.f, ly });
                m_window->draw(t);
                if (changed) {
                    sf::Text w(monoFont(), "WAS  " + from.feel[i], 10);
                    w.setLetterSpacing(1.1f);
                    w.setFillColor(TEXT_DEAD);
                    w.setPosition({ 112.f + t.getGlobalBounds().size.x, ly + 2.f });
                    m_window->draw(w);
                }
            }
        }
        endClip();
    }

    /**
     * @brief One stat row: label, value(s), bar, a tick at the reference build,
     *        and a red tick at the class minimum where there is one.
     *
     * Higher is better for every row shown, so a gain is green and a loss red.
     * `from == to` draws the plain bar.
     */
    void statBar(float y, float panelW, const char* label,
        float from, float to, float refValue, const char* fmt, float minValue = -1.f) {
        const bool delta = std::fabs(to - from) > 1e-4f * std::max(1.f, std::fabs(from));
        const bool belowMin = minValue > 0.f && to < minValue - 1e-4f;
        const bool better = (to >= refValue * 0.995f);
        const sf::Color base = belowMin ? RED : (better ? GREEN : AMBER);

        sf::Text l(monoFont(), belowMin ? std::string(label) + "  BELOW CLASS MINIMUM" : std::string(label), 14);
        l.setLetterSpacing(1.7f);
        l.setFillColor(belowMin ? RED : TEXT_DIM);
        l.setPosition({ 14.f, y });
        m_window->draw(l);

        // Value: "to", or "from -> to" with the arrow coloured by direction.
        char a[32], b[32];
        std::snprintf(a, sizeof(a), fmt, from);
        std::snprintf(b, sizeof(b), fmt, to);
        sf::Text v(monoFont(), b, 17);
        v.setLetterSpacing(1.3f);
        v.setFillColor(delta ? (to > from ? GREEN : RED) : base);
        const float vx = panelW - v.getGlobalBounds().size.x - 16.f;
        v.setPosition({ vx, y - 3.f });
        m_window->draw(v);
        if (delta) {
            sf::Text w(monoFont(), std::string(a) + "  ->", 13);
            w.setLetterSpacing(1.2f);
            w.setFillColor(TEXT_DIM);
            w.setPosition({ vx - w.getGlobalBounds().size.x - 8.f, y });
            m_window->draw(w);
        }

        const float bx = 14.f, by = y + 20.f, bw = panelW - 28.f, bh = 6.f;
        sf::RectangleShape track({ bw, bh });
        track.setPosition({ bx, by });
        track.setFillColor(sf::Color(16, 22, 30));
        m_window->draw(track);

        const float full = std::max(0.0001f, refValue / 0.45f);
        const auto X = [&](float v2) { return bw * std::clamp(v2 / full, 0.f, 1.f); };
        const float lo = std::min(from, to), hi = std::max(from, to);

        sf::RectangleShape fill({ X(delta ? lo : to), bh });
        fill.setPosition({ bx, by });
        fill.setFillColor(base);
        m_window->draw(fill);

        if (delta) {
            // The change itself, a little taller than the bar so it reads at a glance.
            const bool gain = to > from;
            sf::RectangleShape seg({ std::max(2.f, X(hi) - X(lo)), bh + 4.f });
            seg.setPosition({ bx + X(lo), by - 2.f });
            seg.setFillColor(gain ? sf::Color(170, 255, 200) : sf::Color(255, 48, 0, 220));
            m_window->draw(seg);
        }

        vline(bx + bw * 0.45f, by - 3.f, bh + 6.f, sf::Color(214, 222, 232, 170));
        if (minValue > 0.f) {
            const float mx = bx + X(minValue);
            vline(mx, by - 4.f, bh + 8.f, RED);
            vline(mx + 1.f, by - 4.f, bh + 8.f, RED);
        }
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

            // Preview: where the reactor would go. Free units feed regen, so
            // spending one is a cost even when nothing turns red.
            if (m_pvActive && (m_pvFrom.reactorUsed != m_pvTo.reactorUsed || m_pvFrom.reactorUnits != m_pvTo.reactorUnits)) {
                char pb[48];
                std::snprintf(pb, sizeof(pb), "->  %d / %d", m_pvTo.reactorUsed, m_pvTo.reactorUnits);
                sf::Text pt(monoFont(), pb, 14);
                pt.setLetterSpacing(1.6f);
                pt.setFillColor(m_pvTo.reactorUsed > m_pvTo.reactorUnits ? RED
                    : (m_pvTo.reactorUnits - m_pvTo.reactorUsed < m_pvFrom.reactorUnits - m_pvFrom.reactorUsed ? AMBER : GREEN));
                pt.setPosition({ 24.f + rt.getGlobalBounds().size.x, 10.f });
                m_window->draw(pt);
            }

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

            // ---- Counts against class caps (with the preview's value after an arrow) ----
            const bool pv = m_pvActive;
            const auto arrowI = [&](int a, int b2, int cap) {
                char t[32];
                if (pv && a != b2) std::snprintf(t, sizeof(t), "%d>%d/%d", a, b2, cap);
                else std::snprintf(t, sizeof(t), "%d/%d", a, cap);
                return std::string(t);
                };
            std::string counts = "WEAPONS " + arrowI(s.gunCount, m_pvTo.guns, pv ? m_pvTo.maxGuns : spec.maxGuns)
                + "   DRIVES " + arrowI(s.engineCount, m_pvTo.engines, pv ? m_pvTo.maxEngines : spec.maxEngines);
            {
                char t[48];
                if (pv && std::fabs(m_pvTo.tonnage - s.areaPx2) > 0.5f)
                    std::snprintf(t, sizeof(t), "   TONNAGE %.0f>%.0f/%.0f", s.areaPx2, m_pvTo.tonnage, m_pvTo.maxTonnage);
                else
                    std::snprintf(t, sizeof(t), "   TONNAGE %.0f/%.0f", s.areaPx2, spec.maxAreaPx2);
                counts += t;
            }
            sf::Text ct(monoFont(), counts, 13);
            ct.setLetterSpacing(1.4f);
            ct.setFillColor(pv && counts.find('>') != std::string::npos ? CYAN : TEXT);
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

        // What the hovered change would do to airworthiness, or why it is refused.
        std::string verdict;
        sf::Color verdictC = TEXT;
        if (!m_pvRefused.empty()) { verdict = m_pvLabel + " REFUSED - " + m_pvRefused; verdictC = RED; }
        else if (m_pvActive) {
            const char* when = m_pvDrag ? "NOW" : "WOULD BE";
            if (m_pvTo.ok && !m_pvFrom.ok) { verdict = std::string(when) + " AIRWORTHY";                  verdictC = GREEN; }
            else if (!m_pvTo.ok && m_pvFrom.ok) { verdict = std::string(when) + " REJECTED - " + m_pvTo.message; verdictC = RED; }
            else if (!m_pvTo.ok) { verdict = "STILL REJECTED - " + m_pvTo.message;                verdictC = AMBER; }
        }

        beginClip(r);
        {
            sf::Text t(monoFont(), v.message, 15);
            t.setLetterSpacing(1.5f);
            t.setFillColor(v.ok ? GREEN : RED);
            t.setPosition({ 14.f, 12.f });
            m_window->draw(t);

            if (!verdict.empty()) {
                sf::Text pv(monoFont(), verdict, 13);
                pv.setLetterSpacing(1.4f);
                pv.setFillColor(verdictC);
                pv.setPosition({ 14.f, m_mode == Mode::Model ? 74.f : 54.f });
                m_window->draw(pv);
            }

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

    // ---- HANGAR overlay ----------------------------------------------------
    void drawHangar() {
        if (!m_hangarOpen) return;
        const Rect R = hangarRect();

        sf::RectangleShape veil({ R.w, R.h });
        veil.setPosition({ R.x, R.y });
        veil.setFillColor(sf::Color(4, 6, 10, 245));
        veil.setOutlineThickness(2.f);
        veil.setOutlineColor(CYAN_MID);
        m_window->draw(veil);

        // ---- Title ----
        sf::Text title(*m_font, "HANGAR", 26);
        title.setLetterSpacing(1.8f);
        title.setFillColor(CYAN);
        title.setPosition({ R.x + 18.f, R.y + 12.f });
        m_window->draw(title);
        char sub[64];
        std::snprintf(sub, sizeof(sub), "ships/   %d SAVED", static_cast<int>(m_hangar.size()));
        sf::Text st(monoFont(), sub, 12);
        st.setLetterSpacing(1.5f);
        st.setFillColor(TEXT_DIM);
        st.setPosition({ R.x + 30.f + title.getGlobalBounds().size.x, R.y + 24.f });
        m_window->draw(st);

        const Rect close{ R.x + R.w - 44.f, R.y + 12.f, 30.f, 30.f };
        if (consumeClick(close)) { closeHangar(); return; }
        drawMiniButton(close, "X", false);

        // ---- List ----
        const float rowH = 58.f;
        const Rect list{ R.x + 16.f, R.y + 56.f, R.w - 32.f, R.h - 56.f - 112.f };
        m_hangarVisible = std::max(1, static_cast<int>(list.h / rowH));
        const int n = static_cast<int>(m_hangar.size());
        if (n == 0) {
            sf::Text t(monoFont(), "NO SHIPS YET - TYPE A NAME BELOW AND PRESS SAVE", 14);
            t.setLetterSpacing(1.5f);
            t.setFillColor(TEXT_DEAD);
            t.setPosition({ list.x + 12.f, list.y + 12.f });
            m_window->draw(t);
        }
        for (int k = 0; k < m_hangarVisible && m_hangarScroll + k < n; ++k) {
            const int i = m_hangarScroll + k;
            const HangarEntry& e = m_hangar[i];
            const Rect row{ list.x, list.y + static_cast<float>(k) * rowH, list.w, rowH - 4.f };

            if (consumeClick(row)) {
                // Second click on the same row soon after = load it.
                if (m_hangarSel == i && m_time - m_lastRowClick < 0.4f) { hangarLoad(i); return; }
                m_hangarSel = i;
                m_lastRowClick = m_time;
                m_confirm = Confirm::None;
            }
            const bool sel = (m_hangarSel == i), hot = row.contains(m_mouse);
            sf::RectangleShape bg({ row.w, row.h });
            bg.setPosition({ row.x, row.y });
            bg.setFillColor(sel ? sf::Color(30, 42, 56) : (hot ? sf::Color(18, 24, 32) : sf::Color(10, 13, 18)));
            bg.setOutlineThickness(1.f);
            bg.setOutlineColor(sel ? AMBER : sf::Color(30, 38, 48));
            m_window->draw(bg);

            // Thumbnail: the ship as it flies, through the same mesh the game uses.
            const Rect th{ row.x + 6.f, row.y + 4.f, row.h - 8.f, row.h - 8.f };
            sf::RectangleShape tb({ th.w, th.h });
            tb.setPosition({ th.x, th.y });
            tb.setFillColor(sf::Color(6, 8, 12));
            m_window->draw(tb);
            if (e.ok) {
                const auto& outline = e.design.renderOutline();
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& p : outline) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
                const float k2 = (th.w - 6.f) / std::max(1.f, std::max(x1 - x0, y1 - y0));
                const sf::Vector2f c{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f };
                const auto T = [&](sf::Vector2f p) { return sf::Vector2f{ th.cx() + (p.x - c.x) * k2, th.cy() + (p.y - c.y) * k2 }; };
                const ship::LiveInk ink{ e.livery.paint.hull, e.livery.paint.outline };
                std::vector<sf::Vertex> v;
                ship::liveryPass(e.livery, ink, false, v);
                for (auto& q : v) q.position = T(q.position);
                if (!v.empty()) m_window->draw(v.data(), v.size(), sf::PrimitiveType::Triangles);
                fillPolygon(outline, T, e.livery.paint.hull);
                strokePolygon(outline, T, e.livery.paint.outline, 1.f);
                v.clear();
                ship::liveryPass(e.livery, ink, true, v);
                for (auto& q : v) q.position = T(q.position);
                if (!v.empty()) m_window->draw(v.data(), v.size(), sf::PrimitiveType::Triangles);
            }
            else cross({ th.cx(), th.cy() }, 8.f, RED);

            sf::Text nm(monoFont(), e.name, 15);
            nm.setLetterSpacing(1.5f);
            nm.setFillColor(e.ok ? (sel ? TEXT : TEXT_DIM) : RED);
            nm.setPosition({ th.x + th.w + 12.f, row.y + 8.f });
            m_window->draw(nm);

            char info[160];
            if (e.ok) {
                const auto& s = e.design.stats();
                std::snprintf(info, sizeof(info), "%s   %d GUN%s  %d DRIVE%s   %d FIGURES  %d PLATES%s%s",
                    e.design.spec().name, s.gunCount, s.gunCount == 1 ? "" : "S",
                    s.engineCount, s.engineCount == 1 ? "" : "S",
                    static_cast<int>(e.livery.decals.size()), static_cast<int>(e.livery.plates.size()),
                    e.err.empty() ? "" : "   ! ", e.err.c_str());
            }
            else std::snprintf(info, sizeof(info), "%s", e.err.c_str());
            sf::Text it(monoFont(), info, 11);
            it.setLetterSpacing(1.3f);
            it.setFillColor(e.ok ? (e.err.empty() ? TEXT_DEAD : AMBER) : RED);
            it.setPosition({ th.x + th.w + 12.f, row.y + 30.f });
            m_window->draw(it);

            if (!m_shipName.empty() && e.name == m_shipName) {
                sf::Text cur(monoFont(), "EDITING", 11);
                cur.setLetterSpacing(1.5f);
                cur.setFillColor(CYAN);
                cur.setPosition({ row.x + row.w - cur.getGlobalBounds().size.x - 12.f, row.y + 10.f });
                m_window->draw(cur);
            }
        }
        if (n > m_hangarVisible) {
            // Scroll position, so a long list does not hide that it continues.
            const float h = list.h * static_cast<float>(m_hangarVisible) / static_cast<float>(n);
            const float y = list.y + (list.h - h) * static_cast<float>(m_hangarScroll)
                / static_cast<float>(std::max(1, n - m_hangarVisible));
            sf::RectangleShape bar({ 3.f, h });
            bar.setPosition({ list.x + list.w + 6.f, y });
            bar.setFillColor(CYAN_MID);
            m_window->draw(bar);
        }

        // ---- Bottom bar: message, name, actions ----
        const float by = R.y + R.h - 104.f;
        if (!m_hangarMsg.empty()) {
            sf::Text mt(monoFont(), m_hangarMsg, 12);
            mt.setLetterSpacing(1.4f);
            mt.setFillColor(m_hangarMsgBad ? AMBER : GREEN);
            mt.setPosition({ R.x + 18.f, by });
            m_window->draw(mt);
        }

        const float fy = by + 24.f, fh = 32.f;
        sf::Text nl(monoFont(), "NAME", 12);
        nl.setLetterSpacing(1.6f);
        nl.setFillColor(TEXT_DIM);
        nl.setPosition({ R.x + 18.f, fy + 9.f });
        m_window->draw(nl);
        const Rect field{ R.x + 70.f, fy, R.w * 0.36f, fh };
        sf::RectangleShape fb({ field.w, field.h });
        fb.setPosition({ field.x, field.y });
        fb.setFillColor(sf::Color(8, 12, 18));
        fb.setOutlineThickness(1.f);
        fb.setOutlineColor(CYAN_MID);
        m_window->draw(fb);
        const bool caretOn = std::fmod(m_time, 1.f) < 0.55f;
        sf::Text ft(monoFont(), m_nameBuf + (caretOn ? "_" : " "), 15);
        ft.setLetterSpacing(1.6f);
        ft.setFillColor(TEXT);
        ft.setPosition({ field.x + 10.f, field.y + 6.f });
        m_window->draw(ft);

        const float bx0 = field.x + field.w + 12.f;
        const float bw = (R.x + R.w - 18.f - bx0 - 3.f * 8.f) / 4.f;
        const Rect save{ bx0, fy, bw, fh }, load{ bx0 + (bw + 8.f), fy, bw, fh };
        const Rect over{ bx0 + (bw + 8.f) * 2.f, fy, bw, fh }, del{ bx0 + (bw + 8.f) * 3.f, fy, bw, fh };
        const bool haveSel = m_hangarSel >= 0 && m_hangarSel < n;
        const bool armedNow = m_confirm != Confirm::None && m_time < m_confirmUntil;

        drawMiniButton(save, armedNow && m_confirm == Confirm::Replace ? "REPLACE?" : "SAVE", false);
        drawMiniButton(load, "LOAD", false);
        drawMiniButton(over, armedNow && m_confirm == Confirm::Overwrite ? "SURE?" : "OVERWRITE", false);
        drawMiniButton(del, armedNow && m_confirm == Confirm::Delete ? "SURE?" : "DELETE", false);
        if (consumeClick(save)) hangarSaveNew();
        else if (consumeClick(load)) { if (haveSel) hangarLoad(m_hangarSel); return; }
        else if (consumeClick(over)) { if (haveSel) hangarOverwrite(m_hangarSel); }
        else if (consumeClick(del)) { if (haveSel) hangarDelete(m_hangarSel); }

        sf::Text hint(monoFont(),
            "TYPE A NAME   ENTER SAVE   UP/DOWN PICK   DOUBLE-CLICK LOAD   WHEEL SCROLL   ESC CLOSE", 11);
        hint.setLetterSpacing(1.4f);
        hint.setFillColor(TEXT_DEAD);
        hint.setPosition({ R.x + 18.f, fy + fh + 14.f });
        m_window->draw(hint);
        m_clickPending = false;   // nothing under the overlay may take a click
    }

    /// On-screen buttons for every keyboard shortcut on this screen.
    void drawControls(const sf::Vector2f& size) {
        const ControlRects cr = controlRects();
        const bool model = (m_mode == Mode::Model);
        const bool lock = m_hangarOpen;   // the hangar owns the screen while it is open
        const auto R = [](const Rect& r) { return tui::Rect{ r.x, r.y, r.w, r.h }; };

        if (m_ui.button(R(cr.back), "BACK", false, lock, false, 18))
            m_exit = true;

        if (m_ui.button(R(cr.mirror),
            m_design->symmetric() ? "MIRROR ON" : "MIRROR OFF",
            m_design->symmetric(), lock, false, 16))
            toggleMirror();

        if (m_mode == Mode::Paint) {
            if (m_ui.button(R(cr.third), "CLEAR PAINT", false, lock, false, 15) && m_livery) {
                *m_livery = ship::Livery{};
                deselect();
                syncPickerFrom(targetColor(m_paintTarget));
                toast("PAINT CLEARED - CTRL+Z BRINGS IT BACK");
            }
        }
        else if (model) {
            if (m_ui.button(R(cr.third), "RESET MODEL", false, lock || !m_design->decorAuthored(), false, 15)) {
                m_design->resetDecor();
                toast("MODEL RESET TO HULL");
            }
        }
        else if (m_ui.button(R(cr.third), "AUTO-MOUNT", false, lock, false, 16)) {
            m_design->autoMount();
            toast("AUTO-MOUNTED");
        }

        for (int i = 0; i < ship::HULL_CLASS_COUNT; ++i) {
            const auto c = static_cast<ship::HullClass>(i);
            if (m_ui.button(R(cr.cls[i]), ship::classSpec(c).name,
                m_design->hullClass() == c, lock, false, 15))
                loadClass(c);
        }

        // Undo / redo, also on Ctrl+Z and Ctrl+Y (Ctrl+Shift+Z).
        if (m_ui.button(R(cr.undo), "UNDO", false, lock || m_undo.empty(), false, 15)) undo();
        if (m_ui.button(R(cr.redo), "REDO", false, lock || m_redo.empty(), false, 15)) redo();

        if (m_ui.button(R(cr.hangar), m_hangarOpen ? "CLOSE  H" : "HANGAR  H", m_hangarOpen, false, false, 15))
            m_hangarOpen ? closeHangar() : openHangar();

        m_ui.glossaryTab({ size.x - 46.f, size.y * 0.100f, 32.f, 32.f });
        m_ui.drawGlossary("REFIT BAY",
            "HULL is the hitbox: what hits and gets hit, where guns and drives go.\n"
            "MODEL is what the ship looks like. It must cover the hull, stay inside\n"
            "the violet limit and use at most 150% of the hull's area.",
            { { "M / TAB",       "Switch HULL, MODEL and PAINT" },
              { "PAINT",         "Parts, colours and the canopy. No hitbox change" },
              { "W E R T",       "PAINT tools: MOVE, TURN, SIZE, SHAPE (plate points)" },
              { "PLATE",         "An armour panel. Stamp one, reshape it with SHAPE" },
              { "TONE / COLOUR", "Follow the hull (flash, heat) or keep a fixed colour" },
              { "H  HANGAR",     "Every saved ship: load, save by name, overwrite, delete" },
              { "QUICK EXPORT",  "PAINT: save as a new file in ships/ -- share it" },
              { "HOVER",         "Fits, buttons, frames show the stat change before you click" },
              { "SEE-THROUGH",   "Hull alpha in the picker; UNDER-HULL parts show through" },
              { "RIFT VIOLET",   "Never paintable: it reads as the heavy weapon" },
              { "LEFT DRAG",     "Move a point of the layer you are editing" },
              { "RIGHT CLICK",   "HULL: fit/remove weapon (point) or drive (edge)" },
              { "",              "MODEL: remove a point, or add one anywhere else" },
              { "R",             "HULL: fire the Rift from this gun / share-dedicate" },
              { "INS = / DEL",   "Add a point at the cursor / remove the one under it" },
              { "Y",             "Mirror editing" },
              { "CTRL+Z / Y",    "Undo / redo -- every drag, click and key" },
              { "WHEEL  MMB",    "Zoom at the cursor / pan the view" },
              { "F",             "Fit the view to the ship again" },
              { "SHIFT / ALT",   "PAINT: keep ratio, 15 deg, axis lock / from centre" },
              { "ARROWS  CTRL+D","PAINT: nudge in the current tool / duplicate" },
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
                "W MOVE   E TURN   R SIZE   T SHAPE     ARROWS NUDGE   CTRL+D COPY   RMB / DEL REMOVE     WHEEL ZOOM   MMB PAN   F FIT", 13);
            a.setLetterSpacing(1.5f);
            a.setFillColor(TEXT_DIM);
            a.setPosition({ size.x * 0.015f, size.y - 46.f });
            m_window->draw(a);
            sf::Text b(monoFont(), "M HULL     Y MIRROR     1-3 FRAME     H HANGAR     CTRL+Z UNDO   CTRL+Y REDO     ESC BACK", 13);
            b.setLetterSpacing(1.5f);
            b.setFillColor(TEXT_DIM);
            b.setPosition({ size.x * 0.015f, size.y - 28.f });
            m_window->draw(b);
            return;
        }
        const bool model = (m_mode == Mode::Model);
        sf::Text a(monoFont(), model
            ? "LMB DRAG POINT     RMB ADD / REMOVE POINT     INS ADD     DEL REMOVE     WHEEL ZOOM   MMB PAN   F FIT"
            : "LMB DRAG POINT     RMB FIT/REMOVE     R RIFT MOUNT     INS ADD     DEL REMOVE     WHEEL ZOOM   MMB PAN   F FIT", 13);
        a.setLetterSpacing(1.5f);
        a.setFillColor(TEXT_DIM);
        a.setPosition({ size.x * 0.015f, size.y - 46.f });
        m_window->draw(a);

        sf::Text b(monoFont(), model
            ? "M PAINT     Y MIRROR     1-3 FRAME     H HANGAR     CTRL+Z UNDO   CTRL+Y REDO     ESC BACK"
            : "M MODEL     Y MIRROR     1-3 FRAME     A AUTO-MOUNT     H HANGAR     CTRL+Z UNDO   CTRL+Y REDO     ESC BACK", 13);
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
    HoverItem    m_hover;                    ///< Part under the cursor (PAINT)
    bool         m_grabMirror = false;       ///< The drag started on the mirrored copy
    sf::Vector2f m_grabStartMouse;           ///< Screen, for the even-scale drag
    Tool         m_tool = Tool::Move;
    int          m_selPlate = -1;
    int          m_vertDrag = -1;            ///< Plate point being dragged (SHAPE)
    int          m_vertPartner = -1;
    bool         m_vertOnAxis = false;
    int          m_lastVert = -1;            ///< Last point touched: arrow keys move it
    int          m_hoverVert = -1;
    int          m_insEdge = -1;             ///< Edge an RMB would add a point to
    sf::Vector2f m_insPos;
    Rect         m_shadeRect{ -1e6f, -1e6f, 0.f, 0.f };
    bool         m_nudgeHeld = false;
    float        m_nudgeNext = 0.f;
    bool         m_atLimit = false;          ///< The gizmo is sliding along the model limit
    bool         m_clickPending = false;
    sf::Vector2f m_clickPos;
    std::string  m_fileMsg;
    float        m_zoom = 4.2f;
    sf::Vector2f m_viewCenter;
    bool         m_viewInit = false;
    bool         m_viewManual = false;       ///< Player zoomed/panned: auto-fit holds off
    bool         m_panning = false;
    sf::Vector2f m_panMouse, m_panCenter;
    float        m_wheel = 0.f;              ///< Wheel notches since the last update
    bool         m_mDown = false;

    std::vector<Snapshot> m_undo, m_redo;
    Snapshot     m_frameBefore, m_gestureBefore;
    bool         m_gestureOpen = false;
    bool         m_skipCommit = false;

    float        m_rejectFlash = 0.f;
    sf::Vector2f m_rejectAt;
    std::string  m_rejectText;

    std::string m_toast;
    float m_toastTimer = 0.f;

    bool m_exit = false;
    bool m_kY = false, m_k1 = false, m_k2 = false, m_k3 = false;
    bool m_kA = false, m_kR = false, m_kM = false, m_kDel = false, m_kAdd = false, m_kExit = false;
    bool m_kZ = false, m_kF = false;
    bool m_kW = false, m_kE = false, m_kT = false, m_kD = false;
    bool m_kH = false, m_kEnter = false, m_kUp = false, m_kDown = false;

    // ---- Stat preview ----
    bool        m_pvActive = false;      ///< m_pvFrom / m_pvTo hold a real comparison
    bool        m_pvDrag = false;        ///< ...measured from a hull drag's start
    StatRow     m_pvFrom, m_pvTo;
    std::string m_pvLabel;               ///< "FIT WEAPON", "HEAVY FRAME", ...
    std::string m_pvRefused;             ///< Why the hovered change would be refused

    // ---- Hangar ----
    bool        m_hangarOpen = false;
    std::vector<HangarEntry> m_hangar;
    int         m_hangarSel = -1;
    int         m_hangarScroll = 0;
    int         m_hangarVisible = 1;
    std::string m_hangarMsg;
    bool        m_hangarMsgBad = false;
    Confirm     m_confirm = Confirm::None;
    int         m_confirmIdx = -1;
    float       m_confirmUntil = 0.f;
    std::string m_nameBuf;               ///< The hangar's name field
    std::string m_shipName;              ///< Name of the file this ship came from / went to
    float       m_lastRowClick = -10.f;

    float m_inputLock = 0.f;
};