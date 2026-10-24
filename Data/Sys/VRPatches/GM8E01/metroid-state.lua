-- Metroid Prime (NTSC-U, GameCube, 1.00 to 1.02): the game state the VR profile's overrides
-- follow.
--
-- Raises override flags every field, from the game's memory, for the entries of the Sys profile
-- (GameSettingsVR/GM8.ini) to condition on:
--   mp_visor_combat, mp_visor_xray, mp_visor_scan, mp_visor_thermal   the visor in play (not
--                                                                       raised while paused)
--   mp_paused       the map, the pause screens and the game's messages
--   mp_morphball    in morph ball, morphing or unmorphing
--   mp_cinematic    the player's camera is a spawned one (a cutscene)
-- The state manager's address and the player's camera and ball fields per revision come from
-- PrimeHack's tables (shiiion/dolphin, PRIME_1_GCN, _R1, _R2); the player state and its visor from
-- the decompilation (CStateManager::x8b8_playerState, CPlayerState::x14_currentVisor: 0 combat,
-- 1 X-ray, 2 scan, 3 thermal; CPlayer::EPlayerCameraState 4 = spawned). The pause screens were
-- found in the game: the state manager's deferred transition (0xF90) is 1 on the map and 2 on the
-- pause screens while they are open.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "Game state for the VR profile",
  author = "DolphinXR, with PrimeHack's addresses",
  version = "1.0",
  description = "Reads the current visor, the map and pause screens, morph ball and cutscenes "
    .. "from the game, so that the profile's VR overrides follow them: the scan and thermal "
    .. "visors, the 3D HUD, the map and the pause screens.",
  games = {"GM8E01"},
}

-- Per disc revision: the state manager and the player's camera and ball state fields.
local REVISIONS = {
  [0] = {state_manager = 0x8045A1A8, camera = 0x2F4, ball = 0x2F8},
  [1] = {state_manager = 0x8045A388, camera = 0x2F4, ball = 0x2F8},
  [2] = {state_manager = 0x8045B208, camera = 0x304, ball = 0x308},
}
local VISORS = {[0] = "mp_visor_combat", "mp_visor_xray", "mp_visor_scan", "mp_visor_thermal"}
local CAMERA_SPAWNED = 4

local addresses = nil

-- A word behind a pointer, or nil when the pointer is null or outside RAM.
local function read_at(pointer, offset)
  if not pointer or pointer == 0 then
    return nil
  end
  return mem.u32(pointer + offset)
end

function on_load()
  addresses = REVISIONS[game.revision()]
  if not addresses then
    warn("unknown disc revision " .. game.revision() .. ": no flags raised")
  end
end

function on_frame()
  if not addresses then
    return
  end
  local sm = addresses.state_manager
  local transition = mem.u32(sm + 0xF90)
  local player = mem.u32(sm + 0x84C)
  local visor = read_at(read_at(mem.u32(sm + 0x8B8), 0), 0x14)
  local camera = read_at(player, addresses.camera)
  local ball = read_at(player, addresses.ball)
  local paused = transition ~= nil and transition ~= 0
  local in_play = player ~= nil and player ~= 0 and not paused

  for id = 0, 3 do
    vr.set_flag(VISORS[id], in_play and visor == id)
  end
  vr.set_flag("mp_paused", paused)
  vr.set_flag("mp_morphball", in_play and ball ~= nil and ball ~= 0)
  vr.set_flag("mp_cinematic", in_play and camera == CAMERA_SPAWNED)
end
