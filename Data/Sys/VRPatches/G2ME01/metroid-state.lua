-- Metroid Prime 2: Echoes (NTSC-U, GameCube): the game state the VR profile's overrides follow.
--
-- Raises override flags every field, from the game's memory, for the entries of the Sys profile
-- (GameSettingsVR/G2M.ini) to condition on:
--   mp_visor_combat, mp_visor_echo, mp_visor_scan, mp_visor_dark   the visor in play (not raised
--                                                                    on a pause screen)
--   mp_paused       a pause screen: the map, the data network (logbook, inventory, options)
--   mp_morphball    in morph ball, morphing or unmorphing
--   mp_cinematic    the camera is neither first person nor the morph ball's (a cutscene)
-- The chains come from PrimeHack's tables (shiiion/dolphin, PRIME_2_GCN: state manager, player,
-- player state, menu state, camera and ball states) and were checked in the game: the player
-- state's word 0x30 is the visor (0 combat, 1 echo, 2 scan, 3 dark), the state manager's word
-- 0x2470 the pause screen (0 none, 1 map, 3 data network), the camera state 0 in first person,
-- 2 in morph ball and 4 while morphing (when the ball state is 2 or 3).
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Game state for the VR profile",
  author = "DolphinXR, with PrimeHack's addresses",
  version = "1.0",
  description = "Reads the current visor, the pause screens and morph ball from the game, so "
    .. "that the profile's VR overrides follow them: the scan, echo and dark visor effects, the "
    .. "map and the pause screens.",
  games = {"G2ME01"},
  revision = 0,
}

local STATE_MANAGER = 0x803DB6E0
local VISORS = {[0] = "mp_visor_combat", "mp_visor_echo", "mp_visor_scan", "mp_visor_dark"}

-- A word behind a pointer, or nil when the pointer is null or outside RAM.
local function read_at(pointer, offset)
  if not pointer or pointer == 0 then
    return nil
  end
  return mem.u32(pointer + offset)
end

function on_frame()
  local menu = mem.u32(STATE_MANAGER + 0x2470)
  local player = mem.u32(STATE_MANAGER + 0x14FC)
  local visor = read_at(read_at(player, 0x1314), 0x30)
  local camera = read_at(player, 0x388)
  local ball = read_at(player, 0x38C)
  local paused = menu ~= nil and menu ~= 0
  local in_play = player ~= nil and player ~= 0 and not paused

  for id = 0, 3 do
    vr.set_flag(VISORS[id], in_play and visor == id)
  end
  vr.set_flag("mp_paused", paused)
  vr.set_flag("mp_morphball", in_play and ball ~= nil and ball ~= 0)
  vr.set_flag("mp_cinematic", in_play and ball == 0 and camera ~= nil and camera ~= 0)
end
