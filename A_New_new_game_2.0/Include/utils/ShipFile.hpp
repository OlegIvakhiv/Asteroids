/**
 * @file ShipFile.hpp
 * @brief Save and load a whole ship -- hull, mounts, model, paint -- as Lua.
 *
 * One plain Lua table, no binary, no versioned struct dump. A player exports
 * a design, posts the file, someone else drops it in `ships/` and flies it.
 * The same file is what a hand-authored enemy silhouette should look like,
 * so a good-looking player ship can become an enemy skin without a converter.
 *
 * FORMAT (ships/<name>.lua):
 *
 *   return {
 *       format = "voidhunter.ship.v1",
 *       name   = "BASTION-MK2",
 *       class  = "heavy",                      -- light | medium | heavy
 *       hull   = { {0,-42}, {38,18}, ... },    -- <= 8 convex points: the HITBOX
 *       mounts = { guns = {0,1,4}, drives = {2}, spinal = 0, rift_shared = false },
 *       model  = { {0,-56}, {10,-30}, ... },   -- optional: the decorative outline
 *       paint  = { hull = "#2864FFFF", outline = "#FFFFFFFF", ... },
 *       decals = { { kind="bar", x=0, y=-4, w=18, h=5, angle=0,
 *                    thickness=0, color="#28F5FFFF", over=true, mirror=true }, ... },
 *       cockpit = { style = "bubble", x = 0, y = -6, w = 13, h = 17 },
 *   }
 *
 * Loading is defensive: the file is data from the internet. Anything missing
 * falls back to a default, anything malformed is rejected with a reason, and
 * the hull still goes through ShipDesign's own validation -- a shared file
 * cannot smuggle in a 40-point hitbox or a model that hides the hull.
 *
 * The Lua chunk is run in a BARE state (no libraries, no io, no os), so a
 * downloaded "ship" cannot do anything but describe a ship.
 *
 * @author Oleg Ivakhiv
 * @version 1.0
 */

#pragma once

#include "ShipDesign.hpp"
#include "ShipLivery.hpp"
#include <sol/sol.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace ship {

    namespace shipfile {

        inline const char* FORMAT_TAG = "voidhunter.ship.v1";

        inline const char* classKey(HullClass c) {
            switch (c) {
            case HullClass::Light: return "light";
            case HullClass::Heavy: return "heavy";
            default:               return "medium";
            }
        }
        inline HullClass classFromKey(const std::string& s) {
            if (s == "light") return HullClass::Light;
            if (s == "heavy") return HullClass::Heavy;
            return HullClass::Medium;
        }
        inline const char* decalKindKey(DecalKind k) {
            switch (k) {
            case DecalKind::Line: return "line";
            case DecalKind::Bar:  return "bar";
            case DecalKind::Oval: return "oval";
            case DecalKind::Tri:  return "tri";
            default:              return "ring";
            }
        }
        inline DecalKind decalKindFromKey(const std::string& s) {
            if (s == "line") return DecalKind::Line;
            if (s == "oval") return DecalKind::Oval;
            if (s == "tri")  return DecalKind::Tri;
            if (s == "ring") return DecalKind::Ring;
            return DecalKind::Bar;
        }
        inline const char* cockpitKey(CockpitStyle s) {
            switch (s) {
            case CockpitStyle::Bubble: return "bubble";
            case CockpitStyle::Visor:  return "visor";
            case CockpitStyle::Twin:   return "twin";
            case CockpitStyle::Slit:   return "slit";
            case CockpitStyle::Dome:   return "dome";
            default:                   return "none";
            }
        }
        inline CockpitStyle cockpitFromKey(const std::string& s) {
            if (s == "bubble") return CockpitStyle::Bubble;
            if (s == "visor")  return CockpitStyle::Visor;
            if (s == "twin")   return CockpitStyle::Twin;
            if (s == "slit")   return CockpitStyle::Slit;
            if (s == "dome")   return CockpitStyle::Dome;
            return CockpitStyle::None;
        }

        /// Folder ships live in. Created on first export.
        inline std::filesystem::path shipDir() { return std::filesystem::path("ships"); }

        /// Filesystem-safe version of a display name.
        inline std::string slug(std::string s) {
            for (auto& c : s) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) c = '-';
            }
            while (!s.empty() && s.back() == '-') s.pop_back();
            if (s.empty()) s = "SHIP";
            return s;
        }

        // ------------------------------------------------------------------
        // WRITE
        // ------------------------------------------------------------------

        inline std::string toLua(const ShipDesign& d, const Livery& lv, const std::string& name) {
            std::ostringstream o;
            o << "-- Void Hunter ship. Drop this file in the game's ships/ folder.\n";
            o << "return {\n";
            o << "    format = \"" << FORMAT_TAG << "\",\n";
            o << "    name   = \"" << slug(name) << "\",\n";
            o << "    class  = \"" << classKey(d.hullClass()) << "\",\n";

            o << "    hull = { ";
            for (const auto& p : d.points()) o << "{" << p.x << "," << p.y << "}, ";
            o << "},\n";

            o << "    mounts = { guns = { ";
            for (int g : d.mountedGuns()) o << g << ", ";
            o << "}, drives = { ";
            for (int e : d.mountedEngines()) o << e << ", ";
            o << "}, spinal = " << d.stats().spinalVertex
                << ", rift_shared = " << (d.stats().riftShared ? "true" : "false") << " },\n";

            if (d.decorAuthored() && d.decorCheck().ok) {
                o << "    model = { ";
                for (const auto& p : d.decor()) o << "{" << p.x << "," << p.y << "}, ";
                o << "},\n";
            }

            const Paint& pt = lv.paint;
            o << "    paint = {\n";
            o << "        hull = \"" << colorToHex(pt.hull) << "\", outline = \"" << colorToHex(pt.outline) << "\",\n";
            o << "        plasma = \"" << colorToHex(pt.plasma) << "\", thrust = \"" << colorToHex(pt.thrust) << "\",\n";
            o << "        turbo = \"" << colorToHex(pt.turbo) << "\", dodge = \"" << colorToHex(pt.dodge) << "\",\n";
            o << "        parry = \"" << colorToHex(pt.parry) << "\", cockpit = \"" << colorToHex(pt.cockpit) << "\",\n";
            o << "        homing = \"" << colorToHex(pt.homing) << "\",\n";
            o << "    },\n";

            o << "    decals = {\n";
            for (const auto& dc : lv.decals) {
                o << "        { kind = \"" << decalKindKey(dc.kind) << "\", x = " << dc.pos.x << ", y = " << dc.pos.y
                    << ", w = " << dc.w << ", h = " << dc.h << ", angle = " << dc.angle
                    << ", thickness = " << dc.thickness << ", color = \"" << colorToHex(dc.color)
                    << "\", over = " << (dc.over ? "true" : "false")
                    << ", mirror = " << (dc.mirrored ? "true" : "false") << " },\n";
            }
            o << "    },\n";

            o << "    cockpit = { style = \"" << cockpitKey(lv.cockpit.style) << "\", x = " << lv.cockpit.pos.x
                << ", y = " << lv.cockpit.pos.y << ", w = " << lv.cockpit.w << ", h = " << lv.cockpit.h
                << ", angle = " << lv.cockpit.angle << " },\n";
            o << "}\n";
            return o.str();
        }

        /// @return written path, or empty on failure (reason in `err`).
        inline std::string save(const ShipDesign& d, const Livery& lv,
            const std::string& name, std::string& err) {
            std::error_code ec;
            std::filesystem::create_directories(shipDir(), ec);

            const std::string base = slug(name);
            std::filesystem::path path = shipDir() / (base + ".lua");
            for (int n = 2; std::filesystem::exists(path) && n < 100; ++n)
                path = shipDir() / (base + "-" + std::to_string(n) + ".lua");

            std::ofstream f(path);
            if (!f) { err = "CANNOT WRITE ships/"; return {}; }
            f << toLua(d, lv, base);
            if (!f.good()) { err = "WRITE FAILED"; return {}; }
            return path.string();
        }

        inline std::vector<std::string> list() {
            std::vector<std::string> out;
            std::error_code ec;
            if (!std::filesystem::exists(shipDir(), ec)) return out;
            for (const auto& e : std::filesystem::directory_iterator(shipDir(), ec)) {
                if (!e.is_regular_file()) continue;
                if (e.path().extension() == ".lua") out.push_back(e.path().string());
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // ------------------------------------------------------------------
        // READ
        // ------------------------------------------------------------------

        inline std::vector<sf::Vector2f> readPoints(const sol::table& t, std::size_t maxN) {
            std::vector<sf::Vector2f> pts;
            for (std::size_t i = 1; i <= t.size() && pts.size() < maxN; ++i) {
                sol::optional<sol::table> p = t[i];
                if (!p) continue;
                pts.push_back({ (*p)[1].get_or(0.f), (*p)[2].get_or(0.f) });
            }
            return pts;
        }

        /**
         * @brief Load a ship file onto a design and livery.
         *
         * Runs the chunk in a bare Lua state: a downloaded file gets no
         * libraries, so the worst a malicious one can do is describe a bad
         * ship -- which then fails validation here.
         */
        inline bool load(const std::string& path, ShipDesign& outDesign, Livery& outLivery,
            std::string& name, std::string& err) {
            sol::state lua;   // deliberately NO open_libraries
            sol::protected_function_result r;
            try {
                r = lua.safe_script_file(path, sol::script_pass_on_error);
            }
            catch (const std::exception& e) { err = std::string("BAD FILE: ") + e.what(); return false; }
            if (!r.valid()) { err = "BAD FILE - NOT VALID LUA"; return false; }

            sol::optional<sol::table> root = r;
            if (!root) { err = "FILE DOES NOT RETURN A SHIP"; return false; }

            const std::string fmt = (*root)["format"].get_or(std::string());
            if (fmt.rfind("voidhunter.ship.", 0) != 0) { err = "NOT A SHIP FILE"; return false; }

            sol::optional<sol::table> hullT = (*root)["hull"];
            if (!hullT) { err = "NO HULL IN FILE"; return false; }
            std::vector<sf::Vector2f> hull = readPoints(*hullT, MAX_HULL_POINTS);
            if (hull.size() < MIN_HULL_POINTS) { err = "HULL TOO SMALL"; return false; }

            const HullClass cls = classFromKey((*root)["class"].get_or(std::string("medium")));
            ShipDesign d = ShipDesign::fromPoints(hull, cls, false);
            if (!d.fitsClass(cls)) { err = "HULL DOES NOT FIT ITS CLASS FRAME"; return false; }

            // ---- Mounts ----
            sol::optional<sol::table> mt = (*root)["mounts"];
            if (mt) {
                sol::optional<sol::table> guns = (*mt)["guns"];
                sol::optional<sol::table> drives = (*mt)["drives"];
                if (guns)   for (std::size_t i = 1; i <= guns->size(); ++i)   d.mountGun((*guns)[i].get_or(-1));
                if (drives) for (std::size_t i = 1; i <= drives->size(); ++i) d.mountEngine((*drives)[i].get_or(-1));
                // assignRift on the CURRENT spinal mount TOGGLES sharing, so the
                // move and the share flag have to be applied separately or a
                // dedicated Rift comes back shared.
                const int spinal = (*mt)["spinal"].get_or(-1);
                if (spinal >= 0 && spinal != d.stats().spinalVertex) d.assignRift(spinal);
                const bool wantShared = (*mt)["rift_shared"].get_or(false);
                if (d.stats().spinalVertex >= 0 && d.mountedGuns().size() > 1
                    && wantShared != d.stats().riftShared)
                    d.assignRift(d.stats().spinalVertex);
            }
            if (d.mountedGuns().empty() && d.mountedEngines().empty()) d.autoMount();

            // ---- Model ----
            sol::optional<sol::table> model = (*root)["model"];
            if (model) {
                std::vector<sf::Vector2f> pts = readPoints(*model, MAX_DECOR_POINTS);
                if (pts.size() >= MIN_HULL_POINTS && !d.setDecor(pts))
                    err = std::string("MODEL REJECTED: ") + d.lastRejectReason();   // warning, not fatal
            }

            // ---- Paint ----
            Livery lv;
            sol::optional<sol::table> paint = (*root)["paint"];
            if (paint) {
                const auto col = [&](const char* k, sf::Color def) {
                    return colorFromHex((*paint)[k].get_or(std::string()), def);
                    };
                lv.paint.hull = col("hull", lv.paint.hull);
                lv.paint.outline = col("outline", lv.paint.outline);
                lv.paint.plasma = col("plasma", lv.paint.plasma);
                lv.paint.thrust = col("thrust", lv.paint.thrust);
                lv.paint.turbo = col("turbo", lv.paint.turbo);
                lv.paint.dodge = col("dodge", lv.paint.dodge);
                lv.paint.parry = col("parry", lv.paint.parry);
                lv.paint.cockpit = col("cockpit", lv.paint.cockpit);
                lv.paint.homing = col("homing", lv.paint.homing);
            }

            // ---- Decals ----
            sol::optional<sol::table> decals = (*root)["decals"];
            if (decals) {
                const std::vector<sf::Vector2f> env = d.envelope();
                for (std::size_t i = 1; i <= decals->size() && lv.decals.size() < MAX_DECALS; ++i) {
                    sol::optional<sol::table> e = (*decals)[i];
                    if (!e) continue;
                    Decal dc;
                    dc.kind = decalKindFromKey((*e)["kind"].get_or(std::string("bar")));
                    dc.pos = { (*e)["x"].get_or(0.f), (*e)["y"].get_or(0.f) };
                    dc.w = (*e)["w"].get_or(16.f);
                    dc.h = (*e)["h"].get_or(6.f);
                    dc.angle = (*e)["angle"].get_or(0.f);
                    dc.thickness = (*e)["thickness"].get_or(0.f);
                    dc.color = colorFromHex((*e)["color"].get_or(std::string()), sf::Color(40, 245, 255));
                    dc.over = (*e)["over"].get_or(true);
                    dc.mirrored = (*e)["mirror"].get_or(false);
                    clampDecal(dc);
                    if (decalInside(dc, env)) lv.decals.push_back(dc);   // silently drop out-of-bounds art
                }
            }

            // ---- Cockpit ----
            sol::optional<sol::table> cp = (*root)["cockpit"];
            if (cp) {
                lv.cockpit.style = cockpitFromKey((*cp)["style"].get_or(std::string("none")));
                lv.cockpit.pos = { (*cp)["x"].get_or(0.f), (*cp)["y"].get_or(-6.f) };
                lv.cockpit.w = std::clamp((*cp)["w"].get_or(13.f), 3.f, 60.f);
                lv.cockpit.h = std::clamp((*cp)["h"].get_or(17.f), 3.f, 60.f);
                lv.cockpit.angle = (*cp)["angle"].get_or(0.f);
            }

            name = (*root)["name"].get_or(std::string("SHIP"));
            outDesign = d;
            outLivery = lv;
            return true;
        }

    } // namespace shipfile
} // namespace ship