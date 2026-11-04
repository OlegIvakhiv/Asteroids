/**
 * @file AudioSystem.hpp
 * @brief Every sound in the game: the sounds.lua table, a voice pool, and the
 *        watchers that hear the simulation without being wired into it.
 *
 * TWO WAYS A SOUND GETS STARTED
 *
 *   1. RAISED. A system calls `m_em->sfx("rock.break.large", pos)` at the
 *      line where the thing happens -- the same raise-then-drain shape as
 *      codexKills. Used where only that line knows WHAT happened: which of the
 *      six parries landed, which Rift detonation mode fired, stone or metal.
 *
 *   2. WATCHED. Most of the game's state already lives in components as
 *      explicit timers and enums -- RamState::Windup, BashState, DuelAttack,
 *      qteResult, dodgeVerdictFlash... So the AudioSystem keeps last frame's
 *      copy of each and plays on the EDGE. That covers every enemy telegraph
 *      with zero edits to AISystem, and it can never drift out of sync with
 *      the visuals, because it reads the exact field the renderer reads.
 *
 * TELLS ARE TIME-ALIGNED. A tell file has a `peak_at` in sounds.lua: the
 * moment it lands (the lunge, the lock click, the release). When a windup
 * starts, the sound is offset or delayed so that peak falls on the frame the
 * attack fires -- whatever the windup is currently tuned to in Lua. Retune
 * bash_windup and the parry cue still lands on the lunge.
 *
 * LOOPS ARE KEEP-ALIVE. A watcher asks for a loop every frame it should be
 * playing (`wantLoop`). Asked -> it plays and follows its entity; not asked ->
 * it fades out. No start/stop bookkeeping, nothing to leak when an entity is
 * swap-and-popped away mid-frame, because loops are keyed by persistent
 * entity id, never by vector index.
 *
 * BUSES. World sounds are positional (panned and attenuated from the camera),
 * time-scaled in slow-mo/hitstop (except the player's own rewards: parry,
 * perfect dodge, vent -- the reward must not be slowed), muffled and ducked
 * under the pause screen, and silenced in the terminal. UI sounds (terminal)
 * are none of those.
 *
 * CHANNELS (MACHINE RITES). Every sound belongs to one of four channels the
 * player sets in the settings page: EFFECTS (the default), TELLS (`tell.*`,
 * the enemy attack cues), AMBIENCE (`amb.*`) and INTERFACE (`term.*`,
 * `hud.*`). A sounds.lua row may name its own: `bus = "tells"`. The player's
 * levels multiply on top of everything above, the sounds.lua master
 * included -- an F5 reload never overwrites what the player chose.
 *
 * TERMINAL LOOPS. The menu has no watcher here, so it raises its beds
 * (intercept static, the corruption caution) as events: a `loop = true` row
 * raised WITH a key is a keep-alive request, exactly like wantLoop. Raise it
 * every frame it should sound; stop raising and it fades.
 *
 * NOT HERE: the ShadowWorld feed and doctrine. Their EntityManagers raise
 * sounds too; ShadowWorld clears them every pass, so nothing leaks.
 *
 * @author Oleg Ivakhiv
 * @version 1.2 (channels for MACHINE RITES)
 */

#pragma once

#include <SFML/Audio.hpp>
#include <sol/sol.hpp>
#include <box2d/box2d.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "systems/ISystem.hpp"
#include "utils/GameState.hpp"
#include "utils/InputRegistry.hpp"

class AudioSystem : public ISystem {
public:
    static constexpr int VOICES = 32;

    /// The player's mixer channels (MACHINE RITES).
    enum Bus : uint8_t { BUS_EFFECTS = 0, BUS_TELLS, BUS_AMBIENCE, BUS_INTERFACE, BUS_COUNT };

    void init(const SystemContext& ctx) override {
        m_em = ctx.em;
        m_lua = ctx.lua;
        m_registry = ctx.enemyRegistry;
        m_playerId = ctx.playerEntityId;
        m_view = ctx.gameView;
        m_zone = ctx.zone;
        m_worldId = ctx.worldId;

        // restart(): entity ids start again at 1, so every per-entity memory
        // and every world voice now describes somebody else. UI voices (the
        // ENGAGE clamp that started this run) keep playing.
        for (auto& v : m_voices) if (v.active && !v.ui) v.stop();
        m_enemies.clear();
        m_bullets.clear();
        m_delayed.clear();
        m_player = PlayerSnap{};
        m_playerSeen = false;
        m_feral = false;
        m_scrapDying = 0;
        m_rcsDir = 0;

        if (!m_loaded) reload();
    }

    /// Unused: frame() is the entry point, because sound also runs in menus.
    void update(float) override {}

    /**
     * @brief Run scripts/sounds.lua and rebuild the table. F5 calls this.
     *
     * Buffers are cached by path and never freed, so a reload only loads
     * files it has not seen, and no playing voice can lose its buffer.
     */
    bool reload() {
        if (!m_lua) return false;
        try { m_lua->script_file("scripts/sounds.lua"); }
        catch (const std::exception& e) {
            std::cerr << "[Audio] scripts/sounds.lua not loaded: " << e.what()
                << "\n  Running silent.\n";
            return false;
        }

        if (sol::optional<sol::table> a = (*m_lua)["audio"]) {
            m_master = (*a)["master"].get_or(0.8f);
            m_hearRadius = (*a)["hear_radius"].get_or(1900.f);
            m_pauseGain = (*a)["pause_gain"].get_or(0.45f);
            m_panWidth = (*a)["pan_width"].get_or(0.75f);
            m_assetRoot = (*a)["root"].get_or(std::string("assets/"));
        }

        // Scrap lifetime lives in player.lua's `scrap` table (ScrapSystem's).
        if (sol::optional<sol::table> sc = (*m_lua)["scrap"]) m_scrapLife = (*sc)["lifetime"].get_or(30.f);

        sol::optional<sol::table> table = (*m_lua)["sounds"];
        if (!table) { std::cerr << "[Audio] sounds.lua defines no `sounds` table\n"; return false; }

        std::unordered_map<std::string, Def> defs;
        int fileCount = 0;
        for (const auto& kv : *table) {
            if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
            const std::string id = kv.first.as<std::string>();
            const sol::table e = kv.second.as<sol::table>();

            Def d;
            d.gain = e["gain"].get_or(1.f);
            d.pitchVar = e["pitch_var"].get_or(0.f);
            d.maxVoices = std::max(1, e["max_voices"].get_or(4));
            d.priority = e["priority"].get_or(3);
            d.minInterval = e["min_interval"].get_or(0.f);
            d.loop = e["loop"].get_or(false);
            d.peakAt = e["peak_at"].get_or(-1.f);
            d.timeScaled = e["time_scaled"].get_or(defaultTimeScaled(id));
            d.bus = busOf(id, e["bus"].get_or(std::string()));
            if (sol::optional<sol::table> files = e["files"]) {
                for (const auto& f : *files) {
                    if (!f.second.is<std::string>()) continue;
                    if (const sf::SoundBuffer* b = buffer(m_assetRoot + f.second.as<std::string>())) {
                        d.files.push_back(b);
                        ++fileCount;
                    }
                }
            }
            if (auto old = m_defs.find(id); old != m_defs.end()) d.lastPlay = old->second.lastPlay;
            defs[id] = std::move(d);
        }
        m_defs.swap(defs);
        m_loaded = true;
        std::cout << "[Audio] " << m_defs.size() << " sounds, " << fileCount << " files\n";
        return true;
    }

    /**
     * @brief The once-per-frame entry. Call after everything else has run.
     * @param realDt Real seconds (pause and hitstop do not stop sound).
     */
    void frame(float realDt, GameState state) {
        m_now += realDt;
        if (!m_em) return;

        const bool live = (state == GameState::Playing || state == GameState::GameOver);
        const bool paused = (state == GameState::Paused);

        // ---- Buses: the pause screen muffles and ducks the frozen world;
        //      the terminal silences it. Both glide, never snap. ----
        const float worldTarget = live ? 1.f : (paused ? m_pauseGain : 0.f);
        m_worldBus += (worldTarget - m_worldBus) * std::min(1.f, realDt * 8.f);
        const float muffleTarget = paused ? 1.f : 0.f;
        m_muffleV += (muffleTarget - m_muffleV) * std::min(1.f, realDt * 10.f);
        m_muffle.store(m_muffleV, std::memory_order_relaxed);
        m_timeFactor = live ? 0.55f + 0.45f * std::clamp(m_em->timeScale, 0.f, 1.f) : 1.f;

        if (m_view) { m_listener = m_view->getCenter(); m_viewSize = m_view->getSize(); }

        if (live) {
            watchPlayer(realDt);
            watchEnemies(realDt);
            watchBullets();
            watchField();
            watchContacts();
            watchScrap();
            zoneAmbience();
        }

        // ---- Delayed starts (time-aligned tells that begin later) ----
        for (size_t k = 0; k < m_delayed.size();) {
            if (m_delayed[k].at <= m_now) {
                if (live || !m_delayed[k].e.positional) handle(m_delayed[k].e);
                m_delayed[k] = m_delayed.back(); m_delayed.pop_back();
            }
            else ++k;
        }

        // ---- Raised this frame ----
        for (const SoundEvent& e : m_em->sounds) {
            if (e.positional && !live) continue;   // a frozen world raises nothing new
            if (e.id && e.key) {                   // a keyed loop row: keep-alive, not a one-shot
                auto it = m_defs.find(e.id);
                if (it != m_defs.end() && it->second.loop) {
                    m_loopReq.push_back({ e.key, e.id, e.pos, e.gain, e.pitch, !e.positional });
                    continue;
                }
            }
            handle(e);
        }
        m_em->sounds.clear();

        resolveLoops();
        updateVoices(realDt);
    }

    /// sounds.lua's own master (the mix). The player's levels are separate.
    void setMaster(float v) { m_master = std::clamp(v, 0.f, 1.f); }

    /// The player's levels, 0..1 each (MACHINE RITES). Applied to playing
    /// voices on the next frame.
    void setUserVolumes(float master, float effects, float tells, float ambience, float ui) {
        m_userMaster = std::clamp(master, 0.f, 1.f);
        m_userBus[BUS_EFFECTS] = std::clamp(effects, 0.f, 1.f);
        m_userBus[BUS_TELLS] = std::clamp(tells, 0.f, 1.f);
        m_userBus[BUS_AMBIENCE] = std::clamp(ambience, 0.f, 1.f);
        m_userBus[BUS_INTERFACE] = std::clamp(ui, 0.f, 1.f);
    }

private:
    // ========================================================================
    // TABLE
    // ========================================================================
    struct Def {
        std::vector<const sf::SoundBuffer*> files;
        float gain = 1.f, pitchVar = 0.f, minInterval = 0.f, peakAt = -1.f;
        int   maxVoices = 4, priority = 3;
        bool  loop = false, timeScaled = true;
        float lastPlay = -1e9f;
        Bus   bus = BUS_EFFECTS;
    };

    /// Which channel a sound is on: its own `bus` field, else its id prefix.
    static Bus busOf(const std::string& id, const std::string& named) {
        if (named == "tells") return BUS_TELLS;
        if (named == "ambience") return BUS_AMBIENCE;
        if (named == "interface") return BUS_INTERFACE;
        if (named == "effects") return BUS_EFFECTS;
        if (id.rfind("tell.", 0) == 0) return BUS_TELLS;
        if (id.rfind("amb.", 0) == 0) return BUS_AMBIENCE;
        if (id.rfind("term.", 0) == 0 || id.rfind("hud.", 0) == 0) return BUS_INTERFACE;
        return BUS_EFFECTS;
    }

    /// The player's rewards and the terminal must not slide down in slow-mo:
    /// a parry plays DURING its own hitstop, and slowing it would bury it.
    static bool defaultTimeScaled(const std::string& id) {
        static const char* exempt[] = { "player.parry", "player.dodge", "player.bash.perfect",
            "player.vent", "player.overdrive", "player.heat", "player.hull.low", "term.", "amb.", "hud." };
        for (const char* p : exempt) if (id.rfind(p, 0) == 0) return false;
        return true;
    }

    const sf::SoundBuffer* buffer(const std::string& path) {
        auto it = m_buffers.find(path);
        if (it != m_buffers.end()) return it->second.get();
        auto b = std::make_unique<sf::SoundBuffer>();
        if (!b->loadFromFile(path)) {
            std::cerr << "[Audio] missing " << path << "\n";
            b.reset();   // remembered as missing: no retry every reload
        }
        const sf::SoundBuffer* raw = b.get();
        m_buffers.emplace(path, std::move(b));
        return raw;
    }

    // ========================================================================
    // VOICES
    // ========================================================================
    /// One-pole low-pass state for the pause muffle. Lives inside the Voice,
    /// which lives in a fixed array, so the pointer the audio thread holds
    /// stays valid for the life of the sf::Sound that uses it.
    struct Lp { float z[2] = { 0.f, 0.f }; };

    struct Voice {
        std::optional<sf::Sound> snd;
        Lp lp;
        bool active = false, ui = false, loop = false, alive = false, timeScaled = true, positional = true;
        std::string id;
        int priority = 3;
        float defGain = 1.f, gain = 1.f, pitch = 1.f;
        sf::Vector2f pos;
        uint64_t key = 0;
        float env = 1.f, envTarget = 1.f, envRate = 0.f;
        float started = 0.f;
        Bus bus = BUS_EFFECTS;
        void stop() { snd.reset(); active = false; loop = false; key = 0; alive = false; }
    };

    /// Attenuation and pan from the camera. False = out of earshot.
    bool spatial(sf::Vector2f p, float& atten, float& pan) const {
        const sf::Vector2f d = p - m_listener;
        const float dist = std::sqrt(d.x * d.x + d.y * d.y);
        const float nearR = std::max(300.f, 0.5f * std::max(m_viewSize.x, m_viewSize.y));
        if (dist >= m_hearRadius) { atten = 0.f; pan = 0.f; return false; }
        if (dist <= nearR) atten = 1.f - 0.25f * (dist / nearR);   // on screen: nearly full
        else {
            const float u = 1.f - (dist - nearR) / std::max(1.f, m_hearRadius - nearR);
            atten = 0.75f * u * u;
        }
        pan = std::clamp(d.x / std::max(1.f, m_viewSize.x * 0.6f), -1.f, 1.f) * m_panWidth;
        return true;
    }

    int acquire(const Def& d, const std::string& id) {
        int same = 0, oldestSame = -1;
        for (int i = 0; i < VOICES; ++i) {
            const Voice& v = m_voices[i];
            if (!v.active || v.id != id) continue;
            ++same;
            if (oldestSame < 0 || v.started < m_voices[oldestSame].started) oldestSame = i;
        }
        if (same >= d.maxVoices) { m_voices[oldestSame].stop(); return oldestSame; }   // retrigger the oldest
        for (int i = 0; i < VOICES; ++i) if (!m_voices[i].active) return i;

        // Full: steal the least important one-shot (highest number), oldest
        // first. Never something more important than the newcomer; never a
        // loop (it would come straight back next frame and flap).
        int best = -1;
        for (int i = 0; i < VOICES; ++i) {
            const Voice& v = m_voices[i];
            if (v.loop || v.priority < d.priority) continue;
            if (best < 0 || v.priority > m_voices[best].priority
                || (v.priority == m_voices[best].priority && v.started < m_voices[best].started)) best = i;
        }
        if (best >= 0) m_voices[best].stop();
        return best;
    }

    void handle(const SoundEvent& e) {
        if (!e.id) { stopKey(e.key, 0.06f); return; }
        auto it = m_defs.find(e.id);
        if (it == m_defs.end()) { warnOnce(e.id); return; }
        Def& d = it->second;
        if (d.files.empty()) return;
        if (m_now - d.lastPlay < d.minInterval) return;
        if (e.positional) { float a, p; if (!spatial(e.pos, a, p)) return; }
        if (e.key) stopKey(e.key, 0.03f);
        const int vi = acquire(d, it->first);
        if (vi < 0) return;
        d.lastPlay = m_now;
        start(m_voices[vi], d, it->first, e, false);
    }

    void start(Voice& v, const Def& d, const std::string& id, const SoundEvent& e, bool loop) {
        v.stop();
        const sf::SoundBuffer* buf = d.files[d.files.size() == 1 ? 0 : m_rng() % d.files.size()];
        v.snd.emplace(*buf);
        v.lp = Lp{};
        sf::Sound& s = *v.snd;
        s.setSpatializationEnabled(false);   // we pan and attenuate ourselves
        s.setRelativeToListener(true);
        s.setLooping(loop);

        v.active = true;
        v.ui = !e.positional;
        v.loop = loop;
        v.alive = true;
        v.timeScaled = d.timeScaled;
        v.positional = e.positional;
        v.id = id;
        v.priority = d.priority;
        v.bus = d.bus;
        v.defGain = d.gain;
        v.gain = e.gain;
        const float r = std::uniform_real_distribution<float>(-1.f, 1.f)(m_rng);
        v.pitch = std::max(0.1f, e.pitch * (1.f + d.pitchVar * r));
        v.pos = e.pos;
        v.key = e.key;
        v.env = loop ? 0.f : 1.f;     // loops fade in: no click, no pop
        v.envTarget = 1.f;
        v.envRate = loop ? 1.f / 0.15f : 0.f;
        v.started = m_now;

        if (!v.ui) {
            Lp* lp = &v.lp;
            const std::atomic<float>* muffle = &m_muffle;
            s.setEffectProcessor([lp, muffle](const float* in, unsigned int& inCount,
                float* out, unsigned int& outCount, unsigned int ch) {
                const unsigned int n = std::min(inCount, outCount);
                inCount = outCount = n;
                if (!in) { std::fill(out, out + n * ch, 0.f); return; }
                const float m = muffle->load(std::memory_order_relaxed);
                if (m < 0.002f) { std::copy(in, in + n * ch, out); return; }
                const float a = 1.f - 0.92f * m;   // ~560 Hz corner at full muffle
                for (unsigned int f = 0; f < n; ++f)
                    for (unsigned int c = 0; c < ch; ++c) {
                        const float x = in[f * ch + c];
                        if (c < 2) { float& z = lp->z[c]; z += a * (x - z); out[f * ch + c] = z; }
                        else out[f * ch + c] = x;
                    }
            });
        }
        if (e.offset > 0.f) {
            const float len = buf->getDuration().asSeconds();
            s.setPlayingOffset(sf::seconds(std::min(e.offset, std::max(0.f, len - 0.02f))));
        }
        apply(v);
        s.play();
    }

    void apply(Voice& v) {
        float atten = 1.f, pan = 0.f;
        if (v.positional) spatial(v.pos, atten, pan);
        const float bus = v.ui ? 1.f : m_worldBus;
        const float vol = m_master * m_userMaster * m_userBus[v.bus] * v.defGain * v.gain * v.env * atten * bus;
        v.snd->setVolume(std::clamp(vol * 100.f, 0.f, 100.f));
        v.snd->setPan(pan);
        v.snd->setPitch(std::max(0.05f, v.pitch * ((v.timeScaled && !v.ui) ? m_timeFactor : 1.f)));
    }

    void stopKey(uint64_t key, float fade) {
        if (!key) return;
        for (auto& v : m_voices) {
            if (!v.active || v.key != key) continue;
            v.key = 0;   // released: a new sound may take this key now
            v.envTarget = 0.f;
            v.envRate = 1.f / std::max(0.005f, fade);
        }
    }

    void updateVoices(float dt) {
        for (auto& v : m_voices) {
            if (!v.active) continue;
            if (!v.loop && v.snd->getStatus() == sf::SoundSource::Status::Stopped) { v.stop(); continue; }
            if (v.env != v.envTarget) {
                const float step = v.envRate * dt;
                v.env = (v.env < v.envTarget) ? std::min(v.envTarget, v.env + step)
                    : std::max(v.envTarget, v.env - step);
                if (v.env <= 0.f && v.envTarget <= 0.f) { v.stop(); continue; }
            }
            // Out of the terminal's way: a silenced world gives its voices back.
            if (!v.ui && m_worldBus < 0.01f) { v.stop(); continue; }
            apply(v);
        }
    }

    // ---- Keep-alive loops ----
    struct LoopReq {
        uint64_t key; const char* id; sf::Vector2f pos; float gain; float pitch;
        bool ui = false;   ///< terminal bed: not positional, not on the world bus
    };

    void wantLoop(uint64_t key, const char* id, sf::Vector2f pos, float gain = 1.f, float pitch = 1.f) {
        m_loopReq.push_back({ key, id, pos, gain, pitch, false });
    }

    void resolveLoops() {
        for (auto& v : m_voices) if (v.active && v.loop) v.alive = false;
        for (const LoopReq& r : m_loopReq) {
            Voice* found = nullptr;
            for (auto& v : m_voices) if (v.active && v.loop && v.key == r.key) { found = &v; break; }
            if (found) {
                found->alive = true;
                found->pos = r.pos;
                found->gain = r.gain;
                found->pitch = r.pitch;
                found->envTarget = 1.f;
                found->envRate = 1.f / 0.15f;
                continue;
            }
            auto it = m_defs.find(r.id);
            if (it == m_defs.end()) { warnOnce(r.id); continue; }
            if (it->second.files.empty()) continue;
            float a, p;
            if (!r.ui && !spatial(r.pos, a, p)) continue;
            // At its cap a loop does NOT steal from itself: the voices already
            // running were asked for too, and stealing would restart one of
            // them every frame (audible flapping).
            int running = 0;
            for (const auto& v : m_voices) if (v.active && v.loop && v.id == it->first) ++running;
            if (running >= it->second.maxVoices) continue;
            const int vi = acquire(it->second, it->first);
            if (vi < 0) continue;
            SoundEvent e; e.id = r.id; e.pos = r.pos; e.gain = r.gain; e.pitch = r.pitch; e.key = r.key;
            e.positional = !r.ui;
            start(m_voices[vi], it->second, it->first, e, true);
        }
        for (auto& v : m_voices) {
            if (v.active && v.loop && !v.alive) { v.envTarget = 0.f; v.envRate = 1.f / 0.25f; }
        }
        m_loopReq.clear();
    }

    // ========================================================================
    // RAISING (from the watchers)
    // ========================================================================
    static uint64_t key(uint32_t entity, uint32_t tag) { return (static_cast<uint64_t>(entity) << 8) | tag; }

    enum Tag : uint32_t {
        L_ENGINE = 1, L_TURBO, L_VENT, L_OVERDRIVE, L_RIFTBOLT, L_RAM, L_STORM, L_CONE,
        L_FRENZY, L_TURRET, L_ROCKET, L_HOMING, L_FIELD, L_FERAL, L_AMB,
        T_TURBO = 40, T_RIFT, T_BASH, T_RAM, T_FUSE, T_LANCE, T_CONE
    };

    void raise(const char* id, sf::Vector2f pos, float gain = 1.f, float pitch = 1.f,
        uint64_t k = 0, float offset = 0.f) {
        SoundEvent e; e.id = id; e.pos = pos; e.gain = gain; e.pitch = pitch; e.key = k; e.offset = offset;
        m_em->sounds.push_back(e);
    }

    /**
     * @brief Start a tell so its `peak_at` lands `actual` seconds from now.
     *
     * Shorter windup than the file -> start partway in. Longer -> wait. A
     * sound with no peak_at simply plays.
     */
    void raiseAligned(const char* id, sf::Vector2f pos, float actual, uint64_t k = 0,
        float gain = 1.f, float pitch = 1.f) {
        auto it = m_defs.find(id);
        const float peak = (it != m_defs.end()) ? it->second.peakAt : -1.f;
        if (peak <= 0.f || actual <= 0.f) { raise(id, pos, gain, pitch, k); return; }
        // Played at `pitch`, the file's peak arrives after peak / pitch seconds.
        const float heard = peak / std::max(0.1f, pitch);
        if (actual <= heard) { raise(id, pos, gain, pitch, k, (heard - actual) * pitch); return; }
        SoundEvent e; e.id = id; e.pos = pos; e.gain = gain; e.pitch = pitch; e.key = k;
        m_delayed.push_back({ m_now + (actual - heard), e });
    }

    bool raisedThisFrame(const char* id) const {
        for (const auto& e : m_em->sounds) if (e.id && std::strcmp(e.id, id) == 0) return true;
        return false;
    }

    void warnOnce(const char* id) {
        if (m_warned.insert(id).second)
            std::cerr << "[Audio] no sound '" << id << "' in sounds.lua\n";
    }

    // ========================================================================
    // WATCHERS
    // ========================================================================
    struct PlayerSnap {
        float hp = 0.f, maxHp = 1.f;
        bool turbo = false, parrying = false, whiff = false, overheated = false, qte = false;
        bool riftCharging = false, riftFlight = false, phased = false;
        float dashTimer = 0.f, overheatTimer = 0.f, verdict = 0.f, poiseHit = 0.f, stagger = 0.f;
        float riftCd = 0.f, overdrive = 0.f, qteDir = 1.f;
        int scrap = 0;
        sf::Vector2f pos;
    };

    void watchPlayer(float dt) {
        const size_t pi = m_em->getEntityIndex(m_playerId);
        if (pi == (size_t)-1 || pi >= m_em->players.size()) {
            if (m_playerSeen) {
                m_playerSeen = false;
                raise("player.death", m_player.pos);
            }
            return;
        }
        const PlayerComponent& ps = m_em->players[pi];
        const HealthComponent& h = m_em->healths[pi];
        const sf::Vector2f pos = m_em->transforms[pi].position;

        PlayerSnap now;
        now.hp = h.currentHp; now.maxHp = std::max(1.f, h.maxHp);
        now.turbo = ps.isTurbo; now.parrying = ps.isParrying; now.whiff = ps.parryWhiffRecovery;
        now.overheated = ps.weaponOverheated; now.qte = ps.qteActive;
        now.riftCharging = ps.riftCharging; now.riftFlight = ps.riftBoltInFlight; now.phased = ps.dodgePhased;
        now.dashTimer = ps.dashTimer; now.overheatTimer = ps.overheatTimer;
        now.verdict = ps.dodgeVerdictFlash; now.poiseHit = ps.poiseHitFlash; now.stagger = ps.staggerTimer;
        now.riftCd = ps.riftCooldown; now.overdrive = ps.overdriveTimer; now.qteDir = ps.qteDir;
        now.scrap = m_em->scrap; now.pos = pos;

        // Prime on the first frame: a fresh hull has no edges to play.
        if (!m_playerSeen) { m_player = now; m_playerSeen = true; m_turboHeld = 0.f; return; }
        const PlayerSnap was = m_player;
        m_player = now;
        const uint32_t id = m_playerId;

        // ---- Engine: the loop always runs; speed opens it up ----
        float speed = 0.f;
        if (b2Body_IsValid(m_em->physics[pi].bodyId)) {
            const b2Vec2 v = b2Body_GetLinearVelocity(m_em->physics[pi].bodyId);
            speed = std::sqrt(v.x * v.x + v.y * v.y) * SCALE;
        }
        const float k = std::clamp(speed / 600.f, 0.f, 1.f);
        wantLoop(key(id, L_ENGINE), "player.engine.loop", pos, (0.55f + 0.45f * k) * (now.turbo ? 0.6f : 1.f), 0.94f + 0.12f * k);

        // ---- RCS: a puff when you start pushing sideways or backwards ----
        // The main drive is the engine loop; this is the attitude jets. Same
        // input read and ship-local frame as InputSystem section 7/8, and the
        // same gates: no thrust while dashing, staggered, recovering, charging.
        {
            int dir = 0;
            const bool canThrust = !ps.isTurbo && ps.dashTimer <= 0.f && ps.staggerTimer <= 0.f
                && ps.staggerRecoverTimer <= 0.f && !ps.riftCharging;
            if (canThrust && m_lua) {
                if (sol::optional<sol::table> binds = (*m_lua)["key_bindings"]) {
                    auto held = [&](const char* k) {
                        sol::optional<std::string> name = (*binds)[k];
                        return name && InputRegistry::isPressed(*name);
                    };
                    const float dx = (held("right") ? 1.f : 0.f) - (held("left") ? 1.f : 0.f);
                    const float dy = (held("down") ? 1.f : 0.f) - (held("up") ? 1.f : 0.f);
                    if (dx != 0.f || dy != 0.f) {
                        const float len = std::sqrt(dx * dx + dy * dy);
                        const float ux = dx / len, uy = dy / len;
                        const float r = m_em->transforms[pi].rotation * 3.14159f / 180.f;
                        const float lx = ux * std::cos(r) + uy * std::sin(r);
                        const float ly = -ux * std::sin(r) + uy * std::cos(r);   // forward is ly = -1
                        if (ly > 0.3f || std::fabs(lx) > 0.6f)
                            dir = 1 + static_cast<int>(dx + 1.f) + 3 * static_cast<int>(dy + 1.f);
                    }
                }
            }
            if (dir != 0 && dir != m_rcsDir) raise("player.rcs.puff", pos);
            m_rcsDir = dir;
        }

        // ---- Turbo: ignite, then the sustain takes over; release ----
        if (!was.turbo && now.turbo) { raise("player.turbo.ignite", pos, 1.f, 1.f, key(id, T_TURBO)); m_turboHeld = 0.f; }
        if (now.turbo) {
            m_turboHeld += dt;
            if (m_turboHeld > 0.9f) wantLoop(key(id, L_TURBO), "player.turbo.loop", pos);
        }
        if (was.turbo && !now.turbo) {
            stopKey(key(id, T_TURBO), 0.12f);
            raise("player.turbo.release", pos, m_turboHeld > 0.4f ? 1.f : 0.6f);
        }
        if (was.overheatTimer <= 0.f && now.overheatTimer > 0.f) raise("player.energy.dry", pos);

        // ---- Dash: an engine event, voiced by hull class ----
        if (was.dashTimer <= 0.f && now.dashTimer > 0.f) {
            const char* dash = "player.dash.medium";
            if (ps.kit.valid) {
                if (ps.kit.hullClass == ship::HullClass::Light) dash = "player.dash.light";
                else if (ps.kit.hullClass == ship::HullClass::Heavy) dash = "player.dash.heavy";
            }
            raise(dash, pos);
        }
        if (!was.phased && now.phased) raise("player.dash.phase", pos, 0.8f);

        // ---- Perfect dodge / perfect shoulder bash: a semitone up per chain ----
        if (now.verdict > was.verdict + 0.01f) {
            const float semis = static_cast<float>(std::clamp(ps.dodgeChain - 1, 0, 6));
            raise(ps.dodgeVerdictBash ? "player.bash.perfect" : "player.dodge.perfect", pos, 1.f,
                std::pow(2.f, semis / 12.f));
        }

        // ---- Parry raise and whiff (what it parried is raised by DamageSystem) ----
        if (!was.parrying && now.parrying) raise("player.parry.raise", pos);
        if (!was.whiff && now.whiff) raise("player.parry.whiff", pos);

        // ---- Heat and the vent ----
        if (!was.overheated && now.overheated) raise("player.heat.lock", pos);
        if (was.overheated && !now.overheated) raise("player.heat.unlock", pos);
        if (now.qte) {
            wantLoop(key(id, L_VENT), "player.vent.open", pos);
            if ((now.qteDir > 0.f) != (was.qteDir > 0.f)) raise("player.vent.tick", pos);
        }
        if (was.qte && !now.qte) {
            raise(ps.qteResult == 1 ? "player.vent.perfect" : ps.qteResult == 2 ? "player.vent.good"
                : "player.vent.miss", pos);
        }
        if (now.overdrive > 0.f) wantLoop(key(id, L_OVERDRIVE), "player.overdrive.loop", pos);
        if (was.overdrive > 0.f && now.overdrive <= 0.f) raise("player.overdrive.end", pos);

        // ---- Rift Bolt: the charge peaks on the release frame ----
        if (!was.riftCharging && now.riftCharging) {
            const float chargeTime = m_lua ? (*m_lua)["rift_charge_time"].get_or(0.6f) : 0.6f;
            raiseAligned("player.rift.charge", pos, chargeTime, key(id, T_RIFT));
        }
        if (!was.riftFlight && now.riftFlight) {
            stopKey(key(id, T_RIFT), 0.04f);
            raise("player.rift.fire", pos);
        }
        else if (was.riftCharging && !now.riftCharging) stopKey(key(id, T_RIFT), 0.10f);   // let go early
        if (now.riftFlight) {
            const size_t bi = m_em->getEntityIndex(ps.riftBoltEntityId);
            if (bi != (size_t)-1) wantLoop(key(id, L_RIFTBOLT), "player.rift.flight", m_em->transforms[bi].position);
        }
        if (was.riftCd > 0.f && now.riftCd <= 0.f) raise("player.rift.ready", pos);

        // ---- Poise and the hull ----
        if (was.stagger <= 0.f && now.stagger > 0.f) raise("player.poise.break", pos);
        else if (now.poiseHit > was.poiseHit + 0.01f) raise("player.poise.chip", pos);

        const float dmg = was.hp - now.hp;
        if (dmg > 0.5f && !raisedThisFrame("rock.hit.hull") && !raisedThisFrame("player.parry.ram_fail"))
            raise(dmg >= 30.f ? "player.hit.heavy" : dmg < 4.f ? "player.hit.minor" : "player.hit", pos);
        if (now.hp > 0.f && now.hp / now.maxHp < 0.25f) raise("player.hull.low", pos);   // min_interval paces it

        // ---- Scrap: each catch a touch higher across a burst ----
        if (now.scrap > was.scrap) {
            if (m_now - m_lastPickup > 0.6f) m_pickupRun = 0;
            ++m_pickupRun;
            m_lastPickup = m_now;
            raise("scrap.pickup", pos, 1.f, std::min(1.3f, 1.f + 0.025f * m_pickupRun));
        }
    }

    // ---- Enemies ----
    struct EnemySnap {
        uint32_t stamp = 0;
        EnemyState state = EnemyState::PATROL;
        AlertIcon icon = AlertIcon::None;
        RamState ram = RamState::None;
        BashState bash = BashState::None;
        FrenzyState frenzy = FrenzyState::None;
        DuelShift shift = DuelShift::None;
        DuelAttack attack = DuelAttack::None;
        bool dormant = false, telegraph = false, burst = false, storm = false, turretTele = false, duelTrick = false;
        uint8_t shotgun = 0, summon = 0, toss = 0;
        float stormRecover = 0.f, shotgunFlash = 0.f, stun = 0.f, stagger = 0.f, turretAngle = 0.f, feral = 0.f;
        float beep = 0.f;
        sf::Vector2f pos;
    };

    const std::string& archKey(uint8_t arch) const {
        static const std::string none;
        if (!m_registry) return none;
        const enemyarch::ArchetypeDef* a = m_registry->byId(arch);
        return a ? a->key : none;
    }

    static float enginePitch(const std::string& k) {
        if (k == "WARDOG") return 1.25f;
        if (k == "MANIAC") return 1.1f;
        if (k == "BERSERKER") return 0.9f;
        if (k == "BLOODSEEKER") return 0.85f;
        if (k == "BARGE") return 0.7f;
        return 1.f;
    }

    void watchEnemies(float dt) {
        ++m_stamp;
        bool anyFeral = false;
        struct Near { float d; uint32_t id; sf::Vector2f pos; float pitch; };
        std::vector<Near> near;

        for (size_t i = 0; i < m_em->enemies.size() && i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId) || !isBodyType(m_em->physics[i].bodyId, BodyType::Enemy)) continue;
            const EnemyComponent& ec = m_em->enemies[i];
            const uint32_t id = ec.entityId;
            const sf::Vector2f pos = m_em->transforms[i].position;
            const std::string& k = archKey(ec.archetype);

            EnemySnap now;
            now.stamp = m_stamp; now.state = ec.visualState; now.icon = ec.alertIcon;
            now.ram = ec.ramState; now.bash = ec.bashState; now.frenzy = ec.frenzyState;
            now.shift = ec.duelShift; now.attack = ec.duelAttack;
            now.dormant = ec.dormant; now.telegraph = ec.telegraphActive; now.burst = ec.burstCharging;
            now.storm = ec.stormActive; now.turretTele = ec.turretTelegraphActive; now.duelTrick = ec.duelTrick;
            now.shotgun = ec.shotgunState; now.summon = ec.summonState; now.toss = ec.tossState;
            now.stormRecover = ec.stormRecoverTimer; now.shotgunFlash = ec.shotgunFlash;
            now.stun = m_em->healths[i].stunTimer; now.stagger = ec.staggerTimer;
            now.turretAngle = ec.turretAngle; now.feral = ec.feralTimer; now.pos = pos;

            auto it = m_enemies.find(id);
            if (it == m_enemies.end()) { m_enemies.emplace(id, now); continue; }   // first sight: prime
            EnemySnap& was = it->second;
            now.beep = was.beep;

            if (!now.dormant && !now.feral) near.push_back({ 0.f, id, pos, enginePitch(k) });
            if (now.feral > 0.f) anyFeral = true;
            edges(was, now, ec, k, id, pos, dt, i);
            was = now;
        }
        for (auto it = m_enemies.begin(); it != m_enemies.end();)
            it = (it->second.stamp != m_stamp) ? m_enemies.erase(it) : std::next(it);

        // ---- Engines: the four nearest awake hulls carry a loop ----
        for (auto& n : near) { const sf::Vector2f d = n.pos - m_listener; n.d = d.x * d.x + d.y * d.y; }
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
        for (size_t n = 0; n < near.size() && n < 4; ++n)
            wantLoop(key(near[n].id, L_ENGINE), "enemy.engine.rakshari", near[n].pos, 1.f, near[n].pitch);

        // ---- Death-chaos: one bed for the whole pack ----
        if (anyFeral) wantLoop(key(0, L_FERAL), "feral.loop", m_listener);
        if (m_feral && !anyFeral) raise("feral.end", m_listener);
        m_feral = anyFeral;
    }

    void edges(const EnemySnap& was, EnemySnap& now, const EnemyComponent& ec, const std::string& k,
        uint32_t id, sf::Vector2f pos, float dt, size_t idx) {
        // ---- Perception ----
        if (was.dormant && !now.dormant)
            raiseAligned("tell.wake", pos, ec.wakeDuration > 0.f ? ec.wakeDuration : 0.45f);
        if (now.state != was.state) {
            if (now.state == EnemyState::ALERT) raise("enemy.alert", pos);
            else if (now.state == EnemyState::COMBAT) raise("enemy.combat", pos);
        }
        if (now.icon != was.icon && now.icon == AlertIcon::Lost) raise("enemy.lost", pos);
        if (was.stun <= 0.2f && now.stun > 0.2f) raise("enemy.stun", pos);
        if (was.stagger <= 0.f && now.stagger > 0.f) raise("enemy.tumble", pos);

        // ---- Shot telegraphs (the shot itself is heard by watchBullets) ----
        if (!was.telegraph && now.telegraph && k != "WARDOG")
            raiseAligned("tell.shot", pos, ec.telegraphDuration);
        if (!was.burst && now.burst) raise("tell.burst", pos);

        // ---- Raider: Bullet Storm ----
        if (!was.storm && now.storm) raise("raider.storm.spin", pos);
        if (now.storm) wantLoop(key(id, L_STORM), "raider.storm.loop", pos);
        if (was.stormRecover <= 0.f && now.stormRecover > 0.f) raise("raider.storm.recover", pos);

        // ---- Barge: turret, shotgun, summon ----
        if (!was.turretTele && now.turretTele)
            raiseAligned("tell.shot", pos, ec.turretTelegraphDuration, 0, 1.f, 0.72f);
        if (k == "BARGE" && dt > 0.f) {
            float da = std::fabs(now.turretAngle - was.turretAngle);
            if (da > 180.f) da = 360.f - da;
            const float rate = da / dt;
            if (rate > 15.f) wantLoop(key(id, L_TURRET), "barge.turret.loop", pos, std::clamp(rate / 78.f, 0.3f, 1.f));
        }
        if (was.shotgun == 0 && now.shotgun != 0)
            raiseAligned("barge.shotgun.charge", pos, std::max(0.05f, ec.shotgunDuration - 0.28f));
        if (now.shotgunFlash > was.shotgunFlash + 0.01f) raise("barge.shotgun.blast", pos);
        if (was.summon == 0 && now.summon != 0) raiseAligned("barge.summon", pos, ec.summonDuration);

        // ---- RAM (amber): the growl ends on the charge ----
        if (was.ram != RamState::Windup && now.ram == RamState::Windup) {
            const char* tell = (k == "BARGE" || ec.ramDuration >= 0.75f) ? "tell.ram.0_85"
                : (ec.ramDuration <= 0.45f) ? "tell.ram.reaim" : "tell.ram";
            raiseAligned(tell, pos, ec.ramDuration, key(id, T_RAM));
        }
        if (now.ram == RamState::Charge) wantLoop(key(id, L_RAM), "tell.ram.charge", pos);
        if (was.ram == RamState::Charge && now.ram != RamState::Charge && now.duelTrick)
            raise("bloodseeker.cancel", pos);   // the growl stops on the frame the lane drops

        // ---- BASH (cyan): the scrape peaks on the lunge ----
        if (was.bash != BashState::Windup && now.bash == BashState::Windup) {
            const float d = ec.bashDuration;
            const char* tell = (d < 0.355f) ? "tell.bash.0_34" : (d < 0.385f) ? "tell.bash.0_37" : "tell.bash";
            raiseAligned(tell, pos, d, key(id, T_BASH));
        }

        // ---- Bloodseeker ----
        if (now.shift != was.shift) {
            switch (now.shift) {
            case DuelShift::FeintBreak:      // the scrape stops dead; retros flare
                stopKey(key(id, T_BASH), 0.02f);
                raise("bloodseeker.feint", pos, 1.f, 1.f, 0, 0.185f);
                break;
            case DuelShift::DiveWindup:
                if (!now.duelTrick) raise("bloodseeker.dive", pos);
                break;
            case DuelShift::Disengage:  raise("bloodseeker.evade", pos, 0.7f); break;
            case DuelShift::ExecStrike: raise("tell.execute", pos); break;
            default: break;
            }
            if (was.shift == DuelShift::ExecStrike) raise("bloodseeker.execute.kill", pos);
        }
        if (now.attack != was.attack) {
            switch (now.attack) {
            case DuelAttack::LancerCharge: raiseAligned("tell.lance.charge", pos, ec.duelAtkDuration, key(id, T_LANCE)); break;
            case DuelAttack::LancerLock:   raise("tell.lance.lock", pos); break;
            case DuelAttack::LancerRecover:
                if (was.attack == DuelAttack::LancerLock) raise("tell.lance.fire", pos);
                break;
            case DuelAttack::ConeWindup:   raiseAligned("tell.cone", pos, ec.duelAtkDuration, key(id, T_CONE)); break;
            default: break;
            }
            // A stun or tumble cancels the attack outright: so does its sound.
            if (now.attack == DuelAttack::None) { stopKey(key(id, T_LANCE), 0.05f); stopKey(key(id, T_CONE), 0.05f); }
        }
        if (now.attack == DuelAttack::ConeFire) wantLoop(key(id, L_CONE), "bloodseeker.cone.loop", pos);
        // Parried, stunned or staggered out of a windup: the tell must not
        // keep promising an attack that is no longer coming.
        if (was.bash == BashState::Windup && now.bash == BashState::None) stopKey(key(id, T_BASH), 0.05f);
        if (was.ram == RamState::Windup && now.ram == RamState::None) stopKey(key(id, T_RAM), 0.05f);

        // ---- Maniac ----
        if (was.toss == 0 && now.toss != 0) raise("maniac.mine.toss", pos);
        if (was.frenzy != FrenzyState::Ignite && now.frenzy == FrenzyState::Ignite) {
            raiseAligned("maniac.ignite", pos, ec.frenzyTimer > 0.f ? ec.frenzyTimer : 0.8f);
            raise("voice.maniac.laugh", pos);   // ElevenLabs file; silent until it is in sounds.lua
        }
        if (now.frenzy == FrenzyState::Charge) {
            wantLoop(key(id, L_FRENZY), "maniac.charge.loop", pos);
            // One beep per blink: the same clock the renderer flashes on.
            now.beep += ec.frenzyBlinkHz * dt;
            if (now.beep >= 1.f) { now.beep -= std::floor(now.beep); raise("maniac.charge.beep", pos); }
        }
        (void)idx;
    }

    // ---- Projectiles ----
    struct BulletSnap {
        uint32_t stamp = 0;
        bool rocket = false, mine = false, armed = false;
        float track = 0.f, fuse = 0.f, life = 0.f;
        sf::Vector2f pos;
    };

    void watchBullets() {
        ++m_bulletStamp;
        m_shotOwners.clear();
        const size_t pi = m_em->getEntityIndex(m_playerId);
        const float heat = (pi != (size_t)-1 && pi < m_em->players.size())
            ? m_em->players[pi].weaponHeat / std::max(1.f, m_em->players[pi].maxWeaponHeat) : 0.f;

        int homing = 0;
        for (size_t i = 0; i < m_em->bullets.size() && i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId) || !isBodyType(m_em->physics[i].bodyId, BodyType::Bullet)) continue;
            const BulletComponent& b = m_em->bullets[i];
            const uint32_t id = b.entityId;
            const sf::Vector2f pos = m_em->transforms[i].position;

            BulletSnap now;
            now.stamp = m_bulletStamp; now.rocket = b.isRocket; now.mine = b.isMine; now.armed = b.mineArmed;
            now.track = b.trackTimer; now.fuse = b.mineFuse; now.life = b.lifetime; now.pos = pos;

            auto it = m_bullets.find(id);
            if (it == m_bullets.end()) {
                m_bullets.emplace(id, now);
                onSpawn(b, pos, heat);
                if (b.isMine && b.mineFuse > 0.f) startFuse(b, id, pos);
                continue;
            }
            BulletSnap& was = it->second;
            if (now.rocket) {
                wantLoop(key(id, L_ROCKET), "maniac.rocket.loop", pos);
                if (was.track > 0.f && now.track <= 0.f) raise("maniac.rocket.skid", pos);
            }
            if (now.mine) {
                if (!was.armed && now.armed) raise("maniac.mine.arm", pos);
                if (was.fuse <= 0.f && now.fuse > 0.f) startFuse(b, id, pos);
            }
            if (b.isReflected && homing < 3) { ++homing; wantLoop(key(id, L_HOMING), "player.homing.loop", pos); }
            was = now;
        }

        // ---- Gone: rockets and mines always end in a blast (or a fizzle) ----
        for (auto it = m_bullets.begin(); it != m_bullets.end();) {
            if (it->second.stamp == m_bulletStamp) { ++it; continue; }
            const BulletSnap& s = it->second;
            if (s.rocket) raise("maniac.rocket.blast", s.pos);
            else if (s.mine) {
                stopKey(key(it->first, T_FUSE), 0.02f);
                raise((s.fuse > 0.f || s.life > 0.3f) ? "maniac.mine.blast" : "maniac.mine.fizzle", s.pos);
            }
            it = m_bullets.erase(it);
        }
    }

    void startFuse(const BulletComponent& b, uint32_t id, sf::Vector2f pos) {
        const char* f = (b.mineFuseTime >= 1.8f) ? "maniac.mine.fuse" : "maniac.mine.fuse_lit";
        raiseAligned(f, pos, b.mineFuse, key(id, T_FUSE));   // the last beep lands on the blast
    }

    void onSpawn(const BulletComponent& b, sf::Vector2f pos, float heat) {
        if (!b.isEnemyBullet) {
            // Rift bolts are heard by watchPlayer; reflected rounds and
            // parried rockets by the parry that made them.
            if (b.isRiftBolt || b.isReflected || b.isRocket || b.isMine) return;
            // EntityFactory::createBullet (the player's gun) never sets an
            // owner: 0 IS the player here. Anything else non-enemy is not ours.
            if (b.ownerEntityId != 0 && b.ownerEntityId != m_playerId) return;
            const bool hot = heat > 0.6f
                && std::uniform_real_distribution<float>(0.6f, 1.f)(m_rng) < heat;
            raise(hot ? "player.plasma.fire_hot" : "player.plasma.fire", pos);
            return;
        }
        if (b.isRocket) { raise("maniac.rocket.launch", pos); return; }
        if (b.isMine) { if (!b.mineTossed) raise("maniac.mine.drop", pos); return; }

        // One shot sound per gun per frame: a fan of six or a shotgun of
        // eleven is one report, not a chord.
        if (!m_shotOwners.insert(b.ownerEntityId).second) return;
        const size_t oi = m_em->getEntityIndex(b.ownerEntityId);
        if (oi == (size_t)-1 || oi >= m_em->enemies.size()) { raise("raider.shot", pos); return; }
        const EnemyComponent& ec = m_em->enemies[oi];
        // These attacks are loops that already carry their own shots.
        if (ec.stormActive || ec.duelAttack == DuelAttack::ConeFire) return;
        const std::string& k = archKey(ec.archetype);
        if (k == "BARGE") {
            if (ec.shotgunFlash > 0.f || ec.shotgunState != 0) return;   // the blast is the sound
            raise(ec.turretMode == 1 ? "barge.fan" : "barge.slug", pos);
        }
        else if (k == "WARDOG")      raise("wardog.fire", pos);
        else if (k == "BERSERKER")   raise("berserker.burst.round", pos);
        else if (k == "MANIAC")      raise("maniac.scrapfire", pos);
        else if (k == "BLOODSEEKER") raise("raider.shot", pos, 0.8f, 1.15f);
        else                         raise("raider.shot", pos);
    }

    // ---- Field: the things that hum before they go off ----
    void watchField() {
        struct Hot { float d; uint32_t id; sf::Vector2f pos; bool metal; };
        std::vector<Hot> hot;
        int homing = 0;
        for (size_t i = 0; i < m_em->healths.size() && i < m_em->physics.size(); ++i) {
            if (!b2Body_IsValid(m_em->physics[i].bodyId) || !isBodyType(m_em->physics[i].bodyId, BodyType::Asteroid)) continue;
            const HealthComponent& h = m_em->healths[i];
            const sf::Vector2f pos = m_em->transforms[i].position;
            if ((h.isKineticWeapon || h.wasParryLaunched || h.isHoming) && homing < 3) {
                ++homing;
                wantLoop(key(h.entityId, L_HOMING), "player.homing.loop", pos);
            }
            if (!h.isExplosive) continue;
            const sf::Vector2f d = pos - m_player.pos;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y);
            if (dist < 500.f) hot.push_back({ dist, h.entityId, pos, h.metallic });
        }
        std::sort(hot.begin(), hot.end(), [](const Hot& a, const Hot& b) { return a.d < b.d; });
        for (size_t n = 0; n < hot.size() && n < 2; ++n)
            wantLoop(key(hot[n].id, L_FIELD), hot[n].metal ? "core.loop" : "rock.magma.loop",
                hot[n].pos, 1.f - hot[n].d / 500.f);
    }

    // ---- Rocks knocking into rocks: Box2D's begin-touch events ----
    // Read, not consumed: DamageSystem walks the same list. Valid until the
    // next b2World_Step, and the frame runs after the last step.
    void watchContacts() {
        if (!b2World_IsValid(m_worldId)) return;
        const b2ContactEvents ev = b2World_GetContactEvents(m_worldId);
        int n = 0;
        for (int i = 0; i < ev.beginCount && n < 2; ++i) {
            const b2ContactBeginTouchEvent& c = ev.beginEvents[i];
            if (!b2Shape_IsValid(c.shapeIdA) || !b2Shape_IsValid(c.shapeIdB)) continue;
            const b2BodyId a = b2Shape_GetBody(c.shapeIdA), b = b2Shape_GetBody(c.shapeIdB);
            if (!isBodyType(a, BodyType::Asteroid) || !isBodyType(b, BodyType::Asteroid)) continue;
            const b2Vec2 va = b2Body_GetLinearVelocity(a), vb = b2Body_GetLinearVelocity(b);
            const float rel = std::sqrt((va.x - vb.x) * (va.x - vb.x) + (va.y - vb.y) * (va.y - vb.y)) * SCALE;
            if (rel < 60.f) continue;   // resting contact and slow grinding: silence
            b2Vec2 p = c.manifold.pointCount > 0 ? c.manifold.points[0].point
                : b2MulSV(0.5f, b2Add(b2Body_GetPosition(a), b2Body_GetPosition(b)));
            raise("rock.collide", { p.x * SCALE, p.y * SCALE }, std::clamp(rel / 400.f, 0.35f, 1.f));
            ++n;
        }
    }

    // ---- Scrap running out: the cube's last blink ----
    // ScrapSystem removes a cube when age >= lifetime. No identity needed:
    // count the cubes in their final moment; a new one in that window plays.
    void watchScrap() {
        int dying = 0;
        sf::Vector2f at;
        for (const ScrapPickup& s : m_em->scrapPickups)
            if (s.age >= m_scrapLife - 0.15f) { ++dying; at = s.position; }
        if (dying > m_scrapDying) raise("scrap.expire", at);
        m_scrapDying = dying;
    }

    /// The zone's sentence, as a bed under everything. Follows the live zone
    /// (dev-menu switches included); unknown zones are simply quiet.
    void zoneAmbience() {
        const zonearch::ZoneDef* z = m_zone ? m_zone->def() : nullptr;
        const std::string k = z ? z->key : std::string("CASUAL");
        if (k == "RAKSHARI") {
            wantLoop(key(0, L_AMB), "amb.scrapyard", m_listener);
            wantLoop(key(1, L_AMB), "amb.citadel", m_listener, 0.6f);
        }
        else if (k == "CASUAL") wantLoop(key(0, L_AMB), "amb.open_space", m_listener);
    }

    // ========================================================================
    // STATE
    // ========================================================================
    zonearch::ZoneState* m_zone = nullptr;
    EntityManager* m_em = nullptr;
    sol::state* m_lua = nullptr;
    const enemyarch::EnemyRegistry* m_registry = nullptr;
    const sf::View* m_view = nullptr;
    uint32_t m_playerId = 0;
    b2WorldId m_worldId = b2_nullWorldId;

    std::unordered_map<std::string, Def> m_defs;
    std::unordered_map<std::string, std::unique_ptr<sf::SoundBuffer>> m_buffers;
    std::unordered_set<std::string> m_warned;
    bool m_loaded = false;

    std::array<Voice, VOICES> m_voices;
    std::vector<LoopReq> m_loopReq;
    struct Delayed { float at; SoundEvent e; };
    std::vector<Delayed> m_delayed;

    // Settings (sounds.lua `audio` table)
    float m_master = 0.8f, m_hearRadius = 1900.f, m_pauseGain = 0.45f, m_panWidth = 0.75f;
    // The player's levels (MACHINE RITES): never touched by a reload.
    float m_userMaster = 1.f;
    std::array<float, BUS_COUNT> m_userBus{ 1.f, 1.f, 1.f, 1.f };
    std::string m_assetRoot = "assets/";

    // Buses
    float m_now = 0.f, m_worldBus = 0.f, m_muffleV = 0.f, m_timeFactor = 1.f;
    std::atomic<float> m_muffle{ 0.f };
    sf::Vector2f m_listener{ 0.f, 0.f }, m_viewSize{ 1280.f, 720.f };
    std::mt19937 m_rng{ 0x5EED };

    // Watchers
    PlayerSnap m_player;
    bool  m_playerSeen = false;
    float m_turboHeld = 0.f, m_lastPickup = -10.f;
    int   m_pickupRun = 0;
    std::unordered_map<uint32_t, EnemySnap> m_enemies;
    std::unordered_map<uint32_t, BulletSnap> m_bullets;
    std::unordered_set<uint32_t> m_shotOwners;
    uint32_t m_stamp = 0, m_bulletStamp = 0;
    bool m_feral = false;
    float m_scrapLife = 30.f;
    int   m_scrapDying = 0;
    int   m_rcsDir = 0;     ///< which input combination the jets last puffed for
};
