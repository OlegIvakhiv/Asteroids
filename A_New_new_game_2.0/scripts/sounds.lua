-- ============================================================================
-- sounds.lua -- every sound in VOID HUNTER (read by AudioSystem, F5 reloads)
-- ============================================================================
--
-- A row:   ["id"] = { files = { ... }, gain, pitch_var, max_voices, priority,
--                     min_interval, loop, peak_at, time_scaled }
--
--   files         paths under assets/. Several = one is picked at random per play.
--   gain          0..1 balance against the rest. Files are all peak -1 dBFS;
--                 THIS is where loudness is decided.
--   pitch_var     +/- random pitch per play (0.08 = 8%). Kills the machine gun.
--   max_voices    how many of this sound at once. The next one restarts the oldest.
--   priority      1 = tells aimed at the player ... 5 = ambience. When all
--                 voices are busy, a new sound may only steal a LESS important one.
--   min_interval  seconds; a repeat inside this window is dropped.
--   loop          played while a system keeps asking for it (engines, turbo...).
--   peak_at       TELLS: the second in the file where the attack lands. The
--                 AudioSystem offsets or delays the sound so this moment falls
--                 on the frame the attack fires, whatever the windup is tuned
--                 to in Enemy.lua. Change the file -> update this number.
--   time_scaled   slides down with hitstop / slow-mo. Default: yes, except the
--                 player's rewards (parry, perfect dodge, vent), the terminal
--                 and ambience.
--
-- Missing ids are reported ONCE on the console ("[Audio] no sound 'x'"):
-- that is how a typo here shows up -- sol2's get_or would otherwise hide it.
-- ============================================================================

audio = {
    master      = 0.8,     -- the future MACHINE RITES volume slider
    hear_radius = 1900,    -- px from the camera centre; beyond it, silence
    pause_gain  = 0.45,    -- the frozen world under the pause screen (also muffled)
    pan_width   = 0.75,    -- 0 = mono, 1 = hard left/right at the screen edge
    root        = "assets/",
}

sounds = {

    -- Player · flight ---------------------------------------------------
    ["player.engine.loop"] = { files = { "sfx/player.engine.loop.wav" },
        gain = 0.10, max_voices = 1, priority = 5, loop = true },  -- Engine
    ["player.rcs.puff"] = { files = { "sfx/player.rcs.puff_1.wav", "sfx/player.rcs.puff_2.wav", "sfx/player.rcs.puff_3.wav" },
        gain = 0.50, pitch_var = 0.08, max_voices = 2, priority = 4, min_interval = 0.08 },  -- Side thrusters (strafe / reverse)
    ["player.turbo.ignite"] = { files = { "sfx/player.turbo.ignite.wav" },
        gain = 0.90, max_voices = 1, priority = 2 },  -- Turbo ignite
    ["player.turbo.loop"] = { files = { "sfx/player.turbo.loop.wav" },
        gain = 0.14, max_voices = 1, priority = 2, loop = true },  -- Turbo sustain
    ["player.turbo.release"] = { files = { "sfx/player.turbo.release.wav" },
        gain = 0.60, max_voices = 1, priority = 3 },  -- Turbo release
    ["player.energy.dry"] = { files = { "sfx/player.energy.dry.wav" },
        gain = 0.70, max_voices = 1, priority = 1, min_interval = 1.00 },  -- Energy dry
    ["player.dash.light"] = { files = { "sfx/player.dash.light.wav" },
        gain = 0.80, pitch_var = 0.05, max_voices = 2, priority = 2 },  -- Dash · light
    ["player.dash.medium"] = { files = { "sfx/player.dash.medium.wav" },
        gain = 0.80, pitch_var = 0.05, max_voices = 2, priority = 2 },  -- Dash · medium
    ["player.dash.heavy"] = { files = { "sfx/player.dash.heavy.wav" },
        gain = 0.85, pitch_var = 0.05, max_voices = 2, priority = 2 },  -- Dash · heavy
    ["player.dash.phase"] = { files = { "sfx/player.dash.phase.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Dash phase-through (slow-mo)
    ["player.dodge.perfect"] = { files = { "sfx/player.dodge.perfect.wav" },
        gain = 0.90, max_voices = 2, priority = 1 },  -- Perfect dodge (slow-mo snatch)

    -- Player · weapons --------------------------------------------------
    ["player.plasma.fire"] = { files = { "sfx/player.plasma.fire_1.wav", "sfx/player.plasma.fire_2.wav", "sfx/player.plasma.fire_3.wav" },
        gain = 0.70, pitch_var = 0.05, max_voices = 6, priority = 2 },  -- Plasma shot
    ["player.plasma.fire_hot"] = { files = { "sfx/player.plasma.fire_hot_1.wav", "sfx/player.plasma.fire_hot_2.wav" },
        gain = 0.70, pitch_var = 0.05, max_voices = 6, priority = 2 },  -- Plasma shot · hot
    ["player.plasma.hit_stone"] = { files = { "sfx/player.plasma.hit_stone_1.wav", "sfx/player.plasma.hit_stone_2.wav", "sfx/player.plasma.hit_stone_3.wav" },
        gain = 0.60, pitch_var = 0.10, max_voices = 4, priority = 3, min_interval = 0.03 },  -- Hit · stone
    ["player.plasma.hit_metal"] = { files = { "sfx/player.plasma.hit_metal_1.wav", "sfx/player.plasma.hit_metal_2.wav", "sfx/player.plasma.hit_metal_3.wav" },
        gain = 0.60, pitch_var = 0.08, max_voices = 4, priority = 3, min_interval = 0.03 },  -- Hit · metal
    ["player.heat.lock"] = { files = { "sfx/player.heat.lock.wav" },
        gain = 0.90, max_voices = 1, priority = 1 },  -- Coolant lock
    ["player.heat.unlock"] = { files = { "sfx/player.heat.unlock.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Heat unlock
    ["player.vent.open"] = { files = { "sfx/player.vent.open.wav" },
        gain = 0.23, max_voices = 1, priority = 2, loop = true },  -- Vent open (held)
    ["player.vent.tick"] = { files = { "sfx/player.vent.tick.wav" },
        gain = 0.25, max_voices = 1, priority = 4 },  -- Vent marker tick
    ["player.vent.perfect"] = { files = { "sfx/player.vent.perfect.wav" },
        gain = 0.90, max_voices = 1, priority = 1 },  -- Vent · PERFECT (Overdrive)
    ["player.vent.good"] = { files = { "sfx/player.vent.good.wav" },
        gain = 0.80, max_voices = 1, priority = 2 },  -- Vent · OK
    ["player.vent.miss"] = { files = { "sfx/player.vent.miss.wav" },
        gain = 0.80, max_voices = 1, priority = 2 },  -- Vent · MISSED
    ["player.overdrive.loop"] = { files = { "sfx/player.overdrive.loop.wav" },
        gain = 0.14, max_voices = 1, priority = 3, loop = true },  -- Overdrive (4 s)
    ["player.overdrive.end"] = { files = { "sfx/player.overdrive.end.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Overdrive end
    ["player.rift.charge"] = { files = { "sfx/player.rift.charge.wav" },
        gain = 0.85, max_voices = 1, priority = 2, peak_at = 2.60 },  -- Rift Bolt · charge
    -- UNUSED: the Rift fires itself the moment the charge completes, so there
    -- is no "held at full" state to loop over. Kept for a hold-to-aim Rift.
    ["player.rift.full"] = { files = { "sfx/player.rift.full.wav" },
        gain = 0.15, max_voices = 1, priority = 2, loop = true },  -- Rift Bolt · full charge (held)
    ["player.rift.fire"] = { files = { "sfx/player.rift.fire.wav" },
        gain = 0.90, max_voices = 1, priority = 2 },  -- Rift Bolt · fire
    ["player.rift.flight"] = { files = { "sfx/player.rift.flight.wav" },
        gain = 0.22, max_voices = 1, priority = 3, loop = true },  -- Rift Bolt · in flight
    ["player.rift.burst"] = { files = { "sfx/player.rift.burst.wav" },
        gain = 1.00, max_voices = 2, priority = 2 },  -- Rift · AIR BURST
    ["player.rift.hijack"] = { files = { "sfx/player.rift.hijack.wav" },
        gain = 1.00, max_voices = 2, priority = 2 },  -- Rift · KINETIC HIJACK
    ["player.rift.overload"] = { files = { "sfx/player.rift.overload.wav" },
        gain = 1.00, max_voices = 2, priority = 2 },  -- Rift · SYSTEMS OVERLOAD
    ["player.rift.ready"] = { files = { "sfx/player.rift.ready.wav" },
        gain = 0.35, max_voices = 1, priority = 4 },  -- Rift ready

    -- Player · defence --------------------------------------------------
    ["player.parry.raise"] = { files = { "sfx/player.parry.raise.wav" },
        gain = 0.60, max_voices = 1, priority = 2 },  -- Parry raise (field snaps on)
    ["player.parry.whiff"] = { files = { "sfx/player.parry.whiff.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Parry whiff
    ["player.parry.round"] = { files = { "sfx/player.parry.round_1.wav", "sfx/player.parry.round_2.wav" },
        gain = 0.85, pitch_var = 0.03, max_voices = 2, priority = 1 },  -- Parry · round reflected
    ["player.parry.bash"] = { files = { "sfx/player.parry.bash.wav" },
        gain = 1.00, max_voices = 2, priority = 1 },  -- Parry · bash
    ["player.parry.rock"] = { files = { "sfx/player.parry.rock.wav" },
        gain = 1.00, max_voices = 2, priority = 1 },  -- Parry · rock launched
    ["player.parry.rocket"] = { files = { "sfx/player.parry.rocket.wav" },
        gain = 0.90, max_voices = 2, priority = 1 },  -- Parry · rocket turned
    ["player.parry.bomb"] = { files = { "sfx/player.parry.bomb.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Parry · Maniac thrown
    ["player.parry.lance"] = { files = { "sfx/player.parry.lance.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Parry · Lancer reflected
    ["player.parry.ram_fail"] = { files = { "sfx/player.parry.ram_fail.wav" },
        gain = 0.90, max_voices = 1, priority = 1 },  -- Parry · wrong tool (field collapses)
    ["player.homing.loop"] = { files = { "sfx/player.homing.loop.wav" },
        gain = 0.17, max_voices = 3, priority = 4, loop = true },  -- Homing projectile
    ["player.kinetic.hit"] = { files = { "sfx/player.kinetic.hit_1.wav", "sfx/player.kinetic.hit_2.wav" },
        gain = 1.00, pitch_var = 0.05, max_voices = 3, priority = 2 },  -- Kinetic rock hit
    ["player.bash"] = { files = { "sfx/player.bash.wav" },
        gain = 0.90, pitch_var = 0.04, max_voices = 2, priority = 2 },  -- Shoulder bash
    ["player.bash.perfect"] = { files = { "sfx/player.bash.perfect.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- PERFECT SHOULDER BASH (slow-mo)
    ["player.ram.rock"] = { files = { "sfx/player.ram.rock.wav" },
        gain = 0.80, pitch_var = 0.06, max_voices = 2, priority = 3 },  -- Ram smashes rock
    ["player.hit.minor"] = { files = { "sfx/player.hit.minor_1.wav", "sfx/player.hit.minor_2.wav" },
        gain = 0.50, pitch_var = 0.08, max_voices = 2, priority = 2, min_interval = 0.06 },  -- Hull scrape
    ["player.hit"] = { files = { "sfx/player.hit_1.wav", "sfx/player.hit_2.wav", "sfx/player.hit_3.wav" },
        gain = 0.95, pitch_var = 0.05, max_voices = 2, priority = 1 },  -- Hull hit
    ["player.hit.heavy"] = { files = { "sfx/player.hit.heavy_1.wav", "sfx/player.hit.heavy_2.wav" },
        gain = 1.00, pitch_var = 0.04, max_voices = 2, priority = 1 },  -- Hull hit · heavy
    ["player.poise.chip"] = { files = { "sfx/player.poise.chip.wav" },
        gain = 0.40, pitch_var = 0.08, max_voices = 1, priority = 4 },  -- Poise drained (hull strain)
    ["player.poise.break"] = { files = { "sfx/player.poise.break.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Poise break · tumble
    ["player.hull.low"] = { files = { "sfx/player.hull.low.wav" },
        gain = 0.45, max_voices = 1, priority = 1, min_interval = 8.00 },  -- Hull low (heartbeat + muffled alarm)
    ["player.death"] = { files = { "sfx/player.death.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Hunter death

    -- Field objects -----------------------------------------------------
    ["rock.break.large"] = { files = { "sfx/rock.break.large_1.wav", "sfx/rock.break.large_2.wav" },
        gain = 0.90, pitch_var = 0.06, max_voices = 3, priority = 4, min_interval = 0.05 },  -- Rock break · large
    ["rock.break.medium"] = { files = { "sfx/rock.break.medium_1.wav", "sfx/rock.break.medium_2.wav" },
        gain = 0.80, pitch_var = 0.08, max_voices = 4, priority = 4, min_interval = 0.04 },  -- Rock break · medium
    ["rock.break.small"] = { files = { "sfx/rock.break.small_1.wav", "sfx/rock.break.small_2.wav", "sfx/rock.break.small_3.wav" },
        gain = 0.65, pitch_var = 0.10, max_voices = 4, priority = 4, min_interval = 0.03 },  -- Rock break · small
    ["rock.hit.hull"] = { files = { "sfx/rock.hit.hull.wav" },
        gain = 0.90, pitch_var = 0.05, max_voices = 2, priority = 2 },  -- Rock hits hull
    ["rock.collide"] = { files = { "sfx/rock.collide_1.wav", "sfx/rock.collide_2.wav", "sfx/rock.collide_3.wav" },
        gain = 0.35, pitch_var = 0.15, max_voices = 2, priority = 5, min_interval = 0.10 },  -- Rock knock
    ["rock.magma.explode"] = { files = { "sfx/rock.magma.explode.wav" },
        gain = 1.00, max_voices = 2, priority = 2 },  -- Magma rock explodes
    ["rock.magma.loop"] = { files = { "sfx/rock.magma.loop.wav" },
        gain = 0.11, max_voices = 2, priority = 5, loop = true },  -- Magma rock (near)
    ["salvage.break.small"] = { files = { "sfx/salvage.break.small_1.wav", "sfx/salvage.break.small_2.wav" },
        gain = 0.60, pitch_var = 0.10, max_voices = 4, priority = 4, min_interval = 0.03 },  -- Salvage break · small
    ["salvage.break.medium"] = { files = { "sfx/salvage.break.medium_1.wav", "sfx/salvage.break.medium_2.wav" },
        gain = 0.75, pitch_var = 0.08, max_voices = 3, priority = 4, min_interval = 0.04 },  -- Salvage break · pile
    ["salvage.break.large"] = { files = { "sfx/salvage.break.large_1.wav", "sfx/salvage.break.large_2.wav" },
        gain = 0.85, pitch_var = 0.06, max_voices = 2, priority = 4, min_interval = 0.05 },  -- Salvage break · large
    ["wreck.break"] = { files = { "sfx/wreck.break_1.wav", "sfx/wreck.break_2.wav" },
        gain = 0.90, pitch_var = 0.05, max_voices = 2, priority = 3 },  -- Wreck breaks open
    ["core.explode"] = { files = { "sfx/core.explode.wav" },
        gain = 1.00, max_voices = 2, priority = 2 },  -- Unstable core / reactor explodes
    ["core.loop"] = { files = { "sfx/core.loop.wav" },
        gain = 0.09, max_voices = 2, priority = 5, loop = true },  -- Reactor core hum (near)
    ["scrap.drop"] = { files = { "sfx/scrap.drop.wav" },
        gain = 0.60, pitch_var = 0.05, max_voices = 2, priority = 4 },  -- Scrap burst
    ["scrap.pickup"] = { files = { "sfx/scrap.pickup_1.wav", "sfx/scrap.pickup_2.wav", "sfx/scrap.pickup_3.wav" },
        gain = 0.40, pitch_var = 0.06, max_voices = 3, priority = 3, min_interval = 0.03 },  -- Scrap pickup (magnet catch)
    ["scrap.expire"] = { files = { "sfx/scrap.expire.wav" },
        gain = 0.35, max_voices = 1, priority = 5, min_interval = 0.20 },  -- Scrap expires

    -- Tell grammar ------------------------------------------------------
    ["tell.bash"] = { files = { "sfx/tell.bash.wav" },
        gain = 1.00, max_voices = 3, priority = 1, peak_at = 0.40 },  -- BASH windup (cyan) · 0.40 s
    ["tell.bash.0_37"] = { files = { "sfx/tell.bash.0_37.wav" },
        gain = 1.00, max_voices = 2, priority = 1, peak_at = 0.37 },  -- BASH windup · 0.37 s (Bloodseeker)
    ["tell.bash.0_34"] = { files = { "sfx/tell.bash.0_34.wav" },
        gain = 1.00, max_voices = 2, priority = 1, peak_at = 0.34 },  -- BASH windup · 0.34 s (Maniac panic)
    ["tell.ram"] = { files = { "sfx/tell.ram.wav" },
        gain = 1.00, max_voices = 3, priority = 1, peak_at = 0.60 },  -- RAM windup (amber) · 0.60 s
    ["tell.ram.reaim"] = { files = { "sfx/tell.ram.reaim.wav" },
        gain = 0.90, max_voices = 3, priority = 1, peak_at = 0.36 },  -- RAM re-aim per link · 0.36 s
    ["tell.ram.0_85"] = { files = { "sfx/tell.ram.0_85.wav" },
        gain = 1.00, max_voices = 2, priority = 1, peak_at = 0.85 },  -- RAM windup · 0.85 s (Barge)
    ["tell.ram.charge"] = { files = { "sfx/tell.ram.charge.wav" },
        gain = 0.25, max_voices = 3, priority = 1, loop = true },  -- RAM charge (held)
    ["tell.shot"] = { files = { "sfx/tell.shot.wav" },
        gain = 0.90, max_voices = 3, priority = 1, peak_at = 0.30 },  -- Aimed shot windup · 0.30 s
    ["tell.burst"] = { files = { "sfx/tell.burst.wav" },
        gain = 0.90, max_voices = 3, priority = 1 },  -- Charged burst windup · 0.45 s
    ["tell.cone"] = { files = { "sfx/tell.cone.wav" },
        gain = 0.90, max_voices = 1, priority = 1, peak_at = 0.75 },  -- Cone barrage windup · 0.75 s
    ["tell.lance.charge"] = { files = { "sfx/tell.lance.charge.wav" },
        gain = 0.90, max_voices = 1, priority = 1, peak_at = 0.90 },  -- Lancer charge (red lane) · 0.9 s
    ["tell.lance.lock"] = { files = { "sfx/tell.lance.lock.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Lancer LOCK · 0.28 s
    ["tell.lance.fire"] = { files = { "sfx/tell.lance.fire.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Lancer beam
    ["tell.execute"] = { files = { "sfx/tell.execute.wav" },
        gain = 0.70, max_voices = 1, priority = 2 },  -- Execution windup (gold)
    ["tell.wake"] = { files = { "sfx/tell.wake.wav" },
        gain = 1.00, max_voices = 2, priority = 1, peak_at = 0.44 },  -- Dormant ambush wakes · 0.45 s

    -- Enemies · shared --------------------------------------------------
    ["enemy.alert"] = { files = { "sfx/enemy.alert.wav" },
        gain = 0.55, max_voices = 1, priority = 2, min_interval = 1.00 },  -- ALERT "?" (contact ping)
    ["enemy.combat"] = { files = { "sfx/enemy.combat.wav" },
        gain = 0.80, max_voices = 1, priority = 1, min_interval = 1.50 },  -- COMBAT "!!!" (stinger)
    ["enemy.lost"] = { files = { "sfx/enemy.lost.wav" },
        gain = 0.35, max_voices = 1, priority = 4, min_interval = 2.00 },  -- Contact lost
    ["enemy.engine.rakshari"] = { files = { "sfx/enemy.engine.rakshari.wav" },
        gain = 0.07, max_voices = 6, priority = 5, loop = true },  -- Rakshari engine (positional)
    ["enemy.stun"] = { files = { "sfx/enemy.stun.wav" },
        gain = 0.60, max_voices = 3, priority = 3 },  -- Enemy stunned
    ["enemy.tumble"] = { files = { "sfx/enemy.tumble.wav" },
        gain = 0.50, pitch_var = 0.08, max_voices = 3, priority = 4 },  -- Enemy staggered
    ["enemy.death"] = { files = { "sfx/enemy.death_1.wav", "sfx/enemy.death_2.wav", "sfx/enemy.death_3.wav" },
        gain = 0.90, pitch_var = 0.06, max_voices = 3, priority = 3 },  -- Enemy death
    ["enemy.death.visceral"] = { files = { "sfx/enemy.death.visceral_1.wav", "sfx/enemy.death.visceral_2.wav" },
        gain = 1.00, pitch_var = 0.05, max_voices = 2, priority = 3 },  -- Enemy death · visceral

    -- Rakshari ----------------------------------------------------------
    ["wardog.fire"] = { files = { "sfx/wardog.fire_1.wav", "sfx/wardog.fire_2.wav", "sfx/wardog.fire_3.wav" },
        gain = 0.45, pitch_var = 0.10, max_voices = 4, priority = 4, min_interval = 0.04 },  -- Wardog chaff fire
    ["raider.shot"] = { files = { "sfx/raider.shot_1.wav", "sfx/raider.shot_2.wav" },
        gain = 0.75, pitch_var = 0.06, max_voices = 4, priority = 3 },  -- Raider shot
    ["raider.storm.spin"] = { files = { "sfx/raider.storm.spin.wav" },
        gain = 0.80, max_voices = 2, priority = 2 },  -- Bullet Storm spin-up
    ["raider.storm.loop"] = { files = { "sfx/raider.storm.loop.wav" },
        gain = 0.36, max_voices = 2, priority = 2, loop = true },  -- Bullet Storm spray
    ["raider.storm.recover"] = { files = { "sfx/raider.storm.recover.wav" },
        gain = 0.70, max_voices = 2, priority = 2 },  -- Bullet Storm recovery (punish!)
    ["barge.turret.loop"] = { files = { "sfx/barge.turret.loop.wav" },
        gain = 0.11, max_voices = 2, priority = 3, loop = true },  -- Barge turret traverse
    ["barge.slug"] = { files = { "sfx/barge.slug_1.wav", "sfx/barge.slug_2.wav" },
        gain = 0.95, pitch_var = 0.04, max_voices = 2, priority = 2 },  -- Barge heavy slug
    ["barge.fan"] = { files = { "sfx/barge.fan.wav" },
        gain = 0.90, max_voices = 2, priority = 2 },  -- Barge fan burst
    ["barge.shotgun.charge"] = { files = { "sfx/barge.shotgun.charge.wav" },
        gain = 0.90, max_voices = 1, priority = 1, peak_at = 0.57 },  -- Barge shotgun charge (lock at end)
    ["barge.shotgun.blast"] = { files = { "sfx/barge.shotgun.blast.wav" },
        gain = 1.00, max_voices = 1, priority = 2 },  -- Barge shotgun blast
    ["barge.summon"] = { files = { "sfx/barge.summon.wav" },
        gain = 0.90, max_voices = 1, priority = 2, peak_at = 1.00 },  -- Barge summons Wardogs
    ["barge.death"] = { files = { "sfx/barge.death.wav" },
        gain = 1.00, max_voices = 1, priority = 2 },  -- Barge death
    ["berserker.burst.round"] = { files = { "sfx/berserker.burst.round_1.wav", "sfx/berserker.burst.round_2.wav" },
        gain = 0.60, pitch_var = 0.06, max_voices = 3, priority = 3 },  -- Berserker burst round (×3)
    ["maniac.scrapfire"] = { files = { "sfx/maniac.scrapfire_1.wav", "sfx/maniac.scrapfire_2.wav" },
        gain = 0.50, pitch_var = 0.15, max_voices = 3, priority = 4 },  -- Maniac scrapfire
    ["maniac.rocket.launch"] = { files = { "sfx/maniac.rocket.launch_1.wav", "sfx/maniac.rocket.launch_2.wav" },
        gain = 0.80, pitch_var = 0.06, max_voices = 3, priority = 2 },  -- Rocket launch
    ["maniac.rocket.loop"] = { files = { "sfx/maniac.rocket.loop.wav" },
        gain = 0.18, max_voices = 4, priority = 3, loop = true },  -- Rocket in flight
    ["maniac.rocket.skid"] = { files = { "sfx/maniac.rocket.skid.wav" },
        gain = 0.80, max_voices = 3, priority = 1 },  -- Rocket SKID (stops tracking)
    ["maniac.rocket.blast"] = { files = { "sfx/maniac.rocket.blast_1.wav", "sfx/maniac.rocket.blast_2.wav" },
        gain = 0.85, pitch_var = 0.06, max_voices = 3, priority = 3 },  -- Rocket blast
    ["maniac.mine.drop"] = { files = { "sfx/maniac.mine.drop_1.wav", "sfx/maniac.mine.drop_2.wav" },
        gain = 0.50, pitch_var = 0.10, max_voices = 3, priority = 4, min_interval = 0.03 },  -- Mine drop (×7 in a carpet)
    ["maniac.mine.arm"] = { files = { "sfx/maniac.mine.arm.wav" },
        gain = 0.30, max_voices = 2, priority = 4, min_interval = 0.05 },  -- Mine armed
    ["maniac.mine.fuse"] = { files = { "sfx/maniac.mine.fuse.wav" },
        gain = 0.70, max_voices = 3, priority = 1, peak_at = 2.00 },  -- Mine fuse · 2 s
    ["maniac.mine.fuse_lit"] = { files = { "sfx/maniac.mine.fuse_lit.wav" },
        gain = 0.70, max_voices = 3, priority = 1, peak_at = 1.40 },  -- Mine fuse · 1.4 s (tossed)
    ["maniac.mine.blast"] = { files = { "sfx/maniac.mine.blast_1.wav", "sfx/maniac.mine.blast_2.wav" },
        gain = 0.85, pitch_var = 0.06, max_voices = 3, priority = 3 },  -- Mine blast
    ["maniac.mine.fizzle"] = { files = { "sfx/maniac.mine.fizzle.wav" },
        gain = 0.30, max_voices = 2, priority = 5 },  -- Mine fizzles out
    ["maniac.mine.toss"] = { files = { "sfx/maniac.mine.toss.wav" },
        gain = 0.80, max_voices = 1, priority = 1 },  -- Mine toss (spin, 6 mines)
    ["maniac.ignite"] = { files = { "sfx/maniac.ignite.wav" },
        gain = 1.00, max_voices = 1, priority = 1, peak_at = 0.80 },  -- Maniac IGNITE · 0.8 s (+ laugh voice)
    ["maniac.charge.beep"] = { files = { "sfx/maniac.charge.beep.wav" },
        gain = 0.60, max_voices = 1, priority = 1 },  -- Suicide charge beep (rate = distance)
    ["maniac.charge.loop"] = { files = { "sfx/maniac.charge.loop.wav" },
        gain = 0.22, max_voices = 1, priority = 1, loop = true },  -- Suicide charge flame
    ["maniac.detonate"] = { files = { "sfx/maniac.detonate.wav" },
        gain = 1.00, max_voices = 1, priority = 2 },  -- Maniac suicide blast
    ["bloodseeker.feint"] = { files = { "sfx/bloodseeker.feint.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Bloodseeker FEINT (bash cut + retro flare)
    ["bloodseeker.cancel"] = { files = { "sfx/bloodseeker.cancel.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Bloodseeker RAM CANCEL (cut + brake)
    ["bloodseeker.dive"] = { files = { "sfx/bloodseeker.dive.wav" },
        gain = 0.90, max_voices = 1, priority = 2 },  -- Blood Dive (flare + rush)
    ["bloodseeker.cone.loop"] = { files = { "sfx/bloodseeker.cone.loop.wav" },
        gain = 0.32, max_voices = 1, priority = 2, loop = true },  -- Suppression cone barrage
    ["bloodseeker.evade"] = { files = { "sfx/bloodseeker.evade.wav" },
        gain = 0.40, pitch_var = 0.10, max_voices = 2, priority = 4, min_interval = 0.08 },  -- Bloodseeker perfect-dodges a bolt
    ["bloodseeker.execute.kill"] = { files = { "sfx/bloodseeker.execute.kill.wav" },
        gain = 0.90, max_voices = 1, priority = 2 },  -- Execution kill + pack buff surge
    ["bloodseeker.death"] = { files = { "sfx/bloodseeker.death.wav" },
        gain = 1.00, max_voices = 1, priority = 1 },  -- Bloodseeker death → death-chaos
    ["feral.loop"] = { files = { "sfx/feral.loop.wav" },
        gain = 0.20, max_voices = 1, priority = 2, loop = true },  -- Death-chaos FERAL (10 s bed)
    ["feral.end"] = { files = { "sfx/feral.end.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Feral ends

    -- Terminal ----------------------------------------------------------
    ["term.boot.power"] = { files = { "sfx/term.boot.power.wav" },
        gain = 0.80, max_voices = 1, priority = 2 },  -- Boot · power on (CRT)
    ["term.boot.line"] = { files = { "sfx/term.boot.line_1.wav", "sfx/term.boot.line_2.wav", "sfx/term.boot.line_3.wav" },
        gain = 0.35, pitch_var = 0.05, max_voices = 2, priority = 4 },  -- Boot · teletype line
    ["term.boot.roused"] = { files = { "sfx/term.boot.roused.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- Boot · MACHINE SPIRIT ROUSED
    ["term.boot.greeting"] = { files = { "sfx/term.boot.greeting.wav" },
        gain = 0.60, max_voices = 1, priority = 3 },  -- Boot · GREETINGS, HUNTER
    ["term.hover"] = { files = { "sfx/term.hover.wav" },
        gain = 0.25, pitch_var = 0.04, max_voices = 2, priority = 4, min_interval = 0.02 },  -- Selection tick
    ["term.select"] = { files = { "sfx/term.select.wav" },
        gain = 0.60, max_voices = 1, priority = 3 },  -- Route chosen (relay clack)
    ["term.page.open"] = { files = { "sfx/term.page.open.wav" },
        gain = 0.40, max_voices = 1, priority = 4 },  -- Page open (CRT line expands)
    ["term.page.close"] = { files = { "sfx/term.page.close.wav" },
        gain = 0.40, max_voices = 1, priority = 4 },  -- Page fold
    ["term.sealed"] = { files = { "sfx/term.sealed.wav" },
        gain = 0.50, max_voices = 1, priority = 3, min_interval = 0.20 },  -- SEALED · denied
    ["term.engage"] = { files = { "sfx/term.engage.wav" },
        gain = 0.90, max_voices = 1, priority = 1 },  -- ENGAGE THE HUNT
    ["term.pause"] = { files = { "sfx/term.pause.wav" },
        gain = 0.50, max_voices = 1, priority = 3 },  -- HOLD — RUN SUSPENDED
    ["term.gameover"] = { files = { "sfx/term.gameover.wav" },
        gain = 0.70, max_voices = 1, priority = 1 },  -- BEACON SILENT (flatline)
    ["term.feed.lost"] = { files = { "sfx/term.feed.lost.wav" },
        gain = 0.20, max_voices = 1, priority = 4 },  -- Feed · SIGNAL LOST
    ["term.codex.entry"] = { files = { "sfx/term.codex.entry.wav" },
        gain = 0.70, max_voices = 1, priority = 2, min_interval = 1.00 },  -- Codex · new entry
    ["term.refit.mount"] = { files = { "sfx/term.refit.mount.wav" },
        gain = 0.50, pitch_var = 0.05, max_voices = 1, priority = 4 },  -- Refit · mount fitted
    ["term.intercept.bed"] = { files = { "sfx/term.intercept.bed.wav" },
        gain = 0.08, max_voices = 1, priority = 5, loop = true },  -- Intercept stream static
    ["term.intercept.signal"] = { files = { "sfx/term.intercept.signal.wav" },
        gain = 0.15, max_voices = 1, priority = 5 },  -- Intercept · signal line (wrong tone)
    ["term.corruption.caution"] = { files = { "sfx/term.corruption.caution.wav" },
        gain = 0.12, max_voices = 1, priority = 3, loop = true },  -- VOID CORRUPTION ≥ 70 %

    -- Zone ambience -----------------------------------------------------
    ["amb.open_space"] = { files = { "sfx/amb.open_space.wav" },
        gain = 0.14, max_voices = 1, priority = 5, loop = true },  -- Open Space · nothing happened here
    ["amb.scrapyard"] = { files = { "sfx/amb.scrapyard.wav" },
        gain = 0.28, max_voices = 1, priority = 5, loop = true },  -- Rakshari Scrapyard · everything that worked has been taken
    ["amb.citadel"] = { files = { "sfx/amb.citadel.wav" },
        gain = 0.11, max_voices = 1, priority = 5, loop = true },  -- Rust Citadel furnace (far)

    -- Voices (ElevenLabs) ------------------------------------------------------
    -- Add the files to assets/sfx and uncomment. Until then the Maniac's
    -- ignite plays without its laugh, and the console says so once.
    -- ["voice.maniac.laugh"] = { files = { "sfx/voice.maniac.laugh_1.wav", "sfx/voice.maniac.laugh_2.wav" },
    --     gain = 0.9, pitch_var = 0.04, max_voices = 1, priority = 1 },
}

-- Same files, different events.
sounds["enemy.hit"]          = sounds["player.plasma.hit_metal"]
sounds["hud.perfect_dodge"]  = sounds["player.dodge.perfect"]
sounds["term.refit.refused"] = sounds["term.sealed"]
