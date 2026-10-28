-- Metroid Prime 2: Echoes (NTSC-U, GameCube): EFB copies to RAM only while scanning.
--
-- The scan visor reads EFB copies from the game's memory, so the Sys GameSettings (G2M.ini) turn
-- "Store EFB Copies to Texture Only" off for the game. The copies to RAM slow every frame down,
-- the Quest's most, and nothing else in the game needs them. This patch turns the setting on while
-- another visor is in play or a pause screen shows, and off while the scan visor is on, for this
-- session: the game's own value is back when the patch stops. It follows the flags of the "Game
-- state for the VR profile" patch (metroid-state.lua); without them (that patch off, the title
-- screens) the game's own value applies.
--
-- The cost: the game also measures how much of the sun shows from a copy in its memory, so the
-- sun's glare is gone outside the scan visor.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

patch = {
  title = "EFB copies to RAM only while scanning",
  author = "DolphinXR",
  version = "1.0",
  description = "Stores EFB copies to texture only, which is faster, except while the scan visor "
    .. "is on: it reads them from the game's memory. Needs the \"Game state for the VR profile\" "
    .. "patch.",
  games = {"G2ME01"},
}

local KEY = "EFBToTextureEnable"
-- The visors other than scan.
local OTHER_VISORS = {"mp_visor_combat", "mp_visor_echo", "mp_visor_dark"}

local own = nil  -- the setting's value before the patch's first change

-- true: texture only; false: to RAM too; nil: the game's own value.
local function store_to_texture_only(value)
  if own == nil then
    if value == nil then
      return
    end
    own = vr.setting(KEY)
  end
  if value == nil then
    vr.set_setting(KEY, own)
  else
    vr.set_setting(KEY, value)
  end
end

-- Whether the state patch shows the player in play with another visor, or a pause screen.
local function not_scanning()
  if vr.flag("mp_paused") then
    return true
  end
  for _, flag in ipairs(OTHER_VISORS) do
    if vr.flag(flag) then
      return true
    end
  end
  return false
end

function on_frame()
  if vr.flag("mp_visor_scan") then
    store_to_texture_only(false)
  elseif not_scanning() then
    store_to_texture_only(true)
  else
    store_to_texture_only(nil)
  end
end

function on_unload()
  store_to_texture_only(nil)
end
