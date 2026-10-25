-- Metroid Prime 3: Corruption (NTSC-U, Wii): the game state the VR profile's overrides follow.
--
-- Raises override flags every field, from the game's memory, for the entries of the Sys profile
-- (GameSettingsVR/RM3E01.ini) to condition on:
--   mp_visor_combat, mp_visor_scan, mp_visor_command, mp_visor_xray   the visor in play (not
--                                                                       raised while paused)
--   mp_paused       the pause screens (the maps, the menus) and the game's pointer dialogs
--   mp_morphball    in morph ball
--   mp_cinematic    the camera is not the player's own (cutscenes, talking to someone)
-- The state manager, the player, its ball state, its powerups and their visor (0 combat, 1 scan,
-- 2 command, 3 X-ray), the menu state and the pointer dialog byte come from PrimeHack's tables
-- (shiiion/dolphin, PRIME_3_STANDALONE). The rest was found in the game: the menu state is 1 on
-- the map and 3 while the solar system map animates; the player's word 0x3C2C is 1 in first
-- person and 4 when a cutscene camera or a conversation takes over.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Game state for the VR profile",
  author = "DolphinXR, with PrimeHack's addresses",
  version = "1.0",
  description = "Reads the current visor, the pause screens, morph ball and cutscenes from the "
    .. "game, so that the profile's VR overrides follow them: the command visor, the maps and "
    .. "the pause screens.",
  games = {"RM3E01"},
  revision = 0,
}

local STATE_MANAGER = 0x805C4F98  -- -> the state manager
local MENU_STATE = STATE_MANAGER + 0x32C
local POINTER_DIALOG = 0x805C70C7  -- byte: the pointer drives a dialog or a menu
local VISORS = {[0] = "mp_visor_combat", "mp_visor_scan", "mp_visor_command", "mp_visor_xray"}
local CAMERA_CINEMATIC = 4

-- A word behind a pointer, or nil when the pointer is null or outside RAM.
local function read_at(pointer, offset)
  if not pointer or pointer == 0 then
    return nil
  end
  return mem.u32(pointer + offset)
end

function on_frame()
  local menu = mem.u32(MENU_STATE)
  local dialog = mem.u8(POINTER_DIALOG)
  local player = read_at(mem.u32(STATE_MANAGER), 0x2184)
  local visor = read_at(read_at(player, 0x35A0), 0x34)
  local ball = read_at(player, 0x358)
  local camera = read_at(player, 0x3C2C)
  local paused = menu == 1 or menu == 3 or (dialog ~= nil and dialog ~= 0)
  local in_play = player ~= nil and player ~= 0 and not paused

  for id = 0, 3 do
    vr.set_flag(VISORS[id], in_play and visor == id)
  end
  vr.set_flag("mp_paused", paused)
  vr.set_flag("mp_morphball", in_play and ball ~= nil and ball ~= 0)
  vr.set_flag("mp_cinematic", in_play and camera == CAMERA_CINEMATIC)
end
