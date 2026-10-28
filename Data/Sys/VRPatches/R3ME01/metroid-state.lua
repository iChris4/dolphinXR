-- Metroid Prime Trilogy (NTSC-U, Wii): the game state the VR profile's overrides follow.
--
-- Raises override flags every field, from the game's memory, for the entries of the Sys profile
-- (GameSettingsVR/R3ME01.ini) to condition on:
--   mp_game_menu, mp_game_prime, mp_game_echoes, mp_game_corruption
--                   which program runs: the Trilogy's menus or one of the three games
--   mp_visor_combat, mp_visor_scan, and per game mp_visor_xray and mp_visor_thermal (Prime),
--   mp_visor_echo and mp_visor_dark (Echoes), mp_visor_command and mp_visor_xray (Corruption)
--                   the visor in play (not raised while paused); while Prime switches visors,
--                   both the one fading out and the next one
--   mp_paused       the pause screens (the maps, the menus), Prime's save station dialogs and
--                   Corruption's pointer dialogs
--   mp_morphball    in morph ball
--   mp_cinematic    the camera is not the player's own (cutscenes, elevators, talking to someone)
-- Each flag also exists with the game's prefix (mp1_, mp2_ or mp3_ in place of mp_, for example
-- mp3_paused), for the entries of one game only.
--
-- Each game of the disc is its own program, recognized by an instruction of its arm cannon code
-- (the sites of arm-cannon.lua). The state managers, the players, their camera and ball states,
-- their powerups and the visors, the menu states and Corruption's pointer dialog byte come from
-- PrimeHack's tables (shiiion/dolphin, PRIME_1, PRIME_2, PRIME_3). Checked in the game: Prime's
-- menu state is 1 on the map, 2 on the pause screen (4 and 5 at a save station), its camera state
-- 4 in cutscenes and elevators; Echoes' menu state 1 on the map and 3 on the pause screen, its
-- camera state 5 in cutscenes; Corruption's menu state 1 on the map and 3 on the galaxy map, its
-- player's word 0x3C3C 1 in first person and 4 in cutscenes and conversations. As in Metroid
-- Prime 3 (RM3E01), any other menu state of Corruption is a screen of the game too (the map a hint
-- opens, a save station's prompt and notice, an objective notice: not seen here in the Trilogy).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Game state for the VR profile",
  author = "DolphinXR, with PrimeHack's addresses",
  version = "1.2",
  description = "Reads which game of the trilogy runs, the current visor, the pause screens, "
    .. "morph ball and cutscenes from the game, so that the profile's VR overrides follow them: "
    .. "the visors, the maps and the pause screens of the three games.",
  games = {"R3ME01"},
  revision = 0,
}

-- A word behind a pointer, or nil when the pointer is null or outside RAM.
local function read_at(pointer, offset)
  if not pointer or pointer == 0 then
    return nil
  end
  return mem.u32(pointer + offset)
end

local GAMES = {
  {
    name = "prime",
    prefix = "mp1_",
    -- arm-cannon.lua's first site: the hook (its own word, or the patch's branch) and the next
    -- instruction.
    hook = 0x8018BC74, original = 0x387904B0, after = 0x38810380,
    visors = {[0] = "visor_combat", "visor_xray", "visor_scan", "visor_thermal"},
    read = function()
      local state_manager = 0x804BF420
      local player = mem.u32(state_manager + 0x84C)
      local menu = mem.u32(state_manager + 0x117C)
      local powerups = mem.u32(0x804BFCD4)
      return {
        player = player,
        -- The visor on screen, and the one it switches to: when the X-ray visor is dismissed, the
        -- next visor is combat while the X-ray view fades out for a few frames.
        visor = read_at(powerups, 0x18),
        next_visor = read_at(powerups, 0x1C),
        ball = read_at(player, 0x2F4),
        paused = menu == 1 or menu == 2 or menu == 4 or menu == 5,
        cinematic = read_at(player, 0x2F0) == 4,
      }
    end,
  },
  {
    name = "echoes",
    prefix = "mp2_",
    hook = 0x8018ACB0, original = 0xC00102A0, after = 0x387C03B0,
    visors = {[0] = "visor_combat", "visor_echo", "visor_scan", "visor_dark"},
    read = function()
      local state_manager = 0x804E72E8
      local player = mem.u32(state_manager + 0x14F4)
      local menu = mem.u32(state_manager + 0x2C0C)
      return {
        player = player,
        visor = read_at(read_at(player, 0x12EC), 0x34),
        ball = read_at(player, 0x378),
        paused = menu ~= nil and menu ~= 0,
        cinematic = read_at(player, 0x374) == 5,
      }
    end,
  },
  {
    name = "corruption",
    prefix = "mp3_",
    hook = 0x80183C28, original = 0xC0010208, after = 0x7F83E378,
    visors = {[0] = "visor_combat", "visor_scan", "visor_command", "visor_xray"},
    read = function()
      local state_manager = 0x805C6C68
      local player = read_at(mem.u32(state_manager), 0x2184)
      local menu = mem.u32(state_manager + 0x32C)
      local dialog = mem.u8(0x805C8D77)
      return {
        player = player,
        visor = read_at(read_at(player, 0x35A8), 0x34),
        ball = read_at(player, 0x358),
        paused = (menu ~= nil and menu ~= 0) or (dialog ~= nil and dialog ~= 0),
        cinematic = read_at(player, 0x3C3C) == 4,
      }
    end,
  },
}

-- The flags a game can raise besides its visors, without their prefix.
local STATES = {"paused", "morphball", "cinematic"}

-- Whether g's program is in memory.
local function present(g)
  if mem.u32(g.hook + 4) ~= g.after then
    return false
  end
  local word = mem.u32(g.hook)
  return word == g.original or (word ~= nil and word & 0xFC000003 == 0x48000000)
end

-- The mp_ flags of this field, filled from the running game's: every name any game raises.
local shared = {}
for _, g in ipairs(GAMES) do
  for id = 0, 3 do
    shared[g.visors[id]] = false
  end
end
for _, name in ipairs(STATES) do
  shared[name] = false
end

function on_frame()
  for name in pairs(shared) do
    shared[name] = false
  end
  local menu = true
  for _, g in ipairs(GAMES) do
    local running = menu and present(g)
    vr.set_flag("mp_game_" .. g.name, running)
    local s = running and g.read() or {}
    local in_play = s.player ~= nil and s.player ~= 0 and not s.paused
    for id = 0, 3 do
      local active = in_play and (s.visor == id or s.next_visor == id)
      vr.set_flag(g.prefix .. g.visors[id], active)
      shared[g.visors[id]] = shared[g.visors[id]] or active
    end
    local values = {paused = s.paused == true,
                    morphball = in_play and s.ball ~= nil and s.ball ~= 0,
                    cinematic = in_play and s.cinematic == true}
    for _, name in ipairs(STATES) do
      vr.set_flag(g.prefix .. name, values[name])
      shared[name] = shared[name] or values[name]
    end
    if running then
      menu = false
    end
  end
  vr.set_flag("mp_game_menu", menu)
  for name, active in pairs(shared) do
    vr.set_flag("mp_" .. name, active)
  end
end
